// ls_c_api.cpp — the C ABI, implemented as a translation layer over the C++ API.
//
// Nothing here knows how the engine works. Every function converts C arguments
// into C++ ones, calls the same public API a C++ app would call, and converts
// the answer back. That is the whole point: there is one engine, and this file
// is a door onto it rather than a second way in.
//
// Two rules the translation holds to.
//
// Nothing escapes. The C++ API returns std::vector and std::string by value; a
// pointer into one of those, handed to C, would dangle the moment the temporary
// died. So anything with a lifetime is wrapped in an opaque handle that owns
// its storage (ls_raster, ls_blob, ls_package, ls_snapshot), and everything
// else is copied into a caller-supplied buffer.
//
// Nothing throws. The C++ API already promises that, but this layer allocates,
// and allocation can throw where the API below it cannot. Every entry point
// that allocates is wrapped, so a bad_alloc becomes an error code rather than
// an exception unwinding into C, which is undefined behaviour.

#include "livesprite/livesprite_c.h"
#include "livesprite/livesprite.h"

#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

// --------------------------------------------------------------- handles ----

using namespace ls;

int32_t toError(LSError error) {
    return static_cast<int32_t>(error);
}

template<typename Id>
Id asId(ls_id value) {
    Id id;
    id.value = value;
    return id;
}

Vec2f  toVec2f(ls_vec2f v)  { return { v.x, v.y }; }
Vec2i  toVec2i(ls_vec2i v)  { return { v.x, v.y }; }
Color  toColor(ls_color c)  { return { c.r, c.g, c.b, c.a }; }
ls_color fromColor(Color c) { return { c.r, c.g, c.b, c.a }; }

CompileProfile toProfile(const ls_profile& in) {
    CompileProfile profile;
    profile.type = static_cast<CompileProfileType>(in.type);
    profile.outputWidth = in.output_width;
    profile.outputHeight = in.output_height;
    profile.palette = static_cast<PalettePolicy>(in.palette_policy);
    profile.exportOrigin = toVec2i(in.export_origin);
    return profile;
}

// Copies a string into a caller buffer, reporting the size it needed. Passing a
// null buffer is how a caller asks for the size without providing one.
int32_t copyOut(const std::string& text, char* buffer, size_t bufferSize, size_t* needed) {
    const size_t required = text.size() + 1;
    if (needed != nullptr) {
        *needed = required;
    }
    if (buffer == nullptr) {
        return LS_ERROR_BUFFER_TOO_SMALL;
    }
    if (bufferSize < required) {
        return LS_ERROR_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer, text.c_str(), required);
    return LS_OK;
}

// Every entry point that allocates runs inside this, so an allocation failure
// crosses the boundary as an error code instead of as an exception.
template<typename Fn>
int32_t guarded(Fn&& fn) {
    try {
        return fn();
    } catch (const std::bad_alloc&) {
        return LS_ERROR_RASTER_ALLOCATION_FAILED;
    } catch (...) {
        return LS_ERROR_COMPILE_FAILURE;
    }
}

} // namespace

// The opaque types the header promises. Each owns whatever it hands out, so a
// pointer a caller holds stays valid until that caller releases it.
struct ls_context  { std::unique_ptr<LSContext> engine; };
struct ls_raster   { RasterBuffer buffer; };
struct ls_blob     { std::vector<uint8_t> bytes; };
struct ls_package  { std::vector<PackageEntry> entries; };
struct ls_snapshot { DocumentSnapshot snapshot; };

namespace {

// Unwraps a context, or reports why it could not be used. Every entry point
// starts here, so a null context is an error rather than a crash.
#define LS_C_CONTEXT(ctxArg)                                    \
    if ((ctxArg) == nullptr || (ctxArg)->engine == nullptr) {   \
        return LS_ERROR_NULL_ARGUMENT;                          \
    }                                                           \
    LSContext& engine = *(ctxArg)->engine

#define LS_C_REQUIRE(cond)                  \
    if (!(cond)) {                          \
        return LS_ERROR_NULL_ARGUMENT;      \
    }

// Turns a Result<T> carrying an id into an out parameter.
template<typename ResultT>
int32_t outId(const ResultT& result, ls_id* out) {
    if (result.fail()) {
        return toError(result.error);
    }
    *out = result.value.value;
    return LS_OK;
}

} // namespace

namespace {

int32_t setParameter(ls_context* ctx, ls_id op, const char* name, const ParameterValue& value) {
    if (ctx == nullptr || ctx->engine == nullptr || name == nullptr) {
        return LS_ERROR_NULL_ARGUMENT;
    }
    return guarded([&] {
        return toError(ctx->engine->setOperationParameter(asId<OperationId>(op), name, value).error);
    });
}

// Fetches a parameter and hands it to a reader that knows which alternative it
// wants. A parameter that exists but holds a different type is a type mismatch,
// not a missing parameter: the caller asked the wrong question about a real
// field, and saying so is more useful than saying it is not there.
template<typename Fn>
int32_t getParameter(ls_context* ctx, ls_id op, const char* name, Fn&& read) {
    if (ctx == nullptr || ctx->engine == nullptr || name == nullptr) {
        return LS_ERROR_NULL_ARGUMENT;
    }
    return guarded([&] {
        auto value = ctx->engine->getOperationParameter(asId<OperationId>(op), name);
        if (value.fail()) {
            return toError(value.error);
        }
        return read(value.value) ? static_cast<int32_t>(LS_OK)
                                 : static_cast<int32_t>(LS_ERROR_OPERATION_TYPE_MISMATCH);
    });
}

} // namespace

namespace {

int32_t wrapCompile(Result<CompileResult> result, ls_raster** out) {
    if (result.fail()) {
        return toError(result.error);
    }
    auto raster = std::make_unique<ls_raster>();
    raster->buffer = std::move(result.value.raster);
    *out = raster.release();
    return LS_OK;
}

} // namespace

extern "C" {

// ------------------------------------------------------------ engine info ----

uint32_t ls_engine_version(void) {
    return LS_ENGINE_VERSION;
}

const char* ls_error_string(ls_error error) {
    if (error == LS_ERROR_BUFFER_TOO_SMALL) {
        return "BufferTooSmall";
    }
    // lsErrorString returns a view onto a static literal, so the pointer stays
    // valid after this function returns. That is true only because the strings
    // are literals; it would be a dangling pointer if they were built.
    static thread_local std::string scratch;
    scratch = std::string(lsErrorString(static_cast<LSError>(error)));
    return scratch.c_str();
}

// ---------------------------------------------------------------- context ----

ls_context* ls_context_create(void) {
    try {
        auto wrapper = std::make_unique<ls_context>();
        wrapper->engine = LSContext::create();
        if (wrapper->engine == nullptr) {
            return nullptr;
        }
        return wrapper.release();
    } catch (...) {
        return nullptr;
    }
}

void ls_context_destroy(ls_context* ctx) {
    delete ctx;
}

// --------------------------------------------------------------- document ----

ls_error ls_document_create(ls_context* ctx, const char* name,
                            uint32_t width, uint32_t height, ls_id* out_document) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_document != nullptr);
    return guarded([&] {
        DocumentDesc desc;
        desc.name = name != nullptr ? name : "";
        desc.canvasWidth = width;
        desc.canvasHeight = height;
        return outId(engine.createDocument(desc), out_document);
    });
}

ls_error ls_document_destroy(ls_context* ctx, ls_id document) {
    LS_C_CONTEXT(ctx);
    return toError(engine.deleteDocument(asId<DocumentId>(document)).error);
}

ls_error ls_document_canvas(ls_context* ctx, ls_id document,
                            uint32_t* out_width, uint32_t* out_height) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_width != nullptr && out_height != nullptr);
    auto size = engine.getCanvasSize(asId<DocumentId>(document));
    if (size.fail()) {
        return toError(size.error);
    }
    *out_width = static_cast<uint32_t>(size.value.x);
    *out_height = static_cast<uint32_t>(size.value.y);
    return LS_OK;
}

// -------------------------------------------------------- sprites & layers ----

ls_error ls_sprite_create(ls_context* ctx, ls_id document, ls_id* out_sprite) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_sprite != nullptr);
    return guarded([&] {
        return outId(engine.createSprite(asId<DocumentId>(document)), out_sprite);
    });
}

ls_error ls_sprite_destroy(ls_context* ctx, ls_id sprite) {
    LS_C_CONTEXT(ctx);
    return toError(engine.deleteSprite(asId<SpriteId>(sprite)).error);
}

ls_error ls_layer_create(ls_context* ctx, ls_id sprite, const char* name, ls_id* out_layer) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_layer != nullptr);
    return guarded([&] {
        LayerDesc desc;
        desc.name = name != nullptr ? name : "";
        return outId(engine.createLayer(asId<SpriteId>(sprite), desc), out_layer);
    });
}

ls_error ls_layer_destroy(ls_context* ctx, ls_id layer) {
    LS_C_CONTEXT(ctx);
    return toError(engine.deleteLayer(asId<LayerId>(layer)).error);
}

ls_error ls_layer_set_visible(ls_context* ctx, ls_id layer, int visible) {
    LS_C_CONTEXT(ctx);
    return toError(engine.setLayerVisibility(asId<LayerId>(layer), visible != 0).error);
}

ls_error ls_layer_set_opacity(ls_context* ctx, ls_id layer, float opacity) {
    LS_C_CONTEXT(ctx);
    return toError(engine.setLayerOpacity(asId<LayerId>(layer), opacity).error);
}

ls_error ls_sprite_translate(ls_context* ctx, ls_id sprite, ls_vec2f delta) {
    LS_C_CONTEXT(ctx);
    return toError(engine.translateSprite(asId<SpriteId>(sprite), toVec2f(delta)).error);
}

ls_error ls_sprite_rotate(ls_context* ctx, ls_id sprite, float degrees, ls_id pivot) {
    LS_C_CONTEXT(ctx);
    return toError(engine.rotateSprite(asId<SpriteId>(sprite), degrees,
                                       asId<PivotId>(pivot)).error);
}

ls_error ls_sprite_scale(ls_context* ctx, ls_id sprite, ls_vec2f factor, ls_id pivot) {
    LS_C_CONTEXT(ctx);
    return toError(engine.scaleSprite(asId<SpriteId>(sprite), toVec2f(factor),
                                      asId<PivotId>(pivot)).error);
}

ls_error ls_sprite_reset_transform(ls_context* ctx, ls_id sprite) {
    LS_C_CONTEXT(ctx);
    return toError(engine.resetSpriteTransform(asId<SpriteId>(sprite)).error);
}

// --------------------------------------------------------------- geometry ----

ls_error ls_geometry_rect(ls_context* ctx, ls_id document, ls_vec2f origin,
                          float width, float height, float corner_radius,
                          ls_id* out_geometry) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_geometry != nullptr);
    return guarded([&] {
        RectDesc desc;
        desc.origin = toVec2f(origin);
        desc.width = width;
        desc.height = height;
        desc.cornerRadius = corner_radius;
        return outId(engine.createRect(asId<DocumentId>(document), desc), out_geometry);
    });
}

ls_error ls_geometry_circle(ls_context* ctx, ls_id document, ls_vec2f center,
                            float radius, ls_id* out_geometry) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_geometry != nullptr);
    return guarded([&] {
        CircleDesc desc;
        desc.center = toVec2f(center);
        desc.radius = radius;
        return outId(engine.createCircle(asId<DocumentId>(document), desc), out_geometry);
    });
}

ls_error ls_geometry_ellipse(ls_context* ctx, ls_id document, ls_vec2f center,
                             float radius_x, float radius_y, ls_id* out_geometry) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_geometry != nullptr);
    return guarded([&] {
        EllipseDesc desc;
        desc.center = toVec2f(center);
        desc.radiusX = radius_x;
        desc.radiusY = radius_y;
        return outId(engine.createEllipse(asId<DocumentId>(document), desc), out_geometry);
    });
}

ls_error ls_geometry_polyline(ls_context* ctx, ls_id document,
                              const ls_vec2f* points, size_t point_count,
                              int closed, ls_id* out_geometry) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_geometry != nullptr);
    LS_C_REQUIRE(points != nullptr || point_count == 0);
    return guarded([&] {
        PolylineDesc desc;
        desc.points.reserve(point_count);
        for (size_t i = 0; i < point_count; ++i) {
            desc.points.push_back(toVec2f(points[i]));
        }
        desc.closed = closed != 0;
        return outId(engine.createPolyline(asId<DocumentId>(document), desc), out_geometry);
    });
}

ls_error ls_geometry_destroy(ls_context* ctx, ls_id geometry) {
    LS_C_CONTEXT(ctx);
    return toError(engine.deleteGeometry(asId<GeometryId>(geometry)).error);
}

// ---------------------------------------------------------------- regions ----

ls_error ls_region_from_geometry(ls_context* ctx, ls_id geometry, ls_id* out_region) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_region != nullptr);
    return guarded([&] {
        return outId(engine.createRegionFromGeometry(asId<GeometryId>(geometry)), out_region);
    });
}

ls_error ls_region_from_pixels(ls_context* ctx, ls_id document,
                               const ls_vec2i* pixels, const ls_color* colors,
                               size_t pixel_count, ls_id* out_region) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_region != nullptr);
    LS_C_REQUIRE(pixels != nullptr || pixel_count == 0);
    return guarded([&] {
        PixelRegionDesc desc;
        desc.pixels.reserve(pixel_count);
        for (size_t i = 0; i < pixel_count; ++i) {
            PixelInput input;
            input.position = toVec2i(pixels[i]);
            input.color = colors != nullptr ? toColor(colors[i]) : Color{0, 0, 0, 255};
            desc.pixels.push_back(input);
        }
        return outId(engine.createRegionFromPixels(asId<DocumentId>(document), desc), out_region);
    });
}

ls_error ls_region_add_pixels(ls_context* ctx, ls_id region,
                              const ls_vec2i* pixels, size_t pixel_count) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(pixels != nullptr || pixel_count == 0);
    return guarded([&] {
        PixelRegionDesc desc;
        desc.pixels.reserve(pixel_count);
        for (size_t i = 0; i < pixel_count; ++i) {
            PixelInput input;
            input.position = toVec2i(pixels[i]);
            input.color = Color{0, 0, 0, 255};
            desc.pixels.push_back(input);
        }
        return toError(engine.addPixelsToRegion(asId<RegionId>(region), desc).error);
    });
}

ls_error ls_region_erase_pixels(ls_context* ctx, ls_id region,
                                const ls_vec2i* pixels, size_t pixel_count) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(pixels != nullptr || pixel_count == 0);
    return guarded([&] {
        std::vector<Vec2i> points;
        points.reserve(pixel_count);
        for (size_t i = 0; i < pixel_count; ++i) {
            points.push_back(toVec2i(pixels[i]));
        }
        return toError(engine.erasePixelsFromRegion(asId<RegionId>(region), points).error);
    });
}

ls_error ls_region_combine(ls_context* ctx, ls_id a, ls_id b, int32_t op, ls_id* out_region) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_region != nullptr);
    return guarded([&] {
        const RegionId left = asId<RegionId>(a);
        const RegionId right = asId<RegionId>(b);
        switch (op) {
            case LS_REGION_UNION:     return outId(engine.unionRegions(left, right), out_region);
            case LS_REGION_SUBTRACT:  return outId(engine.subtractRegions(left, right), out_region);
            case LS_REGION_INTERSECT: return outId(engine.intersectRegions(left, right), out_region);
            case LS_REGION_XOR:       return outId(engine.xorRegions(left, right), out_region);
            default:                  return static_cast<int32_t>(LS_ERROR_INVALID_PARAMETER);
        }
    });
}

ls_error ls_region_pixel_count(ls_context* ctx, ls_id region, uint64_t* out_count) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_count != nullptr);
    auto intervals = engine.getRegionIntervals(asId<RegionId>(region));
    if (intervals.fail()) {
        return toError(intervals.error);
    }
    uint64_t total = 0;
    for (const Interval& span : intervals.value.intervals) {
        total += static_cast<uint64_t>(span.x1 - span.x0);
    }
    *out_count = total;
    return LS_OK;
}

ls_error ls_region_bind_palette_role(ls_context* ctx, ls_id region, uint32_t role) {
    LS_C_CONTEXT(ctx);
    return toError(engine.bindRegionToPaletteRole(asId<RegionId>(region), role).error);
}

ls_error ls_region_destroy(ls_context* ctx, ls_id region) {
    LS_C_CONTEXT(ctx);
    return toError(engine.deleteRegion(asId<RegionId>(region)).error);
}

// -------------------------------------------------- palettes, ramps, tiles ----

ls_error ls_palette_create(ls_context* ctx, ls_id document, const char* name,
                           const uint32_t* roles, const ls_color* colors,
                           size_t entry_count, ls_id* out_palette) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_palette != nullptr);
    LS_C_REQUIRE((roles != nullptr && colors != nullptr) || entry_count == 0);
    return guarded([&] {
        PaletteDesc desc;
        desc.name = name != nullptr ? name : "";
        desc.entries.reserve(entry_count);
        for (size_t i = 0; i < entry_count; ++i) {
            PaletteColorEntry entry;
            entry.role = roles[i];
            entry.color = toColor(colors[i]);
            desc.entries.push_back(entry);
        }
        return outId(engine.createPalette(asId<DocumentId>(document), desc), out_palette);
    });
}

ls_error ls_palette_set_color(ls_context* ctx, ls_id palette, uint32_t role, ls_color color) {
    LS_C_CONTEXT(ctx);
    return toError(engine.setPaletteColor(asId<PaletteId>(palette), role, toColor(color)).error);
}

ls_error ls_sprite_bind_palette(ls_context* ctx, ls_id sprite, ls_id palette) {
    LS_C_CONTEXT(ctx);
    return toError(engine.bindSpritePalette(asId<SpriteId>(sprite),
                                            asId<PaletteId>(palette)).error);
}

ls_error ls_ramp_create(ls_context* ctx, ls_id document, const char* name,
                        const float* positions, const ls_color* colors,
                        size_t stop_count, int interpolate, ls_id* out_ramp) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_ramp != nullptr);
    LS_C_REQUIRE((positions != nullptr && colors != nullptr) || stop_count == 0);
    return guarded([&] {
        RampDesc desc;
        desc.name = name != nullptr ? name : "";
        desc.interpolate = interpolate != 0;
        desc.stops.reserve(stop_count);
        for (size_t i = 0; i < stop_count; ++i) {
            RampStop stop;
            stop.position = positions[i];
            stop.color = toColor(colors[i]);
            desc.stops.push_back(stop);
        }
        return outId(engine.createRamp(asId<DocumentId>(document), desc), out_ramp);
    });
}

ls_error ls_dither_pattern_create(ls_context* ctx, ls_id document, int32_t kind,
                                  ls_id* out_pattern) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_pattern != nullptr);
    return guarded([&] {
        return outId(engine.createDitherPattern(asId<DocumentId>(document),
                                                static_cast<DitherPatternKind>(kind)),
                     out_pattern);
    });
}

ls_error ls_pattern_create(ls_context* ctx, ls_id document, const char* name,
                           const uint8_t* mask, const ls_color* colors,
                           uint32_t width, uint32_t height, uint32_t levels,
                           ls_id* out_pattern) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_pattern != nullptr);
    LS_C_REQUIRE(mask != nullptr);
    if (width == 0 || height == 0) {
        return LS_ERROR_INVALID_PARAMETER;
    }
    return guarded([&] {
        const size_t cells = static_cast<size_t>(width) * height;
        PatternTileDesc desc;
        desc.name = name != nullptr ? name : "";
        desc.tileWidth = width;
        desc.tileHeight = height;
        desc.levels = levels;
        desc.mask.assign(mask, mask + cells);
        if (colors != nullptr) {
            desc.colors.reserve(cells);
            for (size_t i = 0; i < cells; ++i) {
                desc.colors.push_back(toColor(colors[i]));
            }
        }
        return outId(engine.createPattern(asId<DocumentId>(document), desc), out_pattern);
    });
}

// ------------------------------------------------------------- operations ----

size_t ls_operation_type_count(void) {
    return operationTypeNames().size();
}

ls_error ls_operation_type_name(size_t index, char* buffer, size_t buffer_size,
                                size_t* out_needed) {
    return guarded([&] {
        const std::vector<std::string_view> names = operationTypeNames();
        if (index >= names.size()) {
            return static_cast<int32_t>(LS_ERROR_OUT_OF_BOUNDS);
        }
        return copyOut(std::string(names[index]), buffer, buffer_size, out_needed);
    });
}

ls_error ls_operation_add(ls_context* ctx, ls_id layer, const char* type_name,
                          int32_t at_index, ls_id* out_operation) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_operation != nullptr && type_name != nullptr);
    return guarded([&] {
        Operation op;
        if (!makeOperationOfType(type_name, op)) {
            return static_cast<int32_t>(LS_ERROR_OPERATION_TYPE_MISMATCH);
        }
        return outId(engine.addOperation(asId<LayerId>(layer), op, at_index), out_operation);
    });
}

ls_error ls_operation_remove(ls_context* ctx, ls_id layer, ls_id operation) {
    LS_C_CONTEXT(ctx);
    return toError(engine.removeOperation(asId<LayerId>(layer),
                                          asId<OperationId>(operation)).error);
}

ls_error ls_operation_parameter_count(ls_context* ctx, ls_id operation, size_t* out_count) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_count != nullptr);
    return guarded([&] {
        auto described = engine.describeOperation(asId<OperationId>(operation));
        if (described.fail()) {
            return toError(described.error);
        }
        *out_count = described.value.size();
        return static_cast<int32_t>(LS_OK);
    });
}

ls_error ls_operation_parameter_at(ls_context* ctx, ls_id operation, size_t index,
                                   char* name_buffer, size_t name_buffer_size,
                                   size_t* out_needed, int32_t* out_type) {
    LS_C_CONTEXT(ctx);
    return guarded([&] {
        auto described = engine.describeOperation(asId<OperationId>(operation));
        if (described.fail()) {
            return toError(described.error);
        }
        if (index >= described.value.size()) {
            return static_cast<int32_t>(LS_ERROR_OUT_OF_BOUNDS);
        }
        const ParameterInfo& info = described.value[index];
        if (out_type != nullptr) {
            *out_type = static_cast<int32_t>(info.type);
        }
        return copyOut(info.name, name_buffer, name_buffer_size, out_needed);
    });
}


ls_error ls_operation_set_float(ls_context* ctx, ls_id op, const char* name, float v) {
    return setParameter(ctx, op, name, ParameterValue{v});
}

ls_error ls_operation_set_int(ls_context* ctx, ls_id op, const char* name, int64_t v) {
    return setParameter(ctx, op, name, ParameterValue{v});
}

ls_error ls_operation_set_bool(ls_context* ctx, ls_id op, const char* name, int v) {
    return setParameter(ctx, op, name, ParameterValue{v != 0});
}

ls_error ls_operation_set_id(ls_context* ctx, ls_id op, const char* name, ls_id v) {
    return setParameter(ctx, op, name, ParameterValue{static_cast<uint64_t>(v)});
}

ls_error ls_operation_set_vec2(ls_context* ctx, ls_id op, const char* name, ls_vec2f v) {
    return setParameter(ctx, op, name, ParameterValue{toVec2f(v)});
}

ls_error ls_operation_set_color(ls_context* ctx, ls_id op, const char* name, ls_color v) {
    return setParameter(ctx, op, name, ParameterValue{toColor(v)});
}

ls_error ls_operation_get_float(ls_context* ctx, ls_id op, const char* name, float* out) {
    LS_C_REQUIRE(out != nullptr);
    return getParameter(ctx, op, name, [out](const ParameterValue& value) {
        if (const float* found = std::get_if<float>(&value))    { *out = *found; return true; }
        if (const int64_t* found = std::get_if<int64_t>(&value)) {
            *out = static_cast<float>(*found);
            return true;
        }
        return false;
    });
}

ls_error ls_operation_get_int(ls_context* ctx, ls_id op, const char* name, int64_t* out) {
    LS_C_REQUIRE(out != nullptr);
    return getParameter(ctx, op, name, [out](const ParameterValue& value) {
        if (const int64_t* found = std::get_if<int64_t>(&value)) { *out = *found; return true; }
        if (const bool* found = std::get_if<bool>(&value))       { *out = *found ? 1 : 0; return true; }
        return false;
    });
}

ls_error ls_operation_get_id(ls_context* ctx, ls_id op, const char* name, ls_id* out) {
    LS_C_REQUIRE(out != nullptr);
    return getParameter(ctx, op, name, [out](const ParameterValue& value) {
        if (const uint64_t* found = std::get_if<uint64_t>(&value)) { *out = *found; return true; }
        return false;
    });
}

ls_error ls_operation_get_vec2(ls_context* ctx, ls_id op, const char* name, ls_vec2f* out) {
    LS_C_REQUIRE(out != nullptr);
    return getParameter(ctx, op, name, [out](const ParameterValue& value) {
        if (const Vec2f* found = std::get_if<Vec2f>(&value)) {
            out->x = found->x;
            out->y = found->y;
            return true;
        }
        return false;
    });
}

ls_error ls_operation_get_color(ls_context* ctx, ls_id op, const char* name, ls_color* out) {
    LS_C_REQUIRE(out != nullptr);
    return getParameter(ctx, op, name, [out](const ParameterValue& value) {
        if (const Color* found = std::get_if<Color>(&value)) {
            *out = fromColor(*found);
            return true;
        }
        return false;
    });
}

// ---------------------------------------------------------------- compile ----


ls_error ls_compile_sprite(ls_context* ctx, ls_id sprite, const ls_profile* profile,
                           ls_raster** out_raster) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(profile != nullptr && out_raster != nullptr);
    return guarded([&] {
        return wrapCompile(engine.compileSprite(asId<SpriteId>(sprite), toProfile(*profile)),
                           out_raster);
    });
}

ls_error ls_compile_layer(ls_context* ctx, ls_id layer, const ls_profile* profile,
                          ls_raster** out_raster) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(profile != nullptr && out_raster != nullptr);
    return guarded([&] {
        return wrapCompile(engine.compileLayer(asId<LayerId>(layer), toProfile(*profile)),
                           out_raster);
    });
}

ls_error ls_compile_assembly(ls_context* ctx, ls_id root, const ls_profile* profile,
                             ls_raster** out_raster) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(profile != nullptr && out_raster != nullptr);
    return guarded([&] {
        return wrapCompile(engine.compileAssembly(asId<SpriteId>(root), toProfile(*profile)),
                           out_raster);
    });
}

uint32_t ls_raster_width(const ls_raster* raster) {
    return raster != nullptr ? raster->buffer.width : 0;
}

uint32_t ls_raster_height(const ls_raster* raster) {
    return raster != nullptr ? raster->buffer.height : 0;
}

const uint8_t* ls_raster_data(const ls_raster* raster) {
    if (raster == nullptr || raster->buffer.pixels.empty()) {
        return nullptr;
    }
    return raster->buffer.pixels.data();
}

size_t ls_raster_size(const ls_raster* raster) {
    return raster != nullptr ? raster->buffer.pixels.size() : 0;
}

void ls_raster_release(ls_raster* raster) {
    delete raster;
}

// ----------------------------------------------------- pivots and sockets ----

ls_error ls_pivot_create(ls_context* ctx, ls_id sprite, const char* name,
                         ls_vec2f position, ls_id* out_pivot) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_pivot != nullptr);
    return guarded([&] {
        PivotDesc desc;
        desc.name = name != nullptr ? name : "";
        desc.position = toVec2f(position);
        return outId(engine.createPivot(asId<SpriteId>(sprite), desc), out_pivot);
    });
}

ls_error ls_socket_create(ls_context* ctx, ls_id sprite, const char* name,
                          ls_vec2f position, float angle_degrees, ls_id* out_socket) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_socket != nullptr);
    return guarded([&] {
        SocketDesc desc;
        desc.name = name != nullptr ? name : "";
        desc.position = toVec2f(position);
        desc.angle = angle_degrees;
        return outId(engine.addSocket(asId<SpriteId>(sprite), desc), out_socket);
    });
}

ls_error ls_sprite_attach(ls_context* ctx, ls_id child, ls_id socket, ls_id child_pivot) {
    LS_C_CONTEXT(ctx);
    return guarded([&] {
        AttachmentDesc desc;
        desc.socket = asId<SocketId>(socket);
        desc.childPivot = asId<PivotId>(child_pivot);
        return toError(engine.attachSprite(asId<SpriteId>(child), desc).error);
    });
}

ls_error ls_sprite_detach(ls_context* ctx, ls_id child) {
    LS_C_CONTEXT(ctx);
    return toError(engine.detachSprite(asId<SpriteId>(child)).error);
}

// -------------------------------------------------------------- documents ----

ls_error ls_document_serialize(ls_context* ctx, ls_id document, ls_blob** out_blob) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_blob != nullptr);
    return guarded([&] {
        auto data = engine.serializeDocument(asId<DocumentId>(document));
        if (data.fail()) {
            return toError(data.error);
        }
        auto blob = std::make_unique<ls_blob>();
        blob->bytes = std::move(data.value.bytes);
        *out_blob = blob.release();
        return static_cast<int32_t>(LS_OK);
    });
}

ls_error ls_document_deserialize(ls_context* ctx, const uint8_t* data, size_t size,
                                 ls_id* out_document) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_document != nullptr);
    LS_C_REQUIRE(data != nullptr || size == 0);
    return guarded([&] {
        SerializedData serialized;
        serialized.bytes.assign(data, data + size);
        serialized.formatTag = "livesprite/document";
        return outId(engine.deserializeDocument(serialized), out_document);
    });
}

const uint8_t* ls_blob_data(const ls_blob* blob) {
    if (blob == nullptr || blob->bytes.empty()) {
        return nullptr;
    }
    return blob->bytes.data();
}

size_t ls_blob_size(const ls_blob* blob) {
    return blob != nullptr ? blob->bytes.size() : 0;
}

void ls_blob_release(ls_blob* blob) {
    delete blob;
}

// --------------------------------------------------------------- packages ----

int ls_package_entry_name_valid(const char* name) {
    if (name == nullptr) {
        return 0;
    }
    return LSContext::isValidPackageEntryName(name) ? 1 : 0;
}

ls_package* ls_package_builder_create(void) {
    try {
        return new ls_package();
    } catch (...) {
        return nullptr;
    }
}

ls_error ls_package_builder_add(ls_package* builder, const char* name,
                                const char* content_type,
                                const uint8_t* data, size_t size) {
    LS_C_REQUIRE(builder != nullptr && name != nullptr);
    LS_C_REQUIRE(data != nullptr || size == 0);
    return guarded([&] {
        PackageEntry entry;
        entry.name = name;
        entry.contentType = content_type != nullptr ? content_type : "";
        entry.data.assign(data, data + size);
        builder->entries.push_back(std::move(entry));
        return static_cast<int32_t>(LS_OK);
    });
}

void ls_package_builder_release(ls_package* builder) {
    delete builder;
}

ls_error ls_package_write(ls_context* ctx, ls_id document, const ls_package* builder,
                          ls_blob** out_blob) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_blob != nullptr);
    return guarded([&] {
        const std::vector<PackageEntry> empty;
        auto written = engine.writePackage(asId<DocumentId>(document),
                                           builder != nullptr ? builder->entries : empty);
        if (written.fail()) {
            return toError(written.error);
        }
        auto blob = std::make_unique<ls_blob>();
        blob->bytes = std::move(written.value.bytes);
        *out_blob = blob.release();
        return static_cast<int32_t>(LS_OK);
    });
}

ls_error ls_package_read(ls_context* ctx, const uint8_t* data, size_t size,
                         ls_id* out_document, ls_package** out_reader) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_document != nullptr);
    LS_C_REQUIRE(data != nullptr || size == 0);
    return guarded([&] {
        SerializedData package;
        package.bytes.assign(data, data + size);

        auto reader = std::make_unique<ls_package>();
        auto loaded = engine.loadPackage(package, &reader->entries);
        if (loaded.fail()) {
            return toError(loaded.error);
        }
        *out_document = loaded.value.value;
        if (out_reader != nullptr) {
            *out_reader = reader.release();
        }
        return static_cast<int32_t>(LS_OK);
    });
}

size_t ls_package_entry_count(const ls_package* reader) {
    return reader != nullptr ? reader->entries.size() : 0;
}

ls_error ls_package_entry_at(const ls_package* reader, size_t index,
                             char* name_buffer, size_t name_buffer_size,
                             size_t* out_name_needed,
                             const uint8_t** out_data, size_t* out_size) {
    LS_C_REQUIRE(reader != nullptr);
    if (index >= reader->entries.size()) {
        return LS_ERROR_OUT_OF_BOUNDS;
    }
    const PackageEntry& entry = reader->entries[index];
    if (out_data != nullptr) {
        *out_data = entry.data.empty() ? nullptr : entry.data.data();
    }
    if (out_size != nullptr) {
        *out_size = entry.data.size();
    }
    return guarded([&] {
        return copyOut(entry.name, name_buffer, name_buffer_size, out_name_needed);
    });
}

// ------------------------------------------------------ metadata and undo ----

ls_error ls_metadata_set(ls_context* ctx, ls_id entity, const char* key, const char* value) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(key != nullptr && value != nullptr);
    return guarded([&] {
        return toError(engine.setMetadata(entity, key, value).error);
    });
}

ls_error ls_metadata_get(ls_context* ctx, ls_id entity, const char* key,
                         char* buffer, size_t buffer_size, size_t* out_needed) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(key != nullptr);
    return guarded([&] {
        auto value = engine.getMetadata(entity, key);
        if (value.fail()) {
            return toError(value.error);
        }
        return copyOut(value.value, buffer, buffer_size, out_needed);
    });
}

ls_error ls_snapshot_create(ls_context* ctx, ls_id document, ls_snapshot** out_snapshot) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(out_snapshot != nullptr);
    return guarded([&] {
        auto captured = engine.snapshotDocumentState(asId<DocumentId>(document));
        if (captured.fail()) {
            return toError(captured.error);
        }
        auto wrapper = std::make_unique<ls_snapshot>();
        wrapper->snapshot = std::move(captured.value);
        *out_snapshot = wrapper.release();
        return static_cast<int32_t>(LS_OK);
    });
}

ls_error ls_snapshot_restore(ls_context* ctx, ls_id document, const ls_snapshot* snapshot) {
    LS_C_CONTEXT(ctx);
    LS_C_REQUIRE(snapshot != nullptr);
    return toError(engine.restoreDocumentState(asId<DocumentId>(document),
                                               snapshot->snapshot).error);
}

void ls_snapshot_release(ls_snapshot* snapshot) {
    delete snapshot;
}

} // extern "C"
