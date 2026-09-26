// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors
// ls_geometry.cpp — geometry math for the LiveSprite Engine.
//
// Everything here is a pure function over value types. No engine state, no IDs,
// no allocation of entities. LSContext builds regions on top of these.
//
// The canonical region form is an IntervalSet: horizontal half-open spans
// [x0, x1) at row y, sorted by (y, x0), non-overlapping and non-abutting.

#include "livesprite/ls_geometry.h"
#include "ls_math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace ls {
namespace geom {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// Pixel key packing: two int32 into one uint64 for hashed pixel sets.
inline uint64_t pixelKey(int32_t x, int32_t y) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
            static_cast<uint64_t>(static_cast<uint32_t>(y));
}
inline int32_t keyX(uint64_t key) { return static_cast<int32_t>(key >> 32); }
inline int32_t keyY(uint64_t key) { return static_cast<int32_t>(key & 0xffffffffu); }

using PixelSet = std::unordered_set<uint64_t>;

PixelSet toPixelSet(const IntervalSet& set) {
    PixelSet pixels;
    for (const Interval& interval : set.intervals) {
        for (int32_t x = interval.x0; x < interval.x1; ++x) {
            pixels.insert(pixelKey(x, interval.y));
        }
    }
    return pixels;
}

IntervalSet fromPixelSet(const PixelSet& pixels) {
    std::map<int32_t, std::vector<int32_t>> rows;
    for (uint64_t key : pixels) {
        rows[keyY(key)].push_back(keyX(key));
    }

    IntervalSet set;
    for (auto& [y, xs] : rows) {
        std::sort(xs.begin(), xs.end());
        int32_t runStart = xs.front();
        int32_t previous = xs.front();
        for (size_t i = 1; i < xs.size(); ++i) {
            if (xs[i] == previous + 1) {
                previous = xs[i];
                continue;
            }
            set.intervals.push_back({y, runStart, previous + 1});
            runStart = xs[i];
            previous = xs[i];
        }
        set.intervals.push_back({y, runStart, previous + 1});
    }
    return set;
}

inline bool pixelIn(const PixelSet& pixels, int32_t x, int32_t y) {
    return pixels.find(pixelKey(x, y)) != pixels.end();
}

// Row view of a normalized interval set: y -> sorted spans.
using Span  = std::pair<int32_t, int32_t>;
using Rows  = std::map<int32_t, std::vector<Span>>;

Rows toRows(const IntervalSet& set) {
    Rows rows;
    for (const Interval& interval : set.intervals) {
        if (interval.x1 > interval.x0) {
            rows[interval.y].emplace_back(interval.x0, interval.x1);
        }
    }
    for (auto& [y, spans] : rows) {
        (void)y;
        std::sort(spans.begin(), spans.end());
    }
    return rows;
}

bool spansContain(const std::vector<Span>& spans, int32_t x) {
    // spans sorted, non-overlapping
    size_t lo = 0, hi = spans.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (x < spans[mid].first) {
            hi = mid;
        } else if (x >= spans[mid].second) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

std::vector<Span> combineRow(const std::vector<Span>& a,
                             const std::vector<Span>& b,
                             RegionBoolOp op) {
    std::vector<int32_t> cuts;
    cuts.reserve((a.size() + b.size()) * 2);
    for (const Span& s : a) { cuts.push_back(s.first); cuts.push_back(s.second); }
    for (const Span& s : b) { cuts.push_back(s.first); cuts.push_back(s.second); }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    std::vector<Span> out;
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        const int32_t x0 = cuts[i];
        const int32_t x1 = cuts[i + 1];
        if (x1 <= x0) {
            continue;
        }
        const bool inA = spansContain(a, x0);
        const bool inB = spansContain(b, x0);
        bool keep = false;
        switch (op) {
            case RegionBoolOp::Union:     keep = inA || inB;   break;
            case RegionBoolOp::Subtract:  keep = inA && !inB;  break;
            case RegionBoolOp::Intersect: keep = inA && inB;   break;
            case RegionBoolOp::Xor:       keep = inA != inB;   break;
        }
        if (!keep) {
            continue;
        }
        if (!out.empty() && out.back().second == x0) {
            out.back().second = x1;
        } else {
            out.emplace_back(x0, x1);
        }
    }
    return out;
}

// Scanline fill of a closed polygon using even-odd parity at pixel centers.
IntervalSet fillPolygon(const std::vector<Vec2f>& points) {
    IntervalSet set;
    if (points.size() < 3) {
        return set;
    }

    float minY = points[0].y, maxY = points[0].y;
    for (const Vec2f& p : points) {
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }

    const int32_t y0 = static_cast<int32_t>(std::floor(minY));
    const int32_t y1 = static_cast<int32_t>(std::ceil(maxY));

    std::vector<float> crossings;
    for (int32_t y = y0; y < y1; ++y) {
        const float sampleY = static_cast<float>(y) + 0.5f;
        crossings.clear();

        for (size_t i = 0; i < points.size(); ++i) {
            const Vec2f& p = points[i];
            const Vec2f& q = points[(i + 1) % points.size()];
            if (p.y == q.y) {
                continue;
            }
            const float lo = std::min(p.y, q.y);
            const float hi = std::max(p.y, q.y);
            if (sampleY < lo || sampleY >= hi) {
                continue;
            }
            const float t = (sampleY - p.y) / (q.y - p.y);
            crossings.push_back(p.x + t * (q.x - p.x));
        }

        if (crossings.size() < 2) {
            continue;
        }
        std::sort(crossings.begin(), crossings.end());

        for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
            const float spanStart = crossings[i];
            const float spanEnd   = crossings[i + 1];
            // A pixel is inside when its center is inside the span.
            const int32_t px0 = static_cast<int32_t>(std::ceil(spanStart - 0.5f));
            const int32_t px1 = static_cast<int32_t>(std::ceil(spanEnd - 0.5f));
            if (px1 > px0) {
                set.intervals.push_back({y, px0, px1});
            }
        }
    }

    return normalize(std::move(set));
}

void plotLine(PixelSet& pixels, Vec2f a, Vec2f b) {
    // Bresenham over pixel centers.
    int32_t x0 = static_cast<int32_t>(std::floor(a.x));
    int32_t y0 = static_cast<int32_t>(std::floor(a.y));
    const int32_t x1 = static_cast<int32_t>(std::floor(b.x));
    const int32_t y1 = static_cast<int32_t>(std::floor(b.y));

    const int32_t dx =  std::abs(x1 - x0);
    const int32_t dy = -std::abs(y1 - y0);
    const int32_t sx = x0 < x1 ? 1 : -1;
    const int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;

    while (true) {
        pixels.insert(pixelKey(x0, y0));
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

Vec2f bezierPoint(const CurveDesc::Segment& seg, float t) {
    const float u = 1.f - t;
    const float w0 = u * u * u;
    const float w1 = 3.f * u * u * t;
    const float w2 = 3.f * u * t * t;
    const float w3 = t * t * t;
    return {
        w0 * seg.p0.x + w1 * seg.cp0.x + w2 * seg.cp1.x + w3 * seg.p1.x,
        w0 * seg.p0.y + w1 * seg.cp0.y + w2 * seg.cp1.y + w3 * seg.p1.y
    };
}

float distance(Vec2f a, Vec2f b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

float perpendicularDistance(Vec2f p, Vec2f a, Vec2f b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float lengthSq = dx * dx + dy * dy;
    if (lengthSq <= 0.f) {
        return distance(p, a);
    }
    const float t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSq;
    const float clamped = std::max(0.f, std::min(1.f, t));
    const Vec2f proj { a.x + clamped * dx, a.y + clamped * dy };
    return distance(p, proj);
}

void rdp(const std::vector<Vec2f>& points, size_t first, size_t last,
         float epsilon, std::vector<Vec2f>& out) {
    if (last <= first + 1) {
        return;
    }
    float worst = 0.f;
    size_t worstIndex = first;
    for (size_t i = first + 1; i < last; ++i) {
        const float d = perpendicularDistance(points[i], points[first], points[last]);
        if (d > worst) {
            worst = d;
            worstIndex = i;
        }
    }
    if (worst <= epsilon) {
        return;
    }
    rdp(points, first, worstIndex, epsilon, out);
    out.push_back(points[worstIndex]);
    rdp(points, worstIndex, last, epsilon, out);
}

// Does this pixel component contain a cycle? Ported from the pixel-region
// closing rules: a same-color loop seals its interior, an open stroke does not.
bool hasPixelCycle(const PixelSet& pixels) {
    size_t edgeCount = 0;
    for (uint64_t key : pixels) {
        const int32_t x = keyX(key);
        const int32_t y = keyY(key);
        const Vec2i forward[4] = {
            {x + 1, y},
            {x, y + 1},
            {x + 1, y + 1},
            {x - 1, y + 1},
        };
        for (Vec2i next : forward) {
            if (pixelIn(pixels, next.x, next.y)) {
                ++edgeCount;
            }
        }
    }
    return edgeCount >= pixels.size();
}

uint64_t colorKey(Color c) {
    return (static_cast<uint64_t>(c.r) << 24) |
           (static_cast<uint64_t>(c.g) << 16) |
           (static_cast<uint64_t>(c.b) <<  8) |
            static_cast<uint64_t>(c.a);
}

} // namespace

// ---------------------------------------------------------------------------
// Canonical form
// ---------------------------------------------------------------------------

IntervalSet normalize(IntervalSet set) {
    auto& spans = set.intervals;
    spans.erase(std::remove_if(spans.begin(), spans.end(),
                               [](const Interval& i) { return i.x1 <= i.x0; }),
                spans.end());

    std::sort(spans.begin(), spans.end(), [](const Interval& a, const Interval& b) {
        if (a.y != b.y) return a.y < b.y;
        if (a.x0 != b.x0) return a.x0 < b.x0;
        return a.x1 < b.x1;
    });

    std::vector<Interval> merged;
    merged.reserve(spans.size());
    for (const Interval& interval : spans) {
        if (!merged.empty() &&
            merged.back().y == interval.y &&
            interval.x0 <= merged.back().x1) {
            merged.back().x1 = std::max(merged.back().x1, interval.x1);
        } else {
            merged.push_back(interval);
        }
    }

    set.intervals = std::move(merged);
    return set;
}

bool isNormalized(const IntervalSet& set) {
    for (size_t i = 0; i < set.intervals.size(); ++i) {
        const Interval& current = set.intervals[i];
        if (current.x1 <= current.x0) {
            return false;
        }
        if (i == 0) {
            continue;
        }
        const Interval& previous = set.intervals[i - 1];
        if (previous.y > current.y) {
            return false;
        }
        if (previous.y == current.y && current.x0 <= previous.x1) {
            return false;
        }
    }
    return true;
}

int64_t pixelCount(const IntervalSet& set) {
    int64_t count = 0;
    for (const Interval& interval : set.intervals) {
        count += interval.x1 - interval.x0;
    }
    return count;
}

bool contains(const IntervalSet& set, Vec2i point) {
    for (const Interval& interval : set.intervals) {
        if (interval.y != point.y) {
            continue;
        }
        if (point.x >= interval.x0 && point.x < interval.x1) {
            return true;
        }
    }
    return false;
}

Rect2i bounds(const IntervalSet& set) {
    if (set.empty()) {
        return {};
    }
    const Interval& first = set.intervals.front();
    int32_t minX = first.x0, maxX = first.x1;
    int32_t minY = first.y,  maxY = first.y + 1;
    for (const Interval& interval : set.intervals) {
        minX = std::min(minX, interval.x0);
        maxX = std::max(maxX, interval.x1);
        minY = std::min(minY, interval.y);
        maxY = std::max(maxY, interval.y + 1);
    }
    return {{minX, minY}, {maxX, maxY}};
}

Vec2f centroid(const IntervalSet& set) {
    double sumX = 0.0, sumY = 0.0;
    int64_t count = 0;
    for (const Interval& interval : set.intervals) {
        const int32_t width = interval.x1 - interval.x0;
        // sum of (x + 0.5) over the span
        const double spanSum = (static_cast<double>(interval.x0) + static_cast<double>(interval.x1)) * width / 2.0;
        sumX += spanSum;
        sumY += (static_cast<double>(interval.y) + 0.5) * width;
        count += width;
    }
    if (count == 0) {
        return {};
    }
    return { static_cast<float>(sumX / static_cast<double>(count)),
             static_cast<float>(sumY / static_cast<double>(count)) };
}

// ---------------------------------------------------------------------------
// Boolean operations
// ---------------------------------------------------------------------------

IntervalSet booleanOp(const IntervalSet& a, const IntervalSet& b, RegionBoolOp op) {
    const Rows rowsA = toRows(a);
    const Rows rowsB = toRows(b);

    std::vector<int32_t> ys;
    ys.reserve(rowsA.size() + rowsB.size());
    for (const auto& [y, spans] : rowsA) { (void)spans; ys.push_back(y); }
    for (const auto& [y, spans] : rowsB) { (void)spans; ys.push_back(y); }
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

    static const std::vector<Span> kEmpty;
    IntervalSet out;
    for (int32_t y : ys) {
        auto itA = rowsA.find(y);
        auto itB = rowsB.find(y);
        const std::vector<Span>& spansA = itA == rowsA.end() ? kEmpty : itA->second;
        const std::vector<Span>& spansB = itB == rowsB.end() ? kEmpty : itB->second;
        for (const Span& span : combineRow(spansA, spansB, op)) {
            out.intervals.push_back({y, span.first, span.second});
        }
    }
    return normalize(std::move(out));
}

IntervalSet unionSets(const IntervalSet& a, const IntervalSet& b) {
    return booleanOp(a, b, RegionBoolOp::Union);
}
IntervalSet subtractSets(const IntervalSet& a, const IntervalSet& b) {
    return booleanOp(a, b, RegionBoolOp::Subtract);
}
IntervalSet intersectSets(const IntervalSet& a, const IntervalSet& b) {
    return booleanOp(a, b, RegionBoolOp::Intersect);
}
IntervalSet xorSets(const IntervalSet& a, const IntervalSet& b) {
    return booleanOp(a, b, RegionBoolOp::Xor);
}

IntervalSet invertSet(const IntervalSet& a, Rect2i within) {
    IntervalSet full;
    for (int32_t y = within.min.y; y < within.max.y; ++y) {
        if (within.max.x > within.min.x) {
            full.intervals.push_back({y, within.min.x, within.max.x});
        }
    }
    return subtractSets(normalize(std::move(full)), a);
}

// ---------------------------------------------------------------------------
// Morphology
// ---------------------------------------------------------------------------

IntervalSet expand(const IntervalSet& set, float pixels, bool preserveCorners) {
    if (pixels <= 0.f || set.empty()) {
        return normalize(set);
    }

    const int32_t radius = static_cast<int32_t>(std::floor(pixels + 1e-4f));
    if (radius <= 0) {
        return normalize(set);
    }

    IntervalSet out;
    for (const Interval& interval : set.intervals) {
        for (int32_t dy = -radius; dy <= radius; ++dy) {
            int32_t dx = radius;
            if (!preserveCorners) {
                const float remaining = pixels * pixels - static_cast<float>(dy * dy);
                if (remaining < 0.f) {
                    continue;
                }
                dx = static_cast<int32_t>(std::floor(std::sqrt(remaining) + 1e-4f));
            }
            out.intervals.push_back({interval.y + dy, interval.x0 - dx, interval.x1 + dx});
        }
    }
    return normalize(std::move(out));
}

IntervalSet contract(const IntervalSet& set, float pixels, bool preserveCorners) {
    if (pixels <= 0.f || set.empty()) {
        return normalize(set);
    }
    const int32_t radius = static_cast<int32_t>(std::floor(pixels + 1e-4f));
    if (radius <= 0) {
        return normalize(set);
    }

    // Erosion by duality: complement, dilate, complement — within a frame that
    // is padded so the outside is treated as empty space.
    Rect2i frame = bounds(set);
    frame.min.x -= radius + 1;
    frame.min.y -= radius + 1;
    frame.max.x += radius + 1;
    frame.max.y += radius + 1;

    const IntervalSet complement = invertSet(set, frame);
    const IntervalSet grown      = expand(complement, pixels, preserveCorners);
    return intersectSets(set, invertSet(grown, frame));
}

// ---------------------------------------------------------------------------
// Topology
// ---------------------------------------------------------------------------

std::vector<IntervalSet> connectedComponents(const IntervalSet& set, bool eightConnected) {
    const PixelSet pixels = toPixelSet(set);
    PixelSet visited;
    std::vector<IntervalSet> components;

    // Deterministic seed order: walk the normalized interval list, not the hash set.
    for (const Interval& interval : set.intervals) {
        for (int32_t x = interval.x0; x < interval.x1; ++x) {
            const uint64_t seed = pixelKey(x, interval.y);
            if (visited.find(seed) != visited.end()) {
                continue;
            }

            PixelSet component;
            std::deque<uint64_t> queue { seed };
            visited.insert(seed);

            while (!queue.empty()) {
                const uint64_t key = queue.front();
                queue.pop_front();
                component.insert(key);

                const int32_t cx = keyX(key);
                const int32_t cy = keyY(key);
                const Vec2i neighbors[8] = {
                    {cx - 1, cy}, {cx + 1, cy}, {cx, cy - 1}, {cx, cy + 1},
                    {cx - 1, cy - 1}, {cx + 1, cy - 1}, {cx - 1, cy + 1}, {cx + 1, cy + 1}
                };
                const int count = eightConnected ? 8 : 4;
                for (int i = 0; i < count; ++i) {
                    const uint64_t next = pixelKey(neighbors[i].x, neighbors[i].y);
                    if (pixels.find(next) == pixels.end() ||
                        visited.find(next) != visited.end()) {
                        continue;
                    }
                    visited.insert(next);
                    queue.push_back(next);
                }
            }

            components.push_back(fromPixelSet(component));
        }
    }

    return components;
}

IntervalSet boundaryOf(const IntervalSet& set) {
    const PixelSet pixels = toPixelSet(set);
    IntervalSet out;
    for (const Interval& interval : set.intervals) {
        int32_t runStart = 0;
        bool inRun = false;
        for (int32_t x = interval.x0; x < interval.x1; ++x) {
            const bool isEdge =
                !pixelIn(pixels, x - 1, interval.y) ||
                !pixelIn(pixels, x + 1, interval.y) ||
                !pixelIn(pixels, x, interval.y - 1) ||
                !pixelIn(pixels, x, interval.y + 1);
            if (isEdge) {
                if (!inRun) {
                    runStart = x;
                    inRun = true;
                }
            } else if (inRun) {
                out.intervals.push_back({interval.y, runStart, x});
                inRun = false;
            }
        }
        if (inRun) {
            out.intervals.push_back({interval.y, runStart, interval.x1});
        }
    }
    return normalize(std::move(out));
}

IntervalSet outerBoundaryOf(const IntervalSet& set) {
    return subtractSets(expand(set, 1.f, true), set);
}

// ---------------------------------------------------------------------------
// Primitive rasterization
// ---------------------------------------------------------------------------

IntervalSet rasterizePoint(const PointDesc& desc) {
    IntervalSet set;
    const int32_t x = static_cast<int32_t>(std::floor(desc.position.x));
    const int32_t y = static_cast<int32_t>(std::floor(desc.position.y));
    set.intervals.push_back({y, x, x + 1});
    return set;
}

IntervalSet rasterizeLine(const LineDesc& desc) {
    PixelSet pixels;
    plotLine(pixels, desc.start, desc.end);
    return fromPixelSet(pixels);
}

IntervalSet rasterizePolyline(const PolylineDesc& desc) {
    if (desc.points.empty()) {
        return {};
    }
    if (desc.points.size() == 1) {
        return rasterizePoint({desc.points.front()});
    }

    if (desc.closed && desc.points.size() >= 3) {
        IntervalSet filled = fillPolygon(desc.points);
        PixelSet outline;
        for (size_t i = 0; i < desc.points.size(); ++i) {
            plotLine(outline, desc.points[i], desc.points[(i + 1) % desc.points.size()]);
        }
        return unionSets(filled, fromPixelSet(outline));
    }

    PixelSet pixels;
    for (size_t i = 0; i + 1 < desc.points.size(); ++i) {
        plotLine(pixels, desc.points[i], desc.points[i + 1]);
    }
    return fromPixelSet(pixels);
}

IntervalSet rasterizeRect(const RectDesc& desc) {
    IntervalSet set;
    if (desc.width <= 0.f || desc.height <= 0.f) {
        return set;
    }

    const float left   = desc.origin.x;
    const float top    = desc.origin.y;
    const float right  = desc.origin.x + desc.width;
    const float bottom = desc.origin.y + desc.height;

    const float radius = std::max(0.f, std::min(desc.cornerRadius,
                                                std::min(desc.width, desc.height) * 0.5f));

    const int32_t y0 = static_cast<int32_t>(std::ceil(top - 0.5f));
    const int32_t y1 = static_cast<int32_t>(std::ceil(bottom - 0.5f));

    for (int32_t y = y0; y < y1; ++y) {
        const float centerY = static_cast<float>(y) + 0.5f;
        float inset = 0.f;
        if (radius > 0.f) {
            float dy = 0.f;
            if (centerY < top + radius) {
                dy = (top + radius) - centerY;
            } else if (centerY > bottom - radius) {
                dy = centerY - (bottom - radius);
            }
            if (dy > 0.f) {
                const float remaining = radius * radius - dy * dy;
                inset = radius - (remaining > 0.f ? std::sqrt(remaining) : 0.f);
            }
        }

        const int32_t x0 = static_cast<int32_t>(std::ceil(left + inset - 0.5f));
        const int32_t x1 = static_cast<int32_t>(std::ceil(right - inset - 0.5f));
        if (x1 > x0) {
            set.intervals.push_back({y, x0, x1});
        }
    }

    return normalize(std::move(set));
}

IntervalSet rasterizeEllipse(const EllipseDesc& desc) {
    IntervalSet set;
    if (desc.radiusX <= 0.f || desc.radiusY <= 0.f) {
        return set;
    }

    const int32_t y0 = static_cast<int32_t>(std::floor(desc.center.y - desc.radiusY));
    const int32_t y1 = static_cast<int32_t>(std::ceil(desc.center.y + desc.radiusY));

    for (int32_t y = y0; y < y1; ++y) {
        const float normalizedY = (static_cast<float>(y) + 0.5f - desc.center.y) / desc.radiusY;
        const float inside = 1.f - normalizedY * normalizedY;
        if (inside < 0.f) {
            continue;
        }
        const float halfWidth = std::sqrt(inside) * desc.radiusX;
        const int32_t x0 = static_cast<int32_t>(std::floor(desc.center.x - halfWidth));
        const int32_t x1 = static_cast<int32_t>(std::ceil(desc.center.x + halfWidth));
        if (x1 > x0) {
            set.intervals.push_back({y, x0, x1});
        }
    }

    return normalize(std::move(set));
}

IntervalSet rasterizeCircle(const CircleDesc& desc) {
    return rasterizeEllipse({desc.center, desc.radius, desc.radius});
}

IntervalSet rasterizePolygon(const PolygonDesc& desc) {
    IntervalSet filled = fillPolygon(desc.vertices);
    if (!desc.includeEdges || desc.vertices.empty()) {
        return filled;
    }
    // Each edge as the line of pixels between the corners' pixels.
    IntervalSet edges;
    const size_t n = desc.vertices.size();
    for (size_t i = 0; i < n; ++i) {
        const Vec2f& p = desc.vertices[i];
        const Vec2f& q = desc.vertices[(i + 1) % n];
        int32_t x0 = static_cast<int32_t>(std::floor(p.x));
        int32_t y0 = static_cast<int32_t>(std::floor(p.y));
        const int32_t x1 = static_cast<int32_t>(std::floor(q.x));
        const int32_t y1 = static_cast<int32_t>(std::floor(q.y));
        const int32_t dx =  std::abs(x1 - x0);
        const int32_t dy = -std::abs(y1 - y0);
        const int32_t sx = x0 < x1 ? 1 : -1;
        const int32_t sy = y0 < y1 ? 1 : -1;
        int32_t err = dx + dy;
        while (true) {
            edges.intervals.push_back({ y0, x0, x0 + 1 });
            if (x0 == x1 && y0 == y1) {
                break;
            }
            const int32_t e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
    return unionSets(normalize(std::move(filled)), normalize(std::move(edges)));
}

IntervalSet rasterizeCurve(const CurveDesc& desc) {
    const std::vector<Vec2f> path = flattenCurve(desc);
    if (path.size() < 2) {
        return path.empty() ? IntervalSet{} : rasterizePoint({path.front()});
    }
    if (desc.closed) {
        PolylineDesc polyline;
        polyline.points = path;
        polyline.closed = true;
        return rasterizePolyline(polyline);
    }

    // Open, the curve is a line one pixel wide, and drawn the way pixel
    // artists draw one: the pixels in the order the curve visits them, and
    // then without the L-shaped corners that a stair of short segments leaves
    // doubled. Plotting each flattened segment into a set instead gives a line
    // that thickens wherever it turns.
    return rasterizePixelWalk(path, false);
}

IntervalSet rasterizePixelWalk(const std::vector<Vec2f>& points, bool closed) {
    if (points.empty()) {
        return {};
    }
    std::vector<Vec2i> walk;
    const auto visit = [&walk](int32_t x, int32_t y) {
        if (!walk.empty() && walk.back().x == x && walk.back().y == y) {
            return;
        }
        // Straight back to the pixel before: a wobble of the path, not a
        // stroke -- take the last step back instead.
        if (walk.size() >= 2 && walk[walk.size() - 2].x == x && walk[walk.size() - 2].y == y) {
            walk.pop_back();
            return;
        }
        walk.push_back({ x, y });
    };
    const size_t segments = closed && points.size() > 2 ? points.size() : points.size() - 1;
    if (segments == 0) {
        visit(static_cast<int32_t>(std::floor(points.front().x)),
              static_cast<int32_t>(std::floor(points.front().y)));
    }
    for (size_t i = 0; i < segments; ++i) {
        const Vec2f& from = points[i];
        const Vec2f& to = points[(i + 1) % points.size()];
        int32_t x0 = static_cast<int32_t>(std::floor(from.x));
        int32_t y0 = static_cast<int32_t>(std::floor(from.y));
        const int32_t x1 = static_cast<int32_t>(std::floor(to.x));
        const int32_t y1 = static_cast<int32_t>(std::floor(to.y));
        const int32_t dx =  std::abs(x1 - x0);
        const int32_t dy = -std::abs(y1 - y0);
        const int32_t sx = x0 < x1 ? 1 : -1;
        const int32_t sy = y0 < y1 ? 1 : -1;
        int32_t err = dx + dy;
        while (true) {
            visit(x0, y0);
            if (x0 == x1 && y0 == y1) {
                break;
            }
            const int32_t e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
    // A closed walk comes back to where it started: that pixel once.
    if (closed && walk.size() > 1 && walk.back().x == walk.front().x &&
        walk.back().y == walk.front().y) {
        walk.pop_back();
    }
    const auto corner = [](const Vec2i& a, const Vec2i& b, const Vec2i& p) {
        return std::abs(p.x - a.x) == 1 && std::abs(p.y - a.y) == 1 &&
               (b.x == a.x || b.y == a.y) && (b.x == p.x || b.y == p.y);
    };
    std::vector<Vec2i> kept;
    kept.reserve(walk.size());
    for (const Vec2i& p : walk) {
        if (kept.size() >= 2 && corner(kept[kept.size() - 2], kept.back(), p)) {
            kept.pop_back();
        }
        kept.push_back(p);
    }
    // Round the seam of a closed walk too: the last pixel and the first, each
    // with the pixels either side of it.
    if (closed && kept.size() >= 4) {
        if (corner(kept[kept.size() - 2], kept.back(), kept.front())) {
            kept.pop_back();
        }
        if (kept.size() >= 4 && corner(kept.back(), kept.front(), kept[1])) {
            kept.erase(kept.begin());
        }
    }
    PixelSet pixels;
    for (const Vec2i& p : kept) {
        pixels.insert(pixelKey(p.x, p.y));
    }
    return fromPixelSet(pixels);
}

IntervalSet rasterizeArea(const std::vector<Vec2f>& outline, bool bySpans) {
    if (!bySpans) {
        return fillPolygon(outline);
    }
    IntervalSet set;
    if (outline.size() < 3) {
        return set;
    }
    float minY = outline[0].y, maxY = outline[0].y;
    for (const Vec2f& p : outline) {
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    std::vector<float> crossings;
    for (int32_t y = static_cast<int32_t>(std::floor(minY));
         y < static_cast<int32_t>(std::ceil(maxY)); ++y) {
        const float sampleY = static_cast<float>(y) + 0.5f;
        crossings.clear();
        for (size_t i = 0; i < outline.size(); ++i) {
            const Vec2f& p = outline[i];
            const Vec2f& q = outline[(i + 1) % outline.size()];
            if (p.y == q.y) {
                continue;
            }
            const float lo = std::min(p.y, q.y);
            const float hi = std::max(p.y, q.y);
            if (sampleY < lo || sampleY >= hi) {
                continue;
            }
            crossings.push_back(p.x + (sampleY - p.y) / (q.y - p.y) * (q.x - p.x));
        }
        std::sort(crossings.begin(), crossings.end());
        for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
            const int32_t x0 = static_cast<int32_t>(std::floor(crossings[i]));
            const int32_t x1 = static_cast<int32_t>(std::ceil(crossings[i + 1]));
            if (x1 > x0) {
                set.intervals.push_back({ y, x0, x1 });
            }
        }
    }
    return normalize(std::move(set));
}

bool keepsPixelGrid(const Mat3f& matrix) {
    const auto whole = [](float value) { return std::fabs(value - std::round(value)) < 1e-4f; };
    const float a = matrix.m[0], b = matrix.m[1], c = matrix.m[3], d = matrix.m[4];
    return whole(a) && whole(b) && whole(c) && whole(d) &&
           whole(matrix.m[2]) && whole(matrix.m[5]) &&
           std::fabs(std::fabs(a) + std::fabs(b) - 1.f) < 1e-4f &&
           std::fabs(std::fabs(c) + std::fabs(d) - 1.f) < 1e-4f &&
           std::fabs(std::fabs(a) + std::fabs(c) - 1.f) < 1e-4f;
}

IntervalSet mapAcrossGrid(const IntervalSet& set, const Mat3f& matrix) {
    // A whole-pixel move: the runs shifted, nothing else.
    if (std::fabs(matrix.m[0] - 1.f) < 1e-4f && std::fabs(matrix.m[4] - 1.f) < 1e-4f &&
        std::fabs(matrix.m[1]) < 1e-4f && std::fabs(matrix.m[3]) < 1e-4f) {
        const int32_t dx = static_cast<int32_t>(std::lround(matrix.m[2]));
        const int32_t dy = static_cast<int32_t>(std::lround(matrix.m[5]));
        IntervalSet moved = set;
        for (Interval& interval : moved.intervals) {
            interval.y += dy;
            interval.x0 += dx;
            interval.x1 += dx;
        }
        return moved;
    }
    PixelSet pixels;
    for (const Interval& interval : set.intervals) {
        for (int32_t x = interval.x0; x < interval.x1; ++x) {
            const Vec2f to = matrix.transformPoint({ static_cast<float>(x) + 0.5f,
                                                     static_cast<float>(interval.y) + 0.5f });
            pixels.insert(pixelKey(static_cast<int32_t>(std::floor(to.x)),
                                   static_cast<int32_t>(std::floor(to.y))));
        }
    }
    return fromPixelSet(pixels);
}

// ---------------------------------------------------------------------------
// Freehand marks, areas and fills
// ---------------------------------------------------------------------------

namespace {

// The pixels of a straight run from one pixel to another, both included, in
// order -- the same Bresenham every pixel tool draws.
void walkBetween(Vec2i from, Vec2i to, std::vector<Vec2i>& out) {
    int32_t x0 = from.x;
    int32_t y0 = from.y;
    const int32_t dx =  std::abs(to.x - x0);
    const int32_t dy = -std::abs(to.y - y0);
    const int32_t sx = x0 < to.x ? 1 : -1;
    const int32_t sy = y0 < to.y ? 1 : -1;
    int32_t err = dx + dy;
    while (true) {
        if (out.empty() || out.back().x != x0 || out.back().y != y0) {
            out.push_back({ x0, y0 });
        }
        if (x0 == to.x && y0 == to.y) {
            break;
        }
        const int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

Vec2i pixelOf(Vec2f p) {
    return { static_cast<int32_t>(std::floor(p.x)), static_cast<int32_t>(std::floor(p.y)) };
}

IntervalSet fillContours(const std::vector<std::vector<Vec2f>>& contours);

// Drops the corner of every L in a walk: where the pixels either side of one
// touch diagonally, the walk steps straight between them.
std::vector<Vec2i> withoutCorners(const std::vector<Vec2i>& walk) {
    std::vector<Vec2i> kept;
    kept.reserve(walk.size());
    for (const Vec2i& p : walk) {
        if (kept.size() >= 2) {
            const Vec2i& a = kept[kept.size() - 2];
            const Vec2i& b = kept.back();
            const bool corner = std::abs(p.x - a.x) == 1 && std::abs(p.y - a.y) == 1 &&
                                (b.x == a.x || b.y == a.y) && (b.x == p.x || b.y == p.y);
            if (corner) {
                kept.pop_back();
            }
        }
        kept.push_back(p);
    }
    return kept;
}

// A footprint for a brush of `size` stamped through a move that scales by
// `scale`: the stamp redrawn at its new size, still level with the grid --
// a pixel brush does not turn with what it drew.
int scaledSize(float size, float scale) {
    return std::max(1, static_cast<int>(std::lround(size * scale)));
}

void stamp(PixelSet& into, Vec2i at, const std::vector<Vec2i>& footprint) {
    if (footprint.empty()) {
        into.insert(pixelKey(at.x, at.y));
        return;
    }
    for (const Vec2i& offset : footprint) {
        into.insert(pixelKey(at.x + offset.x, at.y + offset.y));
    }
}

// A brush stamp `w` by `h`, as offsets from the pixel it is placed on --
// brushFootprint's rule, stretched: a brush on a layer scaled more one way
// than the other is stretched with it, and stays level with the grid.
std::vector<Vec2i> stretchedFootprint(int w, int h, bool round) {
    if (w == h) {
        return w <= 1 ? std::vector<Vec2i>{} : brushFootprint(w, round);
    }
    std::vector<Vec2i> out;
    const int beforeX = (w - 1) / 2;
    const int beforeY = (h - 1) / 2;
    const float halfW = static_cast<float>(w) * 0.5f;
    const float halfH = static_cast<float>(h) * 0.5f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (round && std::min(w, h) >= 3) {
                const float dx = (static_cast<float>(x) + 0.5f - halfW) / halfW;
                const float dy = (static_cast<float>(y) + 0.5f - halfH) / halfH;
                if (dx * dx + dy * dy > 0.8f) {
                    continue;
                }
            }
            out.push_back({ x - beforeX, y - beforeY });
        }
    }
    return out;
}

// How a stroke moves: by a matrix, or by a map of the plane that is not
// one (see PointMap). Null for a stroke drawn where it was made.
struct Mover {
    const Mat3f* matrix = nullptr;
    const PointMap* map = nullptr;
    Vec2f apply(Vec2f p) const { return matrix != nullptr ? matrix->transformPoint(p) : (*map)(p); }
    // The move near `p`, less where it goes: how it turns and stretches
    // there, as a b / c d.
    std::array<float, 4> linearAt(Vec2f p) const {
        if (matrix != nullptr) {
            return { matrix->m[0], matrix->m[1], matrix->m[3], matrix->m[4] };
        }
        const Vec2f here = (*map)(p);
        const Vec2f across = (*map)({ p.x + 1.f, p.y });
        const Vec2f down = (*map)({ p.x, p.y + 1.f });
        return { across.x - here.x, down.x - here.x, across.y - here.y, down.y - here.y };
    }
    // How far a step of one pixel across, and one down, goes near `p`.
    Vec2f stretchAt(Vec2f p) const {
        if (matrix != nullptr) {
            return { std::sqrt(matrix->m[0] * matrix->m[0] + matrix->m[3] * matrix->m[3]),
                     std::sqrt(matrix->m[1] * matrix->m[1] + matrix->m[4] * matrix->m[4]) };
        }
        const Vec2f here = (*map)(p);
        const Vec2f across = (*map)({ p.x + 1.f, p.y });
        const Vec2f down = (*map)({ p.x, p.y + 1.f });
        return { std::sqrt((across.x - here.x) * (across.x - here.x) + (across.y - here.y) * (across.y - here.y)),
                 std::sqrt((down.x - here.x) * (down.x - here.x) + (down.y - here.y) * (down.y - here.y)) };
    }
};

// A custom brush's stamp, as offsets from the pixel it is placed on: its
// shape as drawn, or turned and stretched as the move turns and stretches it
// near `at`, about the middle of that pixel.
std::vector<Vec2i> tipFootprint(const AreaDesc& tip, const Mover* mover, Vec2f at) {
    std::vector<std::vector<Vec2f>> contours = tip.contours;
    if (mover != nullptr) {
        const std::array<float, 4> l = mover->linearAt(at);
        for (auto& contour : contours) {
            for (Vec2f& q : contour) {
                const float x = q.x - 0.5f;
                const float y = q.y - 0.5f;
                q = { l[0] * x + l[1] * y + 0.5f, l[2] * x + l[3] * y + 0.5f };
            }
        }
    }
    std::vector<Vec2i> out;
    for (const Interval& run : fillContours(contours).intervals) {
        for (int32_t x = run.x0; x < run.x1; ++x) {
            out.push_back({ x, run.y });
        }
    }
    if (out.empty()) {
        out.push_back({ 0, 0 });            // too thin to cover a pixel centre: one pixel
    }
    return out;
}

// One stroke's pixels. As drawn (`mover` null) the points are the pixels
// laid down, joined where the pointer jumped. Moved, the path is simplified
// back to the lines the hand drew -- the stair of pixels a straight line
// leaves is not the line -- moved, and walked again where it lands, the brush
// stretched as the move stretches each axis. A brush an even number of pixels
// across sits on the corner between pixels, not on a pixel, so the walk for
// it runs half a pixel up and to the left: that is what lands a line doubled
// in size exactly on the pixels of the line, doubled. Moved by a map that is
// not a matrix, the simplified path is cut into half-pixel steps first, so it
// bends where the map bends.
PixelSet strokePixels(const PenStroke& stroke, const Mover* mover) {
    PixelSet pixels;
    if (stroke.points.empty() && stroke.kind != PenKind::Area) {
        return pixels;
    }
    const bool bends = mover != nullptr && mover->matrix == nullptr;
    const auto densified = [bends](std::vector<Vec2f> points, bool closed) {
        return bends ? densifyPath(points, closed) : points;
    };

    if (stroke.kind == PenKind::Area) {
        IntervalSet area;
        if (mover == nullptr) {
            area = fillContours(stroke.area.contours);
        } else {
            std::vector<std::vector<Vec2f>> moved;
            for (const auto& contour : stroke.area.contours) {
                moved.push_back(densified(contour, true));
                for (Vec2f& p : moved.back()) {
                    p = mover->apply(p);
                }
            }
            area = fillContours(moved);
        }
        for (const Interval& interval : area.intervals) {
            for (int32_t x = interval.x0; x < interval.x1; ++x) {
                pixels.insert(pixelKey(x, interval.y));
            }
        }
        return pixels;
    }

    const Vec2f stretch = mover == nullptr ? Vec2f{ 1.f, 1.f } : mover->stretchAt(stroke.points.front());
    // A custom brush stamps its own shape on each pixel of the walk.
    const bool tipped = !stroke.tip.contours.empty();
    const std::vector<Vec2i> tip = tipped ? tipFootprint(stroke.tip, mover, stroke.points.front())
                                          : std::vector<Vec2i>{};
    const auto sizeOf = [&](size_t i) {
        return tipped ? 1.f : i < stroke.sizes.size() ? stroke.sizes[i] : stroke.size;
    };
    const auto widthAt = [&](size_t i) { return scaledSize(sizeOf(i), tipped ? 1.f : stretch.x); };
    const auto heightAt = [&](size_t i) { return scaledSize(sizeOf(i), tipped ? 1.f : stretch.y); };
    std::map<std::pair<int, int>, std::vector<Vec2i>> footprints;
    const auto footprintOf = [&](int w, int h) -> const std::vector<Vec2i>& {
        if (tipped) {
            return tip;
        }
        auto found = footprints.find({ w, h });
        if (found == footprints.end()) {
            found = footprints.emplace(std::make_pair(w, h), stretchedFootprint(w, h, stroke.round)).first;
        }
        return found->second;
    };
    // Where a stamp that big is placed for a point: its pixel, or for an even
    // brush the pixel up and to the left of the corner nearest it.
    const auto placeOf = [](Vec2f at, int w, int h) {
        return Vec2i{ static_cast<int32_t>(std::floor(at.x - (w % 2 == 0 ? 0.5f : 0.f))),
                      static_cast<int32_t>(std::floor(at.y - (h % 2 == 0 ? 0.5f : 0.f))) };
    };
    if (stroke.kind == PenKind::Dots) {
        for (size_t i = 0; i < stroke.points.size(); ++i) {
            const Vec2f at = mover == nullptr ? stroke.points[i] : mover->apply(stroke.points[i]);
            const int w = widthAt(i);
            const int h = heightAt(i);
            stamp(pixels, placeOf(at, w, h), footprintOf(w, h));
        }
        return pixels;
    }

    // The path, and which point each of its corners came from (for the size).
    std::vector<Vec2f> path = stroke.points;
    std::vector<size_t> from(path.size());
    for (size_t i = 0; i < from.size(); ++i) {
        from[i] = i;
    }
    if (mover != nullptr && path.size() > 2 && stroke.sizes.empty()) {
        SimplifyParams params;
        params.epsilon = 0.75f;
        params.preserveCorners = false;
        path = simplifyPath(path, params);
        path = densified(path, false);
        from.assign(path.size(), 0);
    }
    if (mover != nullptr) {
        for (Vec2f& p : path) {
            p = mover->apply(p);
        }
    }
    // The walk runs over stamp places; the first point's brush says whether
    // those are pixels or corners.
    const int firstW = widthAt(from.front());
    const int firstH = heightAt(from.front());
    std::vector<Vec2i> walk;
    std::vector<size_t> walkFrom;
    walk.push_back(placeOf(path.front(), firstW, firstH));
    walkFrom.push_back(from.front());
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        std::vector<Vec2i> run;
        walkBetween(placeOf(path[i], firstW, firstH), placeOf(path[i + 1], firstW, firstH), run);
        for (size_t r = 1; r < run.size(); ++r) {
            walk.push_back(run[r]);
            walkFrom.push_back(from[i]);
        }
    }
    const bool oneWide = !tipped && stroke.sizes.empty() && firstW <= 1 && firstH <= 1;
    if (mover != nullptr && oneWide && stroke.pixelPerfect) {
        walk = withoutCorners(walk);
        walkFrom.assign(walk.size(), 0);
    }
    for (size_t i = 0; i < walk.size(); ++i) {
        stamp(pixels, walk[i], footprintOf(widthAt(walkFrom[i]), heightAt(walkFrom[i])));
    }
    return pixels;
}

// A line or a spray one pixel wide: cut where it is erased, never masked.
bool isThin(const PenStroke& stroke) {
    return stroke.kind != PenKind::Area && stroke.sizes.empty() && stroke.size <= 1.f &&
           stroke.tip.contours.empty();
}

IntervalSet strokesPixels(const StrokesDesc& desc, const Mover* mover) {
    IntervalSet thin;
    IntervalSet solid;
    for (const PenStroke& stroke : desc.strokes) {
        const IntervalSet mark = fromPixelSet(strokePixels(stroke, mover));
        if (stroke.erase) {
            solid = subtractSets(solid, mark);
        } else if (isThin(stroke)) {
            thin = unionSets(thin, mark);
        } else {
            solid = unionSets(solid, mark);
        }
    }
    return unionSets(thin, solid);
}

// Even-odd over every contour at once, a pixel inside when its centre is.
IntervalSet fillContours(const std::vector<std::vector<Vec2f>>& contours) {
    IntervalSet set;
    float minY = 0.f, maxY = 0.f;
    bool any = false;
    for (const auto& contour : contours) {
        for (const Vec2f& p : contour) {
            minY = any ? std::min(minY, p.y) : p.y;
            maxY = any ? std::max(maxY, p.y) : p.y;
            any = true;
        }
    }
    if (!any) {
        return set;
    }
    std::vector<float> crossings;
    for (int32_t y = static_cast<int32_t>(std::floor(minY)); y < static_cast<int32_t>(std::ceil(maxY)); ++y) {
        const float sampleY = static_cast<float>(y) + 0.5f;
        crossings.clear();
        for (const auto& contour : contours) {
            for (size_t i = 0; i < contour.size(); ++i) {
                const Vec2f& p = contour[i];
                const Vec2f& q = contour[(i + 1) % contour.size()];
                if (p.y == q.y) {
                    continue;
                }
                const float lo = std::min(p.y, q.y);
                const float hi = std::max(p.y, q.y);
                if (sampleY < lo || sampleY >= hi) {
                    continue;
                }
                crossings.push_back(p.x + (sampleY - p.y) / (q.y - p.y) * (q.x - p.x));
            }
        }
        std::sort(crossings.begin(), crossings.end());
        for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
            const int32_t x0 = static_cast<int32_t>(std::ceil(crossings[i] - 0.5f));
            const int32_t x1 = static_cast<int32_t>(std::ceil(crossings[i + 1] - 0.5f));
            if (x1 > x0) {
                set.intervals.push_back({ y, x0, x1 });
            }
        }
    }
    return normalize(std::move(set));
}

bool coloursWithin(Color a, Color b, int32_t tolerance) {
    if (a.a == 0 && b.a == 0) {
        return true;
    }
    if (tolerance <= 0) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    }
    return std::abs(static_cast<int>(a.r) - b.r) <= tolerance &&
           std::abs(static_cast<int>(a.g) - b.g) <= tolerance &&
           std::abs(static_cast<int>(a.b) - b.b) <= tolerance &&
           std::abs(static_cast<int>(a.a) - b.a) <= tolerance;
}

Color rasterAt(const RasterBuffer& raster, int32_t x, int32_t y) {
    const uint8_t* p = raster.row(static_cast<uint32_t>(y)) + static_cast<size_t>(x) * 4u;
    return { p[0], p[1], p[2], p[3] };
}

} // namespace

std::vector<Vec2i> brushFootprint(int size, bool round) {
    std::vector<Vec2i> out;
    size = std::max(1, std::min(size, 256));
    const int before = (size - 1) / 2;
    const float centre = static_cast<float>(size) * 0.5f;
    const float radius = centre;
    out.reserve(static_cast<size_t>(size) * static_cast<size_t>(size));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (round && size >= 3) {
                // Inside the circle the stamp is inscribed in, with a touch of
                // slack so a size-3 brush is a plus, not a dot.
                const float dx = static_cast<float>(x) + 0.5f - centre;
                const float dy = static_cast<float>(y) + 0.5f - centre;
                if (dx * dx + dy * dy > radius * radius * 0.8f) {
                    continue;
                }
            }
            out.push_back({ x - before, y - before });
        }
    }
    return out;
}

IntervalSet rasterizeStrokes(const StrokesDesc& desc) {
    return strokesPixels(desc, nullptr);
}

IntervalSet rasterizeStrokesThrough(const StrokesDesc& desc, const Mat3f& matrix) {
    if (keepsPixelGrid(matrix)) {
        return mapAcrossGrid(strokesPixels(desc, nullptr), matrix);
    }
    Mover mover;
    mover.matrix = &matrix;
    return strokesPixels(desc, &mover);
}

IntervalSet rasterizeStrokesAlong(const StrokesDesc& desc, const PointMap& map) {
    Mover mover;
    mover.map = &map;
    return strokesPixels(desc, &mover);
}

std::vector<Vec2f> densifyPath(const std::vector<Vec2f>& points, bool closed, float step) {
    if (points.size() < 2) {
        return points;
    }
    const float safeStep = std::max(0.05f, step);
    std::vector<Vec2f> out;
    const size_t segments = closed ? points.size() : points.size() - 1;
    for (size_t i = 0; i < segments; ++i) {
        const Vec2f a = points[i];
        const Vec2f b = points[(i + 1) % points.size()];
        const float length = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
        const int steps = std::max(1, std::min(4096, static_cast<int>(std::ceil(length / safeStep))));
        for (int k = 0; k < steps; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(steps);
            out.push_back({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t });
        }
    }
    if (!closed) {
        out.push_back(points.back());
    }
    return out;
}

IntervalSet rasterizeAreaDesc(const AreaDesc& desc) {
    return fillContours(desc.contours);
}

IntervalSet rasterizeAreaThrough(const AreaDesc& desc, const Mat3f& matrix) {
    if (keepsPixelGrid(matrix)) {
        return mapAcrossGrid(fillContours(desc.contours), matrix);
    }
    std::vector<std::vector<Vec2f>> moved = desc.contours;
    for (auto& contour : moved) {
        for (Vec2f& p : contour) {
            p = matrix.transformPoint(p);
        }
    }
    return fillContours(moved);
}

IntervalSet rasterizeAreaAlong(const AreaDesc& desc, const PointMap& map) {
    std::vector<std::vector<Vec2f>> moved;
    for (const auto& contour : desc.contours) {
        moved.push_back(densifyPath(contour, true));
        for (Vec2f& p : moved.back()) {
            p = map(p);
        }
    }
    return fillContours(moved);
}

AreaDesc traceArea(const IntervalSet& set) {
    AreaDesc area;
    for (const ContourDesc& contour : traceContours(set, true)) {
        // The tracer walks every pixel edge; a straight run needs its ends only.
        std::vector<Vec2f> corners;
        const size_t n = contour.points.size();
        for (size_t i = 0; i < n; ++i) {
            const Vec2f& before = contour.points[(i + n - 1) % n];
            const Vec2f& here = contour.points[i];
            const Vec2f& after = contour.points[(i + 1) % n];
            const float cross = (here.x - before.x) * (after.y - here.y) -
                                (here.y - before.y) * (after.x - here.x);
            if (cross != 0.f) {
                corners.push_back(here);
            }
        }
        if (corners.size() >= 3) {
            area.contours.push_back(std::move(corners));
        }
    }
    return area;
}

Vec2f deepestPoint(const IntervalSet& set) {
    // How far each pixel is from the outside, in steps any way round (a
    // two-pass distance over the set's box); the furthest is the deepest.
    const Rect2i box = bounds(set);
    if (box.empty()) {
        return { 0.f, 0.f };
    }
    const int32_t w = box.width() + 2;
    const int32_t h = box.height() + 2;
    std::vector<int32_t> depth(static_cast<size_t>(w) * h, 0);
    for (const Interval& interval : set.intervals) {
        for (int32_t x = interval.x0; x < interval.x1; ++x) {
            depth[static_cast<size_t>(interval.y - box.min.y + 1) * w + (x - box.min.x + 1)] = 1 << 20;
        }
    }
    const auto at = [&](int32_t x, int32_t y) -> int32_t& { return depth[static_cast<size_t>(y) * w + x]; };
    for (int32_t y = 1; y < h - 1; ++y) {
        for (int32_t x = 1; x < w - 1; ++x) {
            if (at(x, y) != 0) {
                at(x, y) = std::min({ at(x, y), at(x - 1, y) + 1, at(x, y - 1) + 1,
                                      at(x - 1, y - 1) + 1, at(x + 1, y - 1) + 1 });
            }
        }
    }
    // Of the deepest, the one nearest the middle of the box: a bar's seed is
    // the middle of the bar, not whichever end of its spine came first.
    int32_t best = -1;
    float bestOffCentre = 0.f;
    const float middleX = (static_cast<float>(box.min.x) + static_cast<float>(box.max.x)) * 0.5f;
    const float middleY = (static_cast<float>(box.min.y) + static_cast<float>(box.max.y)) * 0.5f;
    Vec2f deepest { 0.f, 0.f };
    for (int32_t y = h - 2; y >= 1; --y) {
        for (int32_t x = w - 2; x >= 1; --x) {
            if (at(x, y) != 0) {
                at(x, y) = std::min({ at(x, y), at(x + 1, y) + 1, at(x, y + 1) + 1,
                                      at(x + 1, y + 1) + 1, at(x - 1, y + 1) + 1 });
                const Vec2f here { static_cast<float>(x - 1 + box.min.x) + 0.5f,
                                   static_cast<float>(y - 1 + box.min.y) + 0.5f };
                const float offCentre = std::fabs(here.x - middleX) + std::fabs(here.y - middleY);
                if (at(x, y) > best || (at(x, y) == best && offCentre < bestOffCentre)) {
                    best = at(x, y);
                    bestOffCentre = offCentre;
                    deepest = here;
                }
            }
        }
    }
    return deepest;
}

bool cutStrokes(StrokesDesc& desc, const IntervalSet& erased) {
    const PixelSet gone = toPixelSet(erased);
    const auto isGone = [&gone](Vec2i p) { return gone.count(pixelKey(p.x, p.y)) != 0; };
    bool wideTouched = false;
    std::vector<PenStroke> kept;
    kept.reserve(desc.strokes.size());
    for (const PenStroke& stroke : desc.strokes) {
        if (stroke.kind == PenKind::Area && !stroke.erase) {
            // An area loses the pixels from its edge, exactly.
            const IntervalSet was = fillContours(stroke.area.contours);
            const IntervalSet left = subtractSets(was, erased);
            if (pixelCount(left) == pixelCount(was)) {
                kept.push_back(stroke);
            } else if (!left.empty()) {
                PenStroke smaller = stroke;
                smaller.area = traceArea(left);
                kept.push_back(std::move(smaller));
            }
            continue;
        }
        const bool oneWide = isThin(stroke);
        if (stroke.erase || !oneWide) {
            if (!stroke.erase && !wideTouched) {
                const PixelSet drawn = strokePixels(stroke, nullptr);
                for (uint64_t key : drawn) {
                    if (gone.count(key) != 0) {
                        wideTouched = true;
                        break;
                    }
                }
            }
            kept.push_back(stroke);
            continue;
        }
        if (stroke.kind == PenKind::Dots) {
            PenStroke left = stroke;
            left.points.clear();
            for (const Vec2f& p : stroke.points) {
                if (!isGone(pixelOf(p))) {
                    left.points.push_back(p);
                }
            }
            if (!left.points.empty()) {
                kept.push_back(std::move(left));
            }
            continue;
        }
        // A line: its pixels in order, then the runs the eraser left.
        std::vector<Vec2i> walk;
        walk.push_back(pixelOf(stroke.points.front()));
        for (size_t i = 0; i + 1 < stroke.points.size(); ++i) {
            walkBetween(pixelOf(stroke.points[i]), pixelOf(stroke.points[i + 1]), walk);
        }
        bool touched = false;
        for (const Vec2i& p : walk) {
            touched = touched || isGone(p);
        }
        if (!touched) {
            kept.push_back(stroke);
            continue;
        }
        PenStroke piece = stroke;
        piece.points.clear();
        for (const Vec2i& p : walk) {
            if (isGone(p)) {
                if (!piece.points.empty()) {
                    kept.push_back(piece);
                    piece.points.clear();
                }
                continue;
            }
            piece.points.push_back({ static_cast<float>(p.x) + 0.5f, static_cast<float>(p.y) + 0.5f });
        }
        if (!piece.points.empty()) {
            kept.push_back(std::move(piece));
        }
    }
    desc.strokes = std::move(kept);
    return wideTouched;
}

IntervalSet floodRaster(const RasterBuffer& raster, Vec2i seed, int32_t tolerance, bool diagonal,
                        const IntervalSet* within) {
    const int32_t width = static_cast<int32_t>(raster.width);
    const int32_t height = static_cast<int32_t>(raster.height);
    if (seed.x < 0 || seed.y < 0 || seed.x >= width || seed.y >= height ||
        raster.pixels.size() < static_cast<size_t>(raster.stride) * raster.height) {
        return {};
    }
    const PixelSet allowed = within == nullptr ? PixelSet{} : toPixelSet(*within);
    const auto inside = [&](int32_t x, int32_t y) {
        return x >= 0 && y >= 0 && x < width && y < height &&
               (within == nullptr || allowed.count(pixelKey(x, y)) != 0);
    };
    if (!inside(seed.x, seed.y)) {
        return {};
    }
    const Color wanted = rasterAt(raster, seed.x, seed.y);
    std::vector<uint8_t> seen(static_cast<size_t>(width) * height, 0);
    std::deque<Vec2i> queue { seed };
    seen[static_cast<size_t>(seed.y) * width + seed.x] = 1;
    PixelSet filled;
    static const Vec2i kSteps[8] = { {1,0}, {-1,0}, {0,1}, {0,-1}, {1,1}, {1,-1}, {-1,1}, {-1,-1} };
    while (!queue.empty()) {
        const Vec2i at = queue.front();
        queue.pop_front();
        if (!coloursWithin(rasterAt(raster, at.x, at.y), wanted, tolerance)) {
            continue;
        }
        filled.insert(pixelKey(at.x, at.y));
        for (int i = 0; i < (diagonal ? 8 : 4); ++i) {
            const Vec2i next { at.x + kSteps[i].x, at.y + kSteps[i].y };
            if (!inside(next.x, next.y)) {
                continue;
            }
            uint8_t& visited = seen[static_cast<size_t>(next.y) * width + next.x];
            if (visited == 0) {
                visited = 1;
                queue.push_back(next);
            }
        }
    }
    return fromPixelSet(filled);
}

// ---------------------------------------------------------------------------
// Contours
// ---------------------------------------------------------------------------

std::vector<Vec2f> flattenCurve(const CurveDesc& desc, float tolerance) {
    std::vector<Vec2f> out;
    if (desc.segments.empty()) {
        return out;
    }
    const float safeTolerance = std::max(0.01f, tolerance);

    for (const CurveDesc::Segment& segment : desc.segments) {
        // Steps derived from the control polygon length: deterministic and
        // independent of platform floating point beyond basic arithmetic.
        const float controlLength =
            distance(segment.p0, segment.cp0) +
            distance(segment.cp0, segment.cp1) +
            distance(segment.cp1, segment.p1);
        int steps = static_cast<int>(std::ceil(controlLength / safeTolerance));
        steps = std::max(1, std::min(steps, 4096));

        if (out.empty()) {
            out.push_back(segment.p0);
        }
        for (int i = 1; i <= steps; ++i) {
            out.push_back(bezierPoint(segment, static_cast<float>(i) / static_cast<float>(steps)));
        }
    }

    return out;
}

std::vector<Vec2f> simplifyPath(const std::vector<Vec2f>& points, const SimplifyParams& params) {
    if (points.size() < 3 || params.epsilon <= 0.f) {
        return points;
    }

    // Split at corners sharper than cornerAngle, then simplify each run.
    std::vector<size_t> anchors { 0 };
    if (params.preserveCorners) {
        const float cosLimit = math::cosf(params.cornerAngle * kPi / 180.f);
        for (size_t i = 1; i + 1 < points.size(); ++i) {
            const Vec2f a { points[i].x - points[i - 1].x, points[i].y - points[i - 1].y };
            const Vec2f b { points[i + 1].x - points[i].x, points[i + 1].y - points[i].y };
            const float lenA = std::sqrt(a.x * a.x + a.y * a.y);
            const float lenB = std::sqrt(b.x * b.x + b.y * b.y);
            if (lenA <= 0.f || lenB <= 0.f) {
                continue;
            }
            const float cosTurn = (a.x * b.x + a.y * b.y) / (lenA * lenB);
            if (cosTurn < cosLimit) {
                anchors.push_back(i);
            }
        }
    }
    anchors.push_back(points.size() - 1);

    std::vector<Vec2f> out;
    out.push_back(points[anchors.front()]);
    for (size_t i = 0; i + 1 < anchors.size(); ++i) {
        rdp(points, anchors[i], anchors[i + 1], params.epsilon, out);
        out.push_back(points[anchors[i + 1]]);
    }
    return out;
}

std::vector<ContourDesc> traceContours(const IntervalSet& set, bool includeHoles) {
    const PixelSet pixels = toPixelSet(set);

    // Directed unit edges along pixel corners, oriented so the filled side is
    // on the left. Outer loops come out with positive signed area.
    struct Edge { Vec2i from; Vec2i to; };
    std::map<std::pair<int32_t, int32_t>, std::vector<Vec2i>> outgoing;

    auto addEdge = [&](Vec2i from, Vec2i to) {
        outgoing[{from.x, from.y}].push_back(to);
    };

    for (const Interval& interval : set.intervals) {
        for (int32_t x = interval.x0; x < interval.x1; ++x) {
            const int32_t y = interval.y;
            if (!pixelIn(pixels, x, y - 1)) { addEdge({x, y},         {x + 1, y});     }
            if (!pixelIn(pixels, x + 1, y)) { addEdge({x + 1, y},     {x + 1, y + 1}); }
            if (!pixelIn(pixels, x, y + 1)) { addEdge({x + 1, y + 1}, {x, y + 1});     }
            if (!pixelIn(pixels, x - 1, y)) { addEdge({x, y + 1},     {x, y});         }
        }
    }

    std::vector<ContourDesc> contours;
    for (auto& [startKey, targets] : outgoing) {
        while (!targets.empty()) {
            std::vector<Vec2i> loop;
            Vec2i current { startKey.first, startKey.second };
            loop.push_back(current);

            bool closed = false;
            while (true) {
                auto it = outgoing.find({current.x, current.y});
                if (it == outgoing.end() || it->second.empty()) {
                    break;
                }
                const Vec2i next = it->second.front();
                it->second.erase(it->second.begin());
                if (next.x == startKey.first && next.y == startKey.second) {
                    closed = true;
                    break;
                }
                current = next;
                loop.push_back(current);
            }

            if (!closed || loop.size() < 4) {
                continue;
            }

            double area2 = 0.0;
            for (size_t i = 0; i < loop.size(); ++i) {
                const Vec2i& p = loop[i];
                const Vec2i& q = loop[(i + 1) % loop.size()];
                area2 += static_cast<double>(p.x) * q.y - static_cast<double>(q.x) * p.y;
            }

            ContourDesc contour;
            contour.outer = area2 > 0.0;
            if (!contour.outer && !includeHoles) {
                continue;
            }
            contour.points.reserve(loop.size());
            for (const Vec2i& p : loop) {
                contour.points.push_back({static_cast<float>(p.x), static_cast<float>(p.y)});
            }
            contours.push_back(std::move(contour));
        }
    }

    return contours;
}

// ---------------------------------------------------------------------------
// Authored pixels
//
// A hand-drawn stroke stays a stroke. A hand-drawn closed loop of one color
// seals: its interior joins the region, and the loop itself is reported as the
// region boundary. Diagonal-only contacts are bridged so a 1px diagonal outline
// still counts as closed.
// ---------------------------------------------------------------------------

PixelRegionResult buildPixelRegion(const PixelRegionDesc& desc) {
    PixelSet coverage;
    PixelSet boundary;
    std::map<uint64_t, std::vector<Vec2i>> pixelsByColor;

    for (const PixelInput& pixel : desc.pixels) {
        coverage.insert(pixelKey(pixel.position.x, pixel.position.y));
        pixelsByColor[colorKey(pixel.color)].push_back(pixel.position);
    }

    if (!desc.closeSameColorBoundaries) {
        return { fromPixelSet(coverage), {} };
    }

    for (const auto& [color, colorPixels] : pixelsByColor) {
        (void)color;
        if (colorPixels.empty()) {
            continue;
        }

        PixelSet colorSet;
        for (Vec2i p : colorPixels) {
            colorSet.insert(pixelKey(p.x, p.y));
        }

        // Eight-connected components of this color.
        PixelSet visited;
        std::vector<std::vector<Vec2i>> components;
        for (Vec2i seed : colorPixels) {
            const uint64_t seedKey = pixelKey(seed.x, seed.y);
            if (visited.find(seedKey) != visited.end()) {
                continue;
            }
            std::vector<Vec2i> component;
            std::vector<Vec2i> queue { seed };
            visited.insert(seedKey);
            for (size_t i = 0; i < queue.size(); ++i) {
                const Vec2i current = queue[i];
                component.push_back(current);
                const Vec2i neighbors[8] = {
                    {current.x - 1, current.y}, {current.x + 1, current.y},
                    {current.x, current.y - 1}, {current.x, current.y + 1},
                    {current.x - 1, current.y - 1}, {current.x + 1, current.y - 1},
                    {current.x - 1, current.y + 1}, {current.x + 1, current.y + 1},
                };
                for (Vec2i next : neighbors) {
                    const uint64_t key = pixelKey(next.x, next.y);
                    if (colorSet.find(key) == colorSet.end() ||
                        visited.find(key) != visited.end()) {
                        continue;
                    }
                    visited.insert(key);
                    queue.push_back(next);
                }
            }
            components.push_back(std::move(component));
        }

        for (const std::vector<Vec2i>& component : components) {
            if (component.empty()) {
                continue;
            }

            PixelSet wall;
            int32_t minX = component.front().x, maxX = component.front().x;
            int32_t minY = component.front().y, maxY = component.front().y;
            for (Vec2i p : component) {
                minX = std::min(minX, p.x);
                maxX = std::max(maxX, p.x);
                minY = std::min(minY, p.y);
                maxY = std::max(maxY, p.y);
                wall.insert(pixelKey(p.x, p.y));
            }

            if (!hasPixelCycle(wall)) {
                continue;   // an open stroke encloses nothing
            }

            minX -= 1; minY -= 1; maxX += 1; maxY += 1;

            // Bridge diagonal contacts so the flood fill cannot leak through them.
            PixelSet sealedWall = wall;
            PixelSet virtualSeals;
            for (Vec2i p : component) {
                struct DiagonalSeal { Vec2i diagonal, bridgeA, bridgeB; };
                const DiagonalSeal seals[4] = {
                    {{p.x - 1, p.y - 1}, {p.x - 1, p.y}, {p.x, p.y - 1}},
                    {{p.x + 1, p.y - 1}, {p.x + 1, p.y}, {p.x, p.y - 1}},
                    {{p.x - 1, p.y + 1}, {p.x - 1, p.y}, {p.x, p.y + 1}},
                    {{p.x + 1, p.y + 1}, {p.x + 1, p.y}, {p.x, p.y + 1}},
                };
                for (const DiagonalSeal& seal : seals) {
                    const bool diagonalPresent = wall.count(pixelKey(seal.diagonal.x, seal.diagonal.y)) != 0;
                    const bool bridgeAFree     = wall.count(pixelKey(seal.bridgeA.x, seal.bridgeA.y)) == 0;
                    const bool bridgeBFree     = wall.count(pixelKey(seal.bridgeB.x, seal.bridgeB.y)) == 0;
                    if (diagonalPresent && bridgeAFree && bridgeBFree) {
                        const uint64_t a = pixelKey(seal.bridgeA.x, seal.bridgeA.y);
                        const uint64_t b = pixelKey(seal.bridgeB.x, seal.bridgeB.y);
                        sealedWall.insert(a);
                        sealedWall.insert(b);
                        virtualSeals.insert(a);
                        virtualSeals.insert(b);
                    }
                }
            }

            // Flood the outside from the padded corner.
            std::vector<Vec2i> queue { {minX, minY} };
            PixelSet outside { pixelKey(minX, minY) };
            for (size_t i = 0; i < queue.size(); ++i) {
                const Vec2i current = queue[i];
                const Vec2i neighbors[4] = {
                    {current.x - 1, current.y}, {current.x + 1, current.y},
                    {current.x, current.y - 1}, {current.x, current.y + 1},
                };
                for (Vec2i next : neighbors) {
                    if (next.x < minX || next.y < minY || next.x > maxX || next.y > maxY) {
                        continue;
                    }
                    const uint64_t key = pixelKey(next.x, next.y);
                    if (sealedWall.count(key) != 0 || outside.count(key) != 0) {
                        continue;
                    }
                    outside.insert(key);
                    queue.push_back(next);
                }
            }

            bool enclosedAny = false;
            PixelSet interior;
            for (int32_t y = minY + 1; y < maxY; ++y) {
                for (int32_t x = minX + 1; x < maxX; ++x) {
                    const uint64_t key = pixelKey(x, y);
                    if (sealedWall.count(key) == 0 && outside.count(key) == 0) {
                        interior.insert(key);
                        enclosedAny = true;
                    }
                }
            }

            // A virtual seal counts as interior only if it actually touches it.
            for (uint64_t key : virtualSeals) {
                const int32_t x = keyX(key);
                const int32_t y = keyY(key);
                const Vec2i neighbors[4] = { {x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1} };
                for (Vec2i next : neighbors) {
                    if (interior.count(pixelKey(next.x, next.y)) != 0) {
                        interior.insert(key);
                        enclosedAny = true;
                        break;
                    }
                }
            }

            for (uint64_t key : interior) {
                coverage.insert(key);
            }
            if (enclosedAny) {
                for (Vec2i p : component) {
                    boundary.insert(pixelKey(p.x, p.y));
                }
            }
        }
    }

    return { fromPixelSet(coverage), fromPixelSet(boundary) };
}

// ---------------------------------------------------------------------------
// Raster helpers
// ---------------------------------------------------------------------------

IntervalSet maskToIntervals(const RasterBuffer& raster, float alphaThreshold) {
    IntervalSet set;
    if (raster.empty()) {
        return set;
    }
    const int32_t cutoff = static_cast<int32_t>(std::max(0.f, std::min(1.f, alphaThreshold)) * 255.f);

    for (uint32_t y = 0; y < raster.height; ++y) {
        const uint8_t* row = raster.row(y);
        int32_t runStart = 0;
        bool inRun = false;
        for (uint32_t x = 0; x < raster.width; ++x) {
            const bool solid = static_cast<int32_t>(row[x * 4 + 3]) >= cutoff && row[x * 4 + 3] != 0;
            if (solid && !inRun) {
                runStart = static_cast<int32_t>(x);
                inRun = true;
            } else if (!solid && inRun) {
                set.intervals.push_back({static_cast<int32_t>(y), runStart, static_cast<int32_t>(x)});
                inRun = false;
            }
        }
        if (inRun) {
            set.intervals.push_back({static_cast<int32_t>(y), runStart, static_cast<int32_t>(raster.width)});
        }
    }

    return normalize(std::move(set));
}

} // namespace geom

// ---------------------------------------------------------------------------
// Mat3f — row-major 3x3 homogeneous transforms
// ---------------------------------------------------------------------------

Mat3f Mat3f::translation(Vec2f t) {
    Mat3f m;
    m.m[2] = t.x;
    m.m[5] = t.y;
    return m;
}

Mat3f Mat3f::rotation(float angleDegrees) {
    const float radians = angleDegrees * 3.14159265358979323846f / 180.f;
    float c = math::cosf(radians);
    float s = math::sinf(radians);
    // A quarter turn is exact: cos 90 computed is 4e-8, not 0, and that is
    // enough to tip a pattern threshold that sits exactly on a tie.
    const float quarters = angleDegrees / 90.f;
    if (quarters == std::floor(quarters)) {
        const int turn = ((static_cast<int>(quarters) % 4) + 4) % 4;
        c = turn == 0 ? 1.f : turn == 2 ? -1.f : 0.f;
        s = turn == 1 ? 1.f : turn == 3 ? -1.f : 0.f;
    }
    Mat3f m;
    m.m[0] =  c; m.m[1] = -s;
    m.m[3] =  s; m.m[4] =  c;
    return m;
}

Mat3f Mat3f::scaling(Vec2f s) {
    Mat3f m;
    m.m[0] = s.x;
    m.m[4] = s.y;
    return m;
}

Mat3f Mat3f::shearing(Vec2f shear) {
    Mat3f m;
    m.m[1] = shear.x;
    m.m[3] = shear.y;
    return m;
}

Mat3f Mat3f::aroundPivot(const Mat3f& transform, Vec2f pivot) {
    return translation(pivot).mul(transform).mul(translation({-pivot.x, -pivot.y}));
}

Vec2f Mat3f::transformPoint(Vec2f p) const {
    return {
        m[0] * p.x + m[1] * p.y + m[2],
        m[3] * p.x + m[4] * p.y + m[5]
    };
}

Vec2f Mat3f::transformVector(Vec2f v) const {
    return {
        m[0] * v.x + m[1] * v.y,
        m[3] * v.x + m[4] * v.y
    };
}

Mat3f Mat3f::mul(const Mat3f& o) const {
    Mat3f out;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            float sum = 0.f;
            for (int k = 0; k < 3; ++k) {
                sum += m[row * 3 + k] * o.m[k * 3 + col];
            }
            out.m[row * 3 + col] = sum;
        }
    }
    return out;
}

float Mat3f::determinant() const {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) -
           m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}

Result<Mat3f> Mat3f::inverse() const {
    const float det = determinant();
    if (std::fabs(det) < 1e-9f) {
        return Result<Mat3f>::err(LSError::InvalidParameter);
    }
    const float inv = 1.f / det;
    Mat3f out;
    out.m[0] = (m[4] * m[8] - m[5] * m[7]) * inv;
    out.m[1] = (m[2] * m[7] - m[1] * m[8]) * inv;
    out.m[2] = (m[1] * m[5] - m[2] * m[4]) * inv;
    out.m[3] = (m[5] * m[6] - m[3] * m[8]) * inv;
    out.m[4] = (m[0] * m[8] - m[2] * m[6]) * inv;
    out.m[5] = (m[2] * m[3] - m[0] * m[5]) * inv;
    out.m[6] = (m[3] * m[7] - m[4] * m[6]) * inv;
    out.m[7] = (m[1] * m[6] - m[0] * m[7]) * inv;
    out.m[8] = (m[0] * m[4] - m[1] * m[3]) * inv;
    return Result<Mat3f>::ok(out);
}

} // namespace ls
