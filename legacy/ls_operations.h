#pragma once
// ls_operations.h — All operation data types
//
// Operations ARE the source truth. Every fill, stroke, transform, deform,
// and outline is an operation stored in the engine's operation table.
//
// Design rules:
//   - All op structs are plain data (no virtual methods, no runtime polymorphism)
//   - All fields must be serializable
//   - All IDs reference engine-owned objects
//   - std::variant<AllOps...> is the Operation type
//   - Plugin ops are wrapped in PluginOp

#include "ls_types.h"
#include "ls_geometry.h"
#include <variant>
#include <string>
#include <unordered_map>
#include <any>

namespace ls {

// ---------------------------------------------------------------------------
// 1. FILL OPERATIONS
// ---------------------------------------------------------------------------

struct FillSolidOp {
    RegionId        targetRegion;
    ColorRole       paletteRole   = kColorRoleNone;
    Color           fallbackColor;               // used if paletteRole is unbound
    BlendMode       blend         = BlendMode::Normal;
    float           opacity       = 1.f;
};

struct FillGradientOp {
    RegionId        targetRegion;
    RampId          ramp;
    Vec2f           startPoint;
    Vec2f           endPoint;
    CoordinateSpace coordinateSpace = CoordinateSpace::Object;
    bool            repeat          = false;
    BlendMode       blend           = BlendMode::Normal;
    float           opacity         = 1.f;
};

struct FillRampOp {
    RegionId        targetRegion;
    RampId          ramp;
    float           angle           = 0.f;         // degrees
    CoordinateSpace coordinateSpace = CoordinateSpace::Object;
    BlendMode       blend           = BlendMode::Normal;
    float           opacity         = 1.f;
};

struct FillDitherOp {
    RegionId        targetRegion;
    RampId          ramp;
    PatternId       pattern;
    float           density         = 0.5f;        // [0..1]
    float           phase           = 0.f;         // pattern phase offset
    CoordinateSpace coordinateSpace = CoordinateSpace::Object;
    BlendMode       blend           = BlendMode::Normal;
    float           opacity         = 1.f;
};

struct FillNoiseOp {
    RegionId        targetRegion;
    RampId          ramp;
    float           scale           = 1.f;
    float           seed            = 0.f;
    CoordinateSpace coordinateSpace = CoordinateSpace::Object;
    BlendMode       blend           = BlendMode::Normal;
    float           opacity         = 1.f;
};

struct FillLinePatternOp {
    RegionId        targetRegion;
    ColorRole       lineRole        = kColorRoleNone;
    ColorRole       bgRole          = kColorRoleNone;
    float           spacing         = 4.f;         // pixels between lines
    float           angle           = 45.f;        // degrees
    float           lineWidth       = 1.f;
    CoordinateSpace coordinateSpace = CoordinateSpace::Object;
    BlendMode       blend           = BlendMode::Normal;
    float           opacity         = 1.f;
};

struct FillTexturePatternOp {
    RegionId        targetRegion;
    PatternId       pattern;
    Vec2f           scale           = {1.f, 1.f};
    Vec2f           offset;
    float           angle           = 0.f;
    CoordinateSpace coordinateSpace = CoordinateSpace::Object;
    BlendMode       blend           = BlendMode::Normal;
    float           opacity         = 1.f;
};

struct FillSemanticColorOp {
    RegionId        targetRegion;
    ColorRole       paletteRole     = kColorRoleNone;
    BlendMode       blend           = BlendMode::Normal;
    float           opacity         = 1.f;
};

// ---------------------------------------------------------------------------
// 2. STROKE OPERATIONS
// ---------------------------------------------------------------------------

struct StrokePolylineOp {
    GeometryId      polyline;
    float           width           = 1.f;
    StrokeCap       cap             = StrokeCap::Flat;
    StrokeJoin      join            = StrokeJoin::Miter;
    float           miterLimit      = 4.f;
    float           taper           = 0.f;         // [0..1], 0 = no taper
    PatternId       strokePattern;                 // null = solid
    ColorRole       paletteRole     = kColorRoleNone;
    SnapPolicy      snap            = SnapPolicy::Grid;
    BlendMode       blend           = BlendMode::Normal;
    float           opacity         = 1.f;
};

struct StrokeCurveOp {
    GeometryId      curve;
    float           width           = 1.f;
    StrokeCap       cap             = StrokeCap::Round;
    StrokeJoin      join            = StrokeJoin::Round;
    float           miterLimit      = 4.f;
    float           taper           = 0.f;
    ColorRole       paletteRole     = kColorRoleNone;
    BlendMode       blend           = BlendMode::Normal;
    float           opacity         = 1.f;
};

struct StrokeRegionBoundaryOp {
    RegionId        targetRegion;
    float           width           = 1.f;
    StrokeCap       cap             = StrokeCap::Flat;
    StrokeJoin      join            = StrokeJoin::Miter;
    float           miterLimit      = 4.f;
    ColorRole       paletteRole     = kColorRoleNone;
    SnapPolicy      snap            = SnapPolicy::Grid;
    BlendMode       blend           = BlendMode::Normal;
    float           opacity         = 1.f;
};

struct StrokeBrushOp {
    GeometryId      path;
    PatternId       brushPattern;
    float           size            = 4.f;
    float           spacing         = 0.25f;       // fraction of brush size
    float           scatter         = 0.f;
    ColorRole       paletteRole     = kColorRoleNone;
    BlendMode       blend           = BlendMode::Normal;
    float           opacity         = 1.f;
};

struct StrokePixelPathOp {
    GeometryId      path;
    ColorRole       paletteRole     = kColorRoleNone;
    SnapPolicy      snap            = SnapPolicy::Grid;
    BlendMode       blend           = BlendMode::Normal;
    float           opacity         = 1.f;
};

// ---------------------------------------------------------------------------
// 3. OUTLINE OPERATIONS
// ---------------------------------------------------------------------------

enum class OutlineSide : uint8_t { Inside, Outside, Center };
enum class OutlineCorner : uint8_t { Sharp, Round, Bevel };
enum class OutlineDiagonal : uint8_t { Include, Exclude, Bridge };

struct GenerateSilhouetteOutlineOp {
    SpriteId        targetSprite;       // null = current sprite
    float           thickness           = 1.f;
    OutlineSide     side                = OutlineSide::Outside;
    OutlineCorner   corner              = OutlineCorner::Sharp;
    OutlineDiagonal diagonal            = OutlineDiagonal::Include;
    ColorRole       paletteRole         = kColorRoleNone;
    BoundaryId      limitBoundary;      // null = no limit
    BlendMode       blend               = BlendMode::Normal;
    float           opacity             = 1.f;
};

struct GenerateInnerOutlineOp {
    RegionId        targetRegion;
    float           thickness           = 1.f;
    OutlineCorner   corner              = OutlineCorner::Sharp;
    ColorRole       paletteRole         = kColorRoleNone;
    BlendMode       blend               = BlendMode::Normal;
    float           opacity             = 1.f;
};

struct GenerateOuterOutlineOp {
    RegionId        targetRegion;
    float           thickness           = 1.f;
    OutlineCorner   corner              = OutlineCorner::Sharp;
    OutlineDiagonal diagonal            = OutlineDiagonal::Include;
    ColorRole       paletteRole         = kColorRoleNone;
    BlendMode       blend               = BlendMode::Normal;
    float           opacity             = 1.f;
};

struct GenerateRegionOutlineOp {
    RegionId        targetRegion;
    float           thickness           = 1.f;
    OutlineSide     side                = OutlineSide::Center;
    OutlineCorner   corner              = OutlineCorner::Sharp;
    ColorRole       paletteRole         = kColorRoleNone;
    BlendMode       blend               = BlendMode::Normal;
    float           opacity             = 1.f;
};

struct GenerateMaterialBoundaryOutlineOp {
    LayerId         sourceLayer;
    float           thickness           = 1.f;
    ColorRole       paletteRole         = kColorRoleNone;
    BlendMode       blend               = BlendMode::Normal;
    float           opacity             = 1.f;
};

struct CleanupOutlineOp {
    OperationId     targetOutlineOp;    // the outline op to clean up
    bool            removeIsolatedPixels = true;
    bool            smoothCorners        = false;
};

struct JoinCornersOp {
    OperationId     outlineA;
    OperationId     outlineB;
    float           joinRadius = 1.f;
};

struct ResolveOutlineCollisionsOp {
    std::vector<OperationId> outlineOps;
    // Priority order: first wins
};

// ---------------------------------------------------------------------------
// 4. TRANSFORM OPERATIONS
// These store mathematical transform parameters — NOT baked raster results.
// ---------------------------------------------------------------------------

struct TranslateOp {
    SpriteId        target;             // null → target is the layer
    LayerId         targetLayer;        // used if target sprite is null
    RegionId        targetRegion;       // used if targetLayer is null (region-only translate)
    Vec2f           delta;
    PivotId         pivot;              // null = origin
    CoordinateSpace coordinateSpace     = CoordinateSpace::Object;
    RoundingPolicy  rounding            = RoundingPolicy::Nearest;
};

struct RotateOp {
    SpriteId        target;
    LayerId         targetLayer;
    RegionId        targetRegion;
    float           angleDegrees        = 0.f;
    PivotId         pivot;
    CoordinateSpace coordinateSpace     = CoordinateSpace::Object;
    RoundingPolicy  rounding            = RoundingPolicy::Nearest;
    SamplingPolicy  sampling            = SamplingPolicy::Coverage;
};

struct ScaleOp {
    SpriteId        target;
    LayerId         targetLayer;
    RegionId        targetRegion;
    Vec2f           factor              = {1.f, 1.f};
    PivotId         pivot;
    CoordinateSpace coordinateSpace     = CoordinateSpace::Object;
    RoundingPolicy  rounding            = RoundingPolicy::Nearest;
    SamplingPolicy  sampling            = SamplingPolicy::Coverage;
};

struct MirrorOp {
    SpriteId        target;
    LayerId         targetLayer;
    RegionId        targetRegion;
    MirrorAxis      axis                = MirrorAxis::X;
    PivotId         pivot;
    CoordinateSpace coordinateSpace     = CoordinateSpace::Object;
};

struct ShearOp {
    SpriteId        target;
    LayerId         targetLayer;
    Vec2f           shear               = {0.f, 0.f};
    PivotId         pivot;
    CoordinateSpace coordinateSpace     = CoordinateSpace::Object;
};

struct SkewOp {
    SpriteId        target;
    LayerId         targetLayer;
    float           angleX              = 0.f;
    float           angleY              = 0.f;
    PivotId         pivot;
    CoordinateSpace coordinateSpace     = CoordinateSpace::Object;
};

struct SquashOp {
    SpriteId        target;
    BoundaryId      boundary;
    float           factor              = 1.f;      // < 1 = squash
    PivotId         pivot;
    Falloff         falloff             = Falloff::Linear;
};

struct StretchOp {
    SpriteId        target;
    BoundaryId      boundary;
    float           factor              = 1.f;      // > 1 = stretch
    PivotId         pivot;
    Falloff         falloff             = Falloff::Linear;
};

struct MatrixTransformOp {
    SpriteId        target;
    LayerId         targetLayer;
    RegionId        targetRegion;
    Mat3f           matrix;
    CoordinateSpace coordinateSpace     = CoordinateSpace::Object;
    RoundingPolicy  rounding            = RoundingPolicy::Nearest;
    SamplingPolicy  sampling            = SamplingPolicy::Coverage;
};

struct PivotTransformOp {
    SpriteId        target;
    PivotId         pivot;
    float           angleDegrees        = 0.f;
    Vec2f           scale               = {1.f, 1.f};
    RoundingPolicy  rounding            = RoundingPolicy::Nearest;
};

struct AnchorTransformOp {
    SpriteId        child;
    SocketId        socket;
    PivotId         childPivot;
    Mat3f           localOffset;        // additional local transform at attachment
};

// ---------------------------------------------------------------------------
// 5. DEFORMATION OPERATIONS
// All deforms are mathematical — they warp the underlying geometry.
// ---------------------------------------------------------------------------

struct BendOp {
    RegionId        targetRegion;
    BoundaryId      boundary;
    float           strength            = 0.f;
    float           angle               = 0.f;
    Falloff         falloff             = Falloff::Linear;
    CoordinateSpace coordinateSpace     = CoordinateSpace::Object;
};

struct WarpOp {
    RegionId        targetRegion;
    std::vector<Vec2f> handlePoints;
    std::vector<Vec2f> displacements;
    float           strength            = 1.f;
    Falloff         falloff             = Falloff::Smooth;
    BoundaryId      influenceRegion;
};

struct LatticeDeformOp {
    RegionId        targetRegion;
    uint32_t        gridW               = 4;
    uint32_t        gridH               = 4;
    std::vector<Vec2f> controlPoints;  // size = gridW * gridH
    BoundaryId      influenceRegion;
};

struct EnvelopeDeformOp {
    RegionId        targetRegion;
    GeometryId      envelopeCurve;
    Falloff         falloff             = Falloff::Smooth;
};

struct PinDeformOp {
    RegionId        targetRegion;
    std::vector<Vec2f> pins;
    std::vector<Vec2f> pinTargets;
    float           stiffness           = 1.f;
    BoundaryId      influenceRegion;
};

struct WeightedDeformOp {
    RegionId        targetRegion;
    std::vector<Vec2f> handles;
    std::vector<float> weights;         // per-handle weight [0..1]
    std::vector<Vec2f> handleTargets;
    Falloff         falloff             = Falloff::Smooth;
};

struct BoundaryDeformOp {
    RegionId        targetRegion;
    BoundaryId      boundary;
    GeometryId      targetShape;
    float           strength            = 1.f;
    Falloff         falloff             = Falloff::Smooth;
};

struct PathDeformOp {
    RegionId        targetRegion;
    GeometryId      path;
    float           offset              = 0.f;
    bool            followTangent       = true;
    Falloff         falloff             = Falloff::Linear;
};

// ---------------------------------------------------------------------------
// 6. PLUGIN OPERATION (wraps app/toolkit-registered operation types)
// ---------------------------------------------------------------------------

struct PluginOp {
    std::string                             typeId;     // globally unique type string
    std::unordered_map<std::string, std::any> params;   // parameter bag (must be serializable)

    // The engine does not interpret params — it passes them to the registered resolver.
    // Serialization is handled by the plugin's registered schema.
};

// ---------------------------------------------------------------------------
// The Operation variant — the canonical operation union type
// Every operation stored in the engine is one of these.
// ---------------------------------------------------------------------------
using Operation = std::variant<
    // Fills
    FillSolidOp,
    FillGradientOp,
    FillRampOp,
    FillDitherOp,
    FillNoiseOp,
    FillLinePatternOp,
    FillTexturePatternOp,
    FillSemanticColorOp,
    // Strokes
    StrokePolylineOp,
    StrokeCurveOp,
    StrokeRegionBoundaryOp,
    StrokeBrushOp,
    StrokePixelPathOp,
    // Outlines
    GenerateSilhouetteOutlineOp,
    GenerateInnerOutlineOp,
    GenerateOuterOutlineOp,
    GenerateRegionOutlineOp,
    GenerateMaterialBoundaryOutlineOp,
    CleanupOutlineOp,
    JoinCornersOp,
    ResolveOutlineCollisionsOp,
    // Transforms
    TranslateOp,
    RotateOp,
    ScaleOp,
    MirrorOp,
    ShearOp,
    SkewOp,
    SquashOp,
    StretchOp,
    MatrixTransformOp,
    PivotTransformOp,
    AnchorTransformOp,
    // Deforms
    BendOp,
    WarpOp,
    LatticeDeformOp,
    EnvelopeDeformOp,
    PinDeformOp,
    WeightedDeformOp,
    BoundaryDeformOp,
    PathDeformOp,
    // Plugin
    PluginOp
>;

// Helper: get the type name string for an Operation (useful for serialization / debug)
std::string_view operationTypeName(const Operation& op);

} // namespace ls
