// ls_geometry.cpp — Geometry primitive implementations and region math
// LiveSprite Engine
//
// Implements:
//   - Geometry object storage (Point, Line, Polyline, Rect, Ellipse, Circle, Polygon, Curve)
//   - IntervalSet construction from geometry
//   - Region boolean operations (Union, Subtract, Intersect, Xor, Clip, Mask, Invert)
//   - Region utilities (Expand, Contract, Inset, Outset, Simplify, ConnectedComponents)
//   - Mat3f operations
//   - GeometryBounds queries
//
// Key implementation notes:
//   - Regions are stored as sorted IntervalSets (horizontal scanlines)
//   - Boolean ops are performed on IntervalSets using interval merge/diff algorithms
//   - Geometry-to-IntervalSet conversion uses scanline rasterization
//   - All coordinate math is in float; final rasterization applies RoundingPolicy

#include <livesprite/ls_api.h>
#include <livesprite/ls_geometry.h>
#include <unordered_map>
#include <algorithm>
#include <cmath>

namespace ls {

// ---------------------------------------------------------------------------
// Mat3f operations
// ---------------------------------------------------------------------------

Vec2f Mat3f::transformPoint(Vec2f p) const {
    float x = m[0]*p.x + m[1]*p.y + m[2];
    float y = m[3]*p.x + m[4]*p.y + m[5];
    float w = m[6]*p.x + m[7]*p.y + m[8];
    if (w != 0.f && w != 1.f) { x /= w; y /= w; }
    return {x, y};
}

Vec2f Mat3f::transformVector(Vec2f v) const {
    return { m[0]*v.x + m[1]*v.y, m[3]*v.x + m[4]*v.y };
}

Mat3f Mat3f::mul(const Mat3f& o) const {
    Mat3f r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i*3+j] = m[i*3+0]*o.m[0*3+j] + m[i*3+1]*o.m[1*3+j] + m[i*3+2]*o.m[2*3+j];
    return r;
}

// ---------------------------------------------------------------------------
// Internal geometry storage (lives inside LSContext::Impl via a GeometryStore)
// TODO: move this into LSContext::Impl properly in ls_context.cpp
// ---------------------------------------------------------------------------

// Placeholder internal geometry union (expand as needed)
struct GeometryObject {
    enum class Kind { Point, Line, Polyline, Rect, Ellipse, Circle, Polygon, Curve } kind;
    PointDesc   point;
    LineDesc    line;
    PolylineDesc polyline;
    RectDesc    rect;
    EllipseDesc ellipse;
    CircleDesc  circle;
    PolygonDesc polygon;
    CurveDesc   curve;
};

// ---------------------------------------------------------------------------
// IntervalSet utilities
// ---------------------------------------------------------------------------

// Merge overlapping intervals within the same row
static void normalizeIntervalSet(IntervalSet& s) {
    if (s.intervals.empty()) return;
    std::sort(s.intervals.begin(), s.intervals.end(), [](const Interval& a, const Interval& b){
        return a.y < b.y || (a.y == b.y && a.x0 < b.x0);
    });
    std::vector<Interval> out;
    out.reserve(s.intervals.size());
    out.push_back(s.intervals[0]);
    for (size_t i = 1; i < s.intervals.size(); ++i) {
        Interval& last = out.back();
        const Interval& cur = s.intervals[i];
        if (cur.y == last.y && cur.x0 <= last.x1) {
            last.x1 = std::max(last.x1, cur.x1);
        } else {
            out.push_back(cur);
        }
    }
    s.intervals = std::move(out);
}

// Union of two normalized IntervalSets
static IntervalSet intervalUnion(const IntervalSet& a, const IntervalSet& b) {
    IntervalSet result;
    result.intervals.reserve(a.intervals.size() + b.intervals.size());
    for (auto& iv : a.intervals) result.intervals.push_back(iv);
    for (auto& iv : b.intervals) result.intervals.push_back(iv);
    normalizeIntervalSet(result);
    return result;
}

// Subtract b from a (a - b)
static IntervalSet intervalSubtract(const IntervalSet& a, const IntervalSet& b) {
    IntervalSet result;
    // TODO: implement proper interval subtraction per-row
    // For each row in a: subtract all intervals in b on that row
    return result;
}

// Intersect two normalized IntervalSets
static IntervalSet intervalIntersect(const IntervalSet& a, const IntervalSet& b) {
    IntervalSet result;
    // TODO: implement row-aligned interval intersection
    return result;
}

// Scanline rasterize a polygon into an IntervalSet
static IntervalSet rasterizePolygon(const std::vector<Vec2f>& verts, Rect2i bounds) {
    IntervalSet result;
    if (verts.size() < 3) return result;
    // TODO: implement scanline fill (edge table approach)
    //   For each scanline y in bounds:
    //     Find intersections of edges with y + 0.5
    //     Sort intersections by x
    //     Pair up and emit Intervals (even-odd or non-zero winding)
    return result;
}

// Scanline rasterize an ellipse
static IntervalSet rasterizeEllipse(Vec2f center, float rx, float ry) {
    IntervalSet result;
    // TODO: implement midpoint ellipse scanline fill
    return result;
}

// Scanline rasterize a rectangle
static IntervalSet rasterizeRect(const RectDesc& r) {
    IntervalSet result;
    int32_t x0 = (int32_t)std::floor(r.origin.x);
    int32_t y0 = (int32_t)std::floor(r.origin.y);
    int32_t x1 = (int32_t)std::ceil(r.origin.x + r.width);
    int32_t y1 = (int32_t)std::ceil(r.origin.y + r.height);
    for (int32_t y = y0; y < y1; ++y) {
        result.intervals.push_back({ y, x0, x1 });
    }
    return result;
}

// ---------------------------------------------------------------------------
// LSContext geometry creation methods
// ---------------------------------------------------------------------------

Result<GeometryId> LSContext::createRect(DocumentId doc, const RectDesc& desc) {
    if (!impl_->documents.count(doc)) return Result<GeometryId>::err(LSError::InvalidId);
    // TODO: store geometry object, return ID
    return Result<GeometryId>::err(LSError::NotImplemented);
}

Result<GeometryId> LSContext::createEllipse(DocumentId doc, const EllipseDesc& desc) {
    return Result<GeometryId>::err(LSError::NotImplemented);
}

Result<GeometryId> LSContext::createCircle(DocumentId doc, const CircleDesc& desc) {
    return Result<GeometryId>::err(LSError::NotImplemented);
}

Result<GeometryId> LSContext::createPolygon(DocumentId doc, const PolygonDesc& desc) {
    return Result<GeometryId>::err(LSError::NotImplemented);
}

Result<GeometryId> LSContext::createPolyline(DocumentId doc, const PolylineDesc& desc) {
    return Result<GeometryId>::err(LSError::NotImplemented);
}

Result<GeometryId> LSContext::createCurve(DocumentId doc, const CurveDesc& desc) {
    return Result<GeometryId>::err(LSError::NotImplemented);
}

Result<GeometryId> LSContext::createPoint(DocumentId doc, const PointDesc& desc) {
    return Result<GeometryId>::err(LSError::NotImplemented);
}

Result<GeometryId> LSContext::createLine(DocumentId doc, const LineDesc& desc) {
    return Result<GeometryId>::err(LSError::NotImplemented);
}

VoidResult LSContext::deleteGeometry(GeometryId id) {
    // TODO: remove from geometry store, propagate dirty
    return VoidResult::err(LSError::NotImplemented);
}

// ---------------------------------------------------------------------------
// LSContext region operations
// ---------------------------------------------------------------------------

Result<RegionId> LSContext::createRegionFromGeometry(GeometryId geom) {
    // TODO: rasterize geometry → IntervalSet → store as region
    return Result<RegionId>::err(LSError::NotImplemented);
}

Result<RegionId> LSContext::createRegionFromIntervals(const IntervalSet& intervals) {
    RegionId id{ impl_->allocId() };
    IntervalSet normalized = intervals;
    normalizeIntervalSet(normalized);
    impl_->regionCache[id] = std::move(normalized);
    return Result<RegionId>::ok(id);
}

Result<RegionId> LSContext::unionRegions(RegionId a, RegionId b) {
    auto ia = impl_->regionCache.find(a);
    auto ib = impl_->regionCache.find(b);
    if (ia == impl_->regionCache.end() || ib == impl_->regionCache.end())
        return Result<RegionId>::err(LSError::InvalidId);
    return createRegionFromIntervals(intervalUnion(ia->second, ib->second));
}

Result<RegionId> LSContext::subtractRegions(RegionId a, RegionId b) {
    auto ia = impl_->regionCache.find(a);
    auto ib = impl_->regionCache.find(b);
    if (ia == impl_->regionCache.end() || ib == impl_->regionCache.end())
        return Result<RegionId>::err(LSError::InvalidId);
    return createRegionFromIntervals(intervalSubtract(ia->second, ib->second));
}

Result<RegionId> LSContext::intersectRegions(RegionId a, RegionId b) {
    auto ia = impl_->regionCache.find(a);
    auto ib = impl_->regionCache.find(b);
    if (ia == impl_->regionCache.end() || ib == impl_->regionCache.end())
        return Result<RegionId>::err(LSError::InvalidId);
    return createRegionFromIntervals(intervalIntersect(ia->second, ib->second));
}

Result<IntervalSet> LSContext::getRegionIntervals(RegionId r) const {
    auto it = impl_->regionCache.find(r);
    if (it == impl_->regionCache.end()) return Result<IntervalSet>::err(LSError::InvalidId);
    return Result<IntervalSet>::ok(it->second);
}

Result<GeometryBounds> LSContext::getRegionBounds(RegionId r) const {
    auto it = impl_->regionCache.find(r);
    if (it == impl_->regionCache.end()) return Result<GeometryBounds>::err(LSError::InvalidId);
    const IntervalSet& s = it->second;
    if (s.empty()) return Result<GeometryBounds>::err(LSError::RegionEmpty);

    int32_t minX = INT32_MAX, maxX = INT32_MIN, minY = INT32_MAX, maxY = INT32_MIN;
    int64_t areaSum = 0;
    for (const Interval& iv : s.intervals) {
        minX = std::min(minX, iv.x0);
        maxX = std::max(maxX, iv.x1);
        minY = std::min(minY, iv.y);
        maxY = std::max(maxY, iv.y + 1);
        areaSum += (iv.x1 - iv.x0);
    }
    GeometryBounds b;
    b.pixelBounds = { {minX, minY}, {maxX, maxY} };
    b.floatBounds = { {(float)minX, (float)minY}, {(float)maxX, (float)maxY} };
    b.area        = (float)areaSum;
    b.centroid    = { (minX + maxX) * 0.5f, (minY + maxY) * 0.5f }; // TODO: proper centroid
    return Result<GeometryBounds>::ok(b);
}

} // namespace ls
