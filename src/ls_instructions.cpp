// ls_instructions.cpp — addressable parameters and live frame application.
//
// The engine has no idea what a keyframe is. What it provides is the surface an
// animation system needs from underneath: name any operation parameter, set it
// to an absolute value, hand over a batch that lands as a single frame, and
// compile. Whether those values came from a timeline, a puppet solver, a slider
// or a network packet is the caller's business.

#include "ls_internal.h"
#include "ls_reflect.h"

#include <type_traits>

namespace ls {
namespace {

// --- value conversion ------------------------------------------------------

template<typename T>
ParameterType typeOf() {
    if constexpr (std::is_same_v<T, bool>)          { return ParameterType::Bool; }
    else if constexpr (std::is_same_v<T, float>)    { return ParameterType::Float; }
    else if constexpr (std::is_same_v<T, Vec2f>)    { return ParameterType::Vec2; }
    else if constexpr (std::is_same_v<T, Color>)    { return ParameterType::Color; }
    else if constexpr (std::is_same_v<T, Mat3f>)    { return ParameterType::Matrix; }
    else if constexpr (std::is_same_v<T, std::string>) { return ParameterType::Text; }
    else if constexpr (std::is_enum_v<T>)           { return ParameterType::Int; }
    else if constexpr (std::is_integral_v<T>)       { return ParameterType::Int; }
    else if constexpr (std::is_same_v<T, PluginParams>) { return ParameterType::Unsupported; }
    else                                            { return ParameterType::Unsupported; }
}

template<typename T>
bool readValue(const T& field, ParameterValue& out) {
    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, float> ||
                  std::is_same_v<T, Vec2f> || std::is_same_v<T, Color> ||
                  std::is_same_v<T, Mat3f> || std::is_same_v<T, std::string>) {
        out = field;
        return true;
    } else if constexpr (std::is_enum_v<T> || std::is_integral_v<T>) {
        out = static_cast<int64_t>(field);
        return true;
    } else {
        // A field this interface cannot express, such as a list of handles.
        return false;
    }
}

// Numeric values coerce between int and float: a caller driving a float from a
// slider that hands back whole numbers should not have to care.
bool numericOf(const ParameterValue& value, double& out) {
    if (const float* asFloat = std::get_if<float>(&value))       { out = *asFloat; return true; }
    if (const int64_t* asInt = std::get_if<int64_t>(&value))     { out = static_cast<double>(*asInt); return true; }
    if (const uint64_t* asId = std::get_if<uint64_t>(&value))    { out = static_cast<double>(*asId); return true; }
    if (const bool* asBool = std::get_if<bool>(&value))          { out = *asBool ? 1.0 : 0.0; return true; }
    return false;
}

template<typename T>
bool writeValue(T& field, const ParameterValue& value) {
    if constexpr (std::is_same_v<T, bool>) {
        if (const bool* asBool = std::get_if<bool>(&value)) {
            field = *asBool;
            return true;
        }
        double numeric = 0.0;
        if (numericOf(value, numeric)) {
            field = numeric != 0.0;
            return true;
        }
        return false;
    } else if constexpr (std::is_same_v<T, float>) {
        double numeric = 0.0;
        if (!numericOf(value, numeric)) {
            return false;
        }
        field = static_cast<float>(numeric);
        return true;
    } else if constexpr (std::is_enum_v<T>) {
        double numeric = 0.0;
        if (!numericOf(value, numeric)) {
            return false;
        }
        field = static_cast<T>(static_cast<uint32_t>(numeric));
        return true;
    } else if constexpr (std::is_integral_v<T>) {
        double numeric = 0.0;
        if (!numericOf(value, numeric)) {
            return false;
        }
        field = static_cast<T>(numeric);
        return true;
    } else if constexpr (std::is_same_v<T, Vec2f>) {
        if (const Vec2f* asVec = std::get_if<Vec2f>(&value)) {
            field = *asVec;
            return true;
        }
        return false;
    } else if constexpr (std::is_same_v<T, Color>) {
        if (const Color* asColor = std::get_if<Color>(&value)) {
            field = *asColor;
            return true;
        }
        return false;
    } else if constexpr (std::is_same_v<T, Mat3f>) {
        if (const Mat3f* asMatrix = std::get_if<Mat3f>(&value)) {
            field = *asMatrix;
            return true;
        }
        return false;
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (const std::string* asText = std::get_if<std::string>(&value)) {
            field = *asText;
            return true;
        }
        return false;
    } else {
        return false;
    }
}

// --- archives over the shared field tables ---------------------------------

struct ListParameters {
    std::vector<ParameterInfo>* out = nullptr;

    template<typename T>
    void field(const char* name, T& value) {
        (void)value;
        out->push_back(ParameterInfo{ name, typeOf<T>() });
    }

    template<typename Tag>
    void field(const char* name, TypedId<Tag>& value) {
        (void)value;
        out->push_back(ParameterInfo{ name, ParameterType::EntityId });
    }
};

struct GetParameter {
    std::string_view wanted;
    ParameterValue* out = nullptr;
    bool found = false;
    bool readable = false;

    template<typename T>
    void field(const char* name, T& value) {
        if (found || wanted != name) {
            return;
        }
        found = true;
        readable = readValue(value, *out);
    }

    template<typename Tag>
    void field(const char* name, TypedId<Tag>& value) {
        if (found || wanted != name) {
            return;
        }
        found = true;
        *out = static_cast<uint64_t>(value.value);
        readable = true;
    }
};

struct SetParameter {
    std::string_view wanted;
    const ParameterValue* in = nullptr;
    bool found = false;
    bool applied = false;

    template<typename T>
    void field(const char* name, T& value) {
        if (found || wanted != name) {
            return;
        }
        found = true;
        applied = writeValue(value, *in);
    }

    template<typename Tag>
    void field(const char* name, TypedId<Tag>& value) {
        if (found || wanted != name) {
            return;
        }
        found = true;
        double numeric = 0.0;
        if (!numericOf(*in, numeric) || numeric < 0.0) {
            return;
        }
        value.value = static_cast<uint64_t>(numeric);
        applied = true;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Parameter addressing
// ---------------------------------------------------------------------------

Result<std::vector<ParameterInfo>> LSContext::describeOperation(OperationId id) const {
    const OperationData* data = impl_->findOperation(id);
    if (data == nullptr) {
        return Result<std::vector<ParameterInfo>>::err(LSError::InvalidId);
    }

    std::vector<ParameterInfo> parameters;
    Operation copy = data->op;
    ListParameters archive{ &parameters };
    reflect::visitOperation(archive, copy);
    return Result<std::vector<ParameterInfo>>::ok(std::move(parameters));
}

Result<ParameterValue> LSContext::getOperationParameter(OperationId id,
                                                        std::string_view name) const {
    const OperationData* data = impl_->findOperation(id);
    if (data == nullptr) {
        return Result<ParameterValue>::err(LSError::InvalidId);
    }

    ParameterValue value;
    Operation copy = data->op;
    GetParameter archive;
    archive.wanted = name;
    archive.out = &value;
    reflect::visitOperation(archive, copy);

    if (!archive.found) {
        return Result<ParameterValue>::err(LSError::InvalidParameter);
    }
    if (!archive.readable) {
        // A field this interface cannot express, such as a list of handles.
        return Result<ParameterValue>::err(LSError::OperationTypeMismatch);
    }
    return Result<ParameterValue>::ok(std::move(value));
}

VoidResult LSContext::setOperationParameter(OperationId id, std::string_view name,
                                            const ParameterValue& value) {
    OperationData* data = impl_->findOperation(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }

    Operation updated = data->op;
    SetParameter archive;
    archive.wanted = name;
    archive.in = &value;
    reflect::visitOperation(archive, updated);

    if (!archive.found) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    if (!archive.applied) {
        return VoidResult::err(LSError::OperationTypeMismatch);
    }

    // Route through updateOperation so dependencies and dirty state are
    // maintained exactly as they are for any other edit.
    return updateOperation(id, std::move(updated));
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

VoidResult LSContext::applyInstructions(const std::vector<Instruction>& instructions) {
    // Validate the whole batch first. A frame that cannot be applied in full
    // leaves the document as it was, rather than half posed.
    for (const Instruction& instruction : instructions) {
        LSError error = LSError::None;

        if (const auto* set = std::get_if<SetOperationParameter>(&instruction)) {
            const OperationData* data = impl_->findOperation(set->operation);
            if (data == nullptr) {
                error = LSError::InvalidId;
            } else {
                Operation probe = data->op;
                SetParameter archive;
                archive.wanted = set->parameter;
                archive.in = &set->value;
                reflect::visitOperation(archive, probe);
                if (!archive.found) {
                    error = LSError::InvalidParameter;
                } else if (!archive.applied) {
                    error = LSError::OperationTypeMismatch;
                }
            }
        } else if (const auto* transform = std::get_if<SetSpriteTransformInstruction>(&instruction)) {
            error = impl_->findSprite(transform->sprite) == nullptr ? LSError::InvalidId
                                                                    : LSError::None;
        } else if (const auto* visible = std::get_if<SetLayerVisibilityInstruction>(&instruction)) {
            error = impl_->findLayer(visible->layer) == nullptr ? LSError::InvalidId : LSError::None;
        } else if (const auto* opacity = std::get_if<SetLayerOpacityInstruction>(&instruction)) {
            if (impl_->findLayer(opacity->layer) == nullptr) {
                error = LSError::InvalidId;
            } else if (opacity->opacity < 0.f || opacity->opacity > 1.f) {
                error = LSError::InvalidParameter;
            }
        } else if (const auto* palette = std::get_if<SetPaletteColorInstruction>(&instruction)) {
            if (impl_->findPalette(palette->palette) == nullptr) {
                error = LSError::InvalidId;
            } else if (palette->role == kColorRoleNone) {
                error = LSError::InvalidParameter;
            }
        } else if (const auto* attach = std::get_if<AttachInstruction>(&instruction)) {
            error = impl_->findSprite(attach->child) == nullptr ||
                    impl_->findSocket(attach->attachment.socket) == nullptr
                ? LSError::InvalidId : LSError::None;
        } else if (const auto* detach = std::get_if<DetachInstruction>(&instruction)) {
            const SpriteData* sprite = impl_->findSprite(detach->child);
            if (sprite == nullptr) {
                error = LSError::InvalidId;
            } else if (!sprite->attached) {
                error = LSError::InvalidParameter;
            }
        }

        if (error != LSError::None) {
            return VoidResult::err(error);
        }
    }

    for (const Instruction& instruction : instructions) {
        VoidResult applied = VoidResult::success();

        if (const auto* set = std::get_if<SetOperationParameter>(&instruction)) {
            applied = setOperationParameter(set->operation, set->parameter, set->value);
        } else if (const auto* transform = std::get_if<SetSpriteTransformInstruction>(&instruction)) {
            applied = setSpriteTransform(transform->sprite, transform->transform);
        } else if (const auto* visible = std::get_if<SetLayerVisibilityInstruction>(&instruction)) {
            applied = setLayerVisibility(visible->layer, visible->visible);
        } else if (const auto* opacity = std::get_if<SetLayerOpacityInstruction>(&instruction)) {
            applied = setLayerOpacity(opacity->layer, opacity->opacity);
        } else if (const auto* palette = std::get_if<SetPaletteColorInstruction>(&instruction)) {
            applied = setPaletteColor(palette->palette, palette->role, palette->color);
        } else if (const auto* attach = std::get_if<AttachInstruction>(&instruction)) {
            applied = attachSprite(attach->child, attach->attachment);
        } else if (const auto* detach = std::get_if<DetachInstruction>(&instruction)) {
            applied = detachSprite(detach->child);
        }

        if (applied.fail()) {
            // Validation passed, so this can only be a rule the batch could not
            // see coming, such as an attachment that would close a cycle.
            return applied;
        }
    }

    return VoidResult::success();
}

Result<CompileResult> LSContext::renderFrame(SpriteId root,
                                             const std::vector<Instruction>& instructions,
                                             const CompileProfile& profile) {
    if (impl_->findSprite(root) == nullptr) {
        return Result<CompileResult>::err(LSError::InvalidId);
    }
    auto applied = applyInstructions(instructions);
    if (applied.fail()) {
        return Result<CompileResult>::err(applied.error);
    }
    return compileAssembly(root, profile);
}

} // namespace ls
