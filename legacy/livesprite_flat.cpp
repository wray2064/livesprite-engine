#include <livesprite/livesprite.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace ls {
namespace {

constexpr float kPi = 3.14159265358979323846f;

std::string pixelKey(int32_t x, int32_t y);

template<typename Id>
uint64_t key(Id id) {
    return id.value;
}

std::string colorSummary(Color color) {
    std::ostringstream out;
    out << "rgba(" << static_cast<int>(color.r) << ", "
        << static_cast<int>(color.g) << ", "
        << static_cast<int>(color.b) << ", "
        << static_cast<int>(color.a) << ")";
    return out.str();
}

Result<RasterBuffer> makeRaster(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return Result<RasterBuffer>::err(LSError::InvalidParameter);
    }

    RasterBuffer raster;
    raster.width = width;
    raster.height = height;
    raster.stride = width * 4;
    try {
        raster.pixels.assign(static_cast<size_t>(raster.stride) * height, 0);
    } catch (...) {
        return Result<RasterBuffer>::err(LSError::RasterAllocationFailed);
    }
    return Result<RasterBuffer>::ok(raster);
}

void setPixel(RasterBuffer& raster, int32_t x, int32_t y, Color color) {
    if (x < 0 || y < 0 || x >= static_cast<int32_t>(raster.width) ||
        y >= static_cast<int32_t>(raster.height)) {
        return;
    }

    uint8_t* px = raster.row(static_cast<uint32_t>(y)) + static_cast<uint32_t>(x) * 4;
    px[0] = color.r;
    px[1] = color.g;
    px[2] = color.b;
    px[3] = color.a;
}

Color getPixel(const RasterBuffer& raster, int32_t x, int32_t y) {
    if (x < 0 || y < 0 || x >= static_cast<int32_t>(raster.width) ||
        y >= static_cast<int32_t>(raster.height)) {
        return {};
    }

    const uint8_t* px = raster.row(static_cast<uint32_t>(y)) + static_cast<uint32_t>(x) * 4;
    return {px[0], px[1], px[2], px[3]};
}

uint32_t packedColorKey(Color color) {
    return (static_cast<uint32_t>(color.r) << 24) |
        (static_cast<uint32_t>(color.g) << 16) |
        (static_cast<uint32_t>(color.b) << 8) |
        static_cast<uint32_t>(color.a);
}

Color colorFromPackedKey(uint32_t key) {
    return {
        static_cast<uint8_t>((key >> 24) & 0xffu),
        static_cast<uint8_t>((key >> 16) & 0xffu),
        static_cast<uint8_t>((key >> 8) & 0xffu),
        static_cast<uint8_t>(key & 0xffu)
    };
}

Color dominantNeighborColor(const RasterBuffer& raster, int32_t x, int32_t y) {
    static constexpr int32_t offsets[8][2] = {
        {-1, 0}, {1, 0}, {0, -1}, {0, 1},
        {-1, -1}, {1, -1}, {-1, 1}, {1, 1}
    };

    std::unordered_map<uint32_t, int32_t> votes;
    for (const auto& offset : offsets) {
        const Color color = getPixel(raster, x + offset[0], y + offset[1]);
        if (color.a == 0) {
            continue;
        }
        const int32_t weight = (std::abs(offset[0]) + std::abs(offset[1]) == 1) ? 2 : 1;
        votes[packedColorKey(color)] += weight;
    }

    uint32_t bestKey = 0;
    int32_t bestVotes = 0;
    for (const auto& [key, count] : votes) {
        if (count > bestVotes) {
            bestKey = key;
            bestVotes = count;
        }
    }

    return bestVotes > 0 ? colorFromPackedKey(bestKey) : Color{};
}

RasterBuffer repairInternalTransparentGaps(const RasterBuffer& raster) {
    RasterBuffer repaired = raster;
    std::unordered_set<std::string> exterior;
    std::deque<Vec2i> queue;

    auto pushTransparent = [&](int32_t x, int32_t y) {
        if (x < 0 || y < 0 ||
            x >= static_cast<int32_t>(raster.width) ||
            y >= static_cast<int32_t>(raster.height)) {
            return;
        }
        const std::string encoded = pixelKey(x, y);
        if (exterior.find(encoded) != exterior.end()) {
            return;
        }
        if (getPixel(raster, x, y).a != 0) {
            return;
        }
        exterior.insert(encoded);
        queue.push_back({x, y});
    };

    for (int32_t i = 0; i < static_cast<int32_t>(raster.width); ++i) {
        pushTransparent(i, 0);
        pushTransparent(i, static_cast<int32_t>(raster.height) - 1);
    }
    for (int32_t i = 0; i < static_cast<int32_t>(raster.height); ++i) {
        pushTransparent(0, i);
        pushTransparent(static_cast<int32_t>(raster.width) - 1, i);
    }

    while (!queue.empty()) {
        const Vec2i current = queue.front();
        queue.pop_front();
        pushTransparent(current.x - 1, current.y);
        pushTransparent(current.x + 1, current.y);
        pushTransparent(current.x, current.y - 1);
        pushTransparent(current.x, current.y + 1);
    }

    for (int32_t y = 0; y < static_cast<int32_t>(raster.height); ++y) {
        for (int32_t x = 0; x < static_cast<int32_t>(raster.width); ++x) {
            if (getPixel(raster, x, y).a != 0) {
                continue;
            }
            if (exterior.find(pixelKey(x, y)) != exterior.end()) {
                continue;
            }
            const Color fill = dominantNeighborColor(raster, x, y);
            if (fill.a != 0) {
                setPixel(repaired, x, y, fill);
            }
        }
    }

    return repaired;
}

Rect2i findBounds(const RasterBuffer& raster) {
    int32_t minX = static_cast<int32_t>(raster.width);
    int32_t minY = static_cast<int32_t>(raster.height);
    int32_t maxX = 0;
    int32_t maxY = 0;

    for (uint32_t y = 0; y < raster.height; ++y) {
        const uint8_t* row = raster.row(y);
        for (uint32_t x = 0; x < raster.width; ++x) {
            if (row[x * 4 + 3] == 0) {
                continue;
            }
            minX = std::min(minX, static_cast<int32_t>(x));
            minY = std::min(minY, static_cast<int32_t>(y));
            maxX = std::max(maxX, static_cast<int32_t>(x + 1));
            maxY = std::max(maxY, static_cast<int32_t>(y + 1));
        }
    }

    if (minX >= maxX || minY >= maxY) {
        return {};
    }
    return {{minX, minY}, {maxX, maxY}};
}

Rect2i boundsFromIntervals(const IntervalSet& intervals) {
    if (intervals.empty()) {
        return {};
    }

    int32_t minX = intervals.intervals.front().x0;
    int32_t maxX = intervals.intervals.front().x1;
    int32_t minY = intervals.intervals.front().y;
    int32_t maxY = intervals.intervals.front().y + 1;

    for (const Interval& interval : intervals.intervals) {
        minX = std::min(minX, interval.x0);
        maxX = std::max(maxX, interval.x1);
        minY = std::min(minY, interval.y);
        maxY = std::max(maxY, interval.y + 1);
    }

    return {{minX, minY}, {maxX, maxY}};
}

IntervalSet makeRectIntervals(Rect2i rect) {
    IntervalSet intervals;
    for (int32_t y = rect.min.y; y < rect.max.y; ++y) {
        intervals.intervals.push_back({y, rect.min.x, rect.max.x});
    }
    return intervals;
}

IntervalSet makeEllipseIntervals(Vec2f center, float radiusX, float radiusY) {
    IntervalSet intervals;
    if (radiusX <= 0.0f || radiusY <= 0.0f) {
        return intervals;
    }

    const int32_t minY = static_cast<int32_t>(std::floor(center.y - radiusY));
    const int32_t maxY = static_cast<int32_t>(std::ceil(center.y + radiusY));

    for (int32_t y = minY; y < maxY; ++y) {
        const float sampleY = (static_cast<float>(y) + 0.5f - center.y) / radiusY;
        const float inside = 1.0f - sampleY * sampleY;
        if (inside < 0.0f) {
            continue;
        }

        const float halfWidth = std::sqrt(inside) * radiusX;
        const int32_t x0 = static_cast<int32_t>(std::floor(center.x - halfWidth));
        const int32_t x1 = static_cast<int32_t>(std::ceil(center.x + halfWidth));
        if (x0 < x1) {
            intervals.intervals.push_back({y, x0, x1});
        }
    }

    return intervals;
}

IntervalSet pixelSetToIntervals(const std::unordered_set<std::string>& pixels) {
    std::unordered_map<int32_t, std::vector<int32_t>> rows;
    for (const std::string& encoded : pixels) {
        const size_t comma = encoded.find(',');
        const int32_t x = std::stoi(encoded.substr(0, comma));
        const int32_t y = std::stoi(encoded.substr(comma + 1));
        rows[y].push_back(x);
    }

    std::vector<int32_t> ys;
    ys.reserve(rows.size());
    for (const auto& [y, unused] : rows) {
        (void)unused;
        ys.push_back(y);
    }
    std::sort(ys.begin(), ys.end());

    IntervalSet intervals;
    for (int32_t y : ys) {
        std::vector<int32_t>& xs = rows[y];
        std::sort(xs.begin(), xs.end());
        int32_t runStart = xs.front();
        int32_t previous = xs.front();
        for (size_t i = 1; i < xs.size(); ++i) {
            if (xs[i] == previous + 1) {
                previous = xs[i];
                continue;
            }
            intervals.intervals.push_back({y, runStart, previous + 1});
            runStart = xs[i];
            previous = xs[i];
        }
        intervals.intervals.push_back({y, runStart, previous + 1});
    }

    return intervals;
}

uint32_t stableHash(int32_t x, int32_t y, uint32_t seed) {
    uint32_t h = seed ^ 0x9e3779b9u;
    h ^= static_cast<uint32_t>(x) + 0x85ebca6bu + (h << 6) + (h >> 2);
    h ^= static_cast<uint32_t>(y) + 0xc2b2ae35u + (h << 6) + (h >> 2);
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

std::string pixelKey(int32_t x, int32_t y) {
    return std::to_string(x) + "," + std::to_string(y);
}

std::unordered_set<std::string> buildPixelSet(const IntervalSet& intervals) {
    std::unordered_set<std::string> pixels;
    for (const Interval& interval : intervals.intervals) {
        for (int32_t x = interval.x0; x < interval.x1; ++x) {
            pixels.insert(pixelKey(x, interval.y));
        }
    }
    return pixels;
}

bool containsPixel(const std::unordered_set<std::string>& pixels, int32_t x, int32_t y) {
    return pixels.find(pixelKey(x, y)) != pixels.end();
}

bool isBoundaryPixel(const std::unordered_set<std::string>& pixels, int32_t x, int32_t y) {
    return !containsPixel(pixels, x - 1, y) ||
        !containsPixel(pixels, x + 1, y) ||
        !containsPixel(pixels, x, y - 1) ||
        !containsPixel(pixels, x, y + 1);
}

bool hasPixelCycle(const std::unordered_set<std::string>& pixels) {
    size_t edgeCount = 0;
    for (const std::string& encoded : pixels) {
        const size_t comma = encoded.find(',');
        const int32_t x = std::stoi(encoded.substr(0, comma));
        const int32_t y = std::stoi(encoded.substr(comma + 1));
        const Vec2i forwardNeighbors[4] = {
            {x + 1, y},
            {x, y + 1},
            {x + 1, y + 1},
            {x - 1, y + 1},
        };
        for (Vec2i next : forwardNeighbors) {
            if (pixels.find(pixelKey(next.x, next.y)) != pixels.end()) {
                ++edgeCount;
            }
        }
    }
    return edgeCount >= pixels.size();
}

int normalizedAngle(float angleDegrees) {
    int angle = static_cast<int>(std::round(angleDegrees)) % 360;
    if (angle < 0) {
        angle += 360;
    }
    return angle;
}

bool isOrthogonalRotation(float angleDegrees) {
    return std::fabs(angleDegrees - std::round(angleDegrees)) < 0.0001f &&
        normalizedAngle(angleDegrees) % 90 == 0;
}

int orthogonalNeighborCount(const std::unordered_set<std::string>& pixels, int32_t x, int32_t y) {
    int count = 0;
    if (containsPixel(pixels, x - 1, y)) ++count;
    if (containsPixel(pixels, x + 1, y)) ++count;
    if (containsPixel(pixels, x, y - 1)) ++count;
    if (containsPixel(pixels, x, y + 1)) ++count;
    return count;
}

std::unordered_set<std::string> cleanupTransformedPixels(
    const std::unordered_set<std::string>& pixels,
    const std::unordered_set<std::string>& protectedPixels,
    uint32_t width,
    uint32_t height
) {
    std::unordered_set<std::string> cleaned = pixels;

    for (int pass = 0; pass < 2; ++pass) {
        std::vector<std::string> additions;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const std::string encoded = pixelKey(static_cast<int32_t>(x), static_cast<int32_t>(y));
                if (cleaned.find(encoded) == cleaned.end() &&
                    orthogonalNeighborCount(cleaned, static_cast<int32_t>(x), static_cast<int32_t>(y)) >= 3) {
                    additions.push_back(encoded);
                }
            }
        }
        for (const std::string& encoded : additions) {
            cleaned.insert(encoded);
        }

        std::vector<std::string> removals;
        for (const std::string& encoded : cleaned) {
            const size_t comma = encoded.find(',');
            const int32_t x = std::stoi(encoded.substr(0, comma));
            const int32_t y = std::stoi(encoded.substr(comma + 1));
            if (protectedPixels.find(encoded) == protectedPixels.end() &&
                orthogonalNeighborCount(cleaned, x, y) <= 1) {
                removals.push_back(encoded);
            }
        }
        for (const std::string& encoded : removals) {
            cleaned.erase(encoded);
        }
    }

    return cleaned;
}

IntervalSet makeBoundaryIntervals(const IntervalSet& intervals) {
    const std::unordered_set<std::string> pixels = buildPixelSet(intervals);
    IntervalSet boundary;
    for (const Interval& interval : intervals.intervals) {
        int32_t runStart = 0;
        bool inRun = false;
        for (int32_t x = interval.x0; x < interval.x1; ++x) {
            if (isBoundaryPixel(pixels, x, interval.y)) {
                if (!inRun) {
                    runStart = x;
                    inRun = true;
                }
            } else if (inRun) {
                boundary.intervals.push_back({interval.y, runStart, x});
                inRun = false;
            }
        }
        if (inRun) {
            boundary.intervals.push_back({interval.y, runStart, interval.x1});
        }
    }
    return boundary;
}

struct PixelRegionBuild {
    IntervalSet coverage;
    IntervalSet boundary;
};

std::string colorKey(Color color) {
    return std::to_string(color.r) + "," +
        std::to_string(color.g) + "," +
        std::to_string(color.b) + "," +
        std::to_string(color.a);
}

PixelRegionBuild makePixelRegion(const PixelRegionDesc& desc) {
    std::unordered_set<std::string> coveragePixels;
    std::unordered_set<std::string> boundaryPixels;
    std::unordered_map<std::string, std::vector<Vec2i>> pixelsByColor;

    for (const PixelInput& pixel : desc.pixels) {
        coveragePixels.insert(pixelKey(pixel.position.x, pixel.position.y));
        pixelsByColor[colorKey(pixel.color)].push_back(pixel.position);
    }

    if (!desc.closeSameColorBoundaries) {
        return {pixelSetToIntervals(coveragePixels), {}};
    }

    for (const auto& [unused, colorPixels] : pixelsByColor) {
        (void)unused;
        if (colorPixels.empty()) {
            continue;
        }

        std::unordered_set<std::string> colorPixelSet;
        for (Vec2i pixel : colorPixels) {
            colorPixelSet.insert(pixelKey(pixel.x, pixel.y));
        }

        std::unordered_set<std::string> visited;
        std::vector<std::vector<Vec2i>> components;
        for (Vec2i seed : colorPixels) {
            const std::string seedKey = pixelKey(seed.x, seed.y);
            if (visited.find(seedKey) != visited.end()) {
                continue;
            }

            std::vector<Vec2i> component;
            std::vector<Vec2i> queue = {seed};
            visited.insert(seedKey);
            for (size_t i = 0; i < queue.size(); ++i) {
                const Vec2i current = queue[i];
                component.push_back(current);
                const Vec2i neighbors[8] = {
                    {current.x - 1, current.y},
                    {current.x + 1, current.y},
                    {current.x, current.y - 1},
                    {current.x, current.y + 1},
                    {current.x - 1, current.y - 1},
                    {current.x + 1, current.y - 1},
                    {current.x - 1, current.y + 1},
                    {current.x + 1, current.y + 1},
                };
                for (Vec2i next : neighbors) {
                    const std::string encoded = pixelKey(next.x, next.y);
                    if (colorPixelSet.find(encoded) == colorPixelSet.end() ||
                        visited.find(encoded) != visited.end()) {
                        continue;
                    }
                    visited.insert(encoded);
                    queue.push_back(next);
                }
            }
            components.push_back(component);
        }

        for (const std::vector<Vec2i>& component : components) {
            if (component.empty()) {
                continue;
            }

            int32_t minX = component.front().x;
            int32_t minY = component.front().y;
            int32_t maxX = component.front().x;
            int32_t maxY = component.front().y;
            std::unordered_set<std::string> wall;
            for (Vec2i pixel : component) {
                minX = std::min(minX, pixel.x);
                minY = std::min(minY, pixel.y);
                maxX = std::max(maxX, pixel.x);
                maxY = std::max(maxY, pixel.y);
                wall.insert(pixelKey(pixel.x, pixel.y));
            }

            if (!hasPixelCycle(wall)) {
                continue;
            }

            minX -= 1;
            minY -= 1;
            maxX += 1;
            maxY += 1;

            std::unordered_set<std::string> sealedWall = wall;
            std::unordered_set<std::string> virtualSeals;
            for (Vec2i pixel : component) {
                struct DiagonalSeal {
                    Vec2i diagonal;
                    Vec2i bridgeA;
                    Vec2i bridgeB;
                };
                const DiagonalSeal seals[4] = {
                    {{pixel.x - 1, pixel.y - 1}, {pixel.x - 1, pixel.y}, {pixel.x, pixel.y - 1}},
                    {{pixel.x + 1, pixel.y - 1}, {pixel.x + 1, pixel.y}, {pixel.x, pixel.y - 1}},
                    {{pixel.x - 1, pixel.y + 1}, {pixel.x - 1, pixel.y}, {pixel.x, pixel.y + 1}},
                    {{pixel.x + 1, pixel.y + 1}, {pixel.x + 1, pixel.y}, {pixel.x, pixel.y + 1}},
                };
                for (const DiagonalSeal& seal : seals) {
                    if (wall.find(pixelKey(seal.diagonal.x, seal.diagonal.y)) != wall.end() &&
                        wall.find(pixelKey(seal.bridgeA.x, seal.bridgeA.y)) == wall.end() &&
                        wall.find(pixelKey(seal.bridgeB.x, seal.bridgeB.y)) == wall.end()) {
                        const std::string bridgeA = pixelKey(seal.bridgeA.x, seal.bridgeA.y);
                        const std::string bridgeB = pixelKey(seal.bridgeB.x, seal.bridgeB.y);
                        sealedWall.insert(bridgeA);
                        sealedWall.insert(bridgeB);
                        virtualSeals.insert(bridgeA);
                        virtualSeals.insert(bridgeB);
                    }
                }
            }

            std::vector<Vec2i> queue = {{minX, minY}};
            std::unordered_set<std::string> outside = {pixelKey(minX, minY)};
            for (size_t i = 0; i < queue.size(); ++i) {
                const Vec2i current = queue[i];
                const Vec2i neighbors[4] = {
                    {current.x - 1, current.y},
                    {current.x + 1, current.y},
                    {current.x, current.y - 1},
                    {current.x, current.y + 1},
                };
                for (Vec2i next : neighbors) {
                    if (next.x < minX || next.y < minY || next.x > maxX || next.y > maxY) {
                        continue;
                    }
                    const std::string encoded = pixelKey(next.x, next.y);
                    if (sealedWall.find(encoded) != sealedWall.end() || outside.find(encoded) != outside.end()) {
                        continue;
                    }
                    outside.insert(encoded);
                    queue.push_back(next);
                }
            }

            bool enclosedAny = false;
            std::unordered_set<std::string> interior;
            for (int32_t y = minY + 1; y < maxY; ++y) {
                for (int32_t x = minX + 1; x < maxX; ++x) {
                    const std::string encoded = pixelKey(x, y);
                    if (sealedWall.find(encoded) == sealedWall.end() && outside.find(encoded) == outside.end()) {
                        interior.insert(encoded);
                        enclosedAny = true;
                    }
                }
            }

            for (const std::string& encoded : virtualSeals) {
                const size_t comma = encoded.find(',');
                const int32_t x = std::stoi(encoded.substr(0, comma));
                const int32_t y = std::stoi(encoded.substr(comma + 1));
                const Vec2i neighbors[4] = {
                    {x - 1, y},
                    {x + 1, y},
                    {x, y - 1},
                    {x, y + 1},
                };
                bool touchesInterior = false;
                for (Vec2i next : neighbors) {
                    if (interior.find(pixelKey(next.x, next.y)) != interior.end()) {
                        touchesInterior = true;
                        break;
                    }
                }
                if (touchesInterior) {
                    interior.insert(encoded);
                    enclosedAny = true;
                }
            }

            for (const std::string& encoded : interior) {
                coveragePixels.insert(encoded);
            }

            if (enclosedAny) {
                for (Vec2i pixel : component) {
                    boundaryPixels.insert(pixelKey(pixel.x, pixel.y));
                }
            }
        }
    }

    return {pixelSetToIntervals(coveragePixels), pixelSetToIntervals(boundaryPixels)};
}

struct TransformIntervalsResult {
    IntervalSet intervals;
    std::unordered_map<std::string, std::pair<int32_t, int32_t>> sourceByOutput;
};

TransformIntervalsResult transformIntervals(
    const IntervalSet& source,
    uint32_t width,
    uint32_t height,
    float angleDegrees,
    Vec2f pivot,
    bool preserveLoosePixels = false,
    const IntervalSet* protectedSource = nullptr
) {
    if (angleDegrees == 0.0f) {
        TransformIntervalsResult result;
        result.intervals = source;
        for (const Interval& interval : source.intervals) {
            for (int32_t x = interval.x0; x < interval.x1; ++x) {
                result.sourceByOutput[pixelKey(x, interval.y)] = {x, interval.y};
            }
        }
        return result;
    }

    std::unordered_set<std::string> outputPixels;
    std::unordered_set<std::string> protectedOutputPixels;
    std::unordered_map<std::string, std::pair<int32_t, int32_t>> sourceByOutput;
    const float radians = angleDegrees * kPi / 180.0f;
    const float c = std::cos(radians);
    const float s = std::sin(radians);

    auto projectForward = [&](const IntervalSet& intervals, bool protect) {
        for (const Interval& interval : intervals.intervals) {
            for (int32_t x = interval.x0; x < interval.x1; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) - pivot.x;
                const float dy = (static_cast<float>(interval.y) + 0.5f) - pivot.y;
                const float outputX = pivot.x + c * dx - s * dy;
                const float outputY = pivot.y + s * dx + c * dy;
                const int32_t roundedX = static_cast<int32_t>(std::floor(outputX));
                const int32_t roundedY = static_cast<int32_t>(std::floor(outputY));
                if (roundedX >= 0 && roundedY >= 0 &&
                    roundedX < static_cast<int32_t>(width) &&
                    roundedY < static_cast<int32_t>(height)) {
                    const std::string encoded = pixelKey(roundedX, roundedY);
                    outputPixels.insert(encoded);
                    sourceByOutput[encoded] = {x, interval.y};
                    if (protect) {
                        protectedOutputPixels.insert(encoded);
                    }
                }
            }
        }
    };

    if (isOrthogonalRotation(angleDegrees)) {
        projectForward(source, false);
    } else {
        const std::unordered_set<std::string> sourcePixels = buildPixelSet(source);
        const bool useSupersampledCoverage = protectedSource != nullptr && !preserveLoosePixels;
        constexpr float sampleOffsets[3] = {1.0f / 6.0f, 0.5f, 5.0f / 6.0f};

        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) - pivot.x;
                const float dy = (static_cast<float>(y) + 0.5f) - pivot.y;
                const float sourceX = pivot.x + c * dx + s * dy;
                const float sourceY = pivot.y - s * dx + c * dy;

                int hitCount = 0;
                int32_t mappedSourceX = static_cast<int32_t>(std::floor(sourceX));
                int32_t mappedSourceY = static_cast<int32_t>(std::floor(sourceY));

                if (useSupersampledCoverage) {
                    bool capturedFirstHit = false;
                    for (float oy : sampleOffsets) {
                        for (float ox : sampleOffsets) {
                            const float sampleDx = (static_cast<float>(x) + ox) - pivot.x;
                            const float sampleDy = (static_cast<float>(y) + oy) - pivot.y;
                            const int32_t sampleSourceX = static_cast<int32_t>(std::floor(pivot.x + c * sampleDx + s * sampleDy));
                            const int32_t sampleSourceY = static_cast<int32_t>(std::floor(pivot.y - s * sampleDx + c * sampleDy));
                            if (containsPixel(sourcePixels, sampleSourceX, sampleSourceY)) {
                                ++hitCount;
                                if (!capturedFirstHit) {
                                    mappedSourceX = sampleSourceX;
                                    mappedSourceY = sampleSourceY;
                                    capturedFirstHit = true;
                                }
                            }
                        }
                    }
                } else if (containsPixel(sourcePixels, mappedSourceX, mappedSourceY)) {
                    hitCount = 9;
                }

                if (hitCount >= 6) {
                    const std::string encoded = pixelKey(static_cast<int32_t>(x), static_cast<int32_t>(y));
                    outputPixels.insert(encoded);
                    sourceByOutput[encoded] = {mappedSourceX, mappedSourceY};
                }
            }
        }

        if (preserveLoosePixels) {
            projectForward(source, true);
        } else if (protectedSource != nullptr) {
            projectForward(*protectedSource, true);
        }
    }

    const std::unordered_set<std::string> cleanedPixels = isOrthogonalRotation(angleDegrees) || preserveLoosePixels
        ? outputPixels
        : cleanupTransformedPixels(outputPixels, protectedOutputPixels, width, height);

    TransformIntervalsResult result;
    result.intervals = pixelSetToIntervals(cleanedPixels);

    for (const auto& [encoded, sourcePixel] : sourceByOutput) {
        if (cleanedPixels.find(encoded) != cleanedPixels.end()) {
            result.sourceByOutput[encoded] = sourcePixel;
        }
    }

    return result;
}

RasterBuffer rotateRaster(const RasterBuffer& source, float angleDegrees, Vec2f pivot) {
    auto outResult = makeRaster(source.width, source.height);
    RasterBuffer out = outResult.value;

    const float radians = angleDegrees * kPi / 180.0f;
    const float c = std::cos(radians);
    const float s = std::sin(radians);

    for (uint32_t y = 0; y < out.height; ++y) {
        for (uint32_t x = 0; x < out.width; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f) - pivot.x;
            const float dy = (static_cast<float>(y) + 0.5f) - pivot.y;
            const float srcX = pivot.x + c * dx + s * dy;
            const float srcY = pivot.y - s * dx + c * dy;
            Color color = getPixel(
                source,
                static_cast<int32_t>(std::floor(srcX)),
                static_cast<int32_t>(std::floor(srcY))
            );
            if (color.a != 0) {
                setPixel(out, static_cast<int32_t>(x), static_cast<int32_t>(y), color);
            }
        }
    }

    return repairInternalTransparentGaps(out);
}

} // namespace

struct SolidFillOp {
    RegionId region;
    Color color;
};

struct DitherFillOp {
    RegionId region;
    Color colorA;
    Color colorB;
    float density;
    uint32_t seed;
    DitherSpace space;
};

struct RotateOp {
    float angleDegrees;
    Vec2f pivot;
};

using Operation = std::variant<SolidFillOp, DitherFillOp, RotateOp>;

struct DocumentData {
    uint32_t canvasWidth = 0;
    uint32_t canvasHeight = 0;
    std::vector<SpriteId> sprites;
};

struct SpriteData {
    DocumentId document;
    std::vector<LayerId> layers;
};

struct LayerData {
    SpriteId sprite;
    std::string name;
    std::vector<OperationId> operations;
};

struct RegionData {
    DocumentId document;
    Rect2i rect;
    IntervalSet intervals;
    IntervalSet authoredBoundary;
    bool preserveLoosePixels = false;
};

struct OperationData {
    LayerId layer;
    Operation operation;
};

struct LSContext::Impl {
    uint64_t nextId = 1;

    std::unordered_map<uint64_t, DocumentData> documents;
    std::unordered_map<uint64_t, SpriteData> sprites;
    std::unordered_map<uint64_t, LayerData> layers;
    std::unordered_map<uint64_t, RegionData> regions;
    std::unordered_map<uint64_t, OperationData> operations;

    uint64_t allocId() { return nextId++; }
};

std::unique_ptr<LSContext> LSContext::create() {
    return std::unique_ptr<LSContext>(new LSContext());
}

LSContext::LSContext() : impl_(std::make_unique<Impl>()) {}
LSContext::~LSContext() = default;

uint32_t LSContext::engineVersion() const {
    return LS_ENGINE_VERSION;
}

Result<DocumentId> LSContext::createDocument(uint32_t canvasWidth, uint32_t canvasHeight) {
    if (canvasWidth == 0 || canvasHeight == 0) {
        return Result<DocumentId>::err(LSError::InvalidParameter);
    }

    DocumentId id{impl_->allocId()};
    impl_->documents[key(id)] = {canvasWidth, canvasHeight, {}};
    return Result<DocumentId>::ok(id);
}

Result<SpriteId> LSContext::createSprite(DocumentId document) {
    auto docIt = impl_->documents.find(key(document));
    if (docIt == impl_->documents.end()) {
        return Result<SpriteId>::err(LSError::InvalidId);
    }

    SpriteId id{impl_->allocId()};
    impl_->sprites[key(id)] = {document, {}};
    docIt->second.sprites.push_back(id);
    return Result<SpriteId>::ok(id);
}

Result<LayerId> LSContext::createLayer(SpriteId sprite, std::string_view name) {
    auto spriteIt = impl_->sprites.find(key(sprite));
    if (spriteIt == impl_->sprites.end()) {
        return Result<LayerId>::err(LSError::InvalidId);
    }

    LayerId id{impl_->allocId()};
    impl_->layers[key(id)] = {sprite, std::string(name), {}};
    spriteIt->second.layers.push_back(id);
    return Result<LayerId>::ok(id);
}

Result<RegionId> LSContext::createRectRegion(DocumentId document, const RectRegionDesc& desc) {
    auto docIt = impl_->documents.find(key(document));
    if (docIt == impl_->documents.end()) {
        return Result<RegionId>::err(LSError::InvalidId);
    }
    if (desc.rect.empty()) {
        return Result<RegionId>::err(LSError::InvalidParameter);
    }

    IntervalSet intervals = makeRectIntervals(desc.rect);

    RegionId id{impl_->allocId()};
    impl_->regions[key(id)] = {document, desc.rect, intervals, {}, false};
    return Result<RegionId>::ok(id);
}

Result<RegionId> LSContext::createEllipseRegion(DocumentId document, const EllipseRegionDesc& desc) {
    auto docIt = impl_->documents.find(key(document));
    if (docIt == impl_->documents.end()) {
        return Result<RegionId>::err(LSError::InvalidId);
    }
    if (desc.radiusX <= 0.0f || desc.radiusY <= 0.0f) {
        return Result<RegionId>::err(LSError::InvalidParameter);
    }

    IntervalSet intervals = makeEllipseIntervals(desc.center, desc.radiusX, desc.radiusY);
    if (intervals.empty()) {
        return Result<RegionId>::err(LSError::InvalidParameter);
    }

    RegionId id{impl_->allocId()};
    impl_->regions[key(id)] = {document, boundsFromIntervals(intervals), intervals, {}, false};
    return Result<RegionId>::ok(id);
}

Result<RegionId> LSContext::createPixelRegion(DocumentId document, const PixelRegionDesc& desc) {
    auto docIt = impl_->documents.find(key(document));
    if (docIt == impl_->documents.end()) {
        return Result<RegionId>::err(LSError::InvalidId);
    }
    if (desc.pixels.empty()) {
        return Result<RegionId>::err(LSError::InvalidParameter);
    }

    PixelRegionBuild built = makePixelRegion(desc);
    if (built.coverage.empty()) {
        return Result<RegionId>::err(LSError::InvalidParameter);
    }

    RegionId id{impl_->allocId()};
    impl_->regions[key(id)] = {
        document,
        boundsFromIntervals(built.coverage),
        built.coverage,
        built.boundary,
        built.boundary.empty()
    };
    return Result<RegionId>::ok(id);
}

Result<IntervalSet> LSContext::getRegionIntervals(RegionId region) const {
    auto it = impl_->regions.find(key(region));
    if (it == impl_->regions.end()) {
        return Result<IntervalSet>::err(LSError::InvalidId);
    }
    return Result<IntervalSet>::ok(it->second.intervals);
}

Result<IntervalSet> LSContext::getRegionBoundaryIntervals(RegionId region) const {
    auto it = impl_->regions.find(key(region));
    if (it == impl_->regions.end()) {
        return Result<IntervalSet>::err(LSError::InvalidId);
    }
    if (it->second.preserveLoosePixels || !it->second.authoredBoundary.empty()) {
        return Result<IntervalSet>::ok(it->second.authoredBoundary);
    }
    return Result<IntervalSet>::ok(makeBoundaryIntervals(it->second.intervals));
}

Result<OperationId> LSContext::addSolidFill(LayerId layer, const SolidFillDesc& desc) {
    auto layerIt = impl_->layers.find(key(layer));
    if (layerIt == impl_->layers.end() || impl_->regions.find(key(desc.region)) == impl_->regions.end()) {
        return Result<OperationId>::err(LSError::InvalidId);
    }

    OperationId id{impl_->allocId()};
    impl_->operations[key(id)] = {layer, SolidFillOp{desc.region, desc.color}};
    layerIt->second.operations.push_back(id);
    return Result<OperationId>::ok(id);
}

Result<OperationId> LSContext::addDitherFill(LayerId layer, const DitherFillDesc& desc) {
    auto layerIt = impl_->layers.find(key(layer));
    if (layerIt == impl_->layers.end() || impl_->regions.find(key(desc.region)) == impl_->regions.end()) {
        return Result<OperationId>::err(LSError::InvalidId);
    }
    if (desc.density < 0.0f || desc.density > 1.0f) {
        return Result<OperationId>::err(LSError::InvalidParameter);
    }

    OperationId id{impl_->allocId()};
    impl_->operations[key(id)] = {
        layer,
        DitherFillOp{desc.region, desc.colorA, desc.colorB, desc.density, desc.seed, desc.space}
    };
    layerIt->second.operations.push_back(id);
    return Result<OperationId>::ok(id);
}

Result<OperationId> LSContext::addRotate(LayerId layer, const RotateDesc& desc) {
    auto layerIt = impl_->layers.find(key(layer));
    if (layerIt == impl_->layers.end()) {
        return Result<OperationId>::err(LSError::InvalidId);
    }

    OperationId id{impl_->allocId()};
    impl_->operations[key(id)] = {layer, RotateOp{desc.angleDegrees, desc.pivot}};
    layerIt->second.operations.push_back(id);
    return Result<OperationId>::ok(id);
}

Result<std::vector<OperationInfo>> LSContext::getLayerOperations(LayerId layer) const {
    auto layerIt = impl_->layers.find(key(layer));
    if (layerIt == impl_->layers.end()) {
        return Result<std::vector<OperationInfo>>::err(LSError::InvalidId);
    }

    std::vector<OperationInfo> infos;
    for (OperationId id : layerIt->second.operations) {
        const Operation& op = impl_->operations.at(key(id)).operation;
        OperationInfo info;
        info.id = id;

        if (const auto* solid = std::get_if<SolidFillOp>(&op)) {
            info.type = "SolidFill";
            info.summary = "solidFill(region:" + std::to_string(solid->region.value) +
                ", color:" + colorSummary(solid->color) + ")";
        } else if (const auto* dither = std::get_if<DitherFillOp>(&op)) {
            info.type = "DitherFill";
            info.summary = "ditherFill(region:" + std::to_string(dither->region.value) +
                ", density:" + std::to_string(dither->density) +
                ", seed:" + std::to_string(dither->seed) + ")";
        } else if (const auto* rotate = std::get_if<RotateOp>(&op)) {
            info.type = "Rotate";
            info.summary = "rotate(angle:" + std::to_string(rotate->angleDegrees) +
                ", pivot:" + std::to_string(rotate->pivot.x) + "," +
                std::to_string(rotate->pivot.y) + ")";
        }

        infos.push_back(info);
    }

    return Result<std::vector<OperationInfo>>::ok(infos);
}

Result<CompileResult> LSContext::compileLayer(LayerId layer, const CompileProfile& profile) const {
    auto layerIt = impl_->layers.find(key(layer));
    if (layerIt == impl_->layers.end()) {
        return Result<CompileResult>::err(LSError::InvalidId);
    }

    auto rasterResult = makeRaster(profile.outputWidth, profile.outputHeight);
    if (rasterResult.fail()) {
        return Result<CompileResult>::err(rasterResult.error);
    }

    CompileResult result;
    result.raster = rasterResult.value;
    const RotateOp* layerRotate = nullptr;

    if (profile.resolveTransforms) {
        for (OperationId opId : layerIt->second.operations) {
            const Operation& op = impl_->operations.at(key(opId)).operation;
            if (const auto* rotate = std::get_if<RotateOp>(&op)) {
                layerRotate = rotate;
            }
        }
    }

    for (OperationId opId : layerIt->second.operations) {
        const Operation& op = impl_->operations.at(key(opId)).operation;

        if (const auto* solid = std::get_if<SolidFillOp>(&op)) {
            const RegionData& region = impl_->regions.at(key(solid->region));
            for (const Interval& interval : region.intervals.intervals) {
                for (int32_t x = interval.x0; x < interval.x1; ++x) {
                    setPixel(result.raster, x, interval.y, solid->color);
                }
            }
            result.trace.push_back("op " + std::to_string(opId.value) +
                ": resolved solid fill over region " + std::to_string(solid->region.value));
        } else if (const auto* dither = std::get_if<DitherFillOp>(&op)) {
            const RegionData& region = impl_->regions.at(key(dither->region));
            for (const Interval& interval : region.intervals.intervals) {
                for (int32_t x = interval.x0; x < interval.x1; ++x) {
                    int32_t sampleX = x;
                    int32_t sampleY = interval.y;
                    const float threshold = static_cast<float>(stableHash(sampleX, sampleY, dither->seed) % 10000u) / 10000.0f;
                    setPixel(result.raster, x, interval.y, threshold < dither->density ? dither->colorA : dither->colorB);
                }
            }
            result.trace.push_back("op " + std::to_string(opId.value) +
                ": resolved object-space dither over region " + std::to_string(dither->region.value));
        } else if (const auto* rotate = std::get_if<RotateOp>(&op)) {
            if (profile.resolveTransforms) {
                result.trace.push_back("op " + std::to_string(opId.value) +
                    ": transformed source intervals before rasterization");
            } else {
                result.trace.push_back("op " + std::to_string(opId.value) +
                    ": transform recorded but not resolved");
            }
        }
    }

    if (layerRotate != nullptr) {
        result.raster = rotateRaster(result.raster, layerRotate->angleDegrees, layerRotate->pivot);
        result.trace.push_back("resolved layer transform by rotating composed raster");
    }

    result.bounds = findBounds(result.raster);
    return Result<CompileResult>::ok(result);
}

Result<CompileResult> LSContext::compileSprite(SpriteId sprite, const CompileProfile& profile) const {
    auto spriteIt = impl_->sprites.find(key(sprite));
    if (spriteIt == impl_->sprites.end()) {
        return Result<CompileResult>::err(LSError::InvalidId);
    }

    auto rasterResult = makeRaster(profile.outputWidth, profile.outputHeight);
    if (rasterResult.fail()) {
        return Result<CompileResult>::err(rasterResult.error);
    }

    CompileResult result;
    result.raster = rasterResult.value;

    for (LayerId layer : spriteIt->second.layers) {
        auto layerResult = compileLayer(layer, profile);
        if (layerResult.fail()) {
            return Result<CompileResult>::err(layerResult.error);
        }

        for (uint32_t y = 0; y < result.raster.height; ++y) {
            const uint8_t* src = layerResult.value.raster.row(y);
            uint8_t* dst = result.raster.row(y);
            for (uint32_t x = 0; x < result.raster.width; ++x) {
                if (src[x * 4 + 3] != 0) {
                    dst[x * 4 + 0] = src[x * 4 + 0];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 2];
                    dst[x * 4 + 3] = src[x * 4 + 3];
                }
            }
        }

        result.trace.insert(result.trace.end(), layerResult.value.trace.begin(), layerResult.value.trace.end());
    }

    result.bounds = findBounds(result.raster);
    return Result<CompileResult>::ok(result);
}

std::string_view lsErrorString(LSError error) {
    switch (error) {
        case LSError::None: return "None";
        case LSError::InvalidId: return "InvalidId";
        case LSError::InvalidParameter: return "InvalidParameter";
        case LSError::RasterAllocationFailed: return "RasterAllocationFailed";
        default: return "Unknown";
    }
}

} // namespace ls
