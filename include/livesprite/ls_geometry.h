// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors
#pragma once
// ls_geometry.h — Geometry primitives, region math, and boolean operations
// LiveSprite Engine

#include "ls_types.h"

#include <functional>

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
// Freehand marks, areas and fills -- as shapes.
//
// What a pencil, a spray, an eraser and a paint bucket make, kept as what the
// hand did rather than the pixels it left. Where they were made they draw
// exactly the pixels that were drawn; moved -- turned, scaled, mirrored --
// they are drawn again where they land, so a turned pencil line is a pencil
// line at the new angle rather than a picture of one, turned.
// ---------------------------------------------------------------------------

enum class PenKind : uint8_t {
    Line,   // the points walked in order, one pixel wide or with the brush stamped along
    Dots,   // each point on its own, unjoined: a spray
    Area,   // an area laid down whole (see AreaDesc): a lasso fill, a selection filled
};

struct AreaDesc;

// An area, exactly: the edges of its pixels as closed contours, filled
// even-odd, so a hole is a contour too. What a set of pixels becomes when it
// is to be a shape -- it draws those pixels where it is, and moves as one.
struct AreaDesc {
    std::vector<std::vector<Vec2f>> contours;
};

// One mark of a brush: the path the pointer took and the brush, or an area
// laid down whole.
struct PenStroke {
    std::vector<Vec2f> points;      // pixel centres, in the order drawn
    std::vector<float> sizes;       // the brush size at each point, when a pen's
                                    // pressure set it; empty for a steady brush
    float   size = 1.f;             // pixels across
    bool    round = false;          // round from 3 across up, square otherwise
    bool    pixelPerfect = true;    // walked again, a one-pixel line drops its L corners
    // Takes its pixels from the marks before it -- but never from a line one
    // pixel wide: those are cut where they are erased (cutStrokes), since a
    // mask redrawn at an angle could clip a pixel of a line it never touched.
    bool    erase = false;
    PenKind kind = PenKind::Line;
    AreaDesc area;                  // the area, for PenKind::Area
    // A custom brush: its own shape, stamped at every pixel the path walks
    // (or at each dot) in place of the size-and-round brush, and turned with
    // the stroke when it is turned. Pixel edges, the top-left corner of the
    // pixel it is stamped on at 0,0. Empty for the ordinary brush.
    AreaDesc tip;
};

// Freehand marks in the order they were made.
struct StrokesDesc {
    std::vector<PenStroke> strokes;
};

// A fill that finds its own edge -- what a paint bucket makes. Where it was
// made it is exactly `area`, the pixels its flood found. Moved, it is found
// again: a flood from the seed where it lands, over what its layer has drawn
// before it, kept within two pixels of the area moved -- so it meets the line
// that bounds it however that line was redrawn, with no gap and no leak. If
// nothing on the layer bounds it there, the area moved is the fill.
// How one term of a region's clip bears on it (see LSContext::setRegionClip).
enum class ClipOp : uint8_t {
    Add,        // where the term draws, the region may
    Remove,     // where the term draws, it may not
    Within,     // outside what the term draws, it may not
};

// One term of a region's clip: what a region draws, or what a shape covers.
// Exactly one of the two is set.
struct RegionClipTerm {
    RegionId   region;
    GeometryId geometry;
    ClipOp     op = ClipOp::Add;
};

inline bool operator==(const RegionClipTerm& a, const RegionClipTerm& b) {
    return a.region == b.region && a.geometry == b.geometry && a.op == b.op;
}
inline bool operator!=(const RegionClipTerm& a, const RegionClipTerm& b) { return !(a == b); }

struct FaceDesc {
    Vec2f    seed;                  // deep inside, in the layer's own space
    AreaDesc area;
    int32_t  tolerance = 0;         // how far a colour may differ and still be flooded
    bool     diagonal = false;      // diagonal neighbours count as connected
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

// A move of the plane that is not one matrix -- a bend, a warp, a lattice:
// where each point goes. Shapes moved by one are cut into steps no longer
// than half a pixel first (densifyPath), so a bent line bends rather than
// jumping from corner to corner.
using PointMap = std::function<Vec2f(Vec2f)>;
std::vector<Vec2f> densifyPath(const std::vector<Vec2f>& points, bool closed, float step = 0.5f);

// A line one pixel wide through the points, the way a pixel artist draws one:
// the pixels in the order the path visits them, without the doubled L corners
// a stair of short segments leaves. Closed, the last point joins the first.
IntervalSet rasterizePixelWalk(const std::vector<Vec2f>& points, bool closed);

// The pixels inside an outline. By centres: a pixel whose centre is inside
// (the polygon rule). By spans: a pixel any part of whose row centre line is
// inside (the ellipse rule, which keeps small round shapes round).
IntervalSet rasterizeArea(const std::vector<Vec2f>& outline, bool bySpans = false);

// --- Freehand, areas, fills ---------------------------------------------------
// The pixels a brush stamp covers, as offsets from the pixel it is centred on.
// Odd sizes centre on the pixel; even ones hang right and down from it, so the
// pointer's pixel is inside the stamp at every size.
std::vector<Vec2i> brushFootprint(int size, bool round);
IntervalSet rasterizeStrokes(const StrokesDesc& desc);
IntervalSet rasterizeStrokesThrough(const StrokesDesc& desc, const Mat3f& matrix);
IntervalSet rasterizeStrokesAlong(const StrokesDesc& desc, const PointMap& map);
IntervalSet rasterizeAreaDesc(const AreaDesc& desc);
IntervalSet rasterizeAreaThrough(const AreaDesc& desc, const Mat3f& matrix);
IntervalSet rasterizeAreaAlong(const AreaDesc& desc, const PointMap& map);
// The contours of a set of pixels, along their edges.
AreaDesc traceArea(const IntervalSet& set);
// The centre of the pixel furthest inside a set: a seed that stays inside
// whatever the set is turned or scaled to.
Vec2f deepestPoint(const IntervalSet& set);
// Takes `erased` out of the strokes: a one-pixel line loses exactly those
// pixels and is split where it lost them, a spray loses those dots, and an
// area loses them from its edge -- so nothing is left to come back when they
// are drawn somewhere else. A wider stroke cannot lose part of its width that
// way; true when one lay under the pixels, for the caller to add an erasing
// mark over it.
bool cutStrokes(StrokesDesc& desc, const IntervalSet& erased);
// A flood over a picture from `seed`: the connected pixels whose colour is
// within `tolerance` of the seed's. Kept inside `within` when it is given.
IntervalSet floodRaster(const RasterBuffer& raster, Vec2i seed, int32_t tolerance, bool diagonal,
                        const IntervalSet* within = nullptr);

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
