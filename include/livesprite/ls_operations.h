#pragma once
// ls_operations.h — All operation data types
//
// Operations ARE the source truth. Every fill, stroke, transform, deform,
// and outline is an operation stored in the engine operation table.
//
// Design rules:
//   - All op structs are plain data (no virtual methods, no runtime polymorphism)
//   - All fields must be serializable
//   - All IDs reference engine-owned objects
//   - std::variant<AllOps...> is the Operation type
//   - Plugin ops are wrapped in PluginOp

#include "ls_types.h"
#include "ls_geometry.h"

#include <map>
#include <string>
#include <variant>
#include <vector>

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
    ColorRole       foregroundRole  = kColorRoleNone;
    ColorRole       backgroundRole  = kColorRoleNone;
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
    Color           fallbackColor   = Color::black();
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
    Color           fallbackColor   = Color::black();
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
    Color           fallbackColor   = Color::black();
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
    uint32_t        scatterSeed     = 1;           // scatter must stay deterministic
    ColorRole       paletteRole     = kColorRoleNone;
    Color           fallbackColor   = Color::black();
    BlendMode       blend           = BlendMode::Normal;
    float           opacity         = 1.f;
};

struct StrokePixelPathOp {
    GeometryId      path;
    ColorRole       paletteRole     = kColorRoleNone;
    Color           fallbackColor   = Color::black();
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
    SpriteId        targetSprite;       // null = the sprite owning this operation
    float           thickness           = 1.f;
    OutlineSide     side                = OutlineSide::Outside;
    OutlineCorner   corner              = OutlineCorner::Sharp;
    OutlineDiagonal diagonal            = OutlineDiagonal::Include;
    ColorRole       paletteRole         = kColorRoleNone;
    Color           fallbackColor       = Color::black();
    BoundaryId      limitBoundary;      // null = no limit
    BlendMode       blend               = BlendMode::Normal;
    float           opacity             = 1.f;
};

struct GenerateInnerOutlineOp {
    RegionId        targetRegion;
    float           thickness           = 1.f;
    OutlineCorner   corner              = OutlineCorner::Sharp;
    ColorRole       paletteRole         = kColorRoleNone;
    Color           fallbackColor       = Color::black();
    BlendMode       blend               = BlendMode::Normal;
    float           opacity             = 1.f;
};

struct GenerateOuterOutlineOp {
    RegionId        targetRegion;
    float           thickness           = 1.f;
    OutlineCorner   corner              = OutlineCorner::Sharp;
    OutlineDiagonal diagonal            = OutlineDiagonal::Include;
    ColorRole       paletteRole         = kColorRoleNone;
    Color           fallbackColor       = Color::black();
    BlendMode       blend               = BlendMode::Normal;
    float           opacity             = 1.f;
};

struct GenerateRegionOutlineOp {
    RegionId        targetRegion;
    float           thickness           = 1.f;
    OutlineSide     side                = OutlineSide::Center;
    OutlineCorner   corner              = OutlineCorner::Sharp;
    ColorRole       paletteRole         = kColorRoleNone;
    Color           fallbackColor       = Color::black();
    BlendMode       blend               = BlendMode::Normal;
    float           opacity             = 1.f;
};

struct GenerateMaterialBoundaryOutlineOp {
    LayerId         sourceLayer;
    float           thickness           = 1.f;
    ColorRole       paletteRole         = kColorRoleNone;
    Color           fallbackColor       = Color::black();
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
    std::vector<OperationId> outlineOps;   // priority order: first wins
};

// ---------------------------------------------------------------------------
// 4. TRANSFORM OPERATIONS
// These store mathematical transform parameters — NOT baked raster results.
// Targeting rule for every transform: if targetRegion is set the transform
// applies to that region only; else if targetLayer is set it applies to the
// layer; else it applies to the whole sprite owning the operation.
// ---------------------------------------------------------------------------

struct TranslateOp {
    SpriteId        target;
    LayerId         targetLayer;
    RegionId        targetRegion;
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
    Vec2f           pivotFallback;      // used when pivot is null
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
    Vec2f           pivotFallback;
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
    Vec2f           pivotFallback;
    CoordinateSpace coordinateSpace     = CoordinateSpace::Object;
};

struct ShearOp {
    SpriteId        target;
    LayerId         targetLayer;
    RegionId        targetRegion;
    Vec2f           shear               = {0.f, 0.f};
    PivotId         pivot;
    Vec2f           pivotFallback;
    CoordinateSpace coordinateSpace     = CoordinateSpace::Object;
    SamplingPolicy  sampling            = SamplingPolicy::Coverage;
};

struct SkewOp {
    SpriteId        target;
    LayerId         targetLayer;
    RegionId        targetRegion;
    float           angleX              = 0.f;
    float           angleY              = 0.f;
    PivotId         pivot;
    Vec2f           pivotFallback;
    CoordinateSpace coordinateSpace     = CoordinateSpace::Object;
    SamplingPolicy  sampling            = SamplingPolicy::Coverage;
};

struct SquashOp {
    SpriteId        target;
    LayerId         targetLayer;
    RegionId        targetRegion;
    BoundaryId      boundary;
    float           factor              = 1.f;      // < 1 = squash
    PivotId         pivot;
    Vec2f           pivotFallback;
    Falloff         falloff             = Falloff::Linear;
};

struct StretchOp {
    SpriteId        target;
    LayerId         targetLayer;
    RegionId        targetRegion;
    BoundaryId      boundary;
    float           factor              = 1.f;      // > 1 = stretch
    PivotId         pivot;
    Vec2f           pivotFallback;
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
    LayerId         targetLayer;
    PivotId         pivot;
    float           angleDegrees        = 0.f;
    Vec2f           scale               = {1.f, 1.f};
    RoundingPolicy  rounding            = RoundingPolicy::Nearest;
    SamplingPolicy  sampling            = SamplingPolicy::Coverage;
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
    LayerId         targetLayer;
    BoundaryId      boundary;
    float           strength            = 0.f;
    float           angle               = 0.f;
    Falloff         falloff             = Falloff::Linear;
    CoordinateSpace coordinateSpace     = CoordinateSpace::Object;
};

struct WarpOp {
    RegionId        targetRegion;
    LayerId         targetLayer;
    std::vector<Vec2f> handlePoints;
    std::vector<Vec2f> displacements;
    float           strength            = 1.f;
    float           radius              = 8.f;
    Falloff         falloff             = Falloff::Smooth;
    BoundaryId      influenceRegion;
};

struct LatticeDeformOp {
    RegionId        targetRegion;
    LayerId         targetLayer;
    uint32_t        gridW               = 4;
    uint32_t        gridH               = 4;
    std::vector<Vec2f> controlPoints;   // size = gridW * gridH, in pixel space
    BoundaryId      influenceRegion;
};

struct EnvelopeDeformOp {
    RegionId        targetRegion;
    LayerId         targetLayer;
    GeometryId      envelopeCurve;
    float           strength            = 1.f;
    Falloff         falloff             = Falloff::Smooth;
};

struct PinDeformOp {
    RegionId        targetRegion;
    LayerId         targetLayer;
    std::vector<Vec2f> pins;
    std::vector<Vec2f> pinTargets;
    float           stiffness           = 1.f;
    BoundaryId      influenceRegion;
};

struct WeightedDeformOp {
    RegionId        targetRegion;
    LayerId         targetLayer;
    std::vector<Vec2f> handles;
    std::vector<float> weights;         // per-handle weight [0..1]
    std::vector<Vec2f> handleTargets;
    float           radius              = 8.f;
    Falloff         falloff             = Falloff::Smooth;
};

struct BoundaryDeformOp {
    RegionId        targetRegion;
    LayerId         targetLayer;
    BoundaryId      boundary;
    GeometryId      targetShape;
    float           strength            = 1.f;
    Falloff         falloff             = Falloff::Smooth;
};

struct PathDeformOp {
    RegionId        targetRegion;
    LayerId         targetLayer;
    GeometryId      path;
    float           offset              = 0.f;
    bool            followTangent       = true;
    Falloff         falloff             = Falloff::Linear;
};

// ---------------------------------------------------------------------------
// 6. PLUGIN OPERATION (wraps app/toolkit-registered operation types)
// ---------------------------------------------------------------------------

// A serializable parameter value. std::any is deliberately avoided: it needs
// RTTI, and the engine builds without it.
using PluginValue = std::variant<
    bool,
    int64_t,
    double,
    std::string,
    Vec2f,
    Color,
    uint64_t        // raw entity id (RegionId, GeometryId, ... .value)
>;

// std::map, not unordered_map: iteration order must be deterministic because
// it drives serialization output and dependency ordering.
using PluginParams = std::map<std::string, PluginValue>;

struct PluginOp {
    std::string  typeId;    // globally unique type string
    PluginParams params;

    // The engine does not interpret params — it passes them to the registered
    // resolver. Serialization uses the plugin schema.
};

// ---------------------------------------------------------------------------
// The Operation variant — the canonical operation union type
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

// Stable type name for an Operation (serialization keys and debug output).
std::string_view operationTypeName(const Operation& op);

// True if the operation resolves as a transform or deform of prior content
// rather than as a mark that adds content.
bool operationIsTransform(const Operation& op);

// Entity IDs this operation reads. Changing any of them dirties the operation.
std::vector<uint64_t> operationDependencies(const Operation& op);

} // namespace ls
