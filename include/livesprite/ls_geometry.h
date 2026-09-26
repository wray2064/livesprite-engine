// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors
#pragma once
// ls_geometry.h — Geometry primitives, region math, and boolean operations
// LiveSprite Engine

#include "ls_types.h"

#include <vector>

namespace ls {

class LSContext;

// ---------------------------------------------------------------------------
// Geometry primitive descriptors
// Value types used to CREATE geometry objects. Once created, geometry is
// referenced by GeometryId and owned by the document.
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
    Vec2f    origin;               // top-left corner
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
    // The pixels the edges pass through count as inside too, every corner
    // included -- a polygon as a pixel artist draws one through the pixels
    // clicked. Off, a pixel is inside when its centre is (top-left rule), so
    // corners on pixel centres lose the right and bottom edges.
    bool               includeEdges = false;
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
// Authored pixel input — the substrate behind hand-drawn strokes and the
// bucket-fill workflow. Same-color closed loops seal their interior.
// ---------------------------------------------------------------------------
struct PixelInput {
    Vec2i position;
    Color color;
};

struct PixelRegionDesc {
    std::vector<PixelInput> pixels;
    bool closeSameColorBoundaries = true;
};

// ---------------------------------------------------------------------------
// Interval: a horizontal span [x0, x1) at row y — the rasterization primitive
// ---------------------------------------------------------------------------
struct Interval {
    int32_t y  = 0;
    int32_t x0 = 0;
    int32_t x1 = 0;  // exclusive
};

inline bool operator==(const Interval& a, const Interval& b) {
    return a.y == b.y && a.x0 == b.x0 && a.x1 == b.x1;
}

// IntervalSet: a complete region expressed as sorted horizontal intervals.
// This is the primary internal region representation. Canonical form:
// sorted by (y, x0), no empty spans, no overlapping or abutting spans.
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
// Region inset/outset parameters
// ---------------------------------------------------------------------------
struct InsetOutsetParams {
    float   amount = 0.f;         // pixels (always positive; direction is the call)
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
// Simplify parameters
// ---------------------------------------------------------------------------
struct SimplifyParams {
    float   epsilon         = 1.f;   // Ramer-Douglas-Peucker tolerance in pixels
    bool    preserveCorners = true;
    float   cornerAngle     = 45.f;  // degrees — sharper than this is a kept corner
};

// ---------------------------------------------------------------------------
// Free geometry math. These are pure functions over value types: no engine
// state, no IDs. LSContext builds on them.
// ---------------------------------------------------------------------------
namespace geom {

// --- IntervalSet canonical form -------------------------------------------
IntervalSet normalize(IntervalSet set);
bool        isNormalized(const IntervalSet& set);
int64_t     pixelCount(const IntervalSet& set);
bool        contains(const IntervalSet& set, Vec2i point);
Rect2i      bounds(const IntervalSet& set);
Vec2f       centroid(const IntervalSet& set);

// --- Boolean ops (inputs must be normalized; outputs are normalized) -------
IntervalSet booleanOp(const IntervalSet& a, const IntervalSet& b, RegionBoolOp op);
IntervalSet unionSets(const IntervalSet& a, const IntervalSet& b);
IntervalSet subtractSets(const IntervalSet& a, const IntervalSet& b);
IntervalSet intersectSets(const IntervalSet& a, const IntervalSet& b);
IntervalSet xorSets(const IntervalSet& a, const IntervalSet& b);
IntervalSet invertSet(const IntervalSet& a, Rect2i within);

// --- Morphology -----------------------------------------------------------
IntervalSet expand(const IntervalSet& set, float pixels, bool preserveCorners = true);
IntervalSet contract(const IntervalSet& set, float pixels, bool preserveCorners = true);

// --- Topology -------------------------------------------------------------
std::vector<IntervalSet> connectedComponents(const IntervalSet& set, bool eightConnected = true);
IntervalSet boundaryOf(const IntervalSet& set);          // inner boundary pixels
IntervalSet outerBoundaryOf(const IntervalSet& set);     // pixels just outside

// --- Rasterization of primitives into interval sets -----------------------
IntervalSet rasterizePoint(const PointDesc& desc);
IntervalSet rasterizeLine(const LineDesc& desc);
IntervalSet rasterizePolyline(const PolylineDesc& desc);
IntervalSet rasterizeRect(const RectDesc& desc);
IntervalSet rasterizeEllipse(const EllipseDesc& desc);
IntervalSet rasterizeCircle(const CircleDesc& desc);
IntervalSet rasterizePolygon(const PolygonDesc& desc);
IntervalSet rasterizeCurve(const CurveDesc& desc);

// --- Through a transform --------------------------------------------------
// Pixels come last. A shape that moves is moved as a shape and rasterized
// where it lands, so a turned line is drawn as a line at its new angle -- one
// pixel wide and joined up -- and a turned area is the pixels its turned edge
// encloses, rather than a picture of either, turned and resampled.

// True when the matrix carries every pixel onto exactly one pixel: quarter
// turns, mirrors and whole-pixel moves. Such a move loses nothing, so the
// shape's own pixels are carried across (mapAcrossGrid) rather than redrawn.
bool keepsPixelGrid(const Mat3f& matrix);
IntervalSet mapAcrossGrid(const IntervalSet& set, const Mat3f& matrix);

// A line one pixel wide through the points, the way a pixel artist draws one:
// the pixels in the order the path visits them, without the doubled L corners
// a stair of short segments leaves. Closed, the last point joins the first.
IntervalSet rasterizePixelWalk(const std::vector<Vec2f>& points, bool closed);

// The pixels inside an outline. By centres: a pixel whose centre is inside
// (the polygon rule). By spans: a pixel any part of whose row centre line is
// inside (the ellipse rule, which keeps small round shapes round).
IntervalSet rasterizeArea(const std::vector<Vec2f>& outline, bool bySpans = false);

// --- Contours -------------------------------------------------------------
std::vector<Vec2f> flattenCurve(const CurveDesc& desc, float tolerance = 0.25f);
std::vector<Vec2f> simplifyPath(const std::vector<Vec2f>& points, const SimplifyParams& params);
std::vector<ContourDesc> traceContours(const IntervalSet& set, bool includeHoles = true);

// --- Authored pixels ------------------------------------------------------
struct PixelRegionResult {
    IntervalSet coverage;   // authored pixels plus any sealed interior
    IntervalSet boundary;   // authored pixels that formed a closed loop
};
PixelRegionResult buildPixelRegion(const PixelRegionDesc& desc);

// --- Raster helpers -------------------------------------------------------
IntervalSet maskToIntervals(const RasterBuffer& raster, float alphaThreshold);

} // namespace geom
} // namespace ls
