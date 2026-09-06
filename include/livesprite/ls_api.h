#pragma once
// ls_api.h — LSContext: the main engine interface
// LiveSprite Engine
//
// LSContext is the engine. One context = one workspace.
// All state lives inside. Apps hold IDs, never pointers.
//
// Thread safety: NOT thread-safe. Callers must synchronize externally.
// Error handling: all methods return Result<T> — no exceptions.
// Memory: engine owns all storage. IDs become invalid after delete calls.

#include "ls_types.h"
#include "ls_geometry.h"
#include "ls_operations.h"
#include "ls_plugin.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ls {

// ---------------------------------------------------------------------------
// Descriptor structs for entity creation
// ---------------------------------------------------------------------------

struct DocumentDesc {
    std::string name;
    uint32_t    canvasWidth  = 32;
    uint32_t    canvasHeight = 32;
};

struct LayerDesc {
    std::string     name;
    LayerType       type       = LayerType::Drawing;
    float           opacity    = 1.f;
    BlendMode       blend      = BlendMode::Normal;
    bool            visible    = true;
};

struct PaletteColorEntry {
    ColorRole   role = kColorRoleNone;
    Color       color;
    std::string label;          // app-facing label (engine ignores it)
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
    std::vector<uint8_t> mask;      // size = tileWidth * tileHeight
                                    // ordered dither: threshold rank per cell
                                    // binary patterns: 0 or 1
    uint32_t            levels = 2; // number of distinct threshold levels
    Vec2f               phase;
    float               density = 1.f;
    CoordinateSpace     coordinateSpace = CoordinateSpace::Object;
};

struct SocketDesc {
    std::string     name;           // app-side label; engine ignores semantics
    Vec2f           position;
    float           angle = 0.f;
};

struct BoundaryDesc {
    std::string     name;           // app-side label
    GeometryId      shape;          // geometry defining this boundary
    Falloff         falloff = Falloff::Linear;
    float           falloffWidth = 0.f;  // pixels of soft edge inside the shape
};

// ---------------------------------------------------------------------------
// Query results
// ---------------------------------------------------------------------------

struct LayerInfo {
    LayerId     id;
    std::string name;
    LayerType   type    = LayerType::Drawing;
    float       opacity = 1.f;
    BlendMode   blend   = BlendMode::Normal;
    bool        visible = true;
    bool        hasClip = false;
    bool        hasMask = false;
    GroupId     parentId;                   // null = top level
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

struct OperationInfo {
    OperationId id;
    LayerId     layer;
    std::string type;
    std::string summary;
};

struct SnapshotResult {
    RasterBuffer    raster;
    Rect2i          bounds;
    IntervalSet     mask;
    GeometryId      boundaryShape;  // contour of the snapshot silhouette
};

struct DependencyInfo {
    std::vector<uint64_t> dependencies;  // IDs this node reads
    std::vector<uint64_t> dependents;    // IDs that read this node
};

struct SerializedData {
    std::vector<uint8_t>    bytes;
    uint32_t                engineVersion = LS_ENGINE_VERSION;
    std::string             formatTag;    // "livesprite/document", "livesprite/sprite"
};

struct CacheStats {
    size_t entries      = 0;
    size_t hits         = 0;
    size_t misses       = 0;
    size_t dirtyEntities = 0;
};

// ---------------------------------------------------------------------------
// LSContext — the engine
// ---------------------------------------------------------------------------
class LSContext {
public:
    // Engine-private state. Declared here so the implementation files can name
    // it; defined only inside the engine.
    struct Impl;

    // --- Lifecycle ----------------------------------------------------------

    static std::unique_ptr<LSContext> create();

    ~LSContext();
    LSContext(const LSContext&) = delete;
    LSContext& operator=(const LSContext&) = delete;

    uint32_t engineVersion() const;

    // =======================================================================
    // SECTION 1: Document and Sprite Primitives
    // =======================================================================

    Result<DocumentId>  createDocument(const DocumentDesc& desc = {});
    VoidResult          deleteDocument(DocumentId doc);
    Result<Vec2i>       getCanvasSize(DocumentId doc) const;
    VoidResult          setCanvasSize(DocumentId doc, uint32_t width, uint32_t height);
    std::vector<DocumentId> documents() const;

    Result<SpriteId>    createSprite(DocumentId doc);
    Result<SpriteId>    cloneSprite(SpriteId src);
    VoidResult          deleteSprite(SpriteId id);

    Result<LayerId>     createLayer(SpriteId sprite, const LayerDesc& desc);
    VoidResult          deleteLayer(LayerId id);

    Result<GroupId>     createGroup(SpriteId sprite, std::string_view name);
    VoidResult          deleteGroup(GroupId id);
    VoidResult          addLayerToGroup(GroupId group, LayerId layer);
    VoidResult          removeLayerFromGroup(GroupId group, LayerId layer);

    Result<SpriteInfo>  getSpriteInfo(SpriteId id) const;
    Result<LayerInfo>   getLayerInfo(LayerId id) const;
    Result<DocumentId>  getSpriteDocument(SpriteId id) const;

    // =======================================================================
    // SECTION 2: Geometry Primitives
    // =======================================================================

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
    VoidResult          updateRect(GeometryId id, const RectDesc& desc);
    VoidResult          updateEllipse(GeometryId id, const EllipseDesc& desc);
    VoidResult          updatePolygon(GeometryId id, const PolygonDesc& desc);
    Result<GeometryBounds> getGeometryBounds(GeometryId id) const;
    Result<std::vector<Vec2f>> getGeometryPath(GeometryId id) const;

    // =======================================================================
    // SECTION 3: Region Operations
    // =======================================================================

    Result<RegionId>    createRegionFromGeometry(GeometryId geom);
    Result<RegionId>    createRegionFromIntervals(DocumentId doc, const IntervalSet& intervals);
    Result<RegionId>    createRegionFromPixels(DocumentId doc, const PixelRegionDesc& desc);
    Result<RegionId>    createRegionFromCompiledSnapshot(DocumentId doc, const SnapshotResult& snapshot);
    VoidResult          deleteRegion(RegionId id);

    // Boolean operations — return new RegionIds
    Result<RegionId>    unionRegions(RegionId a, RegionId b);
    Result<RegionId>    subtractRegions(RegionId a, RegionId b);
    Result<RegionId>    intersectRegions(RegionId a, RegionId b);
    Result<RegionId>    xorRegions(RegionId a, RegionId b);
    Result<RegionId>    clipRegion(RegionId subject, RegionId clip);
    Result<RegionId>    maskRegion(RegionId subject, RegionId mask);
    Result<RegionId>    invertRegion(RegionId r, Rect2i bounds);
    Result<RegionId>    mergeRegions(const std::vector<RegionId>& regions);
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
    Result<IntervalSet>    getRegionBoundaryIntervals(RegionId r) const;
    Result<bool>           regionContainsPoint(RegionId r, Vec2i point) const;
    Result<RegionId>       traceBoundary(DocumentId doc, const RasterBuffer& source,
                                         const TraceBoundaryParams& params);

    // =======================================================================
    // SECTION 4: Layer Compositing
    // =======================================================================

    VoidResult  setLayerOrder(SpriteId sprite, const std::vector<LayerId>& orderedLayers);
    VoidResult  setLayerVisibility(LayerId id, bool visible);
    VoidResult  setLayerOpacity(LayerId id, float opacity);
    VoidResult  setLayerBlendMode(LayerId id, BlendMode mode);
    VoidResult  setLayerMask(LayerId id, RegionId mask);
    VoidResult  clearLayerMask(LayerId id);
    VoidResult  setLayerClip(LayerId id, LayerId clipBase);
    VoidResult  clearLayerClip(LayerId id);
    VoidResult  setLayerParent(LayerId child, GroupId parent);
    Result<std::vector<LayerId>> flattenLayersForCompile(SpriteId sprite) const;

    // =======================================================================
    // SECTIONS 5-11: Operations (fill, stroke, outline, transform, deform)
    // =======================================================================

    Result<OperationId> addOperation(LayerId layer, Operation op, int32_t atIndex = -1);
    VoidResult          removeOperation(LayerId layer, OperationId id);
    VoidResult          updateOperation(OperationId id, Operation newOp);
    VoidResult          reorderOperations(LayerId layer, const std::vector<OperationId>& newOrder);
    Result<Operation>   getOperation(OperationId id) const;
    Result<OperationInfo> getOperationInfo(OperationId id) const;
    Result<std::vector<OperationInfo>> getLayerOperations(LayerId layer) const;

    // =======================================================================
    // SECTION 8: Palette and Color
    // =======================================================================

    Result<PaletteId>   createPalette(DocumentId doc, const PaletteDesc& desc);
    VoidResult          deletePalette(PaletteId id);
    VoidResult          setPaletteColor(PaletteId palette, ColorRole role, Color color);
    VoidResult          bindDocumentPalette(DocumentId doc, PaletteId palette);
    VoidResult          bindSpritePalette(SpriteId sprite, PaletteId palette);
    Result<PaletteId>   getEffectivePalette(SpriteId sprite) const;
    Result<std::vector<PaletteColorEntry>> getPaletteEntries(PaletteId palette) const;

    Result<RampId>      createRamp(DocumentId doc, const RampDesc& desc);
    VoidResult          deleteRamp(RampId id);
    VoidResult          updateRamp(RampId id, const RampDesc& desc);
    Result<Color>       sampleRamp(RampId ramp, float t) const;

    VoidResult          swapPalette(SpriteId sprite, PaletteId newPalette);
    VoidResult          remapRamp(RampId ramp, PaletteId fromPalette, PaletteId toPalette);

    Result<Color>       resolveSemanticColor(PaletteId palette, ColorRole role) const;
    Result<ColorRole>   nearestPaletteColor(PaletteId palette, Color color) const;
    VoidResult          quantizeToPalette(RasterBuffer& buffer, PaletteId palette) const;
    VoidResult          constrainToPalette(RasterBuffer& buffer, PaletteId palette) const;

    // =======================================================================
    // SECTION 9: Dither and Pattern
    // =======================================================================

    Result<PatternId>   createPattern(DocumentId doc, const PatternTileDesc& desc);
    VoidResult          deletePattern(PatternId id);
    VoidResult          updatePattern(PatternId id, const PatternTileDesc& desc);

    Result<PatternId>   createOrderedDitherPattern(DocumentId doc, uint32_t matrixSize);  // 2,4,8
    Result<PatternId>   createCheckerDitherPattern(DocumentId doc, uint32_t size);
    Result<PatternId>   createLineDitherPattern(DocumentId doc, float angle, float spacing);
    Result<PatternId>   createSeededNoiseDitherPattern(DocumentId doc, uint32_t seed, uint32_t tileSize);
    Result<PatternId>   createThresholdPattern(DocumentId doc, float threshold);

    // Bake a tile from a pattern type registered through registerPatternType().
    // The generated tile is stored like any other pattern, which keeps compile
    // deterministic and independent of the plugin at raster time.
    Result<PatternId>   createPluginPattern(DocumentId doc, std::string_view typeId,
                                            float density = 1.f, float phase = 0.f,
                                            const PluginParams& params = {});

    VoidResult          setPatternPhase(PatternId id, Vec2f phase);
    VoidResult          setPatternDensity(PatternId id, float density);
    VoidResult          setPatternCoordinateSpace(PatternId id, CoordinateSpace space);
    Result<PatternTileDesc> getPattern(PatternId id) const;

    // =======================================================================
    // SECTIONS 10-11: Convenience transform wrappers
    // Each appends the corresponding operation to the sprite top layer.
    // =======================================================================

    Result<OperationId> translateSprite(SpriteId sprite, Vec2f delta, PivotId pivot = PivotId::null());
    Result<OperationId> rotateSprite(SpriteId sprite, float angleDeg, PivotId pivot = PivotId::null());
    Result<OperationId> scaleSprite(SpriteId sprite, Vec2f factor, PivotId pivot = PivotId::null());
    Result<OperationId> mirrorSprite(SpriteId sprite, MirrorAxis axis, PivotId pivot = PivotId::null());
    Result<OperationId> squashSprite(SpriteId sprite, float factor, BoundaryId boundary, PivotId pivot);
    Result<OperationId> stretchSprite(SpriteId sprite, float factor, BoundaryId boundary, PivotId pivot);

    // =======================================================================
    // SECTION 12: Spatial Anchors (Pivot, Socket, Boundary)
    // =======================================================================

    Result<PivotId>     createPivot(SpriteId sprite, Vec2f position);
    VoidResult          deletePivot(PivotId id);
    VoidResult          setPivot(PivotId id, Vec2f position);
    Result<Vec2f>       getPivot(PivotId id) const;
    VoidResult          movePivot(PivotId id, Vec2f delta);
    VoidResult          setSpritePivot(SpriteId sprite, PivotId pivot);

    Result<SocketId>    addSocket(SpriteId sprite, const SocketDesc& desc);
    VoidResult          removeSocket(SocketId id);
    VoidResult          moveSocket(SocketId id, Vec2f position);
    VoidResult          rotateSocket(SocketId id, float angleDeg);
    Result<Mat3f>       getSocketTransform(SocketId id) const;
    Result<Mat3f>       resolveSocketAttachment(SocketId socket, PivotId childPivot) const;

    Result<BoundaryId>  createBoundary(SpriteId sprite, const BoundaryDesc& desc);
    VoidResult          deleteBoundary(BoundaryId id);
    VoidResult          editBoundary(BoundaryId id, GeometryId newShape);
    VoidResult          assignBoundaryToOperation(BoundaryId boundary, OperationId op);
    Result<IntervalSet> compileBoundaryMask(BoundaryId id, const CompileProfile& profile) const;
    Result<GeometryId>  exportBoundaryShape(BoundaryId id) const;
    Result<float>       getBoundaryInfluence(BoundaryId id, Vec2f point) const;

    // =======================================================================
    // SECTIONS 13-14: Compilation
    // =======================================================================

    Result<CompileResult>   compileSprite(SpriteId id, const CompileProfile& profile);
    Result<CompileResult>   compileLayer(LayerId id, const CompileProfile& profile);
    Result<CompileResult>   compileRegion(RegionId id, const CompileProfile& profile);
    Result<CompileResult>   compilePreview(SpriteId id, uint32_t maxWidth, uint32_t maxHeight);
    Result<RasterBuffer>    compileToRaster(SpriteId id, const CompileProfile& profile);
    Result<RasterBuffer>    compileToMask(SpriteId id, const CompileProfile& profile);
    Result<IntervalSet>     compileToIntervals(RegionId id, const CompileProfile& profile);
    Result<GeometryId>      compileToCollisionShape(SpriteId id, const CompileProfile& profile);
    Result<Rect2i>          compileBoundsOnly(SpriteId id, const CompileProfile& profile);

    // =======================================================================
    // SECTION 15: Snapshot Operations
    // =======================================================================

    Result<SnapshotResult>  snapshotCompiledSprite(SpriteId id, const CompileProfile& profile);
    Result<SnapshotResult>  snapshotCompiledLayer(LayerId id, const CompileProfile& profile);
    Result<SnapshotResult>  snapshotRegion(RegionId id, const CompileProfile& profile);
    Result<RegionId>        convertSnapshotToLiveRegion(DocumentId doc, const SnapshotResult& snapshot);
    Result<IntervalSet>     convertMaskToIntervals(const RasterBuffer& mask, float alphaThreshold = 0.5f) const;
    Result<RegionId>        convertRasterToRegionData(DocumentId doc, const RasterBuffer& raster,
                                                      const TraceBoundaryParams& params);

    // =======================================================================
    // SECTION 16: Dependency and Cache
    // =======================================================================

    VoidResult              markDirty(uint64_t entityId);
    Result<bool>            isDirty(uint64_t entityId) const;
    Result<DependencyInfo>  getDependencyInfo(uint64_t entityId) const;
    VoidResult              recompute(uint64_t entityId, const CompileProfile& profile);
    VoidResult              compileDirtyOnly(DocumentId doc, const CompileProfile& profile);
    VoidResult              clearCache(DocumentId doc);
    VoidResult              cachePreview(SpriteId id, const CompileResult& result);
    VoidResult              cacheOperationResult(OperationId id, const RasterBuffer& result);
    CacheStats              cacheStats() const;

    // =======================================================================
    // SECTION 17: Serialization
    // =======================================================================

    Result<SerializedData>  serializeDocument(DocumentId doc) const;
    Result<DocumentId>      deserializeDocument(const SerializedData& data);
    Result<SerializedData>  serializeSprite(SpriteId id) const;
    Result<SpriteId>        deserializeSprite(DocumentId into, const SerializedData& data);
    Result<std::string>     serializeOperation(OperationId id) const;
    Result<OperationId>     deserializeOperation(LayerId into, std::string_view json);
    Result<SerializedData>  migrateVersion(const SerializedData& data, uint32_t targetVersion);

    // =======================================================================
    // SECTION 18: Plugin Registration
    // =======================================================================

    VoidResult  registerOperationType(PluginOperationDesc desc);
    VoidResult  registerPatternType(PluginPatternDesc desc);
    VoidResult  registerFillResolver(PluginFillResolverDesc desc);
    VoidResult  registerTransformResolver(PluginTransformResolverDesc desc);
    VoidResult  registerCompilePolicy(PluginCompilePolicyDesc desc);

    bool                     isOperationTypeRegistered(std::string_view typeId) const;
    std::vector<std::string> registeredOperationTypes() const;

private:
    std::unique_ptr<Impl> impl_;
    LSContext();
};

} // namespace ls
