// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors
#pragma once
// ls_types.h — Core types, IDs, Result<T>, enums, and math primitives
// LiveSprite Engine — do not include application logic here.

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

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
// Limits
//
// A canvas size arrives from an application or, worse, from a file somebody
// sent. Without a bound, "65535 x 65535" costs 17 GB to compile, and a width
// past 2^30 overflows the 32-bit stride and produces a raster that claims a size
// its storage does not have -- which is a memory-safety problem, not a
// performance one.
//
// The numbers below come from measuring this engine rather than from copying
// another program. Compile cost tracks canvas *area* times layer count:
//
//     256 x 256,  8 layers    135 ms      0.2 MB
//     512 x 512,  8 layers    178 ms      1 MB
//    1024 x 1024, 8 layers    839 ms      4 MB
//    2048 x 2048, 8 layers    1.6 s      16 MB
//    4096 x 4096, 8 layers    7.7 s      64 MB
//
// So the limit is on area, not only on a side. A dimension-only cap would admit
// 8192 x 8192 -- 256 MB per raster and half a minute per compile -- while
// refusing a 12000 x 200 sprite sheet that costs almost nothing. Both bounds are
// enforced: either side may reach kMaxCanvasDimension, but the two together may
// not exceed kMaxCanvasPixels.
//
// For reference, Aseprite's 65535 x 65535 ceiling is the range of the 16-bit
// field in its file header rather than a considered working size; its own author
// puts the practical figure near 9000, and users report trouble well below that.
// This engine compiles from operations instead of blitting a stored bitmap, so
// its practical ceiling is lower and worth stating honestly.
constexpr uint32_t kMaxCanvasDimension = 16384;

// 4096 x 4096. Holds one raster to 64 MB whatever the aspect ratio.
constexpr uint64_t kMaxCanvasPixels = 16777216ull;

// Beyond this a sprite is not slow, it is a mistake. Compile cost is linear in
// layer count on top of area.
constexpr uint32_t kMaxLayersPerSprite = 1024;

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
    PackageMalformed,        // the container is not a package this build can read
    PackageLimitExceeded,    // a package asks for more than the limits allow
    PackageEntryRejected,    // an entry name or claim is not acceptable
};

std::string_view lsErrorString(LSError err);

// ---------------------------------------------------------------------------
// Result<T> — no exceptions cross the engine API boundary
// ---------------------------------------------------------------------------
template<typename T>
struct Result {
    T        value {};
    LSError  error = LSError::None;

    bool ok()   const { return error == LSError::None; }
    bool fail() const { return error != LSError::None; }

    static Result ok(T val)      { return { std::move(val), LSError::None }; }
    static Result err(LSError e) { return { T{},            e             }; }
};

template<>
struct Result<void> {
    LSError error = LSError::None;
    bool ok()   const { return error == LSError::None; }
    bool fail() const { return error != LSError::None; }
    static Result success()      { return { LSError::None }; }
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

inline bool operator==(Vec2i a, Vec2i b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(Vec2i a, Vec2i b) { return !(a == b); }

// Row-major 3x3 homogeneous matrix for 2D transforms
struct Mat3f {
    float m[9] = {
        1,0,0,
        0,1,0,
        0,0,1
    };
    static Mat3f identity() { return {}; }
    static Mat3f translation(Vec2f t);
    static Mat3f rotation(float angleDegrees);
    static Mat3f scaling(Vec2f s);
    static Mat3f shearing(Vec2f shear);
    // Wrap a transform so that it happens around a pivot point.
    static Mat3f aroundPivot(const Mat3f& transform, Vec2f pivot);

    Vec2f transformPoint(Vec2f p) const;
    Vec2f transformVector(Vec2f v) const;
    Mat3f mul(const Mat3f& o) const;   // this * o  (o applied first)
    float determinant() const;
    Result<Mat3f> inverse() const;
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

inline bool operator==(Color a, Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
inline bool operator!=(Color a, Color b) { return !(a == b); }

// ---------------------------------------------------------------------------
// Enumerations shared across the API
// ---------------------------------------------------------------------------

// How a pattern is pinned while its object moves. This is about the pattern
// lattice, not about where the shape is:
//   Local  - the pattern rides the object: it translates AND rotates with it,
//            so a dithered blade keeps its texture through a swing.
//   Global - the pattern translates with the object but stays rotation locked
//            to the canvas axes, so hatching keeps pointing the same way.
//   Fixed  - the pattern is nailed to the canvas: parent motion never moves it,
//            which is what a background texture or a screen-door effect wants.
enum class PatternAnchor : uint8_t {
    Local,
    Global,
    Fixed,
};

// What drives the value a dither resolves against. Constant is a flat density;
// the others make the value vary across the fill, which is what produces a
// dithered gradient.
enum class DitherModulation : uint8_t {
    Constant,
    Linear,
    Radial,
    Angular,
};

// The prebaked dither matrices. Every one of these is a full threshold ranking,
// not a 1-bit stamp, so each works at any density and inside a gradient.
enum class DitherPatternKind : uint8_t {
    Bayer2,
    Bayer4,
    Bayer8,
    Checker,
    HorizontalLines,
    VerticalLines,
    DiagonalLines,
    CrossHatch,
    Dots,
    ClusteredDot,
    Noise,
    Grid,
};

enum class CoordinateSpace : uint8_t {
    Object,   // relative to the geometry local origin (stable under motion)
    Sprite,   // relative to the sprite bounding box
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
    Majority,   // color covering more than half the pixel wins
    Median,     // median of covered sub-samples
    Average,    // average of sub-samples
    Threshold,  // binary: covered or not
};

enum class AlphaPolicy : uint8_t {
    Preserve,        // pass through alpha as-is
    Threshold,       // alpha below threshold becomes 0, else 255
    Premultiply,     // premultiply output alpha
    ForceOpaque,     // set all non-empty alpha to 255
};

enum class PalettePolicy : uint8_t {
    NearestMatch,    // snap to nearest palette color
    ExactMatch,      // error if not in palette
    Unconstrained,   // output any color (ignore palette)
};

enum class LayerType : uint8_t {
    Drawing,     // standard drawing layer (app label only)
    Generated,   // app hint: content is generated
    Source,      // app hint: content is a source reference
    Export,      // app hint: this is an export group root
};

// Compositing compile profile types
enum class CompileProfileType : uint8_t {
    Preview,    // fast, lower quality, may skip expensive ops
    Export,     // full quality, all ops resolved
    Debug,      // full quality plus a populated compile trace
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
    uint32_t             width  = 0;
    uint32_t             height = 0;
    uint32_t             stride = 0;   // bytes per row
    std::vector<uint8_t> pixels;       // RGBA8, row-major

    bool empty() const { return pixels.empty(); }
    const uint8_t* row(uint32_t y) const { return pixels.data() + y * stride; }
    uint8_t*       row(uint32_t y)       { return pixels.data() + y * stride; }
};

// Build an empty (fully transparent) raster. Apps use this to hand the engine
// pixels: an imported texture tile, a traced source image, a mask.
inline RasterBuffer makeRaster(uint32_t width, uint32_t height) {
    RasterBuffer raster;
    if (width == 0 || height == 0) {
        return raster;
    }
    // An empty raster is the only safe answer to a size this cannot represent.
    // Returning one that reports a width while holding no storage would leave
    // writePixel's bounds check passing on a buffer that is not there.
    if (width > kMaxCanvasDimension || height > kMaxCanvasDimension ||
        static_cast<uint64_t>(width) * height > kMaxCanvasPixels) {
        return raster;
    }
    raster.width = width;
    raster.height = height;
    raster.stride = width * 4;
    raster.pixels.assign(static_cast<size_t>(raster.stride) * height, 0);
    return raster;
}

// Pixel access on a raster buffer. Bounds-checked: out-of-range writes are
// dropped and out-of-range reads return transparent. Plugin resolvers use these
// to fill the buffer the engine hands them.
inline void writePixel(RasterBuffer& raster, int32_t x, int32_t y, Color color) {
    if (x < 0 || y < 0 ||
        x >= static_cast<int32_t>(raster.width) ||
        y >= static_cast<int32_t>(raster.height)) {
        return;
    }
    uint8_t* px = raster.row(static_cast<uint32_t>(y)) + static_cast<uint32_t>(x) * 4;
    px[0] = color.r;
    px[1] = color.g;
    px[2] = color.b;
    px[3] = color.a;
}

inline Color readPixel(const RasterBuffer& raster, int32_t x, int32_t y) {
    if (x < 0 || y < 0 ||
        x >= static_cast<int32_t>(raster.width) ||
        y >= static_cast<int32_t>(raster.height)) {
        return Color::transparent();
    }
    const uint8_t* px = raster.row(static_cast<uint32_t>(y)) + static_cast<uint32_t>(x) * 4;
    return { px[0], px[1], px[2], px[3] };
}

// ---------------------------------------------------------------------------
// Compile profile
// ---------------------------------------------------------------------------
struct CompileProfile {
    uint32_t            outputWidth        = 0;   // 0 = use the document canvas size
    uint32_t            outputHeight       = 0;
    CompileProfileType  type               = CompileProfileType::Preview;
    SamplingPolicy      sampling           = SamplingPolicy::Coverage;
    RoundingPolicy      rounding           = RoundingPolicy::Nearest;
    AlphaPolicy         alpha              = AlphaPolicy::Threshold;
    PalettePolicy       palette            = PalettePolicy::NearestMatch;
    float               coverageThreshold  = 0.5f;
    float               alphaThreshold     = 0.5f;
    bool                resolveTransforms  = true;
    uint32_t            engineVersion      = LS_ENGINE_VERSION;

    // Where this output frame sits inside the canvas. Only patterns in Export
    // space read it: it is what lets a sprite packed into a sheet cell keep its
    // pattern locked to the cell rather than to the canvas.
    Vec2i               exportOrigin;

    // Optional: type id of a compile policy registered through
    // registerCompilePolicy(). When set, that policy chooses the output colour
    // from the sub-samples a transform gathers, in place of `sampling`.
    std::string         samplingPolicyId;
};

// Compiled result from the engine
struct CompileResult {
    RasterBuffer             raster;
    Rect2i                   bounds;   // tight pixel bounds of non-transparent content
    std::vector<std::string> trace;    // populated for CompileProfileType::Debug
    LSError                  error = LSError::None;
    bool ok() const { return error == LSError::None; }
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
