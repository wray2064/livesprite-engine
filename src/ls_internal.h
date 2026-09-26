// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors
#pragma once
// ls_internal.h — engine-private storage shared by the implementation files.
// Never installed, never included by apps.

#include "livesprite/livesprite.h"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace ls {

// ---------------------------------------------------------------------------
// Entity records
// ---------------------------------------------------------------------------

// Every route a canvas size can take into the engine goes through this: the API,
// a resize, and -- the one that matters -- a document read from a file. The
// ceiling is the engine's; the limits are the application's.
inline bool canvasSizeIsUsable(uint32_t width, uint32_t height,
                               const CanvasLimits& limits) {
    if (width == 0 || height == 0) {
        return false;
    }
    if (width >= kCanvasDimensionCeiling || height >= kCanvasDimensionCeiling) {
        return false;
    }
    if (width > limits.maxDimension || height > limits.maxDimension) {
        return false;
    }
    return static_cast<uint64_t>(width) * height <= limits.maxPixels;
}

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
    std::vector<TilemapId>  tilemaps;
};

struct SpriteData {
    DocumentId              document;
    Mat3f                   transform;      // the sprite own placement
    AttachmentDesc          attachment;     // valid only while attached
    bool                    attached = false;
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
    GroupDesc            desc;
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
    CurveDesc,
    StrokesDesc,
    AreaDesc,
    FaceDesc
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
    // What has been erased from it, as the strokes that erased it (a
    // StrokesDesc), so erasing a shape leaves it a shape. Null when nothing
    // has been. Only a region built from geometry has one.
    GeometryId  erase;
    ColorRole   role = kColorRoleNone;   // standing role, used by fills that name none
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

    // The order a person sees the slots in. A role is an identity -- pixels
    // name it -- so reordering the swatches cannot renumber roles; it
    // reorders this. Matching and quantisation still walk `colors`, so a
    // reorder changes no compiled pixel.
    std::vector<ColorRole> order;

    void noteRole(ColorRole role) {
        if (std::find(order.begin(), order.end(), role) == order.end()) {
            order.push_back(role);
        }
    }
    void forgetRole(ColorRole role) {
        order.erase(std::remove(order.begin(), order.end(), role), order.end());
    }
    // Every role once, in display order, whatever state `order` was left in
    // -- a role it lacks goes at the end in role order, one it names that no
    // longer exists is skipped.
    std::vector<ColorRole> ordered() const {
        std::vector<ColorRole> out;
        out.reserve(colors.size());
        for (ColorRole role : order) {
            if (colors.count(role) != 0 &&
                std::find(out.begin(), out.end(), role) == out.end()) {
                out.push_back(role);
            }
        }
        for (const auto& [role, color] : colors) {
            if (std::find(out.begin(), out.end(), role) == out.end()) {
                out.push_back(role);
            }
        }
        return out;
    }
};

struct RampData {
    DocumentId document;
    RampDesc   desc;
};

struct PatternData {
    DocumentId      document;
    PatternTileDesc desc;
};

struct TilemapData {
    DocumentId      document;
    TilemapDesc     desc;
};

struct PivotData {
    SpriteId    sprite;
    Vec2f       position;
    std::string name;
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
// Boundary influence
//
// How strongly a boundary holds a point: 1 deep inside, falling to 0 across
// falloffWidth as it approaches the edge, 0 outside. Computed once per boundary
// and shared, so the query API and the compile path cannot disagree about the
// same boundary.
// ---------------------------------------------------------------------------

float applyFalloffCurve(float t, Falloff falloff);

struct BoundaryField {
    IntervalSet coverage;
    std::map<uint64_t, int32_t> depth;   // pixel key -> shells survived
    float   width = 0.f;
    Falloff falloff = Falloff::Linear;

    float influenceAt(Vec2f point) const;
};

// ---------------------------------------------------------------------------
// Dependency graph and compile cache
// ---------------------------------------------------------------------------

enum class CacheKind : uint8_t { Layer, Sprite, Assembly };

struct CacheKey {
    uint64_t  entity           = 0;
    uint64_t  profileHash      = 0;
    uint64_t  resourceRevision = 0;   // palettes, ramps, and patterns in use
    uint32_t  engineVersion    = 0;
    CacheKind kind             = CacheKind::Sprite;   // a sprite and the assembly
                                                      // under it share an id

    bool operator<(const CacheKey& o) const {
        if (entity != o.entity)                     return entity < o.entity;
        if (profileHash != o.profileHash)           return profileHash < o.profileHash;
        if (resourceRevision != o.resourceRevision) return resourceRevision < o.resourceRevision;
        if (engineVersion != o.engineVersion)       return engineVersion < o.engineVersion;
        return kind < o.kind;
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
// A captured document: every entity belonging to it, copied out whole.
// ---------------------------------------------------------------------------

struct DocumentState {
    DocumentId id;
    DocumentData document;
    std::map<uint64_t, SpriteData>    sprites;
    std::map<uint64_t, LayerData>     layers;
    std::map<uint64_t, GroupData>     groups;
    std::map<uint64_t, GeometryData>  geometry;
    std::map<uint64_t, RegionData>    regions;
    std::map<uint64_t, OperationData> operations;
    std::map<uint64_t, PaletteData>   palettes;
    std::map<uint64_t, RampData>      ramps;
    std::map<uint64_t, PatternData>   patterns;
    std::map<uint64_t, TilemapData>   tilemaps;
    std::map<uint64_t, PivotData>     pivots;
    std::map<uint64_t, SocketData>    sockets;
    std::map<uint64_t, BoundaryData>  boundaries;
    std::map<uint64_t, std::map<std::string, std::string>> metadata;
    std::map<uint64_t, std::string>   unknownFields;
    uint64_t nextId = 1;
};

// ---------------------------------------------------------------------------
// LSContext::Impl — all engine state
// ---------------------------------------------------------------------------

struct LSContext::Impl {
    CanvasLimits canvasLimits;

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
    std::unordered_map<uint64_t, TilemapData>   tilemaps;
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

    // App metadata: entity id -> namespaced key -> opaque value. Engine never
    // parses a value.
    std::map<uint64_t, std::map<std::string, std::string>> metadata;

    // Everything belonging to one document, used when a restore has to clear
    // the document before putting the captured state back.
    void eraseDocumentContents(DocumentId doc);

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
    TilemapData*   findTilemap(TilemapId id);
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
    const TilemapData*   findTilemap(TilemapId id) const;
    const PivotData*     findPivot(PivotId id) const;
    const SocketData*    findSocket(SocketId id) const;
    const BoundaryData*  findBoundary(BoundaryId id) const;

    // --- ownership --------------------------------------------------------
    DocumentId documentOfSprite(SpriteId id) const;
    SpriteId   spriteOfLayer(LayerId id) const;

    // --- dependency graph (implemented in ls_dependency.cpp) ---------------
    void addDependencyEdge(uint64_t dependency, uint64_t dependent);
    // Drop one edge. Unlike clearDependenciesOf this leaves the other edges
    // touching either entity alone, which matters when something stops reading
    // one input but is still read by others.
    void removeDependencyEdge(uint64_t dependency, uint64_t dependent);
    void clearDependenciesOf(uint64_t dependent);
    void registerOperationDependencies(OperationId id);
    void markDirtyInternal(uint64_t entityId);

    // A palette write dirties every sprite that resolves through the palette,
    // not only the ones bound to it by name. A sprite with no binding of its
    // own inherits the document's, and there is no dependency edge for that:
    // the edge is added by bindSpritePalette, and a sprite created or cloned
    // afterwards never goes through it. Walking the document's sprites and
    // asking each which palette it would actually use is the honest answer.
    void markPaletteDirty(PaletteId palette);
    void invalidateCacheFor(uint64_t entityId);

    // --- boundaries -------------------------------------------------------
    // Memoized: boundaries change rarely and the field costs a few morphology
    // passes to build.
    mutable std::map<uint64_t, BoundaryField> boundaryFields;
    const BoundaryField* boundaryField(BoundaryId id) const;

    // The bounds of what a sprite draws, in its own space: the union of every
    // region and geometry its operations reference. Independent of transforms,
    // so Sprite space does not move when the sprite does.
    Rect2i spriteContentBounds(SpriteId sprite) const;

    // --- attachment chain -------------------------------------------------
    // The frame a sprite ends up in: its own transform with every attachment
    // above it folded in.
    Result<Mat3f> worldTransformOf(SpriteId sprite) const;
    // The chain part alone, without the sprite own transform.
    Result<Mat3f> placementOf(SpriteId sprite) const;

    // --- palette resolution ----------------------------------------------
    PaletteId effectivePalette(SpriteId sprite) const;
    Color     resolveColorRole(PaletteId palette, ColorRole role, Color fallback) const;

    // --- geometry ---------------------------------------------------------
    IntervalSet rasterizeGeometry(const GeometryData& data) const;
    // Rebuilds a region built from geometry from its geometry and its erase.
    void refreshRegion(RegionData& region) const;
    // The shape moved by `matrix`, then rasterized where it lands.
    IntervalSet rasterizeGeometryThrough(const GeometryData& data, const Mat3f& matrix) const;
    std::vector<Vec2f> geometryPath(const GeometryData& data) const;

    // --- compile helpers (implemented in ls_compile.cpp) -------------------
    uint64_t hashProfile(const CompileProfile& profile) const;
    bool profileOutputIsUsable(const CompileProfile& profile) const;
    CompileProfile resolveProfileDefaults(const CompileProfile& profile, DocumentId doc) const;
};

// Shared raster helpers (ls_compile.cpp)
Result<RasterBuffer> allocateRaster(uint32_t width, uint32_t height);
void   setRasterPixel(RasterBuffer& raster, int32_t x, int32_t y, Color color);
Color  getRasterPixel(const RasterBuffer& raster, int32_t x, int32_t y);
Rect2i rasterBounds(const RasterBuffer& raster);
Color  blendPixel(Color dst, Color src, BlendMode mode, float opacity);

} // namespace ls
