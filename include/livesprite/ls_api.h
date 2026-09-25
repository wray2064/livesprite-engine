// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors
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
#include <variant>
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

// One stop of a ramp: where along the ramp it sits, and what colour it is.
//
// The colour is a role first and a value second, the same way every fill's
// colour is. A stop that names a role resolves through the sprite's palette at
// compile time, so a dither built from two roles recolours with everything
// else when a slot changes -- from the drawing, not over it. A stop that names
// no role is the literal colour, which is what ramps were before roles reached
// them and what a file written then still contains.
struct RampStop {
    float     position = 0.f;    // [0..1]
    Color     color;             // the literal, and the fallback when the role resolves to nothing
    ColorRole role = kColorRoleNone;
};

struct RampDesc {
    std::string             name;
    std::vector<RampStop>   stops;
    bool                    interpolate = true;
};

// A pattern tile is a threshold matrix: mask[i] is the rank of that cell, and
// levels is how many ranks exist. A cell wins when the value being dithered
// exceeds (rank + 0.5) / levels, so one tile serves every density and every
// step of a gradient.
//
// A tile may also carry colours. When `colors` is populated it is a tileable
// texture: a texture fill paints those colours directly instead of resolving
// palette roles.
struct PatternTileDesc {
    std::string          name;
    uint32_t             tileWidth  = 4;
    uint32_t             tileHeight = 4;
    std::vector<uint8_t> mask;      // size = tileWidth * tileHeight, threshold rank per cell
    std::vector<Color>   colors;    // empty, or the same size: a colour per cell
    uint32_t             levels = 2;
    Vec2f                phase;
    float                density = 1.f;
    CoordinateSpace      coordinateSpace = CoordinateSpace::Object;
};

// How an imported raster becomes a pattern.
enum class PatternImportMode : uint8_t {
    Threshold,   // luminance becomes the threshold rank: a dither matrix
    Colors,      // pixels are kept as a tileable colour texture
};

// A pivot is a named point in sprite space: where rotation, scale, squash and
// stretch originate, and the point a child presents when it attaches.
struct PivotDesc {
    std::string     name;           // app-side label; engine ignores semantics
    Vec2f           position;
};

// Where a pivot may be placed automatically. Content placements use the
// compiled bounds of the sprite, so they follow whatever the sprite draws.
enum class PivotPlacement : uint8_t {
    ContentCenter,
    ContentTop,
    ContentBottom,
    ContentTopLeft,
    CanvasCenter,
};

// A socket is a named frame on a sprite: position, orientation and scale. It is
// where another sprite pivot connects. The engine owns the attachment maths; an
// app decides whether a socket is a hand, a weapon grip or a backpack mount.
struct SocketDesc {
    std::string     name;           // app-side label; engine ignores semantics
    Vec2f           position;
    float           angle = 0.f;
    Vec2f           scale = {1.f, 1.f};
};

// One sprite hanging off another sprite socket.
struct AttachmentDesc {
    SocketId        socket;         // the parent socket to hang from
    PivotId         childPivot;     // the child pivot that lands on it
    Mat3f           localOffset;    // extra local transform at the joint
    bool            behindParent = false;   // composite under the parent instead of over it
};

struct AttachmentInfo {
    SpriteId        child;
    SpriteId        parent;
    SocketId        socket;
    PivotId         childPivot;
    Mat3f           localOffset;
    bool            behindParent = false;
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

// A group composites its layers into one buffer, then that buffer into the
// sprite. Its opacity and blend apply to the group as a whole, which is not the
// same picture as applying them to each layer in turn.
struct GroupDesc {
    std::string name;
    float       opacity = 1.f;
    BlendMode   blend   = BlendMode::Normal;
    bool        visible = true;
};

struct GroupInfo {
    GroupId              id;
    std::string          name;
    float                opacity = 1.f;
    BlendMode            blend   = BlendMode::Normal;
    bool                 visible = true;
    std::vector<LayerId> layers;
};

struct LayerInfo {
    LayerId     id;
    // Which sprite this layer belongs to. An application holding a layer handle
    // needs it to say anything about the layer's surroundings -- an outline that
    // traces the whole figure names the sprite, and the layer is all the caller
    // has.
    SpriteId    sprite;
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

// What a document contains. This is how an application finds its way around a
// document it did not build -- one it has just loaded from a file, where every
// id was minted fresh by the reader and none of the caller's old handles apply.
struct DocumentInfo {
    DocumentId              id;
    std::string             name;
    uint32_t                canvasWidth  = 0;
    uint32_t                canvasHeight = 0;
    PaletteId               palette;        // null = no document palette bound
    std::vector<SpriteId>   sprites;
    std::vector<PaletteId>  palettes;       // every palette the document holds
};

struct SpriteInfo {
    SpriteId                id;
    PivotId                 pivot;
    std::vector<SocketId>   sockets;
    std::vector<BoundaryId> boundaries;
    std::vector<LayerId>    layers;         // in compositing order
    std::vector<GroupId>    groups;         // every group of the sprite
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

// ---------------------------------------------------------------------------
// Live instructions
//
// The engine knows nothing about time, keyframes, easing or tracks. What it
// offers is this: every operation parameter is addressable by name, every
// instruction states an absolute value rather than a delta, and a batch of them
// lands as one frame followed by one compile.
//
// Absolute values are the important half. An app scrubbing a timeline backwards
// must get the same pixels it got going forwards, which it cannot if the engine
// accumulates deltas.
// ---------------------------------------------------------------------------

enum class ParameterType : uint8_t {
    Unsupported,   // a field this interface cannot address, such as a point list
    Bool,
    Int,           // integers and enumerations
    Float,
    Vec2,
    Color,
    Matrix,
    Text,
    EntityId,      // a handle: region, ramp, pattern, pivot, socket, ...
};

using ParameterValue = std::variant<
    bool,
    int64_t,
    float,
    Vec2f,
    Color,
    Mat3f,
    std::string,
    uint64_t
>;

struct ParameterInfo {
    std::string   name;
    ParameterType type = ParameterType::Unsupported;
};

// The instruction set. Each one names what to change and the value to change it
// to; none of them are relative.
struct SetOperationParameter {
    OperationId    operation;
    std::string    parameter;
    ParameterValue value;
};

struct SetSpriteTransformInstruction {
    SpriteId sprite;
    Mat3f    transform;
};

struct SetLayerVisibilityInstruction {
    LayerId layer;
    bool    visible = true;
};

struct SetLayerOpacityInstruction {
    LayerId layer;
    float   opacity = 1.f;
};

struct SetPaletteColorInstruction {
    PaletteId palette;
    ColorRole role = kColorRoleNone;
    Color     color;
};

struct AttachInstruction {
    SpriteId       child;
    AttachmentDesc attachment;
};

struct DetachInstruction {
    SpriteId child;
};

using Instruction = std::variant<
    SetOperationParameter,
    SetSpriteTransformInstruction,
    SetLayerVisibilityInstruction,
    SetLayerOpacityInstruction,
    SetPaletteColorInstruction,
    AttachInstruction,
    DetachInstruction
>;

// A whole-document state capture. Restoring one puts every id back exactly as
// it was, which is what an editor needs: an undo that renumbered entities would
// invalidate every handle the interface is holding.
//
// This is an in-memory capture, not a file. An editor takes one per action, so
// it copies engine state directly rather than going through the save format,
// which is orders of magnitude slower. Use serializeDocument for anything that
// has to outlive the session.
struct DocumentState;

struct DocumentSnapshot {
    std::shared_ptr<const DocumentState> state;
    bool valid() const { return state != nullptr; }
};

// Limits on app metadata. They exist because a document travels: an unbounded
// side channel is a denial of service on whoever opens the file next.
constexpr size_t kMetadataMaxKeyLength   = 128;
constexpr size_t kMetadataMaxValueLength = 64 * 1024;
constexpr size_t kMetadataMaxPerEntity   = 64;

// ---------------------------------------------------------------------------
// Packages
//
// A LiveSprite package is a container: the engine document, plus whatever files
// an app needs to keep with it. Thumbnails, imported references, a palette file
// somebody dragged in. Bulk data belongs beside the document rather than inside
// it, where it would bloat the JSON and slow every load.
//
// The engine owns the container; apps own the entries. The engine guarantees it
// carries entries it does not understand through a read and write cycle, the
// same promise it makes for unknown fields, so two apps can share one file
// without erasing each other.
//
// The format is a ZIP with stored (uncompressed) entries. That choice is
// deliberate on both counts this file cares about:
//   - Portability: any tool and any language can open it. Rename it to .zip and
//     it expands.
//   - Safety: nothing is compressed, so a decompression bomb cannot exist. A
//     package containing compressed entries is refused rather than expanded.
//
// Everything in a received package is untrusted input. It came from whoever
// sent the file. Content types are a claim by the writer, never a fact, and
// nothing in a package should ever be executed.
// ---------------------------------------------------------------------------

// Entry names are opaque identifiers, not paths. The first segment is the
// owning namespace ("fast/thumbnail.png"). Separators beyond that are allowed
// for the owner to organise its own space, but a name can never climb out of
// it: no "..", no leading slash, no backslash, no drive letter.
constexpr size_t kPackageMaxNameLength   = 255;
constexpr size_t kPackageMaxEntries      = 1024;
constexpr size_t kPackageMaxEntrySize    = 32u * 1024u * 1024u;
constexpr size_t kPackageMaxTotalSize    = 128u * 1024u * 1024u;

struct PackageEntry {
    std::string          name;         // "fast/thumbnail.png"
    std::string          contentType;  // a claim by the writer; never trusted
    std::vector<uint8_t> data;
};

struct PackageContents {
    SerializedData            document;   // the engine document from the package
    std::vector<PackageEntry> entries;    // everything else, in name order
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
    Result<DocumentInfo> getDocumentInfo(DocumentId doc) const;
    VoidResult          setCanvasSize(DocumentId doc, uint32_t width, uint32_t height);
    VoidResult          setDocumentName(DocumentId doc, std::string_view name);

    // How large a canvas this application is willing to work with. Applies to
    // documents created, resized, and read from files, so raising it lets this
    // context open files a stricter one would refuse.
    //
    // Refused if it exceeds what the engine can represent -- see
    // kCanvasDimensionCeiling. Everything below that is a cost decision, and the
    // cost is in the documentation: area times layers, seconds at 4096x4096.
    VoidResult          setCanvasLimits(const CanvasLimits& limits);
    CanvasLimits        canvasLimits() const;
    std::vector<DocumentId> documents() const;

    Result<SpriteId>    createSprite(DocumentId doc);

    // The order the sprites sit in the document. It is what round-trips through
    // a save, so an application that means something by that order -- the frames
    // of an animation, in order -- can keep it without inventing a side list
    // that has to be kept in step.
    //
    // Reordering renumbers nothing: every id stays exactly what it was, so a
    // handle an application is holding survives a reorder the same way it
    // survives an undo.
    VoidResult          setSpriteOrder(DocumentId doc, const std::vector<SpriteId>& ordered);
    Result<SpriteId>    cloneSprite(SpriteId src);
    VoidResult          deleteSprite(SpriteId id);

    Result<LayerId>     createLayer(SpriteId sprite, const LayerDesc& desc);
    VoidResult          deleteLayer(LayerId id);
    // A copy of a layer -- its description, mask and every operation, with the
    // geometry and regions those operations own copied too, so the copy draws
    // the same picture and can then be edited apart from the original. `into`
    // may be another sprite of the same document, which is how a layer moves
    // between frames; `atIndex` places it in that sprite's order, -1 for the
    // top. The copy joins no group and clips to nothing.
    Result<LayerId>     cloneLayer(LayerId source, SpriteId into, int32_t atIndex = -1);

    Result<GroupId>     createGroup(SpriteId sprite, std::string_view name);
    Result<GroupId>     createGroup(SpriteId sprite, const GroupDesc& desc);
    VoidResult          deleteGroup(GroupId id);
    VoidResult          setGroupOpacity(GroupId id, float opacity);
    VoidResult          setGroupBlendMode(GroupId id, BlendMode mode);
    VoidResult          setGroupVisibility(GroupId id, bool visible);
    VoidResult          setGroupName(GroupId id, std::string_view name);
    Result<GroupInfo>   getGroupInfo(GroupId id) const;
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

    // What a geometry was described with, read back exactly: the counterpart
    // of each update. Bounds and paths are rasterised or flattened answers,
    // and an application that edits a shape -- moves a rounded rectangle,
    // turns it with the canvas -- needs the description itself, or it writes
    // the shape back without its corner radius. OperationTypeMismatch when
    // the geometry is some other kind.
    Result<RectDesc>     getRect(GeometryId id) const;
    Result<EllipseDesc>  getEllipse(GeometryId id) const;
    Result<PolylineDesc> getPolyline(GeometryId id) const;

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
    // The geometry a region was built from, or a null id when it has none --
    // because it was authored as pixels, or because it was edited by hand and
    // stopped tracking. An application that offers editable shapes needs this to
    // recognise one in a document it has just loaded: the link survives a save,
    // but without a way to ask, a rectangle comes back as anonymous pixels.
    Result<GeometryId>     getRegionSourceGeometry(RegionId r) const;
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
    // A name is state like any other: it round-trips through a save, it is shown
    // in an interface, and an interface that shows a name is asked to change it.
    VoidResult  setLayerName(LayerId id, std::string_view name);
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

    // Takes a role out of the palette. Nothing that referenced it is touched:
    // every fill, stroke, outline and ramp stop naming it falls back to its own
    // literal colour on the next compile, which is what the fallback is for.
    // An editor deciding whether to warn first can ask usesPaletteRole below.
    VoidResult          removePaletteColor(PaletteId palette, ColorRole role);

    // A slot's name in a panel -- "skin", "outline", "shadow 2". The engine
    // stores it and never reads it. It is in PaletteColorEntry on the way out;
    // this is the way to change one after the palette exists.
    VoidResult          setPaletteLabel(PaletteId palette, ColorRole role, std::string_view label);

    // Whether anything in the document names this role: an operation's
    // paletteRole, a region's standing role, or a ramp stop. What a panel asks
    // before removing a slot, so it can say "used by three layers" instead of
    // silently reverting them.
    Result<bool>        usesPaletteRole(DocumentId doc, ColorRole role) const;
    // The name a palette was made with, and a way to change it. Names reach
    // a panel and the save file, never the pixels.
    Result<std::string> getPaletteName(PaletteId palette) const;
    VoidResult          setPaletteName(PaletteId palette, std::string_view name);

    VoidResult          bindDocumentPalette(DocumentId doc, PaletteId palette);
    // A sprite bound to a palette uses it instead of the document's. Binding
    // to a null id removes the sprite's own binding, so it follows the
    // document's palette again -- which is what "use the document's" means
    // when a frame that had its own is switched back.
    VoidResult          bindSpritePalette(SpriteId sprite, PaletteId palette);
    Result<PaletteId>   getSpritePalette(SpriteId sprite) const;   // the sprite's own, or null
    Result<PaletteId>   getEffectivePalette(SpriteId sprite) const;
    Result<std::vector<PaletteColorEntry>> getPaletteEntries(PaletteId palette) const;

    Result<RampId>      createRamp(DocumentId doc, const RampDesc& desc);
    VoidResult          deleteRamp(RampId id);
    VoidResult          updateRamp(RampId id, const RampDesc& desc);
    Result<Color>       sampleRamp(RampId ramp, float t) const;
    // What the ramp currently holds. An interface showing a ramp's stops has
    // to read them from somewhere, and the only somewhere is here -- keeping
    // its own copy is the parallel-list mistake in miniature.
    Result<RampDesc>    getRamp(RampId ramp) const;

    VoidResult          swapPalette(SpriteId sprite, PaletteId newPalette);
    // Rewrites a ramp's literal stops for a palette change by matching each
    // colour to the role it appears to be. This is a guess -- exact match, then
    // nearest -- and it exists for ramps built from literal colours. A ramp
    // whose stops name roles needs none of it: it follows the palette on its
    // own, and is left untouched here.
    VoidResult          remapRamp(RampId ramp, PaletteId fromPalette, PaletteId toPalette);

    // Give a region a standing colour role: a fill targeting it that names no
    // role of its own paints this one, so "this shape is skin" is said once and
    // survives a palette swap.
    VoidResult          bindRegionToPaletteRole(RegionId region, ColorRole role);
    VoidResult          unbindRegionPaletteRole(RegionId region);
    Result<ColorRole>   getRegionPaletteRole(RegionId region) const;

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

    // The prebaked library. Every kind is a full threshold ranking, so it works
    // at any density and inside a gradient. `scale` enlarges the tile (thicker
    // lines, bigger dots); `seed` only matters for Noise.
    Result<PatternId>   createDitherPattern(DocumentId doc, DitherPatternKind kind,
                                            uint32_t scale = 1, uint32_t seed = 1);
    std::vector<DitherPatternKind> ditherPatternKinds() const;
    std::string_view    ditherPatternName(DitherPatternKind kind) const;

    // External patterns: hand the engine a tile of your own, or import one from
    // a raster (a scanned texture, a painted tile, an exported sprite).
    Result<PatternId>   createPatternFromRaster(DocumentId doc, const RasterBuffer& raster,
                                                PatternImportMode mode = PatternImportMode::Threshold,
                                                std::string_view name = {});

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

    // Sprite placement. These compose into the sprite transform, which is the
    // single thing sockets, pivots and attached children resolve through, so a
    // sprite moved here brings its whole assembly with it. Deforms are not here
    // on purpose: a squash with a boundary and a falloff is not a matrix, so it
    // stays an operation added with addOperation.
    VoidResult  translateSprite(SpriteId sprite, Vec2f delta);
    VoidResult  rotateSprite(SpriteId sprite, float angleDeg, PivotId pivot = PivotId::null());
    VoidResult  scaleSprite(SpriteId sprite, Vec2f factor, PivotId pivot = PivotId::null());
    VoidResult  mirrorSprite(SpriteId sprite, MirrorAxis axis, PivotId pivot = PivotId::null());
    VoidResult  resetSpriteTransform(SpriteId sprite);

    // =======================================================================
    // SECTION 12: Spatial Anchors (Pivot, Socket, Boundary)
    // =======================================================================

    // --- Pivots ------------------------------------------------------------
    Result<PivotId>     createPivot(SpriteId sprite, Vec2f position);
    Result<PivotId>     createPivot(SpriteId sprite, const PivotDesc& desc);
    VoidResult          deletePivot(PivotId id);
    VoidResult          setPivot(PivotId id, Vec2f position);
    Result<Vec2f>       getPivot(PivotId id) const;
    VoidResult          movePivot(PivotId id, Vec2f delta);
    VoidResult          setSpritePivot(SpriteId sprite, PivotId pivot);
    Result<PivotId>     findPivot(SpriteId sprite, std::string_view name) const;
    Result<std::string> getPivotName(PivotId id) const;
    // Place a pivot from the sprite compiled bounds, so it follows the artwork.
    VoidResult          placePivot(PivotId id, PivotPlacement placement,
                                   const CompileProfile& profile = {});
    // Where the pivot ends up once the sprite own transform and any attachment
    // chain above it are resolved.
    Result<Vec2f>       getPivotWorldPosition(PivotId id) const;

    // --- Sprite placement --------------------------------------------------
    // A sprite carries its own transform. compileSprite resolves it, so a
    // sprite placed or turned here compiles where it was put.
    VoidResult          setSpriteTransform(SpriteId sprite, const Mat3f& transform);
    Result<Mat3f>       getSpriteTransform(SpriteId sprite) const;
    // The same transform with every attachment above it folded in.
    Result<Mat3f>       getSpriteWorldTransform(SpriteId sprite) const;

    // --- Sockets -----------------------------------------------------------
    Result<SocketId>    addSocket(SpriteId sprite, const SocketDesc& desc);
    VoidResult          removeSocket(SocketId id);
    VoidResult          moveSocket(SocketId id, Vec2f position);
    VoidResult          rotateSocket(SocketId id, float angleDeg);
    VoidResult          setSocketScale(SocketId id, Vec2f scale);
    Result<SocketDesc>  getSocket(SocketId id) const;
    Result<SocketId>    findSocket(SpriteId sprite, std::string_view name) const;
    // The socket frame in sprite-local space.
    Result<Mat3f>       getSocketTransform(SocketId id) const;
    // The socket frame after the owning sprite transform and attachment chain.
    Result<Mat3f>       getSocketWorldTransform(SocketId id) const;
    Result<Vec2f>       getSocketWorldPosition(SocketId id) const;
    // The placement that lands childPivot on this socket, without any chain.
    Result<Mat3f>       resolveSocketAttachment(SocketId socket, PivotId childPivot) const;

    // --- Attachments -------------------------------------------------------
    // A sprite hangs off at most one socket. Attaching rejects a cycle rather
    // than building one.
    VoidResult          attachSprite(SpriteId child, const AttachmentDesc& desc);
    // Shorthand: the child offers its own pivot.
    VoidResult          attachSprite(SpriteId child, SocketId socket);
    VoidResult          detachSprite(SpriteId child);
    Result<AttachmentInfo> getAttachment(SpriteId child) const;
    Result<std::vector<SpriteId>> getAttachedSprites(SocketId socket) const;
    // Every sprite in the assembly under root, in compositing order.
    Result<std::vector<SpriteId>> assemblyOrder(SpriteId root) const;
    // Compile the whole assembly: each sprite compiled from its own operations,
    // then placed by the attachment chain.
    Result<CompileResult> compileAssembly(SpriteId root, const CompileProfile& profile);

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
    // Live instructions
    // =======================================================================

    // What can be driven on an operation, and what type each field takes.
    Result<std::vector<ParameterInfo>> describeOperation(OperationId id) const;
    Result<ParameterValue> getOperationParameter(OperationId id, std::string_view name) const;
    VoidResult             setOperationParameter(OperationId id, std::string_view name,
                                                 const ParameterValue& value);

    // Apply a batch as one frame. Every instruction is validated before any is
    // applied, so a bad instruction leaves the document untouched rather than
    // half posed.
    VoidResult applyInstructions(const std::vector<Instruction>& instructions);

    // Apply a frame and compile it: the whole assembly under root, so an
    // articulated figure renders in one call.
    Result<CompileResult> renderFrame(SpriteId root,
                                      const std::vector<Instruction>& instructions,
                                      const CompileProfile& profile);

    // =======================================================================
    // Editing support
    // =======================================================================

    // Capture and restore document state with ids intact. This is the
    // foundation an app builds undo on: the engine holds no history of its own,
    // it only makes a moment recoverable.
    Result<DocumentSnapshot> snapshotDocumentState(DocumentId doc) const;
    VoidResult               restoreDocumentState(DocumentId doc, const DocumentSnapshot& snapshot);

    // Regions can be edited in place, so a pencil accumulates into the region
    // it is drawing rather than leaving one region and one operation per
    // stroke. A region edited by hand stops tracking the geometry it came from.
    VoidResult setRegionIntervals(RegionId region, const IntervalSet& intervals);
    VoidResult addPixelsToRegion(RegionId region, const PixelRegionDesc& desc);
    VoidResult erasePixelsFromRegion(RegionId region, const std::vector<Vec2i>& pixels);

    // ---------------------------------------------------------------------
    // App metadata
    //
    // Space for data the engine does not understand: which frame an app thinks
    // a sprite is, what a layer is called in a panel, whatever an app needs to
    // keep with the document rather than beside it.
    //
    // Three rules make it safe to carry:
    //   - Keys are namespaced ("fast.frame", "pract.arranger.cell"). Two apps
    //     writing the same document do not overwrite each other.
    //   - Values are opaque bytes to the engine. It stores and returns them and
    //     never parses or executes them.
    //   - Sizes are capped, because a document is a file that other people open.
    //
    // Metadata written by another app is untrusted input. It arrives from
    // whoever sent the file, so validate it before acting on it, and do not put
    // anything in it you would not send to a stranger.
    // ---------------------------------------------------------------------
    VoidResult setMetadata(uint64_t entityId, std::string_view key, std::string_view value);
    Result<std::string> getMetadata(uint64_t entityId, std::string_view key) const;
    VoidResult clearMetadata(uint64_t entityId, std::string_view key);
    Result<std::vector<std::string>> metadataKeys(uint64_t entityId) const;

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
    // Packages
    // =======================================================================

    // Write a document and a set of app entries as one container.
    Result<SerializedData> writePackage(DocumentId doc,
                                        const std::vector<PackageEntry>& entries) const;

    // Read a container without loading anything into the engine, so a caller
    // can inspect what arrived before acting on it.
    Result<PackageContents> readPackage(const SerializedData& package) const;

    // Read a container and load its document. Entries are handed back untouched
    // for the app to deal with, including entries this app did not write.
    Result<DocumentId> loadPackage(const SerializedData& package,
                                   std::vector<PackageEntry>* entries = nullptr);

    // Whether a name is acceptable as an entry name. Exposed so an app can
    // check before it builds an entry rather than after.
    static bool isValidPackageEntryName(std::string_view name);

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
    // compileLayer, told what the whole sprite draws.
    //
    // A silhouette outline that names a sprite traces everything the sprite
    // draws rather than just its own layer, and a layer compiled on its own
    // cannot know that. compileSprite works it out once and passes it down.
    // Null means the layer is being compiled alone, and such an outline falls
    // back to tracing its own layer rather than disappearing.
    struct LayerCloneTables;
    Result<LayerId> cloneLayerInto(LayerId source, SpriteId into, int32_t atIndex,
                                   LayerCloneTables& tables);
    Result<CompileResult> compileLayerWithin(LayerId id, const CompileProfile& profile,
                                             const RasterBuffer* spriteSilhouette,
                                             bool suppressSpriteOutlines) const;

    std::unique_ptr<Impl> impl_;
    LSContext();
};

} // namespace ls
