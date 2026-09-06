// ls_context.cpp — LSContext PIMPL implementation
// LiveSprite Engine
//
// TODO: Implement each method stub below.
// Build order recommendation: entity storage → operation CRUD → palette/pattern → spatial

#include <livesprite/ls_api.h>
#include <unordered_map>
#include <vector>
#include <string>
#include <cassert>

namespace ls {

// ---------------------------------------------------------------------------
// Internal storage types
// ---------------------------------------------------------------------------

struct PivotData {
    SpriteId    ownerSprite;
    Vec2f       position;
};

struct SocketData {
    SpriteId    ownerSprite;
    std::string name;
    Vec2f       position;
    float       angle;
};

struct BoundaryData {
    SpriteId    ownerSprite;
    std::string name;
    GeometryId  shape;
};

struct LayerData {
    SpriteId    ownerSprite;
    LayerType   type;
    float       opacity;
    BlendMode   blend;
    bool        visible;
    LayerId     parentGroup;
    RegionId    mask;
    LayerId     clipBase;
    std::vector<OperationId> operations;
};

struct SpriteData {
    DocumentId              ownerDocument;
    PivotId                 pivot;
    PaletteId               boundPalette;
    std::vector<SocketId>   sockets;
    std::vector<BoundaryId> boundaries;
    std::vector<LayerId>    layers;         // compositing order, top to bottom
};

struct DocumentData {
    PaletteId               defaultPalette;
    std::vector<SpriteId>   sprites;
};

struct PaletteData {
    std::string                             name;
    std::unordered_map<ColorRole, Color>    entries;
    std::unordered_map<ColorRole, std::string> labels;
};

struct RampData {
    std::string             name;
    std::vector<RampStop>   stops;
    bool                    interpolate;
};

struct PatternData {
    std::string         name;
    uint32_t            tileWidth;
    uint32_t            tileHeight;
    std::vector<bool>   mask;
    Vec2f               phase;
    float               density;
    CoordinateSpace     coordinateSpace;
};

struct OperationData {
    LayerId     ownerLayer;
    Operation   op;
};

// ---------------------------------------------------------------------------
// Dependency graph node
// ---------------------------------------------------------------------------
struct DependencyNode {
    std::vector<uint64_t> dependencies;   // IDs this node reads from
    std::vector<uint64_t> dependents;     // IDs that read from this node
    bool                  dirty = true;
};

// ---------------------------------------------------------------------------
// PIMPL implementation struct
// ---------------------------------------------------------------------------
struct LSContext::Impl {
    // ID counter — monotonically increasing, never reuses IDs
    uint64_t nextId = 1;
    uint64_t allocId() { return nextId++; }

    // Entity storage
    std::unordered_map<DocumentId,  DocumentData>  documents;
    std::unordered_map<SpriteId,    SpriteData>    sprites;
    std::unordered_map<LayerId,     LayerData>     layers;
    std::unordered_map<OperationId, OperationData> operations;
    std::unordered_map<PaletteId,   PaletteData>   palettes;
    std::unordered_map<RampId,      RampData>      ramps;
    std::unordered_map<PatternId,   PatternData>   patterns;
    std::unordered_map<PivotId,     PivotData>     pivots;
    std::unordered_map<SocketId,    SocketData>    sockets;
    std::unordered_map<BoundaryId,  BoundaryData>  boundaries;
    // GeometryId → geometry objects (see ls_geometry.cpp)

    // Compiled region cache: RegionId → IntervalSet
    std::unordered_map<RegionId, IntervalSet>      regionCache;

    // Compiled raster cache: keyed on (entity_id, profile hash)
    // TODO: define cache key struct

    // Dependency graph
    std::unordered_map<uint64_t, DependencyNode>   dependencyGraph;

    // Plugin registrations
    std::unordered_map<std::string, PluginOperationDesc>     pluginOps;
    std::unordered_map<std::string, PluginPatternDesc>       pluginPatterns;
    std::unordered_map<std::string, PluginFillResolverDesc>  pluginFillResolvers;
    std::unordered_map<std::string, PluginTransformResolverDesc> pluginTransformResolvers;
    std::unordered_map<std::string, PluginCompilePolicyDesc> pluginCompilePolicies;

    // Helper: mark an entity and all its dependents dirty
    void propagateDirty(uint64_t entityId);

    // Helper: resolve the effective palette for a sprite
    PaletteId resolveEffectivePalette(SpriteId sprite, DocumentId doc) const;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

std::unique_ptr<LSContext> LSContext::create() {
    return std::unique_ptr<LSContext>(new LSContext());
}

LSContext::LSContext() : impl_(std::make_unique<Impl>()) {}
LSContext::~LSContext() = default;

uint32_t LSContext::engineVersion() const { return LS_ENGINE_VERSION; }

// ---------------------------------------------------------------------------
// Section 1: Document and Sprite Primitives
// ---------------------------------------------------------------------------

Result<DocumentId> LSContext::createDocument() {
    DocumentId id{ impl_->allocId() };
    impl_->documents[id] = DocumentData{};
    return Result<DocumentId>::ok(id);
}

VoidResult LSContext::deleteDocument(DocumentId doc) {
    if (!impl_->documents.count(doc)) return VoidResult::err(LSError::InvalidId);
    // TODO: cascade delete all sprites, layers, operations, palettes
    impl_->documents.erase(doc);
    return VoidResult::ok();
}

Result<SpriteId> LSContext::createSprite(DocumentId doc) {
    if (!impl_->documents.count(doc)) return Result<SpriteId>::err(LSError::InvalidId);
    SpriteId id{ impl_->allocId() };
    impl_->sprites[id] = SpriteData{ .ownerDocument = doc };
    impl_->documents[doc].sprites.push_back(id);
    return Result<SpriteId>::ok(id);
}

Result<SpriteId> LSContext::cloneSprite(SpriteId src) {
    // TODO: deep clone all layers, operations, geometry references
    return Result<SpriteId>::err(LSError::NotImplemented);
}

VoidResult LSContext::deleteSprite(SpriteId id) {
    if (!impl_->sprites.count(id)) return VoidResult::err(LSError::InvalidId);
    // TODO: cascade delete layers and operations
    impl_->sprites.erase(id);
    return VoidResult::ok();
}

Result<LayerId> LSContext::createLayer(SpriteId sprite, const LayerDesc& desc) {
    if (!impl_->sprites.count(sprite)) return Result<LayerId>::err(LSError::InvalidId);
    LayerId id{ impl_->allocId() };
    impl_->layers[id] = LayerData{
        .ownerSprite = sprite,
        .type        = desc.type,
        .opacity     = desc.opacity,
        .blend       = desc.blend,
        .visible     = desc.visible,
    };
    impl_->sprites[sprite].layers.push_back(id);
    return Result<LayerId>::ok(id);
}

VoidResult LSContext::deleteLayer(LayerId id) {
    if (!impl_->layers.count(id)) return VoidResult::err(LSError::InvalidId);
    auto& layer = impl_->layers[id];
    auto& sprite = impl_->sprites[layer.ownerSprite];
    // Remove from sprite's layer list
    auto& lvec = sprite.layers;
    lvec.erase(std::remove(lvec.begin(), lvec.end(), id), lvec.end());
    // TODO: cascade delete operations
    impl_->layers.erase(id);
    return VoidResult::ok();
}

// TODO: createGroup, addLayerToGroup, removeLayerFromGroup
// TODO: getSpriteInfo, getLayerInfo

// ---------------------------------------------------------------------------
// Section 2: Geometry Primitives
// See ls_geometry.cpp for implementations
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Section 4: Layer Compositing
// ---------------------------------------------------------------------------

VoidResult LSContext::setLayerVisibility(LayerId id, bool visible) {
    if (!impl_->layers.count(id)) return VoidResult::err(LSError::InvalidId);
    impl_->layers[id].visible = visible;
    impl_->propagateDirty(id.value);
    return VoidResult::ok();
}

VoidResult LSContext::setLayerOpacity(LayerId id, float opacity) {
    if (!impl_->layers.count(id)) return VoidResult::err(LSError::InvalidId);
    impl_->layers[id].opacity = opacity;
    impl_->propagateDirty(id.value);
    return VoidResult::ok();
}

VoidResult LSContext::setLayerBlendMode(LayerId id, BlendMode mode) {
    if (!impl_->layers.count(id)) return VoidResult::err(LSError::InvalidId);
    impl_->layers[id].blend = mode;
    impl_->propagateDirty(id.value);
    return VoidResult::ok();
}

// ---------------------------------------------------------------------------
// Section 5-6: Operations
// ---------------------------------------------------------------------------

Result<OperationId> LSContext::addOperation(LayerId layer, Operation op, int32_t atIndex) {
    if (!impl_->layers.count(layer)) return Result<OperationId>::err(LSError::InvalidId);
    OperationId id{ impl_->allocId() };
    impl_->operations[id] = OperationData{ .ownerLayer = layer, .op = std::move(op) };
    auto& ops = impl_->layers[layer].operations;
    if (atIndex < 0 || atIndex >= (int32_t)ops.size()) {
        ops.push_back(id);
    } else {
        ops.insert(ops.begin() + atIndex, id);
    }
    impl_->propagateDirty(layer.value);
    return Result<OperationId>::ok(id);
}

VoidResult LSContext::removeOperation(LayerId layer, OperationId id) {
    if (!impl_->layers.count(layer))    return VoidResult::err(LSError::InvalidId);
    if (!impl_->operations.count(id))   return VoidResult::err(LSError::InvalidId);
    auto& ops = impl_->layers[layer].operations;
    ops.erase(std::remove(ops.begin(), ops.end(), id), ops.end());
    impl_->operations.erase(id);
    impl_->propagateDirty(layer.value);
    return VoidResult::ok();
}

VoidResult LSContext::updateOperation(OperationId id, Operation newOp) {
    if (!impl_->operations.count(id)) return VoidResult::err(LSError::InvalidId);
    impl_->operations[id].op = std::move(newOp);
    impl_->propagateDirty(id.value);
    return VoidResult::ok();
}

Result<Operation> LSContext::getOperation(OperationId id) const {
    auto it = impl_->operations.find(id);
    if (it == impl_->operations.end()) return Result<Operation>::err(LSError::InvalidId);
    return Result<Operation>::ok(it->second.op);
}

// ---------------------------------------------------------------------------
// Section 12: Spatial Anchors
// ---------------------------------------------------------------------------

Result<PivotId> LSContext::createPivot(SpriteId sprite, Vec2f position) {
    if (!impl_->sprites.count(sprite)) return Result<PivotId>::err(LSError::InvalidId);
    PivotId id{ impl_->allocId() };
    impl_->pivots[id] = PivotData{ .ownerSprite = sprite, .position = position };
    impl_->sprites[sprite].pivot = id;
    return Result<PivotId>::ok(id);
}

VoidResult LSContext::setPivot(PivotId id, Vec2f position) {
    if (!impl_->pivots.count(id)) return VoidResult::err(LSError::InvalidId);
    impl_->pivots[id].position = position;
    impl_->propagateDirty(id.value);
    return VoidResult::ok();
}

Result<Vec2f> LSContext::getPivot(PivotId id) const {
    auto it = impl_->pivots.find(id);
    if (it == impl_->pivots.end()) return Result<Vec2f>::err(LSError::InvalidId);
    return Result<Vec2f>::ok(it->second.position);
}

// ---------------------------------------------------------------------------
// Section 16: Dependency and Cache
// ---------------------------------------------------------------------------

VoidResult LSContext::markDirty(uint64_t entityId) {
    impl_->propagateDirty(entityId);
    return VoidResult::ok();
}

void LSContext::Impl::propagateDirty(uint64_t entityId) {
    // BFS/DFS through dependents, marking each dirty
    // TODO: implement traversal
    auto it = dependencyGraph.find(entityId);
    if (it == dependencyGraph.end()) return;
    it->second.dirty = true;
    for (uint64_t dep : it->second.dependents) {
        propagateDirty(dep);   // recursive — add cycle guard in full impl
    }
}

// ---------------------------------------------------------------------------
// Section 18: Plugin Registration
// ---------------------------------------------------------------------------

VoidResult LSContext::registerOperationType(PluginOperationDesc desc) {
    if (desc.typeId.empty()) return VoidResult::err(LSError::InvalidParameter);
    impl_->pluginOps[desc.typeId] = std::move(desc);
    return VoidResult::ok();
}

VoidResult LSContext::registerPatternType(PluginPatternDesc desc) {
    if (desc.typeId.empty()) return VoidResult::err(LSError::InvalidParameter);
    impl_->pluginPatterns[desc.typeId] = std::move(desc);
    return VoidResult::ok();
}

VoidResult LSContext::registerFillResolver(PluginFillResolverDesc desc) {
    if (desc.typeId.empty()) return VoidResult::err(LSError::InvalidParameter);
    impl_->pluginFillResolvers[desc.typeId] = std::move(desc);
    return VoidResult::ok();
}

VoidResult LSContext::registerTransformResolver(PluginTransformResolverDesc desc) {
    if (desc.typeId.empty()) return VoidResult::err(LSError::InvalidParameter);
    impl_->pluginTransformResolvers[desc.typeId] = std::move(desc);
    return VoidResult::ok();
}

VoidResult LSContext::registerCompilePolicy(PluginCompilePolicyDesc desc) {
    if (desc.typeId.empty()) return VoidResult::err(LSError::InvalidParameter);
    impl_->pluginCompilePolicies[desc.typeId] = std::move(desc);
    return VoidResult::ok();
}

bool LSContext::isOperationTypeRegistered(std::string_view typeId) const {
    return impl_->pluginOps.count(std::string(typeId)) > 0;
}

std::vector<std::string> LSContext::registeredOperationTypes() const {
    std::vector<std::string> out;
    for (auto& [k, _] : impl_->pluginOps) out.push_back(k);
    return out;
}

// ---------------------------------------------------------------------------
// Error strings
// ---------------------------------------------------------------------------
std::string_view lsErrorString(LSError err) {
    switch (err) {
        case LSError::None:                        return "None";
        case LSError::InvalidId:                   return "InvalidId";
        case LSError::InvalidParameter:            return "InvalidParameter";
        case LSError::NullArgument:                return "NullArgument";
        case LSError::OutOfBounds:                 return "OutOfBounds";
        case LSError::DependencyCycle:             return "DependencyCycle";
        case LSError::CompileFailure:              return "CompileFailure";
        case LSError::SerializationFailure:        return "SerializationFailure";
        case LSError::DeserializationFailure:      return "DeserializationFailure";
        case LSError::VersionMismatch:             return "VersionMismatch";
        case LSError::VersionMigrationFailed:      return "VersionMigrationFailed";
        case LSError::PluginNotFound:              return "PluginNotFound";
        case LSError::PluginError:                 return "PluginError";
        case LSError::PluginDeterminismViolation:  return "PluginDeterminismViolation";
        case LSError::OperationTypeMismatch:       return "OperationTypeMismatch";
        case LSError::RegionEmpty:                 return "RegionEmpty";
        case LSError::PaletteNotBound:             return "PaletteNotBound";
        case LSError::RasterAllocationFailed:      return "RasterAllocationFailed";
        case LSError::NotImplemented:              return "NotImplemented";
        default:                                   return "Unknown";
    }
}

} // namespace ls
