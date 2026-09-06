#pragma once
// ls_types.h — Core types, IDs, Result<T>, enums, and math primitives
// LiveSprite Engine — do not include application logic here.

#include <cstdint>
#include <cstddef>
#include <cassert>
#include <string>
#include <string_view>
#include <optional>
#include <functional>

namespace ls {

// ---------------------------------------------------------------------------
// Engine versioning
// ---------------------------------------------------------------------------
constexpr uint32_t LS_ENGINE_VERSION_MAJOR = 0;
constexpr uint32_t LS_ENGINE_VERSION_MINOR = 1;
constexpr uint32_t LS_ENGINE_VERSION_PATCH = 0;
constexpr uint32_t LS_ENGINE_VERSION =
    (LS_ENGINE_VERSION_MAJOR << 16) |
    (LS_ENGINE_VERSION_MINOR <<  8) |
     LS_ENGINE_VERSION_PATCH;

// ---------------------------------------------------------------------------
// Opaque typed ID handles
// All entities live inside LSContext. Apps hold these IDs.
// ---------------------------------------------------------------------------
template<typename Tag>
struct TypedId {
    uint64_t value = 0;
    bool valid() const { return value != 0; }
    bool operator==(const TypedId& o) const { return value == o.value; }
    bool operator!=(const TypedId& o) const { return value != o.value; }
    bool operator< (const TypedId& o) const { return value <  o.value; }
    static TypedId null() { return {0}; }
};

struct TagDocument  {};
struct TagSprite    {};
struct TagLayer     {};
struct TagGroup     {};
struct TagGeometry  {};
struct TagRegion    {};
struct TagOperation {};
struct TagPalette   {};
struct TagRamp      {};
struct TagPattern   {};
struct TagPivot     {};
struct TagSocket    {};
struct TagBoundary  {};

using DocumentId  = TypedId<TagDocument>;
using SpriteId    = TypedId<TagSprite>;
using LayerId     = TypedId<TagLayer>;
using GroupId     = TypedId<TagGroup>;
using GeometryId  = TypedId<TagGeometry>;
using RegionId    = TypedId<TagRegion>;
using OperationId = TypedId<TagOperation>;
using PaletteId   = TypedId<TagPalette>;
using RampId      = TypedId<TagRamp>;
using PatternId   = TypedId<TagPattern>;
using PivotId     = TypedId<TagPivot>;
using SocketId    = TypedId<TagSocket>;
using BoundaryId  = TypedId<TagBoundary>;

// Semantic color role — index into a palette (not a raw color)
using ColorRole = uint32_t;
constexpr ColorRole kColorRoleNone = UINT32_MAX;

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------
enum class LSError : uint32_t {
    None = 0,
    InvalidId,
    InvalidParameter,
    NullArgument,
    OutOfBounds,
    DependencyCycle,
    CompileFailure,
    SerializationFailure,
    DeserializationFailure,
    VersionMismatch,
    VersionMigrationFailed,
    PluginNotFound,
    PluginError,
    PluginDeterminismViolation,
    OperationTypeMismatch,
    RegionEmpty,
    PaletteNotBound,
    RasterAllocationFailed,
    NotImplemented,
};

std::string_view lsErrorString(LSError err);

// ---------------------------------------------------------------------------
// Result<T> — no exceptions cross the engine API boundary
// ---------------------------------------------------------------------------
template<typename T>
struct Result {
    T        value;
    LSError  error = LSError::None;

    bool ok()   const { return error == LSError::None; }
    bool fail() const { return error != LSError::None; }

    static Result ok(T val)          { return { std::move(val), LSError::None }; }
    static Result err(LSError e)     { return { T{},            e             }; }
};

template<>
struct Result<void> {
    LSError error = LSError::None;
    bool ok()   const { return error == LSError::None; }
    bool fail() const { return error != LSError::None; }
    static Result ok()           { return { LSError::None }; }
    static Result err(LSError e) { return { e             }; }
};

using VoidResult = Result<void>;

// ---------------------------------------------------------------------------
// Math primitives
// ---------------------------------------------------------------------------

struct Vec2i {
    int32_t x = 0, y = 0;
};

struct Vec2f {
    float x = 0.f, y = 0.f;
};

struct Vec3f {
    float x = 0.f, y = 0.f, z = 0.f;
};

// Row-major 3×3 homogeneous matrix for 2D transforms
struct Mat3f {
    float m[9] = {
        1,0,0,
        0,1,0,
        0,0,1
    };
    static Mat3f identity() { return {}; }
    Vec2f transformPoint(Vec2f p) const;
    Vec2f transformVector(Vec2f v) const;
    Mat3f mul(const Mat3f& o) const;
};

struct Rect2i {
    Vec2i min, max;             // inclusive min, exclusive max
    int32_t width()  const { return max.x - min.x; }
    int32_t height() const { return max.y - min.y; }
    bool    empty()  const { return width() <= 0 || height() <= 0; }
};

struct Rect2f {
    Vec2f min, max;
    float width()  const { return max.x - min.x; }
    float height() const { return max.y - min.y; }
    bool  empty()  const { return width() <= 0.f || height() <= 0.f; }
};

// RGBA8 color
struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    static Color transparent() { return {0,0,0,0}; }
    static Color black()       { return {0,0,0,255}; }
    static Color white()       { return {255,255,255,255}; }
};

// ---------------------------------------------------------------------------
// Enumerations shared across the API
// ---------------------------------------------------------------------------

enum class CoordinateSpace : uint8_t {
    Object,   // relative to the geometry's own local origin
    Sprite,   // relative to the sprite's bounding box
    Canvas,   // relative to the document canvas
    Export,   // relative to the export frame
};

enum class BlendMode : uint8_t {
    Normal,
    Multiply,
    Screen,
    Overlay,
    Add,
    Subtract,
    Darken,
    Lighten,
    Difference,
    Erase,
    Replace,
};

enum class RoundingPolicy : uint8_t {
    Nearest,
    Floor,
    Ceil,
    Truncate,
    SubpixelHalf,
};

enum class SamplingPolicy : uint8_t {
    Center,     // sample at pixel center
    Coverage,   // weight by coverage area
    Majority,   // use color covering >50% of pixel
    Median,     // median of covered sub-samples
    Average,    // average of sub-samples
    Threshold,  // binary: covered or not
};

enum class AlphaPolicy : uint8_t {
    Preserve,        // pass through alpha as-is
    Threshold,       // alpha < threshold → 0, else 255
    Premultiply,     // premultiply output alpha
    ForceOpaque,     // set all alpha to 255
};

enum class PalettePolicy : uint8_t {
    NearestMatch,    // snap to nearest palette color
    ExactMatch,      // error if not in palette
    Unconstrained,   // output any color (ignore palette)
};

enum class LayerType : uint8_t {
    Drawing,     // standard drawing layer (app label only — engine treats all layers equally)
    Generated,   // app hint: content is generated
    Source,      // app hint: content is a source reference
    Export,      // app hint: this is an export group root
};

// Compositing compile profile types
enum class CompileProfileType : uint8_t {
    Preview,    // fast, lower quality, may skip expensive ops
    Export,     // full quality, all ops resolved
    Debug,      // annotated output (dependency overlay, boundary vis)
    MaskOnly,   // alpha mask only, no color
    BoundsOnly, // only compute bounds, no raster
};

// Stroke caps and joins
enum class StrokeCap  : uint8_t { Flat, Round, Square };
enum class StrokeJoin : uint8_t { Miter, Round, Bevel };

// Falloff curves for deforms and boundary influence
enum class Falloff : uint8_t {
    Linear,
    Smooth,
    Sharp,
    Cosine,
    Constant,
};

// Mirror axes
enum class MirrorAxis : uint8_t { X, Y, Both };

// Pixel snap policy
enum class SnapPolicy : uint8_t {
    None,
    Grid,
    HalfGrid,
};

// ---------------------------------------------------------------------------
// Raster output buffer (engine-owned, returned by compile operations)
// ---------------------------------------------------------------------------
struct RasterBuffer {
    uint32_t        width  = 0;
    uint32_t        height = 0;
    uint32_t        stride = 0;   // bytes per row
    std::vector<uint8_t> pixels;  // RGBA8, row-major

    bool empty()  const { return pixels.empty(); }
    const uint8_t* row(uint32_t y) const { return pixels.data() + y * stride; }
    uint8_t*       row(uint32_t y)       { return pixels.data() + y * stride; }
};

// ---------------------------------------------------------------------------
// Compile profile
// ---------------------------------------------------------------------------
struct CompileProfile {
    uint32_t            outputWidth        = 0;
    uint32_t            outputHeight       = 0;
    CompileProfileType  type               = CompileProfileType::Preview;
    SamplingPolicy      sampling           = SamplingPolicy::Coverage;
    RoundingPolicy      rounding           = RoundingPolicy::Nearest;
    AlphaPolicy         alpha              = AlphaPolicy::Threshold;
    PalettePolicy       palette            = PalettePolicy::NearestMatch;
    float               coverageThreshold  = 0.5f;
    uint32_t            engineVersion      = LS_ENGINE_VERSION;  // baked at compile time
};

// Compiled result from the engine
struct CompileResult {
    RasterBuffer    raster;
    Rect2i          bounds;         // tight pixel bounds of non-transparent content
    LSError         error = LSError::None;
    bool            ok()  const { return error == LSError::None; }
};

} // namespace ls

// ---------------------------------------------------------------------------
// std::hash specializations for TypedId (enables use in unordered_map)
// ---------------------------------------------------------------------------
namespace std {
    template<typename Tag>
    struct hash<ls::TypedId<Tag>> {
        size_t operator()(const ls::TypedId<Tag>& id) const {
            return std::hash<uint64_t>{}(id.value);
        }
    };
} // namespace std
