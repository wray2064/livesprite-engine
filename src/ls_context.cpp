// ls_context.cpp — LSContext storage and CRUD.
//
// Documents, sprites, layers, groups, geometry, regions, operations, palettes,
// ramps, patterns, pivots, sockets, boundaries, and plugin registration.
// Compilation lives in ls_compile.cpp, dirty tracking in ls_dependency.cpp,
// and save/load in ls_serialize.cpp.

#include "ls_internal.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <type_traits>

namespace ls {
namespace {

// Member detection: most operation structs share field names, so dependency
// collection and type naming can be written once instead of forty times.
#define LS_DETECT_MEMBER(NAME)                                                   \
    template<class T, class = void> struct has_##NAME : std::false_type {};      \
    template<class T> struct has_##NAME<T,                                       \
        std::void_t<decltype(std::declval<const T&>().NAME)>> : std::true_type {}

LS_DETECT_MEMBER(targetRegion);
LS_DETECT_MEMBER(targetLayer);
LS_DETECT_MEMBER(targetSprite);
LS_DETECT_MEMBER(target);
LS_DETECT_MEMBER(child);
LS_DETECT_MEMBER(sourceLayer);
LS_DETECT_MEMBER(ramp);
LS_DETECT_MEMBER(pattern);
LS_DETECT_MEMBER(strokePattern);
LS_DETECT_MEMBER(brushPattern);
LS_DETECT_MEMBER(polyline);
LS_DETECT_MEMBER(curve);
LS_DETECT_MEMBER(path);
LS_DETECT_MEMBER(envelopeCurve);
LS_DETECT_MEMBER(targetShape);
LS_DETECT_MEMBER(boundary);
LS_DETECT_MEMBER(influenceRegion);
LS_DETECT_MEMBER(limitBoundary);
LS_DETECT_MEMBER(pivot);
LS_DETECT_MEMBER(socket);
LS_DETECT_MEMBER(childPivot);
LS_DETECT_MEMBER(targetOutlineOp);
LS_DETECT_MEMBER(outlineA);
LS_DETECT_MEMBER(outlineB);
LS_DETECT_MEMBER(outlineOps);

#undef LS_DETECT_MEMBER

void pushId(std::vector<uint64_t>& out, uint64_t id) {
    if (id != 0 && std::find(out.begin(), out.end(), id) == out.end()) {
        out.push_back(id);
    }
}

std::string colorText(Color c) {
    std::ostringstream out;
    out << "#" << std::hex;
    const uint32_t packed = (static_cast<uint32_t>(c.r) << 16) |
                            (static_cast<uint32_t>(c.g) << 8) |
                             static_cast<uint32_t>(c.b);
    out.width(6);
    out.fill('0');
    out << packed;
    if (c.a != 255) {
        out << std::dec << " a" << static_cast<int>(c.a);
    }
    return out.str();
}

float colorDistanceSq(Color a, Color b) {
    // Weighted RGB distance; alpha counts so a transparent role never wins
    // against an opaque one.
    const float dr = static_cast<float>(a.r) - static_cast<float>(b.r);
    const float dg = static_cast<float>(a.g) - static_cast<float>(b.g);
    const float db = static_cast<float>(a.b) - static_cast<float>(b.b);
    const float da = static_cast<float>(a.a) - static_cast<float>(b.a);
    return 0.30f * dr * dr + 0.59f * dg * dg + 0.11f * db * db + 1.0f * da * da;
}

} // namespace

// ---------------------------------------------------------------------------
// Operation reflection
// ---------------------------------------------------------------------------

std::string_view operationTypeName(const Operation& op) {
    return std::visit([](const auto& concrete) -> std::string_view {
        using Op = std::decay_t<decltype(concrete)>;
#define LS_OP_NAME(T) if constexpr (std::is_same_v<Op, T>) { return #T; } else
        LS_OP_NAME(FillSolidOp)
        LS_OP_NAME(FillGradientOp)
        LS_OP_NAME(FillRampOp)
        LS_OP_NAME(FillDitherOp)
        LS_OP_NAME(FillNoiseOp)
        LS_OP_NAME(FillLinePatternOp)
        LS_OP_NAME(FillTexturePatternOp)
        LS_OP_NAME(FillSemanticColorOp)
        LS_OP_NAME(StrokePolylineOp)
        LS_OP_NAME(StrokeCurveOp)
        LS_OP_NAME(StrokeRegionBoundaryOp)
        LS_OP_NAME(StrokeBrushOp)
        LS_OP_NAME(StrokePixelPathOp)
        LS_OP_NAME(GenerateSilhouetteOutlineOp)
        LS_OP_NAME(GenerateInnerOutlineOp)
        LS_OP_NAME(GenerateOuterOutlineOp)
        LS_OP_NAME(GenerateRegionOutlineOp)
        LS_OP_NAME(GenerateMaterialBoundaryOutlineOp)
        LS_OP_NAME(CleanupOutlineOp)
        LS_OP_NAME(JoinCornersOp)
        LS_OP_NAME(ResolveOutlineCollisionsOp)
        LS_OP_NAME(TranslateOp)
        LS_OP_NAME(RotateOp)
        LS_OP_NAME(ScaleOp)
        LS_OP_NAME(MirrorOp)
        LS_OP_NAME(ShearOp)
        LS_OP_NAME(SkewOp)
        LS_OP_NAME(SquashOp)
        LS_OP_NAME(StretchOp)
        LS_OP_NAME(MatrixTransformOp)
        LS_OP_NAME(PivotTransformOp)
        LS_OP_NAME(AnchorTransformOp)
        LS_OP_NAME(BendOp)
        LS_OP_NAME(WarpOp)
        LS_OP_NAME(LatticeDeformOp)
        LS_OP_NAME(EnvelopeDeformOp)
        LS_OP_NAME(PinDeformOp)
        LS_OP_NAME(WeightedDeformOp)
        LS_OP_NAME(BoundaryDeformOp)
        LS_OP_NAME(PathDeformOp)
        LS_OP_NAME(PluginOp)
        { return "UnknownOp"; }
#undef LS_OP_NAME
    }, op);
}

bool operationIsTransform(const Operation& op) {
    return std::visit([](const auto& concrete) {
        using Op = std::decay_t<decltype(concrete)>;
        return std::is_same_v<Op, TranslateOp> || std::is_same_v<Op, RotateOp> ||
               std::is_same_v<Op, ScaleOp>     || std::is_same_v<Op, MirrorOp> ||
               std::is_same_v<Op, ShearOp>     || std::is_same_v<Op, SkewOp> ||
               std::is_same_v<Op, SquashOp>    || std::is_same_v<Op, StretchOp> ||
               std::is_same_v<Op, MatrixTransformOp> ||
               std::is_same_v<Op, PivotTransformOp>  ||
               std::is_same_v<Op, AnchorTransformOp> ||
               std::is_same_v<Op, BendOp>  || std::is_same_v<Op, WarpOp> ||
               std::is_same_v<Op, LatticeDeformOp>  ||
               std::is_same_v<Op, EnvelopeDeformOp> ||
               std::is_same_v<Op, PinDeformOp>      ||
               std::is_same_v<Op, WeightedDeformOp> ||
               std::is_same_v<Op, BoundaryDeformOp> ||
               std::is_same_v<Op, PathDeformOp>;
    }, op);
}

std::vector<uint64_t> operationDependencies(const Operation& op) {
    std::vector<uint64_t> out;
    std::visit([&out](const auto& concrete) {
        using Op = std::decay_t<decltype(concrete)>;
        if constexpr (has_targetRegion<Op>::value)    pushId(out, concrete.targetRegion.value);
        if constexpr (has_targetLayer<Op>::value)     pushId(out, concrete.targetLayer.value);
        if constexpr (has_targetSprite<Op>::value)    pushId(out, concrete.targetSprite.value);
        if constexpr (has_target<Op>::value)          pushId(out, concrete.target.value);
        if constexpr (has_child<Op>::value)           pushId(out, concrete.child.value);
        if constexpr (has_sourceLayer<Op>::value)     pushId(out, concrete.sourceLayer.value);
        if constexpr (has_ramp<Op>::value)            pushId(out, concrete.ramp.value);
        if constexpr (has_pattern<Op>::value)         pushId(out, concrete.pattern.value);
        if constexpr (has_strokePattern<Op>::value)   pushId(out, concrete.strokePattern.value);
        if constexpr (has_brushPattern<Op>::value)    pushId(out, concrete.brushPattern.value);
        if constexpr (has_polyline<Op>::value)        pushId(out, concrete.polyline.value);
        if constexpr (has_curve<Op>::value)           pushId(out, concrete.curve.value);
        if constexpr (has_path<Op>::value)            pushId(out, concrete.path.value);
        if constexpr (has_envelopeCurve<Op>::value)   pushId(out, concrete.envelopeCurve.value);
        if constexpr (has_targetShape<Op>::value)     pushId(out, concrete.targetShape.value);
        if constexpr (has_boundary<Op>::value)        pushId(out, concrete.boundary.value);
        if constexpr (has_influenceRegion<Op>::value) pushId(out, concrete.influenceRegion.value);
        if constexpr (has_limitBoundary<Op>::value)   pushId(out, concrete.limitBoundary.value);
        if constexpr (has_pivot<Op>::value)           pushId(out, concrete.pivot.value);
        if constexpr (has_socket<Op>::value)          pushId(out, concrete.socket.value);
        if constexpr (has_childPivot<Op>::value)      pushId(out, concrete.childPivot.value);
        if constexpr (has_targetOutlineOp<Op>::value) pushId(out, concrete.targetOutlineOp.value);
        if constexpr (has_outlineA<Op>::value)        pushId(out, concrete.outlineA.value);
        if constexpr (has_outlineB<Op>::value)        pushId(out, concrete.outlineB.value);
        if constexpr (has_outlineOps<Op>::value) {
            for (const OperationId& id : concrete.outlineOps) {
                pushId(out, id.value);
            }
        }
        if constexpr (std::is_same_v<Op, PluginOp>) {
            // Built-in traversal cannot know plugin semantics; the registry
            // supplies those through PluginOperationDesc::getDependencies.
        }
    }, op);
    return out;
}

std::string_view lsErrorString(LSError err) {
    switch (err) {
        case LSError::None:                       return "None";
        case LSError::InvalidId:                  return "InvalidId";
        case LSError::InvalidParameter:           return "InvalidParameter";
        case LSError::NullArgument:               return "NullArgument";
        case LSError::OutOfBounds:                return "OutOfBounds";
        case LSError::DependencyCycle:            return "DependencyCycle";
        case LSError::CompileFailure:             return "CompileFailure";
        case LSError::SerializationFailure:       return "SerializationFailure";
        case LSError::DeserializationFailure:     return "DeserializationFailure";
        case LSError::VersionMismatch:            return "VersionMismatch";
        case LSError::VersionMigrationFailed:     return "VersionMigrationFailed";
        case LSError::PluginNotFound:             return "PluginNotFound";
        case LSError::PluginError:                return "PluginError";
        case LSError::PluginDeterminismViolation: return "PluginDeterminismViolation";
        case LSError::OperationTypeMismatch:      return "OperationTypeMismatch";
        case LSError::RegionEmpty:                return "RegionEmpty";
        case LSError::PaletteNotBound:            return "PaletteNotBound";
        case LSError::RasterAllocationFailed:     return "RasterAllocationFailed";
        case LSError::NotImplemented:             return "NotImplemented";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Impl lookups
// ---------------------------------------------------------------------------

namespace {
template<typename Table, typename IdT>
auto* lookup(Table& table, IdT id) {
    auto it = table.find(id.value);
    return it == table.end() ? nullptr : &it->second;
}
} // namespace

DocumentData*  LSContext::Impl::findDocument(DocumentId id)   { return lookup(documents, id); }
SpriteData*    LSContext::Impl::findSprite(SpriteId id)       { return lookup(sprites, id); }
LayerData*     LSContext::Impl::findLayer(LayerId id)         { return lookup(layers, id); }
GroupData*     LSContext::Impl::findGroup(GroupId id)         { return lookup(groups, id); }
GeometryData*  LSContext::Impl::findGeometry(GeometryId id)   { return lookup(geometry, id); }
RegionData*    LSContext::Impl::findRegion(RegionId id)       { return lookup(regions, id); }
OperationData* LSContext::Impl::findOperation(OperationId id) { return lookup(operations, id); }
PaletteData*   LSContext::Impl::findPalette(PaletteId id)     { return lookup(palettes, id); }
RampData*      LSContext::Impl::findRamp(RampId id)           { return lookup(ramps, id); }
PatternData*   LSContext::Impl::findPattern(PatternId id)     { return lookup(patterns, id); }
PivotData*     LSContext::Impl::findPivot(PivotId id)         { return lookup(pivots, id); }
SocketData*    LSContext::Impl::findSocket(SocketId id)       { return lookup(sockets, id); }
BoundaryData*  LSContext::Impl::findBoundary(BoundaryId id)   { return lookup(boundaries, id); }

const DocumentData*  LSContext::Impl::findDocument(DocumentId id) const   { return lookup(documents, id); }
const SpriteData*    LSContext::Impl::findSprite(SpriteId id) const       { return lookup(sprites, id); }
const LayerData*     LSContext::Impl::findLayer(LayerId id) const         { return lookup(layers, id); }
const GroupData*     LSContext::Impl::findGroup(GroupId id) const         { return lookup(groups, id); }
const GeometryData*  LSContext::Impl::findGeometry(GeometryId id) const   { return lookup(geometry, id); }
const RegionData*    LSContext::Impl::findRegion(RegionId id) const       { return lookup(regions, id); }
const OperationData* LSContext::Impl::findOperation(OperationId id) const { return lookup(operations, id); }
const PaletteData*   LSContext::Impl::findPalette(PaletteId id) const     { return lookup(palettes, id); }
const RampData*      LSContext::Impl::findRamp(RampId id) const           { return lookup(ramps, id); }
const PatternData*   LSContext::Impl::findPattern(PatternId id) const     { return lookup(patterns, id); }
const PivotData*     LSContext::Impl::findPivot(PivotId id) const         { return lookup(pivots, id); }
const SocketData*    LSContext::Impl::findSocket(SocketId id) const       { return lookup(sockets, id); }
const BoundaryData*  LSContext::Impl::findBoundary(BoundaryId id) const   { return lookup(boundaries, id); }

DocumentId LSContext::Impl::documentOfSprite(SpriteId id) const {
    const SpriteData* sprite = findSprite(id);
    return sprite ? sprite->document : DocumentId::null();
}

SpriteId LSContext::Impl::spriteOfLayer(LayerId id) const {
    const LayerData* layer = findLayer(id);
    return layer ? layer->sprite : SpriteId::null();
}

DocumentId LSContext::Impl::documentOfLayer(LayerId id) const {
    return documentOfSprite(spriteOfLayer(id));
}

PaletteId LSContext::Impl::effectivePalette(SpriteId sprite) const {
    const SpriteData* data = findSprite(sprite);
    if (data == nullptr) {
        return PaletteId::null();
    }
    if (data->palette.valid()) {
        return data->palette;
    }
    const DocumentData* doc = findDocument(data->document);
    return doc ? doc->palette : PaletteId::null();
}

Color LSContext::Impl::resolveColorRole(PaletteId palette, ColorRole role, Color fallback) const {
    if (role == kColorRoleNone) {
        return fallback;
    }
    const PaletteData* data = findPalette(palette);
    if (data == nullptr) {
        return fallback;
    }
    auto it = data->colors.find(role);
    return it == data->colors.end() ? fallback : it->second;
}

IntervalSet LSContext::Impl::rasterizeGeometry(const GeometryData& data) const {
    return std::visit([](const auto& shape) -> IntervalSet {
        using Shape = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<Shape, PointDesc>)    return geom::rasterizePoint(shape);
        else if constexpr (std::is_same_v<Shape, LineDesc>)     return geom::rasterizeLine(shape);
        else if constexpr (std::is_same_v<Shape, PolylineDesc>) return geom::rasterizePolyline(shape);
        else if constexpr (std::is_same_v<Shape, RectDesc>)     return geom::rasterizeRect(shape);
        else if constexpr (std::is_same_v<Shape, EllipseDesc>)  return geom::rasterizeEllipse(shape);
        else if constexpr (std::is_same_v<Shape, CircleDesc>)   return geom::rasterizeCircle(shape);
        else if constexpr (std::is_same_v<Shape, PolygonDesc>)  return geom::rasterizePolygon(shape);
        else                                                    return geom::rasterizeCurve(shape);
    }, data.shape);
}

std::vector<Vec2f> LSContext::Impl::geometryPath(const GeometryData& data) const {
    return std::visit([](const auto& shape) -> std::vector<Vec2f> {
        using Shape = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<Shape, PointDesc>) {
            return { shape.position };
        } else if constexpr (std::is_same_v<Shape, LineDesc>) {
            return { shape.start, shape.end };
        } else if constexpr (std::is_same_v<Shape, PolylineDesc>) {
            return shape.points;
        } else if constexpr (std::is_same_v<Shape, PolygonDesc>) {
            return shape.vertices;
        } else if constexpr (std::is_same_v<Shape, RectDesc>) {
            return {
                shape.origin,
                {shape.origin.x + shape.width, shape.origin.y},
                {shape.origin.x + shape.width, shape.origin.y + shape.height},
                {shape.origin.x, shape.origin.y + shape.height}
            };
        } else if constexpr (std::is_same_v<Shape, EllipseDesc>) {
            std::vector<Vec2f> points;
            constexpr int kSteps = 64;
            for (int i = 0; i < kSteps; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(kSteps) * 6.28318530718f;
                points.push_back({ shape.center.x + std::cos(t) * shape.radiusX,
                                   shape.center.y + std::sin(t) * shape.radiusY });
            }
            return points;
        } else if constexpr (std::is_same_v<Shape, CircleDesc>) {
            std::vector<Vec2f> points;
            constexpr int kSteps = 64;
            for (int i = 0; i < kSteps; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(kSteps) * 6.28318530718f;
                points.push_back({ shape.center.x + std::cos(t) * shape.radius,
                                   shape.center.y + std::sin(t) * shape.radius });
            }
            return points;
        } else {
            return geom::flattenCurve(shape);
        }
    }, data.shape);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

std::unique_ptr<LSContext> LSContext::create() {
    return std::unique_ptr<LSContext>(new LSContext());
}

LSContext::LSContext() : impl_(std::make_unique<Impl>()) {}
LSContext::~LSContext() = default;

uint32_t LSContext::engineVersion() const {
    return LS_ENGINE_VERSION;
}

// ---------------------------------------------------------------------------
// SECTION 1: Documents, sprites, layers, groups
// ---------------------------------------------------------------------------

Result<DocumentId> LSContext::createDocument(const DocumentDesc& desc) {
    if (desc.canvasWidth == 0 || desc.canvasHeight == 0) {
        return Result<DocumentId>::err(LSError::InvalidParameter);
    }
    const DocumentId id = impl_->mint<DocumentId>();
    DocumentData data;
    data.name = desc.name;
    data.canvasWidth = desc.canvasWidth;
    data.canvasHeight = desc.canvasHeight;
    impl_->documents.emplace(id.value, std::move(data));
    impl_->documentOrder.push_back(id);
    return Result<DocumentId>::ok(id);
}

VoidResult LSContext::deleteDocument(DocumentId doc) {
    DocumentData* data = impl_->findDocument(doc);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }

    const std::vector<SpriteId> sprites = data->sprites;
    for (SpriteId sprite : sprites) {
        deleteSprite(sprite);
    }
    for (GeometryId id : data->geometry)  { impl_->geometry.erase(id.value); }
    for (RegionId id : data->regions)     { impl_->regions.erase(id.value); }
    for (PaletteId id : data->palettes)   { impl_->palettes.erase(id.value); }
    for (RampId id : data->ramps)         { impl_->ramps.erase(id.value); }
    for (PatternId id : data->patterns)   { impl_->patterns.erase(id.value); }

    impl_->documents.erase(doc.value);
    impl_->documentOrder.erase(
        std::remove(impl_->documentOrder.begin(), impl_->documentOrder.end(), doc),
        impl_->documentOrder.end());
    impl_->invalidateCacheFor(doc.value);
    return VoidResult::success();
}

Result<Vec2i> LSContext::getCanvasSize(DocumentId doc) const {
    const DocumentData* data = impl_->findDocument(doc);
    if (data == nullptr) {
        return Result<Vec2i>::err(LSError::InvalidId);
    }
    return Result<Vec2i>::ok(Vec2i{ static_cast<int32_t>(data->canvasWidth),
                                    static_cast<int32_t>(data->canvasHeight) });
}

VoidResult LSContext::setCanvasSize(DocumentId doc, uint32_t width, uint32_t height) {
    DocumentData* data = impl_->findDocument(doc);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (width == 0 || height == 0) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    data->canvasWidth = width;
    data->canvasHeight = height;
    impl_->markDirtyInternal(doc.value);
    return VoidResult::success();
}

std::vector<DocumentId> LSContext::documents() const {
    return impl_->documentOrder;
}

Result<SpriteId> LSContext::createSprite(DocumentId doc) {
    DocumentData* document = impl_->findDocument(doc);
    if (document == nullptr) {
        return Result<SpriteId>::err(LSError::InvalidId);
    }
    const SpriteId id = impl_->mint<SpriteId>();
    SpriteData data;
    data.document = doc;
    impl_->sprites.emplace(id.value, std::move(data));
    document->sprites.push_back(id);
    impl_->addDependencyEdge(id.value, doc.value);
    return Result<SpriteId>::ok(id);
}

Result<SpriteId> LSContext::cloneSprite(SpriteId src) {
    const SpriteData* source = impl_->findSprite(src);
    if (source == nullptr) {
        return Result<SpriteId>::err(LSError::InvalidId);
    }

    const SpriteData sourceCopy = *source;
    auto created = createSprite(sourceCopy.document);
    if (created.fail()) {
        return created;
    }
    const SpriteId cloneId = created.value;

    // Pivots first: layer operations may reference them.
    std::map<uint64_t, uint64_t> pivotRemap;
    for (PivotId pivot : sourceCopy.pivots) {
        const PivotData* data = impl_->findPivot(pivot);
        if (data == nullptr) {
            continue;
        }
        auto clonedPivot = createPivot(cloneId, data->position);
        if (clonedPivot.ok()) {
            pivotRemap[pivot.value] = clonedPivot.value.value;
        }
    }
    for (SocketId socket : sourceCopy.sockets) {
        const SocketData* data = impl_->findSocket(socket);
        if (data != nullptr) {
            addSocket(cloneId, data->desc);
        }
    }
    for (BoundaryId boundary : sourceCopy.boundaries) {
        const BoundaryData* data = impl_->findBoundary(boundary);
        if (data != nullptr) {
            createBoundary(cloneId, data->desc);
        }
    }

    for (LayerId layer : sourceCopy.layers) {
        const LayerData* layerData = impl_->findLayer(layer);
        if (layerData == nullptr) {
            continue;
        }
        const LayerData layerCopy = *layerData;
        auto clonedLayer = createLayer(cloneId, layerCopy.desc);
        if (clonedLayer.fail()) {
            continue;
        }
        if (LayerData* target = impl_->findLayer(clonedLayer.value)) {
            target->mask = layerCopy.mask;
        }
        for (OperationId op : layerCopy.operations) {
            const OperationData* opData = impl_->findOperation(op);
            if (opData != nullptr) {
                addOperation(clonedLayer.value, opData->op);
            }
        }
    }

    if (SpriteData* clone = impl_->findSprite(cloneId)) {
        clone->palette = sourceCopy.palette;
        auto remapped = pivotRemap.find(sourceCopy.pivot.value);
        if (remapped != pivotRemap.end()) {
            clone->pivot = PivotId{ remapped->second };
        }
    }

    return Result<SpriteId>::ok(cloneId);
}

VoidResult LSContext::deleteSprite(SpriteId id) {
    SpriteData* data = impl_->findSprite(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }

    const std::vector<LayerId> layers = data->layers;
    for (LayerId layer : layers) {
        deleteLayer(layer);
    }
    for (GroupId group : data->groups)       { impl_->groups.erase(group.value); }
    for (SocketId socket : data->sockets)    { impl_->sockets.erase(socket.value); }
    for (BoundaryId b : data->boundaries)    { impl_->boundaries.erase(b.value); }
    for (PivotId pivot : data->pivots)       { impl_->pivots.erase(pivot.value); }

    if (DocumentData* doc = impl_->findDocument(data->document)) {
        doc->sprites.erase(std::remove(doc->sprites.begin(), doc->sprites.end(), id),
                           doc->sprites.end());
    }

    impl_->sprites.erase(id.value);
    impl_->clearDependenciesOf(id.value);
    impl_->invalidateCacheFor(id.value);
    return VoidResult::success();
}

Result<LayerId> LSContext::createLayer(SpriteId sprite, const LayerDesc& desc) {
    SpriteData* data = impl_->findSprite(sprite);
    if (data == nullptr) {
        return Result<LayerId>::err(LSError::InvalidId);
    }
    if (desc.opacity < 0.f || desc.opacity > 1.f) {
        return Result<LayerId>::err(LSError::InvalidParameter);
    }

    const LayerId id = impl_->mint<LayerId>();
    LayerData layer;
    layer.sprite = sprite;
    layer.desc = desc;
    impl_->layers.emplace(id.value, std::move(layer));
    data->layers.push_back(id);
    impl_->addDependencyEdge(id.value, sprite.value);
    impl_->markDirtyInternal(sprite.value);
    return Result<LayerId>::ok(id);
}

VoidResult LSContext::deleteLayer(LayerId id) {
    LayerData* data = impl_->findLayer(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }

    for (OperationId op : data->operations) {
        impl_->clearDependenciesOf(op.value);
        impl_->operations.erase(op.value);
    }
    if (SpriteData* sprite = impl_->findSprite(data->sprite)) {
        sprite->layers.erase(std::remove(sprite->layers.begin(), sprite->layers.end(), id),
                             sprite->layers.end());
        impl_->markDirtyInternal(sprite->layers.empty() ? 0 : data->sprite.value);
    }
    if (GroupData* group = impl_->findGroup(data->parent)) {
        group->layers.erase(std::remove(group->layers.begin(), group->layers.end(), id),
                            group->layers.end());
    }

    const SpriteId owner = data->sprite;
    impl_->layers.erase(id.value);
    impl_->clearDependenciesOf(id.value);
    impl_->invalidateCacheFor(id.value);
    impl_->markDirtyInternal(owner.value);
    return VoidResult::success();
}

Result<GroupId> LSContext::createGroup(SpriteId sprite, std::string_view name) {
    SpriteData* data = impl_->findSprite(sprite);
    if (data == nullptr) {
        return Result<GroupId>::err(LSError::InvalidId);
    }
    const GroupId id = impl_->mint<GroupId>();
    GroupData group;
    group.sprite = sprite;
    group.name = std::string(name);
    impl_->groups.emplace(id.value, std::move(group));
    data->groups.push_back(id);
    return Result<GroupId>::ok(id);
}

VoidResult LSContext::deleteGroup(GroupId id) {
    GroupData* group = impl_->findGroup(id);
    if (group == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    for (LayerId layer : group->layers) {
        if (LayerData* data = impl_->findLayer(layer)) {
            data->parent = GroupId::null();
        }
    }
    if (SpriteData* sprite = impl_->findSprite(group->sprite)) {
        sprite->groups.erase(std::remove(sprite->groups.begin(), sprite->groups.end(), id),
                             sprite->groups.end());
    }
    impl_->groups.erase(id.value);
    return VoidResult::success();
}

VoidResult LSContext::addLayerToGroup(GroupId group, LayerId layer) {
    GroupData* groupData = impl_->findGroup(group);
    LayerData* layerData = impl_->findLayer(layer);
    if (groupData == nullptr || layerData == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (groupData->sprite != layerData->sprite) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    if (layerData->parent.valid() && layerData->parent != group) {
        removeLayerFromGroup(layerData->parent, layer);
    }
    if (std::find(groupData->layers.begin(), groupData->layers.end(), layer) == groupData->layers.end()) {
        groupData->layers.push_back(layer);
    }
    layerData->parent = group;
    impl_->markDirtyInternal(layerData->sprite.value);
    return VoidResult::success();
}

VoidResult LSContext::removeLayerFromGroup(GroupId group, LayerId layer) {
    GroupData* groupData = impl_->findGroup(group);
    LayerData* layerData = impl_->findLayer(layer);
    if (groupData == nullptr || layerData == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    groupData->layers.erase(std::remove(groupData->layers.begin(), groupData->layers.end(), layer),
                            groupData->layers.end());
    if (layerData->parent == group) {
        layerData->parent = GroupId::null();
    }
    impl_->markDirtyInternal(layerData->sprite.value);
    return VoidResult::success();
}

Result<SpriteInfo> LSContext::getSpriteInfo(SpriteId id) const {
    const SpriteData* data = impl_->findSprite(id);
    if (data == nullptr) {
        return Result<SpriteInfo>::err(LSError::InvalidId);
    }
    SpriteInfo info;
    info.id = id;
    info.pivot = data->pivot;
    info.sockets = data->sockets;
    info.boundaries = data->boundaries;
    info.layers = data->layers;
    info.boundPalette = data->palette;
    return Result<SpriteInfo>::ok(info);
}

Result<LayerInfo> LSContext::getLayerInfo(LayerId id) const {
    const LayerData* data = impl_->findLayer(id);
    if (data == nullptr) {
        return Result<LayerInfo>::err(LSError::InvalidId);
    }
    LayerInfo info;
    info.id = id;
    info.name = data->desc.name;
    info.type = data->desc.type;
    info.opacity = data->desc.opacity;
    info.blend = data->desc.blend;
    info.visible = data->desc.visible;
    info.hasClip = data->clipBase.valid();
    info.hasMask = data->mask.valid();
    info.parentId = data->parent;
    info.operations = data->operations;
    return Result<LayerInfo>::ok(info);
}

Result<DocumentId> LSContext::getSpriteDocument(SpriteId id) const {
    const SpriteData* data = impl_->findSprite(id);
    if (data == nullptr) {
        return Result<DocumentId>::err(LSError::InvalidId);
    }
    return Result<DocumentId>::ok(data->document);
}

// ---------------------------------------------------------------------------
// SECTION 2: Geometry
// ---------------------------------------------------------------------------

namespace {
template<typename Desc>
Result<GeometryId> createGeometryImpl(LSContext::Impl& impl, DocumentId doc, const Desc& desc) {
    DocumentData* document = impl.findDocument(doc);
    if (document == nullptr) {
        return Result<GeometryId>::err(LSError::InvalidId);
    }
    const GeometryId id = impl.mint<GeometryId>();
    GeometryData data;
    data.document = doc;
    data.shape = desc;
    impl.geometry.emplace(id.value, std::move(data));
    document->geometry.push_back(id);
    return Result<GeometryId>::ok(id);
}
} // namespace

Result<GeometryId> LSContext::createPoint(DocumentId doc, const PointDesc& desc) {
    return createGeometryImpl(*impl_, doc, desc);
}
Result<GeometryId> LSContext::createLine(DocumentId doc, const LineDesc& desc) {
    return createGeometryImpl(*impl_, doc, desc);
}
Result<GeometryId> LSContext::createPolyline(DocumentId doc, const PolylineDesc& desc) {
    if (desc.points.empty()) {
        return Result<GeometryId>::err(LSError::InvalidParameter);
    }
    return createGeometryImpl(*impl_, doc, desc);
}
Result<GeometryId> LSContext::createRect(DocumentId doc, const RectDesc& desc) {
    if (desc.width <= 0.f || desc.height <= 0.f) {
        return Result<GeometryId>::err(LSError::InvalidParameter);
    }
    return createGeometryImpl(*impl_, doc, desc);
}
Result<GeometryId> LSContext::createEllipse(DocumentId doc, const EllipseDesc& desc) {
    if (desc.radiusX <= 0.f || desc.radiusY <= 0.f) {
        return Result<GeometryId>::err(LSError::InvalidParameter);
    }
    return createGeometryImpl(*impl_, doc, desc);
}
Result<GeometryId> LSContext::createCircle(DocumentId doc, const CircleDesc& desc) {
    if (desc.radius <= 0.f) {
        return Result<GeometryId>::err(LSError::InvalidParameter);
    }
    return createGeometryImpl(*impl_, doc, desc);
}
Result<GeometryId> LSContext::createPolygon(DocumentId doc, const PolygonDesc& desc) {
    if (desc.vertices.size() < 3) {
        return Result<GeometryId>::err(LSError::InvalidParameter);
    }
    return createGeometryImpl(*impl_, doc, desc);
}
Result<GeometryId> LSContext::createCurve(DocumentId doc, const CurveDesc& desc) {
    if (desc.segments.empty()) {
        return Result<GeometryId>::err(LSError::InvalidParameter);
    }
    return createGeometryImpl(*impl_, doc, desc);
}

VoidResult LSContext::deleteGeometry(GeometryId id) {
    GeometryData* data = impl_->findGeometry(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (DocumentData* doc = impl_->findDocument(data->document)) {
        doc->geometry.erase(std::remove(doc->geometry.begin(), doc->geometry.end(), id),
                            doc->geometry.end());
    }
    impl_->geometry.erase(id.value);
    impl_->markDirtyInternal(id.value);
    impl_->clearDependenciesOf(id.value);
    return VoidResult::success();
}

namespace {
template<typename Desc>
VoidResult updateGeometryImpl(LSContext::Impl& impl, GeometryId id, const Desc& desc) {
    GeometryData* data = impl.findGeometry(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (!std::holds_alternative<Desc>(data->shape)) {
        return VoidResult::err(LSError::OperationTypeMismatch);
    }
    data->shape = desc;
    impl.markDirtyInternal(id.value);

    // Regions built from this geometry rebuild immediately: geometry edits must
    // propagate to every dependent fill through the region.
    for (auto& [regionId, region] : impl.regions) {
        if (region.source == id) {
            region.coverage = impl.rasterizeGeometry(*data);
            region.boundary = geom::boundaryOf(region.coverage);
            impl.markDirtyInternal(regionId);
        }
    }
    return VoidResult::success();
}
} // namespace

VoidResult LSContext::updatePolyline(GeometryId id, const PolylineDesc& desc) {
    return updateGeometryImpl(*impl_, id, desc);
}
VoidResult LSContext::updateCurve(GeometryId id, const CurveDesc& desc) {
    return updateGeometryImpl(*impl_, id, desc);
}
VoidResult LSContext::updateRect(GeometryId id, const RectDesc& desc) {
    return updateGeometryImpl(*impl_, id, desc);
}
VoidResult LSContext::updateEllipse(GeometryId id, const EllipseDesc& desc) {
    return updateGeometryImpl(*impl_, id, desc);
}
VoidResult LSContext::updatePolygon(GeometryId id, const PolygonDesc& desc) {
    return updateGeometryImpl(*impl_, id, desc);
}

Result<GeometryBounds> LSContext::getGeometryBounds(GeometryId id) const {
    const GeometryData* data = impl_->findGeometry(id);
    if (data == nullptr) {
        return Result<GeometryBounds>::err(LSError::InvalidId);
    }
    const IntervalSet set = impl_->rasterizeGeometry(*data);
    const Rect2i pixels = geom::bounds(set);

    GeometryBounds out;
    out.pixelBounds = pixels;
    out.floatBounds = { { static_cast<float>(pixels.min.x), static_cast<float>(pixels.min.y) },
                        { static_cast<float>(pixels.max.x), static_cast<float>(pixels.max.y) } };
    out.centroid = geom::centroid(set);
    out.area = static_cast<float>(geom::pixelCount(set));
    return Result<GeometryBounds>::ok(out);
}

Result<std::vector<Vec2f>> LSContext::getGeometryPath(GeometryId id) const {
    const GeometryData* data = impl_->findGeometry(id);
    if (data == nullptr) {
        return Result<std::vector<Vec2f>>::err(LSError::InvalidId);
    }
    return Result<std::vector<Vec2f>>::ok(impl_->geometryPath(*data));
}

// ---------------------------------------------------------------------------
// SECTION 3: Regions
// ---------------------------------------------------------------------------

namespace {
Result<RegionId> storeRegion(LSContext::Impl& impl, DocumentId doc, IntervalSet coverage,
                             IntervalSet boundary, GeometryId source) {
    DocumentData* document = impl.findDocument(doc);
    if (document == nullptr) {
        return Result<RegionId>::err(LSError::InvalidId);
    }
    const RegionId id = impl.mint<RegionId>();
    RegionData data;
    data.document = doc;
    data.coverage = std::move(coverage);
    data.boundary = std::move(boundary);
    data.source = source;
    impl.regions.emplace(id.value, std::move(data));
    document->regions.push_back(id);
    if (source.valid()) {
        impl.addDependencyEdge(source.value, id.value);
    }
    return Result<RegionId>::ok(id);
}
} // namespace

Result<RegionId> LSContext::createRegionFromGeometry(GeometryId geomId) {
    const GeometryData* data = impl_->findGeometry(geomId);
    if (data == nullptr) {
        return Result<RegionId>::err(LSError::InvalidId);
    }
    IntervalSet coverage = impl_->rasterizeGeometry(*data);
    IntervalSet boundary = geom::boundaryOf(coverage);
    return storeRegion(*impl_, data->document, std::move(coverage), std::move(boundary), geomId);
}

Result<RegionId> LSContext::createRegionFromIntervals(DocumentId doc, const IntervalSet& intervals) {
    IntervalSet coverage = geom::normalize(intervals);
    IntervalSet boundary = geom::boundaryOf(coverage);
    return storeRegion(*impl_, doc, std::move(coverage), std::move(boundary), GeometryId::null());
}

Result<RegionId> LSContext::createRegionFromPixels(DocumentId doc, const PixelRegionDesc& desc) {
    geom::PixelRegionResult built = geom::buildPixelRegion(desc);
    return storeRegion(*impl_, doc, std::move(built.coverage), std::move(built.boundary),
                       GeometryId::null());
}

Result<RegionId> LSContext::createRegionFromCompiledSnapshot(DocumentId doc, const SnapshotResult& snapshot) {
    IntervalSet coverage = snapshot.mask.empty()
        ? geom::maskToIntervals(snapshot.raster, 0.5f)
        : geom::normalize(snapshot.mask);
    IntervalSet boundary = geom::boundaryOf(coverage);
    return storeRegion(*impl_, doc, std::move(coverage), std::move(boundary), GeometryId::null());
}

VoidResult LSContext::deleteRegion(RegionId id) {
    RegionData* data = impl_->findRegion(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (DocumentData* doc = impl_->findDocument(data->document)) {
        doc->regions.erase(std::remove(doc->regions.begin(), doc->regions.end(), id),
                           doc->regions.end());
    }
    impl_->regions.erase(id.value);
    impl_->markDirtyInternal(id.value);
    impl_->clearDependenciesOf(id.value);
    return VoidResult::success();
}

namespace {
Result<RegionId> binaryRegionOp(LSContext::Impl& impl, RegionId a, RegionId b, RegionBoolOp op) {
    const RegionData* left  = impl.findRegion(a);
    const RegionData* right = impl.findRegion(b);
    if (left == nullptr || right == nullptr) {
        return Result<RegionId>::err(LSError::InvalidId);
    }
    IntervalSet coverage = geom::booleanOp(left->coverage, right->coverage, op);
    IntervalSet boundary = geom::boundaryOf(coverage);
    return storeRegion(impl, left->document, std::move(coverage), std::move(boundary),
                       GeometryId::null());
}
} // namespace

Result<RegionId> LSContext::unionRegions(RegionId a, RegionId b) {
    return binaryRegionOp(*impl_, a, b, RegionBoolOp::Union);
}
Result<RegionId> LSContext::subtractRegions(RegionId a, RegionId b) {
    return binaryRegionOp(*impl_, a, b, RegionBoolOp::Subtract);
}
Result<RegionId> LSContext::intersectRegions(RegionId a, RegionId b) {
    return binaryRegionOp(*impl_, a, b, RegionBoolOp::Intersect);
}
Result<RegionId> LSContext::xorRegions(RegionId a, RegionId b) {
    return binaryRegionOp(*impl_, a, b, RegionBoolOp::Xor);
}
Result<RegionId> LSContext::clipRegion(RegionId subject, RegionId clip) {
    return binaryRegionOp(*impl_, subject, clip, RegionBoolOp::Intersect);
}
Result<RegionId> LSContext::maskRegion(RegionId subject, RegionId mask) {
    return binaryRegionOp(*impl_, subject, mask, RegionBoolOp::Intersect);
}

Result<RegionId> LSContext::invertRegion(RegionId r, Rect2i within) {
    const RegionData* data = impl_->findRegion(r);
    if (data == nullptr) {
        return Result<RegionId>::err(LSError::InvalidId);
    }
    if (within.empty()) {
        return Result<RegionId>::err(LSError::InvalidParameter);
    }
    IntervalSet coverage = geom::invertSet(data->coverage, within);
    IntervalSet boundary = geom::boundaryOf(coverage);
    return storeRegion(*impl_, data->document, std::move(coverage), std::move(boundary),
                       GeometryId::null());
}

Result<RegionId> LSContext::mergeRegions(const std::vector<RegionId>& regions) {
    if (regions.empty()) {
        return Result<RegionId>::err(LSError::InvalidParameter);
    }
    const RegionData* first = impl_->findRegion(regions.front());
    if (first == nullptr) {
        return Result<RegionId>::err(LSError::InvalidId);
    }

    IntervalSet coverage = first->coverage;
    for (size_t i = 1; i < regions.size(); ++i) {
        const RegionData* next = impl_->findRegion(regions[i]);
        if (next == nullptr) {
            return Result<RegionId>::err(LSError::InvalidId);
        }
        coverage = geom::unionSets(coverage, next->coverage);
    }
    IntervalSet boundary = geom::boundaryOf(coverage);
    return storeRegion(*impl_, first->document, std::move(coverage), std::move(boundary),
                       GeometryId::null());
}

Result<std::vector<RegionId>> LSContext::splitRegion(RegionId r) {
    return connectedComponents(r);
}

Result<std::vector<RegionId>> LSContext::connectedComponents(RegionId r) {
    const RegionData* data = impl_->findRegion(r);
    if (data == nullptr) {
        return Result<std::vector<RegionId>>::err(LSError::InvalidId);
    }
    const DocumentId doc = data->document;
    const std::vector<IntervalSet> parts = geom::connectedComponents(data->coverage, true);

    std::vector<RegionId> out;
    out.reserve(parts.size());
    for (const IntervalSet& part : parts) {
        auto created = storeRegion(*impl_, doc, part, geom::boundaryOf(part), GeometryId::null());
        if (created.fail()) {
            return Result<std::vector<RegionId>>::err(created.error);
        }
        out.push_back(created.value);
    }
    return Result<std::vector<RegionId>>::ok(out);
}

Result<RegionId> LSContext::simplifyRegion(RegionId r, const SimplifyParams& params) {
    const RegionData* data = impl_->findRegion(r);
    if (data == nullptr) {
        return Result<RegionId>::err(LSError::InvalidId);
    }

    const std::vector<ContourDesc> contours = geom::traceContours(data->coverage, true);
    IntervalSet coverage;
    for (const ContourDesc& contour : contours) {
        PolygonDesc polygon;
        polygon.vertices = geom::simplifyPath(contour.points, params);
        if (polygon.vertices.size() < 3) {
            continue;
        }
        const IntervalSet filled = geom::rasterizePolygon(polygon);
        coverage = contour.outer ? geom::unionSets(coverage, filled)
                                 : geom::subtractSets(coverage, filled);
    }

    IntervalSet boundary = geom::boundaryOf(coverage);
    return storeRegion(*impl_, data->document, std::move(coverage), std::move(boundary),
                       GeometryId::null());
}

Result<RegionId> LSContext::insetRegion(RegionId r, const InsetOutsetParams& params) {
    const RegionData* data = impl_->findRegion(r);
    if (data == nullptr) {
        return Result<RegionId>::err(LSError::InvalidId);
    }
    IntervalSet coverage = geom::contract(data->coverage, params.amount, params.preserveCorners);
    IntervalSet boundary = geom::boundaryOf(coverage);
    return storeRegion(*impl_, data->document, std::move(coverage), std::move(boundary),
                       GeometryId::null());
}

Result<RegionId> LSContext::outsetRegion(RegionId r, const InsetOutsetParams& params) {
    const RegionData* data = impl_->findRegion(r);
    if (data == nullptr) {
        return Result<RegionId>::err(LSError::InvalidId);
    }
    IntervalSet coverage = geom::expand(data->coverage, params.amount, params.preserveCorners);
    IntervalSet boundary = geom::boundaryOf(coverage);
    return storeRegion(*impl_, data->document, std::move(coverage), std::move(boundary),
                       GeometryId::null());
}

Result<RegionId> LSContext::expandRegion(RegionId r, float pixels) {
    return outsetRegion(r, { pixels, true, 2.f });
}

Result<RegionId> LSContext::contractRegion(RegionId r, float pixels) {
    return insetRegion(r, { pixels, true, 2.f });
}

Result<GeometryBounds> LSContext::getRegionBounds(RegionId r) const {
    const RegionData* data = impl_->findRegion(r);
    if (data == nullptr) {
        return Result<GeometryBounds>::err(LSError::InvalidId);
    }
    const Rect2i pixels = geom::bounds(data->coverage);
    GeometryBounds out;
    out.pixelBounds = pixels;
    out.floatBounds = { { static_cast<float>(pixels.min.x), static_cast<float>(pixels.min.y) },
                        { static_cast<float>(pixels.max.x), static_cast<float>(pixels.max.y) } };
    out.centroid = geom::centroid(data->coverage);
    out.area = static_cast<float>(geom::pixelCount(data->coverage));
    return Result<GeometryBounds>::ok(out);
}

Result<IntervalSet> LSContext::getRegionIntervals(RegionId r) const {
    const RegionData* data = impl_->findRegion(r);
    if (data == nullptr) {
        return Result<IntervalSet>::err(LSError::InvalidId);
    }
    return Result<IntervalSet>::ok(data->coverage);
}

Result<IntervalSet> LSContext::getRegionBoundaryIntervals(RegionId r) const {
    const RegionData* data = impl_->findRegion(r);
    if (data == nullptr) {
        return Result<IntervalSet>::err(LSError::InvalidId);
    }
    return Result<IntervalSet>::ok(data->boundary);
}

Result<bool> LSContext::regionContainsPoint(RegionId r, Vec2i point) const {
    const RegionData* data = impl_->findRegion(r);
    if (data == nullptr) {
        return Result<bool>::err(LSError::InvalidId);
    }
    return Result<bool>::ok(geom::contains(data->coverage, point));
}

Result<RegionId> LSContext::traceBoundary(DocumentId doc, const RasterBuffer& source,
                                          const TraceBoundaryParams& params) {
    if (source.empty()) {
        return Result<RegionId>::err(LSError::InvalidParameter);
    }
    IntervalSet coverage = geom::maskToIntervals(source, params.threshold);
    if (params.simplifyEpsilon > 0.f) {
        SimplifyParams simplify;
        simplify.epsilon = params.simplifyEpsilon;
        const std::vector<ContourDesc> contours = geom::traceContours(coverage, params.includeHoles);
        IntervalSet simplified;
        for (const ContourDesc& contour : contours) {
            PolygonDesc polygon;
            polygon.vertices = geom::simplifyPath(contour.points, simplify);
            if (polygon.vertices.size() < 3) {
                continue;
            }
            const IntervalSet filled = geom::rasterizePolygon(polygon);
            simplified = contour.outer ? geom::unionSets(simplified, filled)
                                       : geom::subtractSets(simplified, filled);
        }
        coverage = std::move(simplified);
    }
    IntervalSet boundary = geom::boundaryOf(coverage);
    return storeRegion(*impl_, doc, std::move(coverage), std::move(boundary), GeometryId::null());
}

// ---------------------------------------------------------------------------
// SECTION 4: Layer compositing
// ---------------------------------------------------------------------------

VoidResult LSContext::setLayerOrder(SpriteId sprite, const std::vector<LayerId>& orderedLayers) {
    SpriteData* data = impl_->findSprite(sprite);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (orderedLayers.size() != data->layers.size()) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    for (LayerId layer : orderedLayers) {
        const LayerData* layerData = impl_->findLayer(layer);
        if (layerData == nullptr || layerData->sprite != sprite) {
            return VoidResult::err(LSError::InvalidParameter);
        }
        if (std::count(orderedLayers.begin(), orderedLayers.end(), layer) != 1) {
            return VoidResult::err(LSError::InvalidParameter);
        }
    }
    data->layers = orderedLayers;
    impl_->markDirtyInternal(sprite.value);
    return VoidResult::success();
}

VoidResult LSContext::setLayerVisibility(LayerId id, bool visible) {
    LayerData* data = impl_->findLayer(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    data->desc.visible = visible;
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::setLayerOpacity(LayerId id, float opacity) {
    LayerData* data = impl_->findLayer(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (opacity < 0.f || opacity > 1.f) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    data->desc.opacity = opacity;
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::setLayerBlendMode(LayerId id, BlendMode mode) {
    LayerData* data = impl_->findLayer(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    data->desc.blend = mode;
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::setLayerMask(LayerId id, RegionId mask) {
    LayerData* data = impl_->findLayer(id);
    if (data == nullptr || impl_->findRegion(mask) == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    data->mask = mask;
    impl_->addDependencyEdge(mask.value, id.value);
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::clearLayerMask(LayerId id) {
    LayerData* data = impl_->findLayer(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    data->mask = RegionId::null();
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::setLayerClip(LayerId id, LayerId clipBase) {
    LayerData* data = impl_->findLayer(id);
    const LayerData* base = impl_->findLayer(clipBase);
    if (data == nullptr || base == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (id == clipBase || data->sprite != base->sprite) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    data->clipBase = clipBase;
    impl_->addDependencyEdge(clipBase.value, id.value);
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::clearLayerClip(LayerId id) {
    LayerData* data = impl_->findLayer(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    data->clipBase = LayerId::null();
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::setLayerParent(LayerId child, GroupId parent) {
    if (!parent.valid()) {
        LayerData* data = impl_->findLayer(child);
        if (data == nullptr) {
            return VoidResult::err(LSError::InvalidId);
        }
        if (data->parent.valid()) {
            return removeLayerFromGroup(data->parent, child);
        }
        return VoidResult::success();
    }
    return addLayerToGroup(parent, child);
}

Result<std::vector<LayerId>> LSContext::flattenLayersForCompile(SpriteId sprite) const {
    const SpriteData* data = impl_->findSprite(sprite);
    if (data == nullptr) {
        return Result<std::vector<LayerId>>::err(LSError::InvalidId);
    }
    std::vector<LayerId> visible;
    for (LayerId layer : data->layers) {
        const LayerData* layerData = impl_->findLayer(layer);
        if (layerData == nullptr || !layerData->desc.visible) {
            continue;
        }
        if (layerData->parent.valid()) {
            // A layer inside a group is only composited if the group still exists.
            if (impl_->findGroup(layerData->parent) == nullptr) {
                continue;
            }
        }
        visible.push_back(layer);
    }
    return Result<std::vector<LayerId>>::ok(visible);
}

// ---------------------------------------------------------------------------
// SECTIONS 5-11: Operations
// ---------------------------------------------------------------------------

Result<OperationId> LSContext::addOperation(LayerId layer, Operation op, int32_t atIndex) {
    LayerData* data = impl_->findLayer(layer);
    if (data == nullptr) {
        return Result<OperationId>::err(LSError::InvalidId);
    }
    if (const PluginOp* plugin = std::get_if<PluginOp>(&op)) {
        const bool known =
            impl_->plugins.operations.count(plugin->typeId) != 0 ||
            impl_->plugins.fillResolvers.count(plugin->typeId) != 0 ||
            impl_->plugins.transformResolvers.count(plugin->typeId) != 0;
        if (!known) {
            return Result<OperationId>::err(LSError::PluginNotFound);
        }
    }

    const OperationId id = impl_->mint<OperationId>();
    OperationData record;
    record.layer = layer;
    record.op = std::move(op);
    impl_->operations.emplace(id.value, std::move(record));

    if (atIndex < 0 || static_cast<size_t>(atIndex) >= data->operations.size()) {
        data->operations.push_back(id);
    } else {
        data->operations.insert(data->operations.begin() + atIndex, id);
    }

    impl_->registerOperationDependencies(id);
    impl_->addDependencyEdge(id.value, layer.value);
    impl_->markDirtyInternal(layer.value);
    return Result<OperationId>::ok(id);
}

VoidResult LSContext::removeOperation(LayerId layer, OperationId id) {
    LayerData* data = impl_->findLayer(layer);
    if (data == nullptr || impl_->findOperation(id) == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    auto it = std::find(data->operations.begin(), data->operations.end(), id);
    if (it == data->operations.end()) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    data->operations.erase(it);
    impl_->operations.erase(id.value);
    impl_->clearDependenciesOf(id.value);
    impl_->markDirtyInternal(layer.value);
    return VoidResult::success();
}

VoidResult LSContext::updateOperation(OperationId id, Operation newOp) {
    OperationData* data = impl_->findOperation(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (data->op.index() != newOp.index()) {
        return VoidResult::err(LSError::OperationTypeMismatch);
    }
    data->op = std::move(newOp);
    impl_->clearDependenciesOf(id.value);
    impl_->registerOperationDependencies(id);
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::reorderOperations(LayerId layer, const std::vector<OperationId>& newOrder) {
    LayerData* data = impl_->findLayer(layer);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (newOrder.size() != data->operations.size()) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    for (OperationId id : newOrder) {
        if (std::find(data->operations.begin(), data->operations.end(), id) == data->operations.end() ||
            std::count(newOrder.begin(), newOrder.end(), id) != 1) {
            return VoidResult::err(LSError::InvalidParameter);
        }
    }
    data->operations = newOrder;
    impl_->markDirtyInternal(layer.value);
    return VoidResult::success();
}

Result<Operation> LSContext::getOperation(OperationId id) const {
    const OperationData* data = impl_->findOperation(id);
    if (data == nullptr) {
        return Result<Operation>::err(LSError::InvalidId);
    }
    return Result<Operation>::ok(data->op);
}

namespace {
std::string summarizeOperation(const LSContext::Impl& impl, const Operation& op) {
    std::ostringstream out;
    out << operationTypeName(op);
    std::visit([&](const auto& concrete) {
        using Op = std::decay_t<decltype(concrete)>;
        if constexpr (has_targetRegion<Op>::value) {
            if (concrete.targetRegion.valid()) {
                out << " region:" << concrete.targetRegion.value;
                if (const RegionData* region = impl.findRegion(concrete.targetRegion)) {
                    out << " px:" << geom::pixelCount(region->coverage);
                }
            }
        }
        if constexpr (std::is_same_v<Op, FillSolidOp>) {
            out << " color:" << colorText(concrete.fallbackColor);
        }
        if constexpr (std::is_same_v<Op, FillDitherOp>) {
            out << " density:" << concrete.density << " pattern:" << concrete.pattern.value;
        }
        if constexpr (std::is_same_v<Op, RotateOp>) {
            out << " angle:" << concrete.angleDegrees;
        }
        if constexpr (std::is_same_v<Op, ScaleOp>) {
            out << " factor:" << concrete.factor.x << "," << concrete.factor.y;
        }
        if constexpr (std::is_same_v<Op, TranslateOp>) {
            out << " delta:" << concrete.delta.x << "," << concrete.delta.y;
        }
        if constexpr (std::is_same_v<Op, PluginOp>) {
            out << " type:" << concrete.typeId << " params:" << concrete.params.size();
        }
    }, op);
    return out.str();
}
} // namespace

Result<OperationInfo> LSContext::getOperationInfo(OperationId id) const {
    const OperationData* data = impl_->findOperation(id);
    if (data == nullptr) {
        return Result<OperationInfo>::err(LSError::InvalidId);
    }
    OperationInfo info;
    info.id = id;
    info.layer = data->layer;
    info.type = std::string(operationTypeName(data->op));
    info.summary = summarizeOperation(*impl_, data->op);
    return Result<OperationInfo>::ok(info);
}

Result<std::vector<OperationInfo>> LSContext::getLayerOperations(LayerId layer) const {
    const LayerData* data = impl_->findLayer(layer);
    if (data == nullptr) {
        return Result<std::vector<OperationInfo>>::err(LSError::InvalidId);
    }
    std::vector<OperationInfo> out;
    out.reserve(data->operations.size());
    for (OperationId id : data->operations) {
        auto info = getOperationInfo(id);
        if (info.ok()) {
            out.push_back(info.value);
        }
    }
    return Result<std::vector<OperationInfo>>::ok(out);
}

// ---------------------------------------------------------------------------
// SECTION 8: Palettes, ramps, color
// ---------------------------------------------------------------------------

Result<PaletteId> LSContext::createPalette(DocumentId doc, const PaletteDesc& desc) {
    DocumentData* document = impl_->findDocument(doc);
    if (document == nullptr) {
        return Result<PaletteId>::err(LSError::InvalidId);
    }
    const PaletteId id = impl_->mint<PaletteId>();
    PaletteData data;
    data.document = doc;
    data.name = desc.name;
    for (const PaletteColorEntry& entry : desc.entries) {
        if (entry.role == kColorRoleNone) {
            continue;
        }
        data.colors[entry.role] = entry.color;
        if (!entry.label.empty()) {
            data.labels[entry.role] = entry.label;
        }
    }
    impl_->palettes.emplace(id.value, std::move(data));
    document->palettes.push_back(id);
    if (!document->palette.valid()) {
        document->palette = id;
    }
    return Result<PaletteId>::ok(id);
}

VoidResult LSContext::deletePalette(PaletteId id) {
    PaletteData* data = impl_->findPalette(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (DocumentData* doc = impl_->findDocument(data->document)) {
        doc->palettes.erase(std::remove(doc->palettes.begin(), doc->palettes.end(), id),
                            doc->palettes.end());
        if (doc->palette == id) {
            doc->palette = doc->palettes.empty() ? PaletteId::null() : doc->palettes.front();
        }
    }
    for (auto& [spriteId, sprite] : impl_->sprites) {
        if (sprite.palette == id) {
            sprite.palette = PaletteId::null();
            impl_->markDirtyInternal(spriteId);
        }
    }
    impl_->palettes.erase(id.value);
    ++impl_->resourceRevision;
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::setPaletteColor(PaletteId palette, ColorRole role, Color color) {
    PaletteData* data = impl_->findPalette(palette);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (role == kColorRoleNone) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    data->colors[role] = color;
    ++impl_->resourceRevision;
    impl_->markDirtyInternal(palette.value);
    return VoidResult::success();
}

VoidResult LSContext::bindDocumentPalette(DocumentId doc, PaletteId palette) {
    DocumentData* document = impl_->findDocument(doc);
    if (document == nullptr || impl_->findPalette(palette) == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    document->palette = palette;
    ++impl_->resourceRevision;
    impl_->markDirtyInternal(doc.value);
    impl_->markDirtyInternal(palette.value);
    return VoidResult::success();
}

VoidResult LSContext::bindSpritePalette(SpriteId sprite, PaletteId palette) {
    SpriteData* data = impl_->findSprite(sprite);
    if (data == nullptr || impl_->findPalette(palette) == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    data->palette = palette;
    ++impl_->resourceRevision;
    impl_->addDependencyEdge(palette.value, sprite.value);
    impl_->markDirtyInternal(sprite.value);
    return VoidResult::success();
}

Result<PaletteId> LSContext::getEffectivePalette(SpriteId sprite) const {
    if (impl_->findSprite(sprite) == nullptr) {
        return Result<PaletteId>::err(LSError::InvalidId);
    }
    return Result<PaletteId>::ok(impl_->effectivePalette(sprite));
}

Result<std::vector<PaletteColorEntry>> LSContext::getPaletteEntries(PaletteId palette) const {
    const PaletteData* data = impl_->findPalette(palette);
    if (data == nullptr) {
        return Result<std::vector<PaletteColorEntry>>::err(LSError::InvalidId);
    }
    std::vector<PaletteColorEntry> entries;
    entries.reserve(data->colors.size());
    for (const auto& [role, color] : data->colors) {
        PaletteColorEntry entry;
        entry.role = role;
        entry.color = color;
        auto label = data->labels.find(role);
        if (label != data->labels.end()) {
            entry.label = label->second;
        }
        entries.push_back(std::move(entry));
    }
    return Result<std::vector<PaletteColorEntry>>::ok(entries);
}

Result<RampId> LSContext::createRamp(DocumentId doc, const RampDesc& desc) {
    DocumentData* document = impl_->findDocument(doc);
    if (document == nullptr) {
        return Result<RampId>::err(LSError::InvalidId);
    }
    if (desc.stops.empty()) {
        return Result<RampId>::err(LSError::InvalidParameter);
    }
    const RampId id = impl_->mint<RampId>();
    RampData data;
    data.document = doc;
    data.desc = desc;
    std::sort(data.desc.stops.begin(), data.desc.stops.end(),
              [](const RampStop& a, const RampStop& b) { return a.position < b.position; });
    impl_->ramps.emplace(id.value, std::move(data));
    document->ramps.push_back(id);
    return Result<RampId>::ok(id);
}

VoidResult LSContext::deleteRamp(RampId id) {
    RampData* data = impl_->findRamp(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (DocumentData* doc = impl_->findDocument(data->document)) {
        doc->ramps.erase(std::remove(doc->ramps.begin(), doc->ramps.end(), id), doc->ramps.end());
    }
    impl_->ramps.erase(id.value);
    ++impl_->resourceRevision;
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::updateRamp(RampId id, const RampDesc& desc) {
    RampData* data = impl_->findRamp(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (desc.stops.empty()) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    data->desc = desc;
    std::sort(data->desc.stops.begin(), data->desc.stops.end(),
              [](const RampStop& a, const RampStop& b) { return a.position < b.position; });
    ++impl_->resourceRevision;
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

Result<Color> LSContext::sampleRamp(RampId ramp, float t) const {
    const RampData* data = impl_->findRamp(ramp);
    if (data == nullptr) {
        return Result<Color>::err(LSError::InvalidId);
    }
    const std::vector<RampStop>& stops = data->desc.stops;
    if (stops.empty()) {
        return Result<Color>::err(LSError::InvalidParameter);
    }

    const float clamped = std::max(0.f, std::min(1.f, t));
    if (clamped <= stops.front().position) {
        return Result<Color>::ok(stops.front().color);
    }
    if (clamped >= stops.back().position) {
        return Result<Color>::ok(stops.back().color);
    }

    for (size_t i = 0; i + 1 < stops.size(); ++i) {
        const RampStop& a = stops[i];
        const RampStop& b = stops[i + 1];
        if (clamped < a.position || clamped > b.position) {
            continue;
        }
        if (!data->desc.interpolate) {
            return Result<Color>::ok(a.color);
        }
        const float span = b.position - a.position;
        const float local = span > 0.f ? (clamped - a.position) / span : 0.f;
        auto mix = [local](uint8_t x, uint8_t y) {
            const float value = static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * local;
            return static_cast<uint8_t>(std::max(0.f, std::min(255.f, value + 0.5f)));
        };
        return Result<Color>::ok(Color{ mix(a.color.r, b.color.r),
                                        mix(a.color.g, b.color.g),
                                        mix(a.color.b, b.color.b),
                                        mix(a.color.a, b.color.a) });
    }
    return Result<Color>::ok(stops.back().color);
}

VoidResult LSContext::swapPalette(SpriteId sprite, PaletteId newPalette) {
    return bindSpritePalette(sprite, newPalette);
}

VoidResult LSContext::remapRamp(RampId ramp, PaletteId fromPalette, PaletteId toPalette) {
    RampData* data = impl_->findRamp(ramp);
    const PaletteData* source = impl_->findPalette(fromPalette);
    const PaletteData* target = impl_->findPalette(toPalette);
    if (data == nullptr || source == nullptr || target == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }

    // Each stop moves to the color that occupies the same role in the target
    // palette; stops that do not match a source role snap to nearest.
    for (RampStop& stop : data->desc.stops) {
        ColorRole matchedRole = kColorRoleNone;
        for (const auto& [role, color] : source->colors) {
            if (color == stop.color) {
                matchedRole = role;
                break;
            }
        }
        if (matchedRole == kColorRoleNone) {
            auto nearest = nearestPaletteColor(fromPalette, stop.color);
            if (nearest.fail()) {
                continue;
            }
            matchedRole = nearest.value;
        }
        auto replacement = target->colors.find(matchedRole);
        if (replacement != target->colors.end()) {
            stop.color = replacement->second;
        }
    }

    ++impl_->resourceRevision;
    impl_->markDirtyInternal(ramp.value);
    return VoidResult::success();
}

Result<Color> LSContext::resolveSemanticColor(PaletteId palette, ColorRole role) const {
    const PaletteData* data = impl_->findPalette(palette);
    if (data == nullptr) {
        return Result<Color>::err(LSError::InvalidId);
    }
    auto it = data->colors.find(role);
    if (it == data->colors.end()) {
        return Result<Color>::err(LSError::PaletteNotBound);
    }
    return Result<Color>::ok(it->second);
}

Result<ColorRole> LSContext::nearestPaletteColor(PaletteId palette, Color color) const {
    const PaletteData* data = impl_->findPalette(palette);
    if (data == nullptr) {
        return Result<ColorRole>::err(LSError::InvalidId);
    }
    if (data->colors.empty()) {
        return Result<ColorRole>::err(LSError::PaletteNotBound);
    }

    ColorRole best = kColorRoleNone;
    float bestDistance = 0.f;
    for (const auto& [role, entry] : data->colors) {
        const float distance = colorDistanceSq(entry, color);
        if (best == kColorRoleNone || distance < bestDistance) {
            best = role;
            bestDistance = distance;
        }
    }
    return Result<ColorRole>::ok(best);
}

VoidResult LSContext::quantizeToPalette(RasterBuffer& buffer, PaletteId palette) const {
    const PaletteData* data = impl_->findPalette(palette);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (data->colors.empty()) {
        return VoidResult::err(LSError::PaletteNotBound);
    }

    for (uint32_t y = 0; y < buffer.height; ++y) {
        uint8_t* row = buffer.row(y);
        for (uint32_t x = 0; x < buffer.width; ++x) {
            uint8_t* px = row + x * 4;
            if (px[3] == 0) {
                continue;
            }
            const Color source { px[0], px[1], px[2], px[3] };
            Color best = source;
            float bestDistance = 0.f;
            bool found = false;
            for (const auto& [role, entry] : data->colors) {
                (void)role;
                const float distance = colorDistanceSq(entry, source);
                if (!found || distance < bestDistance) {
                    best = entry;
                    bestDistance = distance;
                    found = true;
                }
            }
            px[0] = best.r;
            px[1] = best.g;
            px[2] = best.b;
            px[3] = best.a;
        }
    }
    return VoidResult::success();
}

VoidResult LSContext::constrainToPalette(RasterBuffer& buffer, PaletteId palette) const {
    return quantizeToPalette(buffer, palette);
}

// ---------------------------------------------------------------------------
// SECTION 9: Patterns
// ---------------------------------------------------------------------------

namespace {
Result<PatternId> storePattern(LSContext::Impl& impl, DocumentId doc, PatternTileDesc desc) {
    DocumentData* document = impl.findDocument(doc);
    if (document == nullptr) {
        return Result<PatternId>::err(LSError::InvalidId);
    }
    if (desc.tileWidth == 0 || desc.tileHeight == 0 ||
        desc.mask.size() != static_cast<size_t>(desc.tileWidth) * desc.tileHeight) {
        return Result<PatternId>::err(LSError::InvalidParameter);
    }
    const PatternId id = impl.mint<PatternId>();
    PatternData data;
    data.document = doc;
    data.desc = std::move(desc);
    impl.patterns.emplace(id.value, std::move(data));
    document->patterns.push_back(id);
    return Result<PatternId>::ok(id);
}
} // namespace

Result<PatternId> LSContext::createPattern(DocumentId doc, const PatternTileDesc& desc) {
    return storePattern(*impl_, doc, desc);
}

VoidResult LSContext::deletePattern(PatternId id) {
    PatternData* data = impl_->findPattern(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (DocumentData* doc = impl_->findDocument(data->document)) {
        doc->patterns.erase(std::remove(doc->patterns.begin(), doc->patterns.end(), id),
                            doc->patterns.end());
    }
    impl_->patterns.erase(id.value);
    ++impl_->resourceRevision;
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::updatePattern(PatternId id, const PatternTileDesc& desc) {
    PatternData* data = impl_->findPattern(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (desc.tileWidth == 0 || desc.tileHeight == 0 ||
        desc.mask.size() != static_cast<size_t>(desc.tileWidth) * desc.tileHeight) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    data->desc = desc;
    ++impl_->resourceRevision;
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

Result<PatternId> LSContext::createOrderedDitherPattern(DocumentId doc, uint32_t matrixSize) {
    if (matrixSize != 2 && matrixSize != 4 && matrixSize != 8) {
        return Result<PatternId>::err(LSError::InvalidParameter);
    }

    // Recursive Bayer construction: M(2n) = [4M(n), 4M(n)+2; 4M(n)+3, 4M(n)+1]
    std::vector<uint32_t> matrix { 0 };
    uint32_t size = 1;
    while (size < matrixSize) {
        const uint32_t next = size * 2;
        std::vector<uint32_t> grown(static_cast<size_t>(next) * next, 0);
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const uint32_t base = matrix[static_cast<size_t>(y) * size + x] * 4;
                grown[static_cast<size_t>(y) * next + x]                   = base + 0;
                grown[static_cast<size_t>(y) * next + x + size]            = base + 2;
                grown[static_cast<size_t>(y + size) * next + x]            = base + 3;
                grown[static_cast<size_t>(y + size) * next + x + size]     = base + 1;
            }
        }
        matrix = std::move(grown);
        size = next;
    }

    PatternTileDesc desc;
    desc.name = "ordered" + std::to_string(matrixSize);
    desc.tileWidth = matrixSize;
    desc.tileHeight = matrixSize;
    desc.levels = matrixSize * matrixSize;
    desc.mask.reserve(matrix.size());
    for (uint32_t value : matrix) {
        desc.mask.push_back(static_cast<uint8_t>(value));
    }
    return storePattern(*impl_, doc, std::move(desc));
}

Result<PatternId> LSContext::createCheckerDitherPattern(DocumentId doc, uint32_t size) {
    if (size == 0) {
        return Result<PatternId>::err(LSError::InvalidParameter);
    }
    PatternTileDesc desc;
    desc.name = "checker" + std::to_string(size);
    desc.tileWidth = size * 2;
    desc.tileHeight = size * 2;
    desc.levels = 2;
    desc.mask.resize(static_cast<size_t>(desc.tileWidth) * desc.tileHeight, 0);
    for (uint32_t y = 0; y < desc.tileHeight; ++y) {
        for (uint32_t x = 0; x < desc.tileWidth; ++x) {
            const bool on = ((x / size) + (y / size)) % 2 == 0;
            desc.mask[static_cast<size_t>(y) * desc.tileWidth + x] = on ? 1 : 0;
        }
    }
    return storePattern(*impl_, doc, std::move(desc));
}

Result<PatternId> LSContext::createLineDitherPattern(DocumentId doc, float angle, float spacing) {
    if (spacing < 1.f) {
        return Result<PatternId>::err(LSError::InvalidParameter);
    }
    const uint32_t tile = static_cast<uint32_t>(std::max(2.f, std::ceil(spacing * 2.f)));
    const float radians = angle * 3.14159265358979323846f / 180.f;
    const float nx = std::cos(radians);
    const float ny = std::sin(radians);

    PatternTileDesc desc;
    desc.name = "lines";
    desc.tileWidth = tile;
    desc.tileHeight = tile;
    desc.levels = 2;
    desc.mask.resize(static_cast<size_t>(tile) * tile, 0);
    for (uint32_t y = 0; y < tile; ++y) {
        for (uint32_t x = 0; x < tile; ++x) {
            const float projected = (static_cast<float>(x) + 0.5f) * nx + (static_cast<float>(y) + 0.5f) * ny;
            const float phase = projected - std::floor(projected / spacing) * spacing;
            desc.mask[static_cast<size_t>(y) * tile + x] = phase < 1.f ? 1 : 0;
        }
    }
    return storePattern(*impl_, doc, std::move(desc));
}

Result<PatternId> LSContext::createSeededNoiseDitherPattern(DocumentId doc, uint32_t seed, uint32_t tileSize) {
    if (tileSize == 0) {
        return Result<PatternId>::err(LSError::InvalidParameter);
    }
    PatternTileDesc desc;
    desc.name = "noise" + std::to_string(seed);
    desc.tileWidth = tileSize;
    desc.tileHeight = tileSize;
    desc.levels = 256;
    desc.mask.resize(static_cast<size_t>(tileSize) * tileSize, 0);
    for (uint32_t y = 0; y < tileSize; ++y) {
        for (uint32_t x = 0; x < tileSize; ++x) {
            uint32_t h = seed ^ 0x9e3779b9u;
            h ^= x + 0x85ebca6bu + (h << 6) + (h >> 2);
            h ^= y + 0xc2b2ae35u + (h << 6) + (h >> 2);
            h ^= h >> 16; h *= 0x7feb352du;
            h ^= h >> 15; h *= 0x846ca68bu;
            h ^= h >> 16;
            desc.mask[static_cast<size_t>(y) * tileSize + x] = static_cast<uint8_t>(h & 0xffu);
        }
    }
    return storePattern(*impl_, doc, std::move(desc));
}

Result<PatternId> LSContext::createThresholdPattern(DocumentId doc, float threshold) {
    if (threshold < 0.f || threshold > 1.f) {
        return Result<PatternId>::err(LSError::InvalidParameter);
    }
    PatternTileDesc desc;
    desc.name = "threshold";
    desc.tileWidth = 1;
    desc.tileHeight = 1;
    desc.levels = 256;
    desc.mask = { static_cast<uint8_t>(std::min(255.f, threshold * 255.f)) };
    return storePattern(*impl_, doc, std::move(desc));
}

Result<PatternId> LSContext::createPluginPattern(DocumentId doc, std::string_view typeId,
                                                 float density, float phase,
                                                 const PluginParams& params) {
    auto registered = impl_->plugins.patterns.find(std::string(typeId));
    if (registered == impl_->plugins.patterns.end()) {
        return Result<PatternId>::err(LSError::PluginNotFound);
    }
    const PluginPatternDesc& plugin = registered->second;
    if (plugin.tileWidth == 0 || plugin.tileHeight == 0) {
        return Result<PatternId>::err(LSError::InvalidParameter);
    }

    const std::vector<bool> tile = plugin.generateTile(plugin.tileWidth, plugin.tileHeight,
                                                       density, phase, params);
    if (tile.size() != static_cast<size_t>(plugin.tileWidth) * plugin.tileHeight) {
        return Result<PatternId>::err(LSError::PluginError);
    }

    PatternTileDesc desc;
    desc.name = plugin.displayName.empty() ? plugin.typeId : plugin.displayName;
    desc.tileWidth = plugin.tileWidth;
    desc.tileHeight = plugin.tileHeight;
    desc.levels = 2;
    desc.density = density;
    desc.mask.reserve(tile.size());
    for (bool cell : tile) {
        desc.mask.push_back(cell ? 0 : 1);   // 0 = foreground, matching threshold order
    }
    return storePattern(*impl_, doc, std::move(desc));
}

VoidResult LSContext::setPatternPhase(PatternId id, Vec2f phase) {
    PatternData* data = impl_->findPattern(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    data->desc.phase = phase;
    ++impl_->resourceRevision;
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::setPatternDensity(PatternId id, float density) {
    PatternData* data = impl_->findPattern(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (density < 0.f || density > 1.f) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    data->desc.density = density;
    ++impl_->resourceRevision;
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::setPatternCoordinateSpace(PatternId id, CoordinateSpace space) {
    PatternData* data = impl_->findPattern(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    data->desc.coordinateSpace = space;
    ++impl_->resourceRevision;
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

Result<PatternTileDesc> LSContext::getPattern(PatternId id) const {
    const PatternData* data = impl_->findPattern(id);
    if (data == nullptr) {
        return Result<PatternTileDesc>::err(LSError::InvalidId);
    }
    return Result<PatternTileDesc>::ok(data->desc);
}

// ---------------------------------------------------------------------------
// SECTIONS 10-11: Convenience transform wrappers
// ---------------------------------------------------------------------------

namespace {
Result<LayerId> topLayerOf(LSContext::Impl& impl, SpriteId sprite) {
    SpriteData* data = impl.findSprite(sprite);
    if (data == nullptr) {
        return Result<LayerId>::err(LSError::InvalidId);
    }
    if (data->layers.empty()) {
        return Result<LayerId>::err(LSError::InvalidParameter);
    }
    return Result<LayerId>::ok(data->layers.back());
}
} // namespace

Result<OperationId> LSContext::translateSprite(SpriteId sprite, Vec2f delta, PivotId pivot) {
    auto layer = topLayerOf(*impl_, sprite);
    if (layer.fail()) {
        return Result<OperationId>::err(layer.error);
    }
    TranslateOp op;
    op.target = sprite;
    op.delta = delta;
    op.pivot = pivot;
    return addOperation(layer.value, op);
}

Result<OperationId> LSContext::rotateSprite(SpriteId sprite, float angleDeg, PivotId pivot) {
    auto layer = topLayerOf(*impl_, sprite);
    if (layer.fail()) {
        return Result<OperationId>::err(layer.error);
    }
    RotateOp op;
    op.target = sprite;
    op.angleDegrees = angleDeg;
    op.pivot = pivot;
    return addOperation(layer.value, op);
}

Result<OperationId> LSContext::scaleSprite(SpriteId sprite, Vec2f factor, PivotId pivot) {
    auto layer = topLayerOf(*impl_, sprite);
    if (layer.fail()) {
        return Result<OperationId>::err(layer.error);
    }
    ScaleOp op;
    op.target = sprite;
    op.factor = factor;
    op.pivot = pivot;
    return addOperation(layer.value, op);
}

Result<OperationId> LSContext::mirrorSprite(SpriteId sprite, MirrorAxis axis, PivotId pivot) {
    auto layer = topLayerOf(*impl_, sprite);
    if (layer.fail()) {
        return Result<OperationId>::err(layer.error);
    }
    MirrorOp op;
    op.target = sprite;
    op.axis = axis;
    op.pivot = pivot;
    return addOperation(layer.value, op);
}

Result<OperationId> LSContext::squashSprite(SpriteId sprite, float factor, BoundaryId boundary, PivotId pivot) {
    auto layer = topLayerOf(*impl_, sprite);
    if (layer.fail()) {
        return Result<OperationId>::err(layer.error);
    }
    SquashOp op;
    op.target = sprite;
    op.factor = factor;
    op.boundary = boundary;
    op.pivot = pivot;
    return addOperation(layer.value, op);
}

Result<OperationId> LSContext::stretchSprite(SpriteId sprite, float factor, BoundaryId boundary, PivotId pivot) {
    auto layer = topLayerOf(*impl_, sprite);
    if (layer.fail()) {
        return Result<OperationId>::err(layer.error);
    }
    StretchOp op;
    op.target = sprite;
    op.factor = factor;
    op.boundary = boundary;
    op.pivot = pivot;
    return addOperation(layer.value, op);
}

// ---------------------------------------------------------------------------
// SECTION 12: Pivots, sockets, boundaries
// ---------------------------------------------------------------------------

Result<PivotId> LSContext::createPivot(SpriteId sprite, Vec2f position) {
    SpriteData* data = impl_->findSprite(sprite);
    if (data == nullptr) {
        return Result<PivotId>::err(LSError::InvalidId);
    }
    const PivotId id = impl_->mint<PivotId>();
    impl_->pivots.emplace(id.value, PivotData{ sprite, position });
    data->pivots.push_back(id);
    if (!data->pivot.valid()) {
        data->pivot = id;
    }
    return Result<PivotId>::ok(id);
}

VoidResult LSContext::deletePivot(PivotId id) {
    PivotData* data = impl_->findPivot(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (SpriteData* sprite = impl_->findSprite(data->sprite)) {
        sprite->pivots.erase(std::remove(sprite->pivots.begin(), sprite->pivots.end(), id),
                             sprite->pivots.end());
        if (sprite->pivot == id) {
            sprite->pivot = sprite->pivots.empty() ? PivotId::null() : sprite->pivots.front();
        }
    }
    impl_->pivots.erase(id.value);
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::setPivot(PivotId id, Vec2f position) {
    PivotData* data = impl_->findPivot(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    data->position = position;
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

Result<Vec2f> LSContext::getPivot(PivotId id) const {
    const PivotData* data = impl_->findPivot(id);
    if (data == nullptr) {
        return Result<Vec2f>::err(LSError::InvalidId);
    }
    return Result<Vec2f>::ok(data->position);
}

VoidResult LSContext::movePivot(PivotId id, Vec2f delta) {
    PivotData* data = impl_->findPivot(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    data->position.x += delta.x;
    data->position.y += delta.y;
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::setSpritePivot(SpriteId sprite, PivotId pivot) {
    SpriteData* data = impl_->findSprite(sprite);
    const PivotData* pivotData = impl_->findPivot(pivot);
    if (data == nullptr || pivotData == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (pivotData->sprite != sprite) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    data->pivot = pivot;
    impl_->markDirtyInternal(sprite.value);
    return VoidResult::success();
}

Result<SocketId> LSContext::addSocket(SpriteId sprite, const SocketDesc& desc) {
    SpriteData* data = impl_->findSprite(sprite);
    if (data == nullptr) {
        return Result<SocketId>::err(LSError::InvalidId);
    }
    const SocketId id = impl_->mint<SocketId>();
    impl_->sockets.emplace(id.value, SocketData{ sprite, desc });
    data->sockets.push_back(id);
    return Result<SocketId>::ok(id);
}

VoidResult LSContext::removeSocket(SocketId id) {
    SocketData* data = impl_->findSocket(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (SpriteData* sprite = impl_->findSprite(data->sprite)) {
        sprite->sockets.erase(std::remove(sprite->sockets.begin(), sprite->sockets.end(), id),
                              sprite->sockets.end());
    }
    impl_->sockets.erase(id.value);
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::moveSocket(SocketId id, Vec2f position) {
    SocketData* data = impl_->findSocket(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    data->desc.position = position;
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::rotateSocket(SocketId id, float angleDeg) {
    SocketData* data = impl_->findSocket(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    data->desc.angle = angleDeg;
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

Result<Mat3f> LSContext::getSocketTransform(SocketId id) const {
    const SocketData* data = impl_->findSocket(id);
    if (data == nullptr) {
        return Result<Mat3f>::err(LSError::InvalidId);
    }
    const Mat3f transform = Mat3f::translation(data->desc.position)
                                .mul(Mat3f::rotation(data->desc.angle));
    return Result<Mat3f>::ok(transform);
}

Result<Mat3f> LSContext::resolveSocketAttachment(SocketId socket, PivotId childPivot) const {
    auto socketTransform = getSocketTransform(socket);
    if (socketTransform.fail()) {
        return socketTransform;
    }
    const PivotData* pivot = impl_->findPivot(childPivot);
    if (pivot == nullptr) {
        return Result<Mat3f>::err(LSError::InvalidId);
    }
    // The child pivot lands on the socket: socket transform, then pivot offset.
    const Mat3f attach = socketTransform.value.mul(
        Mat3f::translation({ -pivot->position.x, -pivot->position.y }));
    return Result<Mat3f>::ok(attach);
}

Result<BoundaryId> LSContext::createBoundary(SpriteId sprite, const BoundaryDesc& desc) {
    SpriteData* data = impl_->findSprite(sprite);
    if (data == nullptr) {
        return Result<BoundaryId>::err(LSError::InvalidId);
    }
    if (desc.shape.valid() && impl_->findGeometry(desc.shape) == nullptr) {
        return Result<BoundaryId>::err(LSError::InvalidId);
    }
    const BoundaryId id = impl_->mint<BoundaryId>();
    impl_->boundaries.emplace(id.value, BoundaryData{ sprite, desc });
    data->boundaries.push_back(id);
    if (desc.shape.valid()) {
        impl_->addDependencyEdge(desc.shape.value, id.value);
    }
    return Result<BoundaryId>::ok(id);
}

VoidResult LSContext::deleteBoundary(BoundaryId id) {
    BoundaryData* data = impl_->findBoundary(id);
    if (data == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    if (SpriteData* sprite = impl_->findSprite(data->sprite)) {
        sprite->boundaries.erase(
            std::remove(sprite->boundaries.begin(), sprite->boundaries.end(), id),
            sprite->boundaries.end());
    }
    impl_->boundaries.erase(id.value);
    impl_->markDirtyInternal(id.value);
    impl_->clearDependenciesOf(id.value);
    return VoidResult::success();
}

VoidResult LSContext::editBoundary(BoundaryId id, GeometryId newShape) {
    BoundaryData* data = impl_->findBoundary(id);
    if (data == nullptr || impl_->findGeometry(newShape) == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    data->desc.shape = newShape;
    impl_->addDependencyEdge(newShape.value, id.value);
    impl_->markDirtyInternal(id.value);
    return VoidResult::success();
}

VoidResult LSContext::assignBoundaryToOperation(BoundaryId boundary, OperationId op) {
    OperationData* opData = impl_->findOperation(op);
    if (opData == nullptr || impl_->findBoundary(boundary) == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    opData->assignedBoundary = boundary;
    impl_->addDependencyEdge(boundary.value, op.value);
    impl_->markDirtyInternal(op.value);
    return VoidResult::success();
}

Result<GeometryId> LSContext::exportBoundaryShape(BoundaryId id) const {
    const BoundaryData* data = impl_->findBoundary(id);
    if (data == nullptr) {
        return Result<GeometryId>::err(LSError::InvalidId);
    }
    if (!data->desc.shape.valid()) {
        return Result<GeometryId>::err(LSError::InvalidParameter);
    }
    return Result<GeometryId>::ok(data->desc.shape);
}

Result<float> LSContext::getBoundaryInfluence(BoundaryId id, Vec2f point) const {
    const BoundaryData* data = impl_->findBoundary(id);
    if (data == nullptr) {
        return Result<float>::err(LSError::InvalidId);
    }
    const GeometryData* shape = impl_->findGeometry(data->desc.shape);
    if (shape == nullptr) {
        return Result<float>::err(LSError::InvalidId);
    }

    const IntervalSet coverage = impl_->rasterizeGeometry(*shape);
    const Vec2i pixel { static_cast<int32_t>(std::floor(point.x)),
                        static_cast<int32_t>(std::floor(point.y)) };
    if (!geom::contains(coverage, pixel)) {
        return Result<float>::ok(0.f);
    }
    if (data->desc.falloffWidth <= 0.f) {
        return Result<float>::ok(1.f);
    }

    // Distance to the boundary, measured in expanding shells.
    const int32_t maxSteps = static_cast<int32_t>(std::ceil(data->desc.falloffWidth));
    IntervalSet core = coverage;
    float depth = static_cast<float>(maxSteps);
    for (int32_t step = 1; step <= maxSteps; ++step) {
        core = geom::contract(core, 1.f, true);
        if (!geom::contains(core, pixel)) {
            depth = static_cast<float>(step - 1);
            break;
        }
    }

    const float t = std::min(1.f, depth / data->desc.falloffWidth);
    float influence = t;
    switch (data->desc.falloff) {
        case Falloff::Linear:   influence = t; break;
        case Falloff::Smooth:   influence = t * t * (3.f - 2.f * t); break;
        case Falloff::Sharp:    influence = t * t; break;
        case Falloff::Cosine:   influence = 0.5f - 0.5f * std::cos(t * 3.14159265358979323846f); break;
        case Falloff::Constant: influence = 1.f; break;
    }
    return Result<float>::ok(influence);
}

// ---------------------------------------------------------------------------
// SECTION 18: Plugin registration
// ---------------------------------------------------------------------------

VoidResult LSContext::registerOperationType(PluginOperationDesc desc) {
    if (desc.typeId.empty() || !desc.resolve) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    if (LS_ENGINE_VERSION < desc.minEngineVersion || LS_ENGINE_VERSION > desc.maxEngineVersion) {
        return VoidResult::err(LSError::VersionMismatch);
    }
    impl_->plugins.operations[desc.typeId] = std::move(desc);
    return VoidResult::success();
}

VoidResult LSContext::registerPatternType(PluginPatternDesc desc) {
    if (desc.typeId.empty() || !desc.generateTile) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    impl_->plugins.patterns[desc.typeId] = std::move(desc);
    return VoidResult::success();
}

VoidResult LSContext::registerFillResolver(PluginFillResolverDesc desc) {
    if (desc.typeId.empty() || !desc.resolve) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    impl_->plugins.fillResolvers[desc.typeId] = std::move(desc);
    return VoidResult::success();
}

VoidResult LSContext::registerTransformResolver(PluginTransformResolverDesc desc) {
    if (desc.typeId.empty() || !desc.resolve) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    impl_->plugins.transformResolvers[desc.typeId] = std::move(desc);
    return VoidResult::success();
}

VoidResult LSContext::registerCompilePolicy(PluginCompilePolicyDesc desc) {
    if (desc.typeId.empty() || !desc.resolve) {
        return VoidResult::err(LSError::InvalidParameter);
    }
    impl_->plugins.compilePolicies[desc.typeId] = std::move(desc);
    return VoidResult::success();
}

bool LSContext::isOperationTypeRegistered(std::string_view typeId) const {
    return impl_->plugins.operations.find(std::string(typeId)) != impl_->plugins.operations.end();
}

std::vector<std::string> LSContext::registeredOperationTypes() const {
    std::vector<std::string> out;
    out.reserve(impl_->plugins.operations.size());
    for (const auto& [typeId, desc] : impl_->plugins.operations) {
        (void)desc;
        out.push_back(typeId);
    }
    return out;
}

} // namespace ls
