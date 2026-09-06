#pragma once
// ls_api.h — LSContext: the main engine interface
// LiveSprite Engine
//
// LSContext is the engine. One context = one workspace.
// All state lives inside. Apps hold IDs, never pointers.
//
// Thread safety: NOT thread-safe. Callers must synchronize externally.
// Error handling: all methods return Result<T> — no exceptions.
// Memory: engine owns all storage. IDs become invalid after Delete calls.

#include "ls_types.h"
#include "ls_geometry.h"
#include "ls_operations.h"
#include "ls_plugin.h"
#include <memory>
#include <vector>
#include <string>
#include <string_view>
#include <functional>
#include <span>

namespace ls {

// ---------------------------------------------------------------------------
// Descriptor structs for entity creation
// ---------------------------------------------------------------------------

struct LayerDesc {
    std::string     name;
    LayerType       type       = LayerType::Drawing;
    float           opacity    = 1.f;
    BlendMode       blend      = BlendMode::Normal;
    bool            visible    = true;
};

struct PaletteColorEntry {
    ColorRole   role;
    Color       color;
    std::string label;          // app-facing label (not used by engine logic)
};

struct PaletteDesc {
    std::string                     name;
    std::vector<PaletteColorEntry>  entries;
};

struct RampStop {
    float   position = 0.f;    // [0..1]
    Color   color;
};

struct RampDesc {
    std::string             name;
    std::vector<RampStop>   stops;
    bool                    interpolate = true;
};

struct PatternTileDesc {
    std::string         name;
    uint32_t            tileWidth  = 4;
    uint32_t            tileHeight = 4;
    std::vector<bool>   mask;       // size = tileWidth * tileHeight, true = foreground
};

struct SocketDesc {
    std::string     name;           // app-side label; engine ignores semantics
    Vec2f           position;
    float           angle = 0.f;
};

struct BoundaryDesc {
    std::string     name;           // app-side label
    GeometryId      shape;          // the geometry defining this boundary
};

// Layer compositing query result
struct LayerInfo {
    LayerId     id;
    LayerType   type;
    float       opacity;
    BlendMode   blend;
    bool        visible;
    bool        hasClip;
    bool        hasMask;
    LayerId     parentId;           // null = top level
    std::vector<OperationId> operations;    // in order
};

struct SpriteInfo {
    SpriteId                id;
    PivotId                 pivot;
    std::vector<SocketId>   sockets;
    std::vector<BoundaryId> boundaries;
    std::vector<LayerId>    layers;         // in compositing order
    PaletteId               boundPalette;   // null = use document palette
};

// Snapshot result (from SnapshotCompiledSprite etc.)
struct SnapshotResult {
    RasterBuffer    raster;
    Rect2i          bounds;
    IntervalSet     mask;
    GeometryId      boundaryShape;  // contour of the snapshot's silhouette
};

// Dependency info
struct DependencyInfo {
    std::vector<uint64_t> dependencies;  // IDs this node reads
    std::vector<uint64_t> dependents;    // IDs that read this node
};

// Serialized data blob (opaque bytes — format is engine-internal)
struct SerializedData {
    std::vector<uint8_t>    bytes;
    uint32_t                engineVersion = LS_ENGINE_VERSION;
    std::string             formatTag;    // "livesprite/document", "livesprite/sprite", etc.
};

// ---------------------------------------------------------------------------
// LSContext — the engine
// ---------------------------------------------------------------------------
class LSContext {
public:
    // --- Lifecycle -----------------------------------------------------------

    // Create a new engine context. This is the only way to instantiate.
    static std::unique_ptr<LSContext> create();

    ~LSContext();
    LSContext(const LSContext&) = delete;
    LSContext& operator=(const LSContext&) = delete;

    // Engine version embedded in this build
    uint32_t engineVersion() const;

    // ==========================================================================
    // SECTION 1: Document and Sprite Primitives
    // ==========================================================================

    Result<DocumentId>  createDocument();
    VoidResult          deleteDocument(DocumentId doc);

    Result<SpriteId>    createSprite(DocumentId doc);
    Result<SpriteId>    cloneSprite(SpriteId src);
    VoidResult          deleteSprite(SpriteId id);

    Result<LayerId>     createLayer(SpriteId sprite, const LayerDesc& desc);
    VoidResult          deleteLayer(LayerId id);

    Result<GroupId>     createGroup(SpriteId sprite, std::string_view name);
    VoidResult          addLayerToGroup(GroupId group, LayerId layer);
    VoidResult          removeLayerFromGroup(GroupId group, LayerId layer);

    Result<SpriteInfo>  getSpriteInfo(SpriteId id) const;
    Result<LayerInfo>   getLayerInfo(LayerId id) const;

    // ==========================================================================
    // SECTION 2: Geometry Primitives
    // ==========================================================================

    Result<GeometryId>  createPoint(DocumentId doc, const PointDesc& desc);
    Result<GeometryId>  createLine(DocumentId doc, const LineDesc& desc);
    Result<GeometryId>  createPolyline(DocumentId doc, const PolylineDesc& desc);
    Result<GeometryId>  createRect(DocumentId doc, const RectDesc& desc);
    Result<GeometryId>  createEllipse(DocumentId doc, const EllipseDesc& desc);
    Result<GeometryId>  createCircle(DocumentId doc, const CircleDesc& desc);
    Result<GeometryId>  createPolygon(DocumentId doc, const PolygonDesc& desc);
    Result<GeometryId>  createCurve(DocumentId doc, const CurveDesc& desc);
    VoidResult          deleteGeometry(GeometryId id);
    VoidResult          updatePolyline(GeometryId id, const PolylineDesc& desc);
    VoidResult          updateCurve(GeometryId id, const CurveDesc& desc);

    // ==========================================================================
    // SECTION 3: Region Operations
    // Regions are compiled from geometry and boolean ops.
    // They are stored as IntervalSets internally.
    // ==========================================================================

    Result<RegionId>    createRegionFromGeometry(GeometryId geom);
    Result<RegionId>    createRegionFromIntervals(const IntervalSet& intervals);
    Result<RegionId>    createRegionFromCompiledSnapshot(const SnapshotResult& snapshot);
    VoidResult          deleteRegion(RegionId id);

    // Boolean operations — return new RegionIds
    Result<RegionId>    unionRegions(RegionId a, RegionId b);
    Result<RegionId>    subtractRegions(RegionId a, RegionId b);
    Result<RegionId>    intersectRegions(RegionId a, RegionId b);
    Result<RegionId>    xorRegions(RegionId a, RegionId b);
    Result<RegionId>    clipRegion(RegionId subject, RegionId clip);
    Result<RegionId>    maskRegion(RegionId subject, RegionId mask);
    Result<RegionId>    invertRegion(RegionId r, Rect2i bounds);
    Result<RegionId>    mergeRegions(std::span<const RegionId> regions);
    Result<std::vector<RegionId>> splitRegion(RegionId r);
    Result<std::vector<RegionId>> connectedComponents(RegionId r);
    Result<RegionId>    simplifyRegion(RegionId r, const SimplifyParams& params);
    Result<RegionId>    insetRegion(RegionId r, const InsetOutsetParams& params);
    Result<RegionId>    outsetRegion(RegionId r, const InsetOutsetParams& params);
    Result<RegionId>    expandRegion(RegionId r, float pixels);
    Result<RegionId>    contractRegion(RegionId r, float pixels);

    // Region queries
    Result<GeometryBounds> getRegionBounds(RegionId r) const;
    Result<IntervalSet>    getRegionIntervals(RegionId r) const;
    Result<bool>           regionContainsPoint(RegionId r, Vec2i point) const;
    Result<RegionId>       traceBoundary(const RasterBuffer& source, const TraceBoundaryParams& params);

    // ==========================================================================
    // SECTION 4: Layer Compositing
    // ==========================================================================

    VoidResult  setLayerOrder(SpriteId sprite, std::span<const LayerId> orderedLayers);
    VoidResult  setLayerVisibility(LayerId id, bool visible);
    VoidResult  setLayerOpacity(LayerId id, float opacity);
    VoidResult  setLayerBlendMode(LayerId id, BlendMode mode);
    VoidResult  setLayerMask(LayerId id, RegionId mask);
    VoidResult  clearLayerMask(LayerId id);
    VoidResult  setLayerClip(LayerId id, LayerId clipBase);
    VoidResult  clearLayerClip(LayerId id);
    VoidResult  setLayerParent(LayerId child, GroupId parent);
    VoidResult  flattenLayersForCompile(SpriteId sprite, std::span<const LayerId>& outOrder) const;

    // ==========================================================================
    // SECTION 5–6: Operations (Fill, Stroke, Outline, Transform, Deform)
    // Add any Operation variant to a layer at a given position.
    // ==========================================================================

    // Add an operation to a layer (appends by default, or insert at index)
    Result<OperationId> addOperation(LayerId layer, Operation op, int32_t atIndex = -1);

    // Remove an operation
    VoidResult          removeOperation(LayerId layer, OperationId id);

    // Replace an operation's parameters (same ID, new data)
    VoidResult          updateOperation(OperationId id, Operation newOp);

    // Reorder operations within a layer
    VoidResult          reorderOperations(LayerId layer, std::span<const OperationId> newOrder);

    // Query an operation
    Result<Operation>   getOperation(OperationId id) const;

    // ==========================================================================
    // SECTION 8: Palette and Color
    // ==========================================================================

    Result<PaletteId>   createPalette(DocumentId doc, const PaletteDesc& desc);
    VoidResult          deletePalette(PaletteId id);
    VoidResult          setPaletteColor(PaletteId palette, ColorRole role, Color color);
    VoidResult          bindDocumentPalette(DocumentId doc, PaletteId palette);
    VoidResult          bindSpritePalette(SpriteId sprite, PaletteId palette);  // overrides doc palette

    Result<RampId>      createRamp(DocumentId doc, const RampDesc& desc);
    VoidResult          deleteRamp(RampId id);
    VoidResult          updateRamp(RampId id, const RampDesc& desc);

    VoidResult          swapPalette(SpriteId sprite, PaletteId newPalette);
    VoidResult          remapRamp(RampId ramp, PaletteId fromPalette, PaletteId toPalette);

    Result<Color>       resolveSemanticColor(PaletteId palette, ColorRole role) const;
    Result<ColorRole>   nearestPaletteColor(PaletteId palette, Color color) const;
    VoidResult          quantizeToPalette(RasterBuffer& buffer, PaletteId palette) const;
    VoidResult          constrainToPalette(RasterBuffer& buffer, PaletteId palette) const;

    // ==========================================================================
    // SECTION 9: Dither and Pattern
    // ==========================================================================

    Result<PatternId>   createPattern(DocumentId doc, const PatternTileDesc& desc);
    VoidResult          deletePattern(PatternId id);
    VoidResult          updatePattern(PatternId id, const PatternTileDesc& desc);

    Result<PatternId>   createOrderedDitherPattern(DocumentId doc, uint32_t matrixSize);  // 2,4,8
    Result<PatternId>   createCheckerDitherPattern(DocumentId doc, uint32_t size);
    Result<PatternId>   createLineDitherPattern(DocumentId doc, float angle, float spacing);
    Result<PatternId>   createSeededNoiseDitherPattern(DocumentId doc, uint32_t seed, float scale);
    Result<PatternId>   createThresholdPattern(DocumentId doc, float threshold);

    VoidResult          setPatternPhase(PatternId id, Vec2f phase);
    VoidResult          setPatternDensity(PatternId id, float density);
    VoidResult          setPatternCoordinateSpace(PatternId id, CoordinateSpace space);

    // ==========================================================================
    // SECTION 10–11: Transforms and Deforms
    // Transforms are applied via addOperation() — see Operation types in ls_operations.h.
    // These are convenience wrappers for common cases.
    // ==========================================================================

    Result<OperationId> translateSprite(SpriteId sprite, Vec2f delta, PivotId pivot = PivotId::null());
    Result<OperationId> rotateSprite(SpriteId sprite, float angleDeg, PivotId pivot = PivotId::null());
    Result<OperationId> scaleSprite(SpriteId sprite, Vec2f factor, PivotId pivot = PivotId::null());
    Result<OperationId> mirrorSprite(SpriteId sprite, MirrorAxis axis, PivotId pivot = PivotId::null());
    Result<OperationId> squashSprite(SpriteId sprite, float factor, BoundaryId boundary, PivotId pivot);
    Result<OperationId> stretchSprite(SpriteId sprite, float factor, BoundaryId boundary, PivotId pivot);

    // ==========================================================================
    // SECTION 12: Spatial Anchors (Pivot, Socket, Boundary)
    // ==========================================================================

    // Pivot
    Result<PivotId>     createPivot(SpriteId sprite, Vec2f position);
    VoidResult          deletePivot(PivotId id);
    VoidResult          setPivot(PivotId id, Vec2f position);
    Result<Vec2f>       getPivot(PivotId id) const;
    VoidResult          movePivot(PivotId id, Vec2f delta);

    // Socket
    Result<SocketId>    addSocket(SpriteId sprite, const SocketDesc& desc);
    VoidResult          removeSocket(SocketId id);
    VoidResult          moveSocket(SocketId id, Vec2f position);
    VoidResult          rotateSocket(SocketId id, float angleDeg);
    Result<Mat3f>       getSocketTransform(SocketId id) const;
    Result<Mat3f>       resolveSocketAttachment(SocketId socket, PivotId childPivot) const;

    // Boundary
    Result<BoundaryId>  createBoundary(SpriteId sprite, const BoundaryDesc& desc);
    VoidResult          deleteBoundary(BoundaryId id);
    VoidResult          editBoundary(BoundaryId id, GeometryId newShape);
    VoidResult          assignBoundaryToOperation(BoundaryId boundary, OperationId op);
    Result<IntervalSet> compileBoundaryMask(BoundaryId id, const CompileProfile& profile) const;
    Result<GeometryId>  exportBoundaryShape(BoundaryId id) const;
    Result<float>       getBoundaryInfluence(BoundaryId id, Vec2f point) const;

    // ==========================================================================
    // SECTION 13–14: Compilation
    // ==========================================================================

    Result<CompileResult>   compileSprite(SpriteId id, const CompileProfile& profile);
    Result<CompileResult>   compileLayer(LayerId id, const CompileProfile& profile);
    Result<CompileResult>   compileRegion(RegionId id, const CompileProfile& profile);
    Result<CompileResult>   compilePreview(SpriteId id, uint32_t maxWidth, uint32_t maxHeight);
    Result<RasterBuffer>    compileToRaster(SpriteId id, const CompileProfile& profile);
    Result<RasterBuffer>    compileToMask(SpriteId id, const CompileProfile& profile);
    Result<IntervalSet>     compileToIntervals(RegionId id, const CompileProfile& profile);
    Result<GeometryId>      compileToCollisionShape(SpriteId id, const CompileProfile& profile);
    Result<Rect2i>          compileBoundsOnly(SpriteId id, const CompileProfile& profile);

    // ==========================================================================
    // SECTION 15: Snapshot Operations
    // ==========================================================================

    Result<SnapshotResult>  snapshotCompiledSprite(SpriteId id, const CompileProfile& profile);
    Result<SnapshotResult>  snapshotCompiledLayer(LayerId id, const CompileProfile& profile);
    Result<SnapshotResult>  snapshotRegion(RegionId id, const CompileProfile& profile);
    Result<RegionId>        convertSnapshotToLiveRegion(const SnapshotResult& snapshot);
    Result<IntervalSet>     convertMaskToIntervals(const RasterBuffer& mask, float alphaThreshold = 0.5f);
    Result<RegionId>        convertRasterToRegionData(const RasterBuffer& raster, const TraceBoundaryParams& params);

    // ==========================================================================
    // SECTION 16: Dependency and Cache
    // ==========================================================================

    VoidResult              markDirty(uint64_t entityId);
    Result<DependencyInfo>  getDependencyInfo(uint64_t entityId) const;
    VoidResult              recompute(uint64_t entityId, const CompileProfile& profile);
    VoidResult              compileDirtyOnly(DocumentId doc, const CompileProfile& profile);
    VoidResult              clearCache(DocumentId doc);
    VoidResult              cachePreview(SpriteId id, const CompileResult& result);
    VoidResult              cacheOperationResult(OperationId id, const RasterBuffer& result);

    // ==========================================================================
    // SECTION 17: Serialization
    // ==========================================================================

    Result<SerializedData>  serializeDocument(DocumentId doc) const;
    Result<DocumentId>      deserializeDocument(const SerializedData& data);
    Result<SerializedData>  serializeSprite(SpriteId id) const;
    Result<SpriteId>        deserializeSprite(DocumentId into, const SerializedData& data);
    Result<std::string>     serializeOperation(OperationId id) const;  // JSON string
    Result<OperationId>     deserializeOperation(LayerId into, std::string_view json);
    Result<SerializedData>  migrateVersion(const SerializedData& data, uint32_t targetVersion);
    // Unknown fields in data are preserved across migrate/deserialize round-trips

    // ==========================================================================
    // SECTION 18: Plugin Registration
    // ==========================================================================

    VoidResult  registerOperationType(PluginOperationDesc desc);
    VoidResult  registerPatternType(PluginPatternDesc desc);
    VoidResult  registerFillResolver(PluginFillResolverDesc desc);
    VoidResult  registerTransformResolver(PluginTransformResolverDesc desc);
    VoidResult  registerCompilePolicy(PluginCompilePolicyDesc desc);

    bool        isOperationTypeRegistered(std::string_view typeId) const;
    std::vector<std::string> registeredOperationTypes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    LSContext();
};

} // namespace ls
