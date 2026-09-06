#pragma once
// ls_geometry.h — Geometry primitives, region math, and boolean operations
// LiveSprite Engine

#include "ls_types.h"
#include <vector>
#include <span>

namespace ls {

// ---------------------------------------------------------------------------
// Forward declaration
// ---------------------------------------------------------------------------
class LSContext;

// ---------------------------------------------------------------------------
// Geometry primitive descriptors
// These are value types used to CREATE geometry objects in the engine.
// Once created, geometry is referenced by GeometryId.
// ---------------------------------------------------------------------------

struct PointDesc {
    Vec2f position;
};

struct LineDesc {
    Vec2f start, end;
};

struct PolylineDesc {
    std::vector<Vec2f> points;
    bool               closed = false;
};

struct RectDesc {
    Vec2f    origin;           // top-left corner
    float    width  = 0.f;
    float    height = 0.f;
    float    cornerRadius = 0.f;
};

struct EllipseDesc {
    Vec2f    center;
    float    radiusX = 0.f;
    float    radiusY = 0.f;
};

struct CircleDesc {
    Vec2f    center;
    float    radius = 0.f;
};

struct PolygonDesc {
    std::vector<Vec2f> vertices;
};

// Cubic Bezier curve
struct CurveDesc {
    struct Segment {
        Vec2f p0, cp0, cp1, p1;
    };
    std::vector<Segment> segments;
    bool                 closed = false;
};

// ---------------------------------------------------------------------------
// Interval: a horizontal span [x0, x1) at row y — the rasterization primitive
// ---------------------------------------------------------------------------
struct Interval {
    int32_t y  = 0;
    int32_t x0 = 0;
    int32_t x1 = 0;  // exclusive
};

// IntervalSet: a complete region expressed as sorted horizontal intervals
// This is the engine's primary internal region representation.
struct IntervalSet {
    std::vector<Interval> intervals;  // sorted by y then x0

    bool empty() const { return intervals.empty(); }
    void clear()       { intervals.clear(); }
};

// Contour: a closed polygon path (used for boolean ops before rasterization)
struct ContourDesc {
    std::vector<Vec2f> points;
    bool               outer = true;   // true = outer boundary, false = hole
};

// ---------------------------------------------------------------------------
// Geometry query results
// ---------------------------------------------------------------------------
struct GeometryBounds {
    Rect2f  floatBounds;
    Rect2i  pixelBounds;
    Vec2f   centroid;
    float   area = 0.f;
};

// ---------------------------------------------------------------------------
// Region boolean operation types
// ---------------------------------------------------------------------------
enum class RegionBoolOp : uint8_t {
    Union,
    Subtract,
    Intersect,
    Xor,
};

// ---------------------------------------------------------------------------
// Geometry API (methods on LSContext — declared here, defined in ls_api.h)
// Geometry objects are created, stored, and referenced by GeometryId.
// ---------------------------------------------------------------------------

// All geometry creation functions live on LSContext.
// See ls_api.h for the full context class.
// This header defines the descriptor types used as arguments.

// ---------------------------------------------------------------------------
// Region inset/outset parameters
// ---------------------------------------------------------------------------
struct InsetOutsetParams {
    float   amount = 0.f;         // pixels (negative = inset, positive = outset)
    bool    preserveCorners = true;
    float   miterLimit = 2.f;
};

// ---------------------------------------------------------------------------
// Trace boundary parameters
// ---------------------------------------------------------------------------
struct TraceBoundaryParams {
    float   threshold       = 0.5f;  // alpha threshold for boundary detection
    bool    includeHoles    = true;
    float   simplifyEpsilon = 0.f;   // 0 = no simplification
};

// ---------------------------------------------------------------------------
// Simplify contour parameters
// ---------------------------------------------------------------------------
struct SimplifyParams {
    float   epsilon         = 1.f;   // Ramer-Douglas-Peucker tolerance in pixels
    bool    preserveCorners = true;
    float   cornerAngle     = 45.f;  // degrees — sharper than this = preserved corner
};

} // namespace ls
