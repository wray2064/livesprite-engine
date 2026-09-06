#pragma once
// ls_internal.h — engine-private storage shared by the implementation files.
// Never installed, never included by apps.

#include "livesprite/livesprite.h"

#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace ls {

// ---------------------------------------------------------------------------
// Entity records
// ---------------------------------------------------------------------------

struct DocumentData {
    std::string             name;
    uint32_t                canvasWidth  = 32;
    uint32_t                canvasHeight = 32;
    PaletteId               palette;
    std::vector<SpriteId>   sprites;
    std::vector<GeometryId> geometry;
    std::vector<RegionId>   regions;
    std::vector<PaletteId>  palettes;
    std::vector<RampId>     ramps;
    std::vector<PatternId>  patterns;
};

struct SpriteData {
    DocumentId              document;
    std::vector<LayerId>    layers;         // compositing order, bottom first
    std::vector<GroupId>    groups;
    std::vector<SocketId>   sockets;
    std::vector<BoundaryId> boundaries;
    std::vector<PivotId>    pivots;
    PivotId                 pivot;          // the sprite primary pivot
    PaletteId               palette;        // null = inherit from document
};

struct LayerData {
    SpriteId                 sprite;
    LayerDesc                desc;
    std::vector<OperationId> operations;
    RegionId                 mask;
    LayerId                  clipBase;
    GroupId                  parent;
};

struct GroupData {
    SpriteId             sprite;
    std::string          name;
    std::vector<LayerId> layers;
};

using GeometryShape = std::variant<
    PointDesc,
    LineDesc,
    PolylineDesc,
    RectDesc,
    EllipseDesc,
    CircleDesc,
    PolygonDesc,
    CurveDesc
>;

struct GeometryData {
    DocumentId    document;
    GeometryShape shape;
};

struct RegionData {
    DocumentId  document;
    IntervalSet coverage;
    IntervalSet boundary;    // authored closed loop, when the region came from pixels
    GeometryId  source;      // null unless the region was built from geometry
};

struct OperationData {
    LayerId    layer;
    Operation  op;
    BoundaryId assignedBoundary;
};

struct PaletteData {
    DocumentId                 document;
    std::string                name;
    std::map<ColorRole, Color> colors;    // ordered: iteration must be deterministic
    std::map<ColorRole, std::string> labels;
};

struct RampData {
    DocumentId document;
    RampDesc   desc;
};

struct PatternData {
    DocumentId      document;
    PatternTileDesc desc;
};

struct PivotData {
    SpriteId sprite;
    Vec2f    position;
};

struct SocketData {
    SpriteId   sprite;
    SocketDesc desc;
};

struct BoundaryData {
    SpriteId     sprite;
    BoundaryDesc desc;
};

// ---------------------------------------------------------------------------
// Dependency graph and compile cache
// ---------------------------------------------------------------------------

struct CacheKey {
    uint64_t entity           = 0;
    uint64_t profileHash      = 0;
    uint64_t resourceRevision = 0;   // palettes, ramps, and patterns in use
    uint32_t engineVersion    = 0;

    bool operator<(const CacheKey& o) const {
        if (entity != o.entity)                     return entity < o.entity;
        if (profileHash != o.profileHash)           return profileHash < o.profileHash;
        if (resourceRevision != o.resourceRevision) return resourceRevision < o.resourceRevision;
        return engineVersion < o.engineVersion;
    }
};

struct DependencyGraph {
    // edge: dependency -> dependents. "when X changes, these must recompile"
    std::map<uint64_t, std::set<uint64_t>> dependents;
    std::map<uint64_t, std::set<uint64_t>> dependencies;
    std::set<uint64_t>                     dirty;
};

// ---------------------------------------------------------------------------
// Plugin registry
// ---------------------------------------------------------------------------

struct PluginRegistry {
    std::map<std::string, PluginOperationDesc>         operations;
    std::map<std::string, PluginPatternDesc>           patterns;
    std::map<std::string, PluginFillResolverDesc>      fillResolvers;
    std::map<std::string, PluginTransformResolverDesc> transformResolvers;
    std::map<std::string, PluginCompilePolicyDesc>     compilePolicies;
};

// ---------------------------------------------------------------------------
// LSContext::Impl — all engine state
// ---------------------------------------------------------------------------

struct LSContext::Impl {
    uint64_t nextId = 1;

    // Bumped whenever a palette, ramp, or pattern changes. Colour resources are
    // read by operations without being their declared dependency, so this
    // revision is what keeps cached compiles honest.
    uint64_t resourceRevision = 1;

    std::unordered_map<uint64_t, DocumentData>  documents;
    std::unordered_map<uint64_t, SpriteData>    sprites;
    std::unordered_map<uint64_t, LayerData>     layers;
    std::unordered_map<uint64_t, GroupData>     groups;
    std::unordered_map<uint64_t, GeometryData>  geometry;
    std::unordered_map<uint64_t, RegionData>    regions;
    std::unordered_map<uint64_t, OperationData> operations;
    std::unordered_map<uint64_t, PaletteData>   palettes;
    std::unordered_map<uint64_t, RampData>      ramps;
    std::unordered_map<uint64_t, PatternData>   patterns;
    std::unordered_map<uint64_t, PivotData>     pivots;
    std::unordered_map<uint64_t, SocketData>    sockets;
    std::unordered_map<uint64_t, BoundaryData>  boundaries;

    std::vector<DocumentId> documentOrder;

    DependencyGraph              graph;
    std::map<CacheKey, CompileResult>  compileCache;
    std::map<uint64_t, RasterBuffer>   operationCache;
    size_t cacheHits   = 0;
    size_t cacheMisses = 0;

    PluginRegistry plugins;

    // Fields written by a newer engine, kept verbatim per entity id (0 keys the
    // document root) so a save from this build never strips them.
    std::map<uint64_t, std::string> unknownFields;

    template<typename IdT>
    IdT mint() { return IdT { nextId++ }; }

    // --- lookups ----------------------------------------------------------
    DocumentData*  findDocument(DocumentId id);
    SpriteData*    findSprite(SpriteId id);
    LayerData*     findLayer(LayerId id);
    GroupData*     findGroup(GroupId id);
    GeometryData*  findGeometry(GeometryId id);
    RegionData*    findRegion(RegionId id);
    OperationData* findOperation(OperationId id);
    PaletteData*   findPalette(PaletteId id);
    RampData*      findRamp(RampId id);
    PatternData*   findPattern(PatternId id);
    PivotData*     findPivot(PivotId id);
    SocketData*    findSocket(SocketId id);
    BoundaryData*  findBoundary(BoundaryId id);

    const DocumentData*  findDocument(DocumentId id) const;
    const SpriteData*    findSprite(SpriteId id) const;
    const LayerData*     findLayer(LayerId id) const;
    const GroupData*     findGroup(GroupId id) const;
    const GeometryData*  findGeometry(GeometryId id) const;
    const RegionData*    findRegion(RegionId id) const;
    const OperationData* findOperation(OperationId id) const;
    const PaletteData*   findPalette(PaletteId id) const;
    const RampData*      findRamp(RampId id) const;
    const PatternData*   findPattern(PatternId id) const;
    const PivotData*     findPivot(PivotId id) const;
    const SocketData*    findSocket(SocketId id) const;
    const BoundaryData*  findBoundary(BoundaryId id) const;

    // --- ownership --------------------------------------------------------
    DocumentId documentOfSprite(SpriteId id) const;
    SpriteId   spriteOfLayer(LayerId id) const;

    // --- dependency graph (implemented in ls_dependency.cpp) ---------------
    void addDependencyEdge(uint64_t dependency, uint64_t dependent);
    void clearDependenciesOf(uint64_t dependent);
    void registerOperationDependencies(OperationId id);
    void markDirtyInternal(uint64_t entityId);
    void invalidateCacheFor(uint64_t entityId);

    // --- palette resolution ----------------------------------------------
    PaletteId effectivePalette(SpriteId sprite) const;
    Color     resolveColorRole(PaletteId palette, ColorRole role, Color fallback) const;

    // --- geometry ---------------------------------------------------------
    IntervalSet rasterizeGeometry(const GeometryData& data) const;
    std::vector<Vec2f> geometryPath(const GeometryData& data) const;

    // --- compile helpers (implemented in ls_compile.cpp) -------------------
    uint64_t hashProfile(const CompileProfile& profile) const;
    CompileProfile resolveProfileDefaults(const CompileProfile& profile, DocumentId doc) const;
};

// Shared raster helpers (ls_compile.cpp)
Result<RasterBuffer> allocateRaster(uint32_t width, uint32_t height);
void   setRasterPixel(RasterBuffer& raster, int32_t x, int32_t y, Color color);
Color  getRasterPixel(const RasterBuffer& raster, int32_t x, int32_t y);
Rect2i rasterBounds(const RasterBuffer& raster);
Color  blendPixel(Color dst, Color src, BlendMode mode, float opacity);

} // namespace ls
