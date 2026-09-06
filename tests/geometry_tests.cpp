// geometry_tests.cpp — interval sets, boolean ops, morphology, primitives,
// contours, and the authored-pixel region rules.

#include "ls_test.h"

#include <livesprite/livesprite.h>

#include <cmath>

using namespace ls;

namespace {

IntervalSet rect(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    IntervalSet set;
    for (int32_t y = y0; y < y1; ++y) {
        set.intervals.push_back({y, x0, x1});
    }
    return set;
}

void testNormalize() {
    IntervalSet messy;
    messy.intervals.push_back({2, 5, 9});
    messy.intervals.push_back({1, 0, 4});
    messy.intervals.push_back({1, 4, 6});     // abuts the previous span
    messy.intervals.push_back({1, 2, 3});     // fully contained
    messy.intervals.push_back({3, 7, 7});     // empty

    const IntervalSet clean = geom::normalize(messy);
    LS_CHECK(geom::isNormalized(clean));
    LS_CHECK(clean.intervals.size() == 2);
    LS_CHECK(clean.intervals[0].y == 1 && clean.intervals[0].x0 == 0 && clean.intervals[0].x1 == 6);
    LS_CHECK(clean.intervals[1].y == 2);
    LS_CHECK(geom::pixelCount(clean) == 6 + 4);
}

void testBooleanOps() {
    const IntervalSet a = geom::normalize(rect(0, 0, 10, 10));   // 100 px
    const IntervalSet b = geom::normalize(rect(5, 5, 15, 15));   // 100 px

    const IntervalSet unionSet     = geom::unionSets(a, b);
    const IntervalSet intersectSet = geom::intersectSets(a, b);
    const IntervalSet subtractSet  = geom::subtractSets(a, b);
    const IntervalSet xorSet       = geom::xorSets(a, b);

    LS_CHECK(geom::pixelCount(intersectSet) == 25);
    LS_CHECK(geom::pixelCount(unionSet) == 175);
    LS_CHECK(geom::pixelCount(subtractSet) == 75);
    LS_CHECK(geom::pixelCount(xorSet) == 150);
    LS_CHECK(geom::isNormalized(unionSet));

    // Inclusion-exclusion has to hold exactly.
    LS_CHECK(geom::pixelCount(unionSet) ==
             geom::pixelCount(a) + geom::pixelCount(b) - geom::pixelCount(intersectSet));

    const IntervalSet inverted = geom::invertSet(a, {{0, 0}, {20, 20}});
    LS_CHECK(geom::pixelCount(inverted) == 400 - 100);
    LS_CHECK(geom::pixelCount(geom::intersectSets(inverted, a)) == 0);

    // Boolean ops must be stable: union with self changes nothing.
    LS_CHECK(geom::pixelCount(geom::unionSets(a, a)) == 100);
    LS_CHECK(geom::pixelCount(geom::subtractSets(a, a)) == 0);
}

void testMorphology() {
    const IntervalSet square = geom::normalize(rect(4, 4, 8, 8));   // 4x4

    const IntervalSet grown = geom::expand(square, 1.f, true);
    LS_CHECK(geom::pixelCount(grown) == 6 * 6);

    const IntervalSet shrunk = geom::contract(square, 1.f, true);
    LS_CHECK(geom::pixelCount(shrunk) == 2 * 2);

    // Contracting past the shape leaves nothing behind.
    LS_CHECK(geom::pixelCount(geom::contract(square, 4.f, true)) == 0);

    const IntervalSet round = geom::expand(square, 2.f, false);
    LS_CHECK(geom::pixelCount(round) > geom::pixelCount(square));
    LS_CHECK(geom::pixelCount(round) < geom::pixelCount(geom::expand(square, 2.f, true)));

    const IntervalSet edge = geom::boundaryOf(square);
    LS_CHECK(geom::pixelCount(edge) == 16 - 4);     // 4x4 minus its 2x2 interior

    const IntervalSet bigger = geom::normalize(rect(0, 0, 5, 5));
    LS_CHECK(geom::pixelCount(geom::boundaryOf(bigger)) == 25 - 9);
    LS_CHECK(geom::pixelCount(geom::outerBoundaryOf(bigger)) == 49 - 25);
}

void testComponents() {
    IntervalSet two = geom::normalize(rect(0, 0, 3, 3));
    two = geom::unionSets(two, geom::normalize(rect(10, 10, 12, 12)));

    const std::vector<IntervalSet> parts = geom::connectedComponents(two, true);
    LS_CHECK(parts.size() == 2);
    if (parts.size() == 2) {
        LS_CHECK(geom::pixelCount(parts[0]) == 9);
        LS_CHECK(geom::pixelCount(parts[1]) == 4);
    }

    // Diagonal contact: connected under 8-connectivity, separate under 4.
    IntervalSet diagonal;
    diagonal.intervals.push_back({0, 0, 1});
    diagonal.intervals.push_back({1, 1, 2});
    diagonal = geom::normalize(diagonal);
    LS_CHECK(geom::connectedComponents(diagonal, true).size() == 1);
    LS_CHECK(geom::connectedComponents(diagonal, false).size() == 2);
}

void testPrimitives() {
    const IntervalSet rectSet = geom::rasterizeRect({{2.f, 3.f}, 6.f, 4.f, 0.f});
    LS_CHECK(geom::pixelCount(rectSet) == 24);
    LS_CHECK(geom::bounds(rectSet).min.x == 2);
    LS_CHECK(geom::bounds(rectSet).max.y == 7);

    const IntervalSet rounded = geom::rasterizeRect({{0.f, 0.f}, 8.f, 8.f, 3.f});
    LS_CHECK(geom::pixelCount(rounded) < 64);
    LS_CHECK(geom::pixelCount(rounded) > 40);

    const IntervalSet circle = geom::rasterizeCircle({{8.f, 8.f}, 4.f});
    const float area = static_cast<float>(geom::pixelCount(circle));
    LS_CHECK(std::fabs(area - 3.14159f * 16.f) < 12.f);
    const Vec2f center = geom::centroid(circle);
    LS_CHECK(std::fabs(center.x - 8.f) < 0.5f);
    LS_CHECK(std::fabs(center.y - 8.f) < 0.5f);

    PolygonDesc triangle;
    triangle.vertices = { {0.f, 0.f}, {10.f, 0.f}, {0.f, 10.f} };
    const IntervalSet filled = geom::rasterizePolygon(triangle);
    const float triangleArea = static_cast<float>(geom::pixelCount(filled));
    LS_CHECK(std::fabs(triangleArea - 50.f) < 10.f);

    const IntervalSet line = geom::rasterizeLine({{0.f, 0.f}, {9.f, 0.f}});
    LS_CHECK(geom::pixelCount(line) == 10);

    CurveDesc curve;
    curve.segments.push_back({{0.f, 0.f}, {5.f, 0.f}, {5.f, 10.f}, {10.f, 10.f}});
    const std::vector<Vec2f> flattened = geom::flattenCurve(curve);
    LS_CHECK(flattened.size() > 4);
    LS_CHECK(std::fabs(flattened.front().x - 0.f) < 0.001f);
    LS_CHECK(std::fabs(flattened.back().x - 10.f) < 0.001f);
    LS_CHECK(!geom::rasterizeCurve(curve).empty());
}

void testContours() {
    const IntervalSet square = geom::normalize(rect(0, 0, 5, 5));
    const std::vector<ContourDesc> contours = geom::traceContours(square, true);
    LS_CHECK(contours.size() == 1);
    if (!contours.empty()) {
        LS_CHECK(contours[0].outer);
        LS_CHECK(contours[0].points.size() == 20);   // 4 sides x 5 unit edges
    }

    // A square with a hole traces one outer contour and one inner contour.
    const IntervalSet ring = geom::subtractSets(geom::normalize(rect(0, 0, 7, 7)),
                                                geom::normalize(rect(2, 2, 5, 5)));
    const std::vector<ContourDesc> ringContours = geom::traceContours(ring, true);
    LS_CHECK(ringContours.size() == 2);
    int outerCount = 0;
    for (const ContourDesc& contour : ringContours) {
        outerCount += contour.outer ? 1 : 0;
    }
    LS_CHECK(outerCount == 1);
    LS_CHECK(geom::traceContours(ring, false).size() == 1);

    std::vector<Vec2f> path = { {0.f, 0.f}, {1.f, 0.05f}, {2.f, 0.f}, {3.f, 0.f}, {3.f, 3.f} };
    SimplifyParams params;
    params.epsilon = 0.5f;
    const std::vector<Vec2f> simplified = geom::simplifyPath(path, params);
    LS_CHECK(simplified.size() < path.size());
    LS_CHECK(simplified.size() >= 3);
}

void testAuthoredPixels() {
    const Color ink = Color::black();

    // A closed 4x4 ring seals its 2x2 interior.
    PixelRegionDesc loop;
    for (int y = 4; y < 8; ++y) {
        for (int x = 4; x < 8; ++x) {
            if (x == 4 || x == 7 || y == 4 || y == 7) {
                loop.pixels.push_back({{x, y}, ink});
            }
        }
    }
    const geom::PixelRegionResult sealed = geom::buildPixelRegion(loop);
    LS_CHECK(geom::pixelCount(sealed.coverage) == 16);
    LS_CHECK(geom::pixelCount(sealed.boundary) == 12);

    // An open stroke encloses nothing.
    PixelRegionDesc open;
    open.pixels = {
        {{14, 8}, ink}, {{14, 9}, ink}, {{14, 10}, ink}, {{14, 11}, ink},
        {{15, 11}, ink}, {{16, 11}, ink}, {{17, 11}, ink}
    };
    const geom::PixelRegionResult stroke = geom::buildPixelRegion(open);
    LS_CHECK(geom::pixelCount(stroke.coverage) == 7);
    LS_CHECK(stroke.boundary.empty());

    // A diagonal loop still seals: diagonal contacts get bridged.
    PixelRegionDesc diamond;
    diamond.pixels = {
        {{14, 8}, ink}, {{15, 8}, ink},
        {{13, 9}, ink}, {{16, 9}, ink},
        {{12, 10}, ink}, {{17, 10}, ink},
        {{13, 11}, ink}, {{16, 11}, ink},
        {{14, 12}, ink}, {{15, 12}, ink}
    };
    const geom::PixelRegionResult sealedDiamond = geom::buildPixelRegion(diamond);
    LS_CHECK(geom::pixelCount(sealedDiamond.coverage) > static_cast<int64_t>(diamond.pixels.size()));
    LS_CHECK(!sealedDiamond.boundary.empty());

    // Opting out keeps only what was drawn.
    PixelRegionDesc raw = loop;
    raw.closeSameColorBoundaries = false;
    LS_CHECK(geom::pixelCount(geom::buildPixelRegion(raw).coverage) == 12);
}

void testMatrix() {
    const Mat3f rotate90 = Mat3f::rotation(90.f);
    const Vec2f rotated = rotate90.transformPoint({1.f, 0.f});
    LS_CHECK(std::fabs(rotated.x) < 0.001f);
    LS_CHECK(std::fabs(rotated.y - 1.f) < 0.001f);

    const Mat3f aroundPivot = Mat3f::aroundPivot(Mat3f::rotation(180.f), {5.f, 5.f});
    const Vec2f flipped = aroundPivot.transformPoint({6.f, 5.f});
    LS_CHECK(std::fabs(flipped.x - 4.f) < 0.001f);
    LS_CHECK(std::fabs(flipped.y - 5.f) < 0.001f);

    const Mat3f compound = Mat3f::translation({3.f, 4.f}).mul(Mat3f::scaling({2.f, 2.f}));
    const Vec2f mapped = compound.transformPoint({1.f, 1.f});
    LS_CHECK(std::fabs(mapped.x - 5.f) < 0.001f);
    LS_CHECK(std::fabs(mapped.y - 6.f) < 0.001f);

    auto inverse = compound.inverse();
    LS_CHECK(inverse.ok());
    if (inverse.ok()) {
        const Vec2f roundTrip = inverse.value.transformPoint(mapped);
        LS_CHECK(std::fabs(roundTrip.x - 1.f) < 0.001f);
        LS_CHECK(std::fabs(roundTrip.y - 1.f) < 0.001f);
    }
    LS_CHECK(Mat3f::scaling({0.f, 1.f}).inverse().fail());
}

void testDeterminism() {
    // Same inputs, same outputs — the whole engine rests on this.
    const IntervalSet a = geom::rasterizeEllipse({{7.5f, 7.5f}, 5.f, 3.f});
    const IntervalSet b = geom::rasterizeEllipse({{7.5f, 7.5f}, 5.f, 3.f});
    LS_CHECK(a.intervals.size() == b.intervals.size());
    bool identical = a.intervals.size() == b.intervals.size();
    for (size_t i = 0; identical && i < a.intervals.size(); ++i) {
        identical = a.intervals[i] == b.intervals[i];
    }
    LS_CHECK(identical);
}

} // namespace

int main() {
    testNormalize();
    testBooleanOps();
    testMorphology();
    testComponents();
    testPrimitives();
    testContours();
    testAuthoredPixels();
    testMatrix();
    testDeterminism();
    return lstest::report("geometry");
}
