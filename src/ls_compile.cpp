// ls_compile.cpp — raster helpers and the compilation pipeline.
//
// The pipeline follows the spec order:
//   dependency/cache lookup -> operation stack resolution -> region compilation
//   -> fill/stroke rasterization -> transform + deform resolution
//   -> layer compositing -> sampling -> palette quantization -> alpha policy.
//
// Two rules shape everything here:
//   1. Marks are rasterized from regions and geometry, never from a stored
//      bitmap. A transform re-resolves coverage geometrically and carries the
//      colour of the source pixel across, so a rotation never interpolates
//      pixel-art colours into mud.
//   2. Patterns are evaluated in their declared coordinate space *before* a
//      transform is applied, so a dithered sprite does not shimmer when it
//      moves.

#include "ls_internal.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace ls {

// ---------------------------------------------------------------------------
// Raster helpers
// ---------------------------------------------------------------------------

Result<RasterBuffer> allocateRaster(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return Result<RasterBuffer>::err(LSError::InvalidParameter);
    }
    RasterBuffer raster;
    raster.width = width;
    raster.height = height;
    raster.stride = width * 4;
    const size_t bytes = static_cast<size_t>(raster.stride) * height;
    raster.pixels.resize(bytes, 0);
    if (raster.pixels.size() != bytes) {
        return Result<RasterBuffer>::err(LSError::RasterAllocationFailed);
    }
    return Result<RasterBuffer>::ok(std::move(raster));
}

void setRasterPixel(RasterBuffer& raster, int32_t x, int32_t y, Color color) {
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

Color getRasterPixel(const RasterBuffer& raster, int32_t x, int32_t y) {
    if (x < 0 || y < 0 ||
        x >= static_cast<int32_t>(raster.width) ||
        y >= static_cast<int32_t>(raster.height)) {
        return Color::transparent();
    }
    const uint8_t* px = raster.row(static_cast<uint32_t>(y)) + static_cast<uint32_t>(x) * 4;
    return { px[0], px[1], px[2], px[3] };
}

Rect2i rasterBounds(const RasterBuffer& raster) {
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
            maxX = std::max(maxX, static_cast<int32_t>(x) + 1);
            maxY = std::max(maxY, static_cast<int32_t>(y) + 1);
        }
    }

    if (minX >= maxX || minY >= maxY) {
        return {};
    }
    return { {minX, minY}, {maxX, maxY} };
}

namespace {

constexpr float kPi = 3.14159265358979323846f;

inline float toFloat(uint8_t channel) { return static_cast<float>(channel) / 255.f; }

inline uint8_t toByte(float value) {
    const float clamped = std::max(0.f, std::min(1.f, value));
    return static_cast<uint8_t>(clamped * 255.f + 0.5f);
}

inline float clamp01(float value) { return std::max(0.f, std::min(1.f, value)); }

float blendChannel(float dst, float src, BlendMode mode) {
    switch (mode) {
        case BlendMode::Normal:     return src;
        case BlendMode::Multiply:   return dst * src;
        case BlendMode::Screen:     return dst + src - dst * src;
        case BlendMode::Overlay:    return dst <= 0.5f ? 2.f * dst * src
                                                       : 1.f - 2.f * (1.f - dst) * (1.f - src);
        case BlendMode::Add:        return dst + src;
        case BlendMode::Subtract:   return dst - src;
        case BlendMode::Darken:     return std::min(dst, src);
        case BlendMode::Lighten:    return std::max(dst, src);
        case BlendMode::Difference: return std::fabs(dst - src);
        case BlendMode::Erase:      return dst;
        case BlendMode::Replace:    return src;
    }
    return src;
}

uint32_t stableHash(int32_t x, int32_t y, uint32_t seed) {
    uint32_t h = seed ^ 0x9e3779b9u;
    h ^= static_cast<uint32_t>(x) + 0x85ebca6bu + (h << 6) + (h >> 2);
    h ^= static_cast<uint32_t>(y) + 0xc2b2ae35u + (h << 6) + (h >> 2);
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

inline uint64_t pixelKey(int32_t x, int32_t y) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
            static_cast<uint64_t>(static_cast<uint32_t>(y));
}
inline int32_t keyX(uint64_t key) { return static_cast<int32_t>(key >> 32); }
inline int32_t keyY(uint64_t key) { return static_cast<int32_t>(key & 0xffffffffu); }

float applyFalloff(float t, Falloff falloff) {
    const float clamped = clamp01(t);
    switch (falloff) {
        case Falloff::Linear:   return clamped;
        case Falloff::Smooth:   return clamped * clamped * (3.f - 2.f * clamped);
        case Falloff::Sharp:    return clamped * clamped;
        case Falloff::Cosine:   return 0.5f - 0.5f * std::cos(clamped * kPi);
        case Falloff::Constant: return clamped > 0.f ? 1.f : 0.f;
    }
    return clamped;
}

// -------------------------------------------------------------------------
// Repair: fill single-pixel holes that a forward-mapped transform can leave
// behind, using the dominant surrounding colour. Only interior holes are
// touched; the silhouette is left alone.
// -------------------------------------------------------------------------
RasterBuffer repairInteriorGaps(const RasterBuffer& raster) {
    RasterBuffer repaired = raster;
    std::unordered_set<uint64_t> exterior;
    std::deque<uint64_t> queue;

    auto pushTransparent = [&](int32_t x, int32_t y) {
        if (x < 0 || y < 0 ||
            x >= static_cast<int32_t>(raster.width) ||
            y >= static_cast<int32_t>(raster.height)) {
            return;
        }
        const uint64_t key = pixelKey(x, y);
        if (exterior.count(key) != 0 || getRasterPixel(raster, x, y).a != 0) {
            return;
        }
        exterior.insert(key);
        queue.push_back(key);
    };

    for (int32_t x = 0; x < static_cast<int32_t>(raster.width); ++x) {
        pushTransparent(x, 0);
        pushTransparent(x, static_cast<int32_t>(raster.height) - 1);
    }
    for (int32_t y = 0; y < static_cast<int32_t>(raster.height); ++y) {
        pushTransparent(0, y);
        pushTransparent(static_cast<int32_t>(raster.width) - 1, y);
    }
    while (!queue.empty()) {
        const uint64_t key = queue.front();
        queue.pop_front();
        pushTransparent(keyX(key) - 1, keyY(key));
        pushTransparent(keyX(key) + 1, keyY(key));
        pushTransparent(keyX(key), keyY(key) - 1);
        pushTransparent(keyX(key), keyY(key) + 1);
    }

    static constexpr int32_t kOffsets[8][2] = {
        {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}
    };

    for (int32_t y = 0; y < static_cast<int32_t>(raster.height); ++y) {
        for (int32_t x = 0; x < static_cast<int32_t>(raster.width); ++x) {
            if (getRasterPixel(raster, x, y).a != 0 || exterior.count(pixelKey(x, y)) != 0) {
                continue;
            }
            std::map<uint32_t, int32_t> votes;
            for (const auto& offset : kOffsets) {
                const Color neighbor = getRasterPixel(raster, x + offset[0], y + offset[1]);
                if (neighbor.a == 0) {
                    continue;
                }
                const uint32_t packed = (static_cast<uint32_t>(neighbor.r) << 24) |
                                        (static_cast<uint32_t>(neighbor.g) << 16) |
                                        (static_cast<uint32_t>(neighbor.b) << 8) |
                                         static_cast<uint32_t>(neighbor.a);
                votes[packed] += (std::abs(offset[0]) + std::abs(offset[1]) == 1) ? 2 : 1;
            }
            uint32_t bestKey = 0;
            int32_t bestVotes = 0;
            for (const auto& [packed, count] : votes) {
                if (count > bestVotes) {
                    bestKey = packed;
                    bestVotes = count;
                }
            }
            if (bestVotes > 0) {
                setRasterPixel(repaired, x, y, Color{
                    static_cast<uint8_t>((bestKey >> 24) & 0xffu),
                    static_cast<uint8_t>((bestKey >> 16) & 0xffu),
                    static_cast<uint8_t>((bestKey >> 8) & 0xffu),
                    static_cast<uint8_t>(bestKey & 0xffu)
                });
            }
        }
    }

    return repaired;
}

int orthogonalNeighbors(const RasterBuffer& raster, int32_t x, int32_t y) {
    int count = 0;
    count += getRasterPixel(raster, x - 1, y).a != 0 ? 1 : 0;
    count += getRasterPixel(raster, x + 1, y).a != 0 ? 1 : 0;
    count += getRasterPixel(raster, x, y - 1).a != 0 ? 1 : 0;
    count += getRasterPixel(raster, x, y + 1).a != 0 ? 1 : 0;
    return count;
}

// Pixel-art cleanup after a non-orthogonal transform: close notches that have
// three filled orthogonal neighbours, drop pixels dangling by one. Pixels the
// forward projection guaranteed (protected) are never dropped.
RasterBuffer cleanupTransformed(const RasterBuffer& raster,
                                const std::unordered_map<uint64_t, Color>& protectedPixels) {
    RasterBuffer cleaned = raster;

    for (int pass = 0; pass < 2; ++pass) {
        std::vector<std::pair<uint64_t, Color>> additions;
        for (int32_t y = 0; y < static_cast<int32_t>(cleaned.height); ++y) {
            for (int32_t x = 0; x < static_cast<int32_t>(cleaned.width); ++x) {
                if (getRasterPixel(cleaned, x, y).a != 0) {
                    continue;
                }
                if (orthogonalNeighbors(cleaned, x, y) < 3) {
                    continue;
                }
                std::map<uint32_t, int32_t> votes;
                Color pick = Color::transparent();
                int32_t best = 0;
                const Vec2i neighbors[4] = { {x-1,y}, {x+1,y}, {x,y-1}, {x,y+1} };
                for (Vec2i n : neighbors) {
                    const Color color = getRasterPixel(cleaned, n.x, n.y);
                    if (color.a == 0) {
                        continue;
                    }
                    const uint32_t packed = (static_cast<uint32_t>(color.r) << 24) |
                                            (static_cast<uint32_t>(color.g) << 16) |
                                            (static_cast<uint32_t>(color.b) << 8) |
                                             static_cast<uint32_t>(color.a);
                    const int32_t count = ++votes[packed];
                    if (count > best) {
                        best = count;
                        pick = color;
                    }
                }
                if (pick.a != 0) {
                    additions.emplace_back(pixelKey(x, y), pick);
                }
            }
        }
        for (const auto& [key, color] : additions) {
            setRasterPixel(cleaned, keyX(key), keyY(key), color);
        }

        std::vector<uint64_t> removals;
        for (int32_t y = 0; y < static_cast<int32_t>(cleaned.height); ++y) {
            for (int32_t x = 0; x < static_cast<int32_t>(cleaned.width); ++x) {
                if (getRasterPixel(cleaned, x, y).a == 0) {
                    continue;
                }
                if (protectedPixels.count(pixelKey(x, y)) != 0) {
                    continue;
                }
                if (orthogonalNeighbors(cleaned, x, y) <= 1) {
                    removals.push_back(pixelKey(x, y));
                }
            }
        }
        for (uint64_t key : removals) {
            setRasterPixel(cleaned, keyX(key), keyY(key), Color::transparent());
        }
    }

    return cleaned;
}

bool isOrthogonalTransform(const Mat3f& matrix) {
    auto nearInteger = [](float value) {
        return std::fabs(value - std::round(value)) < 1e-4f;
    };
    // A transform is grid-preserving when every basis vector maps onto an axis
    // with unit length: 90-degree turns, mirrors, integer translations.
    return nearInteger(matrix.m[0]) && nearInteger(matrix.m[1]) &&
           nearInteger(matrix.m[3]) && nearInteger(matrix.m[4]) &&
           nearInteger(matrix.m[2]) && nearInteger(matrix.m[5]) &&
           std::fabs(std::fabs(matrix.m[0]) + std::fabs(matrix.m[1]) - 1.f) < 1e-4f &&
           std::fabs(std::fabs(matrix.m[3]) + std::fabs(matrix.m[4]) - 1.f) < 1e-4f;
}

// -------------------------------------------------------------------------
// Geometric transform of a colour field.
//
// Coverage is decided by inverse-mapped sub-samples (never by interpolating
// colours), and each output pixel takes the colour of the source pixel it
// sampled. Forward projection guarantees thin features survive.
// -------------------------------------------------------------------------
using SamplePolicyFn = std::function<Color(const std::vector<Color>&)>;

RasterBuffer transformRaster(const RasterBuffer& source, const Mat3f& matrix,
                             SamplingPolicy sampling, float coverageThreshold,
                             const SamplePolicyFn& policy = {}) {
    auto allocated = allocateRaster(source.width, source.height);
    if (allocated.fail()) {
        return source;
    }
    RasterBuffer out = allocated.value;

    auto inverse = matrix.inverse();
    if (inverse.fail()) {
        return out;   // a degenerate transform collapses the content
    }
    const Mat3f invert = inverse.value;

    std::unordered_map<uint64_t, Color> projected;
    auto forwardProject = [&]() {
        for (int32_t y = 0; y < static_cast<int32_t>(source.height); ++y) {
            for (int32_t x = 0; x < static_cast<int32_t>(source.width); ++x) {
                const Color color = getRasterPixel(source, x, y);
                if (color.a == 0) {
                    continue;
                }
                const Vec2f mapped = matrix.transformPoint({ static_cast<float>(x) + 0.5f,
                                                             static_cast<float>(y) + 0.5f });
                const int32_t ox = static_cast<int32_t>(std::floor(mapped.x));
                const int32_t oy = static_cast<int32_t>(std::floor(mapped.y));
                if (ox < 0 || oy < 0 ||
                    ox >= static_cast<int32_t>(out.width) ||
                    oy >= static_cast<int32_t>(out.height)) {
                    continue;
                }
                projected[pixelKey(ox, oy)] = color;
            }
        }
    };

    if (isOrthogonalTransform(matrix)) {
        // Grid-preserving: forward projection is exact and loses nothing.
        forwardProject();
        for (const auto& [key, color] : projected) {
            setRasterPixel(out, keyX(key), keyY(key), color);
        }
        return out;
    }

    const bool singleSample = sampling == SamplingPolicy::Center && !policy;
    const float threshold = clamp01(coverageThreshold);
    constexpr float kOffsets[3] = { 1.f / 6.f, 0.5f, 5.f / 6.f };

    for (int32_t y = 0; y < static_cast<int32_t>(out.height); ++y) {
        for (int32_t x = 0; x < static_cast<int32_t>(out.width); ++x) {
            if (singleSample) {
                const Vec2f mapped = invert.transformPoint({ static_cast<float>(x) + 0.5f,
                                                             static_cast<float>(y) + 0.5f });
                const Color color = getRasterPixel(source,
                                                   static_cast<int32_t>(std::floor(mapped.x)),
                                                   static_cast<int32_t>(std::floor(mapped.y)));
                if (color.a != 0) {
                    setRasterPixel(out, x, y, color);
                }
                continue;
            }

            std::map<uint32_t, int> votes;
            std::vector<Color> samples;
            int hits = 0;
            Color firstHit = Color::transparent();
            Color majority = Color::transparent();
            int majorityVotes = 0;
            float sumR = 0.f, sumG = 0.f, sumB = 0.f, sumA = 0.f;

            for (float oy : kOffsets) {
                for (float ox : kOffsets) {
                    const Vec2f mapped = invert.transformPoint({ static_cast<float>(x) + ox,
                                                                 static_cast<float>(y) + oy });
                    const Color color = getRasterPixel(source,
                                                       static_cast<int32_t>(std::floor(mapped.x)),
                                                       static_cast<int32_t>(std::floor(mapped.y)));
                    if (color.a == 0) {
                        continue;
                    }
                    if (hits == 0) {
                        firstHit = color;
                    }
                    ++hits;
                    if (policy) {
                        samples.push_back(color);
                    }
                    sumR += static_cast<float>(color.r);
                    sumG += static_cast<float>(color.g);
                    sumB += static_cast<float>(color.b);
                    sumA += static_cast<float>(color.a);
                    const uint32_t packed = (static_cast<uint32_t>(color.r) << 24) |
                                            (static_cast<uint32_t>(color.g) << 16) |
                                            (static_cast<uint32_t>(color.b) << 8) |
                                             static_cast<uint32_t>(color.a);
                    const int count = ++votes[packed];
                    if (count > majorityVotes) {
                        majorityVotes = count;
                        majority = color;
                    }
                }
            }

            if (hits == 0) {
                continue;
            }

            const float coverage = static_cast<float>(hits) / 9.f;
            const bool covered = sampling == SamplingPolicy::Threshold
                ? hits > 0
                : coverage >= threshold;
            if (!covered) {
                continue;
            }

            if (policy) {
                const Color chosenByPolicy = policy(samples);
                if (chosenByPolicy.a != 0) {
                    setRasterPixel(out, x, y, chosenByPolicy);
                }
                continue;
            }

            Color chosen = firstHit;
            switch (sampling) {
                case SamplingPolicy::Majority:
                case SamplingPolicy::Median:
                    chosen = majority;
                    break;
                case SamplingPolicy::Average:
                    chosen = { toByte(sumR / static_cast<float>(hits) / 255.f),
                               toByte(sumG / static_cast<float>(hits) / 255.f),
                               toByte(sumB / static_cast<float>(hits) / 255.f),
                               toByte(sumA / static_cast<float>(hits) / 255.f) };
                    break;
                default:
                    chosen = majority.a != 0 ? majority : firstHit;
                    break;
            }
            setRasterPixel(out, x, y, chosen);
        }
    }

    // Thin features (a 1px blade edge) can miss every sub-sample. Forward
    // projection puts them back and protects them from cleanup. A registered
    // policy stays authoritative over colour, so rescued pixels go through it
    // too, as a single-sample decision.
    forwardProject();
    for (const auto& [key, color] : projected) {
        if (getRasterPixel(out, keyX(key), keyY(key)).a != 0) {
            continue;
        }
        const Color rescued = policy ? policy(std::vector<Color>{color}) : color;
        if (rescued.a != 0) {
            setRasterPixel(out, keyX(key), keyY(key), rescued);
        }
    }

    RasterBuffer cleaned = cleanupTransformed(out, projected);
    return repairInteriorGaps(cleaned);
}

// Forward displacement (used by deforms), followed by hole repair.
//
// `trackedPoints`, when given, are carried through the same displacement. That
// is how a Global pattern origin follows a warp: the mapping happens here,
// where the displacement function is still in scope.
RasterBuffer displaceRaster(const RasterBuffer& source,
                            const std::function<Vec2f(Vec2f)>& displace,
                            std::vector<Vec2f>* trackedPoints = nullptr) {
    if (trackedPoints != nullptr) {
        for (Vec2f& point : *trackedPoints) {
            point = displace(point);
        }
    }
    auto allocated = allocateRaster(source.width, source.height);
    if (allocated.fail()) {
        return source;
    }
    RasterBuffer out = allocated.value;

    for (int32_t y = 0; y < static_cast<int32_t>(source.height); ++y) {
        for (int32_t x = 0; x < static_cast<int32_t>(source.width); ++x) {
            const Color color = getRasterPixel(source, x, y);
            if (color.a == 0) {
                continue;
            }
            const Vec2f moved = displace({ static_cast<float>(x) + 0.5f,
                                           static_cast<float>(y) + 0.5f });
            setRasterPixel(out,
                           static_cast<int32_t>(std::floor(moved.x)),
                           static_cast<int32_t>(std::floor(moved.y)),
                           color);
        }
    }

    return repairInteriorGaps(out);
}

} // namespace

Color blendPixel(Color dst, Color src, BlendMode mode, float opacity) {
    const float alpha = toFloat(src.a) * clamp01(opacity);
    if (mode == BlendMode::Erase) {
        Color out = dst;
        out.a = toByte(toFloat(dst.a) * (1.f - alpha));
        return out;
    }
    if (alpha <= 0.f) {
        return dst;
    }
    if (mode == BlendMode::Replace || dst.a == 0) {
        Color out = src;
        out.a = toByte(alpha);
        return out;
    }

    const float dstAlpha = toFloat(dst.a);
    const float outAlpha = alpha + dstAlpha * (1.f - alpha);
    if (outAlpha <= 0.f) {
        return Color::transparent();
    }

    auto channel = [&](uint8_t dstChannel, uint8_t srcChannel) {
        const float d = toFloat(dstChannel);
        const float s = toFloat(srcChannel);
        const float blended = blendChannel(d, s, mode);
        const float mixed = (1.f - dstAlpha) * s + dstAlpha * blended;
        return toByte((alpha * mixed + dstAlpha * d * (1.f - alpha)) / outAlpha);
    };

    return { channel(dst.r, src.r), channel(dst.g, src.g), channel(dst.b, src.b), toByte(outAlpha) };
}

// ---------------------------------------------------------------------------
// Profile plumbing
// ---------------------------------------------------------------------------

uint64_t LSContext::Impl::hashProfile(const CompileProfile& profile) const {
    auto mix = [](uint64_t hash, uint64_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        return hash;
    };

    uint64_t hash = 0xcbf29ce484222325ull;
    hash = mix(hash, profile.outputWidth);
    hash = mix(hash, profile.outputHeight);
    hash = mix(hash, static_cast<uint64_t>(profile.type));
    hash = mix(hash, static_cast<uint64_t>(profile.sampling));
    hash = mix(hash, static_cast<uint64_t>(profile.rounding));
    hash = mix(hash, static_cast<uint64_t>(profile.alpha));
    hash = mix(hash, static_cast<uint64_t>(profile.palette));
    hash = mix(hash, static_cast<uint64_t>(profile.coverageThreshold * 10000.f));
    hash = mix(hash, static_cast<uint64_t>(profile.alphaThreshold * 10000.f));
    hash = mix(hash, profile.resolveTransforms ? 1u : 0u);
    hash = mix(hash, profile.engineVersion);
    for (char c : profile.samplingPolicyId) {
        hash = mix(hash, static_cast<uint64_t>(static_cast<unsigned char>(c)));
    }
    return hash;
}

CompileProfile LSContext::Impl::resolveProfileDefaults(const CompileProfile& profile,
                                                       DocumentId doc) const {
    CompileProfile resolved = profile;
    if (resolved.outputWidth == 0 || resolved.outputHeight == 0) {
        if (const DocumentData* document = findDocument(doc)) {
            if (resolved.outputWidth == 0)  { resolved.outputWidth = document->canvasWidth; }
            if (resolved.outputHeight == 0) { resolved.outputHeight = document->canvasHeight; }
        }
    }
    resolved.engineVersion = LS_ENGINE_VERSION;
    return resolved;
}

// ---------------------------------------------------------------------------
// Operation resolution
// ---------------------------------------------------------------------------

namespace {

struct CompileEnv {
    const LSContext::Impl* impl = nullptr;
    CompileProfile profile;
    DocumentId doc;
    SpriteId sprite;
    PaletteId palette;
    Rect2i spriteBounds;                 // content bounds so far, for Sprite space
    std::vector<std::string>* trace = nullptr;
    SamplePolicyFn samplePolicy;         // set when the profile names a plugin policy

    Color role(ColorRole colorRole, Color fallback) const {
        return impl->resolveColorRole(palette, colorRole, fallback);
    }

    void note(const std::string& line) const {
        if (trace != nullptr) {
            trace->push_back(line);
        }
    }
};

// Origin of a pattern coordinate space. Object space is anchored to the shape
// itself, which is what keeps dither stable while a sprite moves.
Vec2f spaceOrigin(const CompileEnv& env, CoordinateSpace space, const IntervalSet& coverage) {
    switch (space) {
        case CoordinateSpace::Object: {
            const Rect2i box = geom::bounds(coverage);
            return { static_cast<float>(box.min.x), static_cast<float>(box.min.y) };
        }
        case CoordinateSpace::Sprite:
            return { static_cast<float>(env.spriteBounds.min.x),
                     static_cast<float>(env.spriteBounds.min.y) };
        case CoordinateSpace::Canvas:
        case CoordinateSpace::Export:
        default:
            return { 0.f, 0.f };
    }
}

const IntervalSet* regionCoverage(const CompileEnv& env, RegionId id) {
    const RegionData* region = env.impl->findRegion(id);
    return region == nullptr ? nullptr : &region->coverage;
}

Color sampleRampData(const RampData* ramp, float t, Color fallback) {
    if (ramp == nullptr || ramp->desc.stops.empty()) {
        return fallback;
    }
    const std::vector<RampStop>& stops = ramp->desc.stops;
    const float clamped = clamp01(t);
    if (clamped <= stops.front().position) {
        return stops.front().color;
    }
    if (clamped >= stops.back().position) {
        return stops.back().color;
    }
    for (size_t i = 0; i + 1 < stops.size(); ++i) {
        const RampStop& a = stops[i];
        const RampStop& b = stops[i + 1];
        if (clamped < a.position || clamped > b.position) {
            continue;
        }
        if (!ramp->desc.interpolate) {
            return a.color;
        }
        const float span = b.position - a.position;
        const float local = span > 0.f ? (clamped - a.position) / span : 0.f;
        auto mix = [local](uint8_t x, uint8_t y) {
            return toByte((static_cast<float>(x) +
                           (static_cast<float>(y) - static_cast<float>(x)) * local) / 255.f);
        };
        return { mix(a.color.r, b.color.r), mix(a.color.g, b.color.g),
                 mix(a.color.b, b.color.b), mix(a.color.a, b.color.a) };
    }
    return stops.back().color;
}

// What a pattern says at a pixel: the threshold rank it carries, and the colour
// it holds if the tile is a colour texture rather than a bare dither matrix.
struct PatternSample {
    float threshold = 0.5f;
    bool  hasColor  = false;
    Color color;
};

// Sample a tile at a pixel, honouring the pattern origin, an extra phase, a
// scale and a rotation. The origin is what the anchor modes move: everything
// else here is tile-local arithmetic.
PatternSample samplePattern(const PatternData* pattern, float x, float y, Vec2f origin,
                            float phase, Vec2f scale = {1.f, 1.f}, float angleDegrees = 0.f) {
    PatternSample sample;

    float localX = x - origin.x - phase;
    float localY = y - origin.y;
    if (pattern != nullptr) {
        localX -= pattern->desc.phase.x;
        localY -= pattern->desc.phase.y;
    }
    if (angleDegrees != 0.f) {
        const float radians = -angleDegrees * kPi / 180.f;
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        const float rotatedX = c * localX - s * localY;
        const float rotatedY = s * localX + c * localY;
        localX = rotatedX;
        localY = rotatedY;
    }
    const float safeScaleX = std::fabs(scale.x) < 0.0001f ? 1.f : scale.x;
    const float safeScaleY = std::fabs(scale.y) < 0.0001f ? 1.f : scale.y;
    localX /= safeScaleX;
    localY /= safeScaleY;

    if (pattern == nullptr || pattern->desc.mask.empty()) {
        // No pattern bound: fall back to an ordered 4x4 Bayer matrix so a
        // dither fill still produces a stable, non-random pattern.
        static const uint8_t kBayer4[16] = {
            0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5
        };
        const int32_t px = ((static_cast<int32_t>(std::floor(localX)) % 4) + 4) % 4;
        const int32_t py = ((static_cast<int32_t>(std::floor(localY)) % 4) + 4) % 4;
        sample.threshold = (static_cast<float>(kBayer4[py * 4 + px]) + 0.5f) / 16.f;
        return sample;
    }

    const int32_t tileW = static_cast<int32_t>(pattern->desc.tileWidth);
    const int32_t tileH = static_cast<int32_t>(pattern->desc.tileHeight);
    const int32_t px = ((static_cast<int32_t>(std::floor(localX)) % tileW) + tileW) % tileW;
    const int32_t py = ((static_cast<int32_t>(std::floor(localY)) % tileH) + tileH) % tileH;
    const size_t index = static_cast<size_t>(py) * tileW + px;

    const uint8_t rank = pattern->desc.mask[index];
    const float levels = static_cast<float>(std::max<uint32_t>(2, pattern->desc.levels));
    sample.threshold = (static_cast<float>(rank) + 0.5f) / levels;

    if (index < pattern->desc.colors.size()) {
        sample.hasColor = true;
        sample.color = pattern->desc.colors[index];
    }
    return sample;
}

// The value a dither resolves against. Constant is a flat density; the others
// vary across the fill, which is what turns a dither into a gradient.
float modulationValue(DitherModulation modulation, float density,
                      float x, float y, Vec2f origin, Vec2f start, Vec2f end) {
    if (modulation == DitherModulation::Constant) {
        return clamp01(density);
    }

    // Gradient geometry is stated in the same frame as the pattern origin, so a
    // gradient travels with its pattern under every anchor mode.
    const float px = x - origin.x;
    const float py = y - origin.y;
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;

    switch (modulation) {
        case DitherModulation::Linear: {
            const float lengthSq = dx * dx + dy * dy;
            if (lengthSq < 0.0001f) {
                return clamp01(density);
            }
            return clamp01(((px - start.x) * dx + (py - start.y) * dy) / lengthSq);
        }
        case DitherModulation::Radial: {
            const float radius = std::sqrt(dx * dx + dy * dy);
            if (radius < 0.0001f) {
                return clamp01(density);
            }
            const float distance = std::sqrt((px - start.x) * (px - start.x) +
                                             (py - start.y) * (py - start.y));
            return clamp01(distance / radius);
        }
        case DitherModulation::Angular: {
            const float reference = std::atan2(dy, dx);
            float angle = std::atan2(py - start.y, px - start.x) - reference;
            while (angle < 0.f)          { angle += 2.f * kPi; }
            while (angle >= 2.f * kPi)   { angle -= 2.f * kPi; }
            return clamp01(angle / (2.f * kPi));
        }
        case DitherModulation::Constant:
        default:
            return clamp01(density);
    }
}

// Pick a colour by dithering between the two ramp stops the value falls
// between. Two stops plus a constant value is classic two-tone dithering; more
// stops and a modulated value is a dithered gradient, one band per stop pair.
Color ditherRampColor(const RampData* ramp, float value, float threshold) {
    if (ramp == nullptr || ramp->desc.stops.empty()) {
        return value > threshold ? Color::white() : Color::black();
    }
    const std::vector<RampStop>& stops = ramp->desc.stops;
    if (stops.size() == 1) {
        return stops.front().color;
    }

    const float scaled = clamp01(value) * static_cast<float>(stops.size() - 1);
    const size_t lower = std::min(static_cast<size_t>(scaled), stops.size() - 2);
    const float fraction = scaled - static_cast<float>(lower);
    return fraction > threshold ? stops[lower + 1].color : stops[lower].color;
}

// Legacy helper kept for the fills that only need a threshold.
float patternThreshold(const PatternData* pattern, int32_t x, int32_t y, Vec2f origin, float phase) {
    return samplePattern(pattern, static_cast<float>(x), static_cast<float>(y),
                         origin, phase).threshold;
}

// A mark: coverage plus a per-pixel colour function, ready to composite.
//
// A patterned mark also records how it is anchored. Local marks are finished at
// paint time and simply ride the transforms with the pixels they painted.
// Global and Fixed marks are re-resolved after the transform stack, so they
// keep their own frame instead of inheriting the rotation of the content.
struct Mark {
    IntervalSet coverage;
    std::function<Color(int32_t, int32_t)> color;
    BlendMode blend = BlendMode::Normal;
    float opacity = 1.f;

    PatternAnchor anchor = PatternAnchor::Local;
    Vec2f patternOrigin;
    std::function<Color(int32_t, int32_t, Vec2f)> evaluateAt;   // set when anchor is not Local
};

// Where a pattern lattice is pinned.
Vec2f patternOriginFor(const CompileEnv& env, PatternAnchor anchor, CoordinateSpace space,
                       const IntervalSet& coverage) {
    switch (anchor) {
        case PatternAnchor::Local:
            return spaceOrigin(env, space, coverage);
        case PatternAnchor::Global: {
            // Travels with the object: the origin starts on the shape and is
            // carried through each transform, while the lattice axes stay put.
            const Rect2i box = geom::bounds(coverage);
            return { static_cast<float>(box.min.x), static_cast<float>(box.min.y) };
        }
        case PatternAnchor::Fixed:
        default:
            return { 0.f, 0.f };
    }
}

void compositeMark(RasterBuffer& target, const Mark& mark) {
    for (const Interval& interval : mark.coverage.intervals) {
        for (int32_t x = interval.x0; x < interval.x1; ++x) {
            const Color source = mark.color(x, interval.y);
            if (source.a == 0 && mark.blend != BlendMode::Erase) {
                continue;
            }
            const Color existing = getRasterPixel(target, x, interval.y);
            setRasterPixel(target, x, interval.y,
                           blendPixel(existing, source, mark.blend, mark.opacity));
        }
    }
}

// --- stroke rasterization -------------------------------------------------

IntervalSet strokePath(const std::vector<Vec2f>& points, bool closed, float width,
                       StrokeCap cap, float taper) {
    IntervalSet out;
    if (points.empty() || width <= 0.f) {
        return out;
    }
    if (points.size() == 1) {
        return geom::rasterizePoint({ points.front() });
    }

    std::vector<std::pair<Vec2f, Vec2f>> segments;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        segments.emplace_back(points[i], points[i + 1]);
    }
    if (closed && points.size() > 2) {
        segments.emplace_back(points.back(), points.front());
    }

    float totalLength = 0.f;
    for (const auto& [a, b] : segments) {
        totalLength += std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    }
    if (totalLength <= 0.f) {
        totalLength = 1.f;
    }

    Rect2f box { points.front(), points.front() };
    for (const Vec2f& p : points) {
        box.min.x = std::min(box.min.x, p.x);
        box.min.y = std::min(box.min.y, p.y);
        box.max.x = std::max(box.max.x, p.x);
        box.max.y = std::max(box.max.y, p.y);
    }
    const float margin = width + 2.f;
    const int32_t x0 = static_cast<int32_t>(std::floor(box.min.x - margin));
    const int32_t x1 = static_cast<int32_t>(std::ceil(box.max.x + margin));
    const int32_t y0 = static_cast<int32_t>(std::floor(box.min.y - margin));
    const int32_t y1 = static_cast<int32_t>(std::ceil(box.max.y + margin));

    const bool squareCap = cap == StrokeCap::Square || cap == StrokeCap::Flat;

    for (int32_t y = y0; y < y1; ++y) {
        for (int32_t x = x0; x < x1; ++x) {
            const Vec2f p { static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f };
            bool inside = false;
            float travelled = 0.f;

            for (const auto& [a, b] : segments) {
                const float dx = b.x - a.x;
                const float dy = b.y - a.y;
                const float lengthSq = dx * dx + dy * dy;
                const float segmentLength = std::sqrt(lengthSq);
                float t = lengthSq > 0.f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSq : 0.f;
                const bool beyondEnds = t < 0.f || t > 1.f;
                t = clamp01(t);
                const float projX = a.x + t * dx;
                const float projY = a.y + t * dy;
                const float distance = std::sqrt((p.x - projX) * (p.x - projX) +
                                                 (p.y - projY) * (p.y - projY));

                const float along = (travelled + t * segmentLength) / totalLength;
                const float localWidth = width * (1.f - taper * along);
                const float radius = std::max(0.5f, localWidth * 0.5f);

                // Flat and square caps stop at the segment end unless the join
                // with the next segment covers it; round caps extend past it.
                if (beyondEnds && squareCap && !closed) {
                    const bool interiorJoint = (&a != &segments.front().first) ||
                                               (&b != &segments.back().second);
                    if (!interiorJoint && distance > radius) {
                        travelled += segmentLength;
                        continue;
                    }
                }

                if (distance <= radius) {
                    inside = true;
                    break;
                }
                travelled += segmentLength;
            }

            if (inside) {
                out.intervals.push_back({ y, x, x + 1 });
            }
        }
    }

    return geom::normalize(std::move(out));
}

IntervalSet outlineOf(const IntervalSet& coverage, float thickness, OutlineSide side,
                      OutlineDiagonal diagonal) {
    const float amount = std::max(1.f, thickness);
    const bool square = diagonal != OutlineDiagonal::Exclude;

    switch (side) {
        case OutlineSide::Inside:
            return geom::subtractSets(coverage, geom::contract(coverage, amount, square));
        case OutlineSide::Outside:
            return geom::subtractSets(geom::expand(coverage, amount, square), coverage);
        case OutlineSide::Center:
        default: {
            const float half = std::max(1.f, amount * 0.5f);
            return geom::subtractSets(geom::expand(coverage, half, square),
                                      geom::contract(coverage, half, square));
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Layer compilation
// ---------------------------------------------------------------------------

namespace {

// Resolve one transform or deform operation against the layer colour field.
// Provenance tags travel in a parallel raster: tag colours encode which mark
// owns a pixel. Coverage decisions inside a transform depend only on alpha, so
// pushing the tag plane through the same call keeps the two in lockstep.
Color encodeTag(uint32_t tag) {
    return { static_cast<uint8_t>((tag >> 16) & 0xffu),
             static_cast<uint8_t>((tag >> 8) & 0xffu),
             static_cast<uint8_t>(tag & 0xffu),
             255 };
}

uint32_t decodeTag(Color color) {
    if (color.a == 0) {
        return 0;
    }
    return (static_cast<uint32_t>(color.r) << 16) |
           (static_cast<uint32_t>(color.g) << 8) |
            static_cast<uint32_t>(color.b);
}

// `trackedPoints`, when given, are moved exactly the way this operation moves
// the content: through its matrix for an affine transform, through its
// displacement field for a deform. Anchored pattern origins ride along in it.
RasterBuffer applyTransformOperation(const CompileEnv& env, const Operation& op,
                                     const RasterBuffer& source,
                                     std::vector<Vec2f>* trackedPoints = nullptr) {
    auto pivotOf = [&](PivotId pivot, Vec2f fallback) -> Vec2f {
        if (const PivotData* data = env.impl->findPivot(pivot)) {
            return data->position;
        }
        if (fallback.x != 0.f || fallback.y != 0.f) {
            return fallback;
        }
        const Rect2i box = rasterBounds(source);
        if (box.empty()) {
            return { static_cast<float>(env.profile.outputWidth) * 0.5f,
                     static_cast<float>(env.profile.outputHeight) * 0.5f };
        }
        return { (static_cast<float>(box.min.x) + static_cast<float>(box.max.x)) * 0.5f,
                 (static_cast<float>(box.min.y) + static_cast<float>(box.max.y)) * 0.5f };
    };

    // Restrict a transform to a target region when the operation names one.
    auto restricted = [&](const RasterBuffer& transformed, RegionId targetRegion) {
        const IntervalSet* coverage = regionCoverage(env, targetRegion);
        if (coverage == nullptr) {
            return transformed;
        }
        RasterBuffer merged = source;
        // Clear the source pixels inside the region, then lay the moved ones in.
        for (const Interval& interval : coverage->intervals) {
            for (int32_t x = interval.x0; x < interval.x1; ++x) {
                setRasterPixel(merged, x, interval.y, Color::transparent());
            }
        }
        for (int32_t y = 0; y < static_cast<int32_t>(transformed.height); ++y) {
            for (int32_t x = 0; x < static_cast<int32_t>(transformed.width); ++x) {
                const Color color = getRasterPixel(transformed, x, y);
                if (color.a != 0) {
                    setRasterPixel(merged, x, y, color);
                }
            }
        }
        return merged;
    };

    // Isolate the region content before transforming it, so a region transform
    // does not drag the rest of the layer with it.
    auto isolate = [&](RegionId targetRegion) {
        const IntervalSet* coverage = regionCoverage(env, targetRegion);
        if (coverage == nullptr) {
            return source;
        }
        auto allocated = allocateRaster(source.width, source.height);
        RasterBuffer isolated = allocated.value;
        for (const Interval& interval : coverage->intervals) {
            for (int32_t x = interval.x0; x < interval.x1; ++x) {
                setRasterPixel(isolated, x, interval.y, getRasterPixel(source, x, interval.y));
            }
        }
        return isolated;
    };

    auto runMatrix = [&](const Mat3f& matrix, RegionId targetRegion, SamplingPolicy sampling) {
        if (trackedPoints != nullptr) {
            for (Vec2f& point : *trackedPoints) {
                point = matrix.transformPoint(point);
            }
        }
        const RasterBuffer input = targetRegion.valid() ? isolate(targetRegion) : source;
        RasterBuffer moved = transformRaster(input, matrix, sampling,
                                            env.profile.coverageThreshold, env.samplePolicy);
        return targetRegion.valid() ? restricted(moved, targetRegion) : moved;
    };

    if (const auto* translate = std::get_if<TranslateOp>(&op)) {
        Vec2f delta = translate->delta;
        if (translate->rounding != RoundingPolicy::SubpixelHalf) {
            delta = { std::round(delta.x), std::round(delta.y) };
        }
        return runMatrix(Mat3f::translation(delta), translate->targetRegion, SamplingPolicy::Center);
    }
    if (const auto* rotate = std::get_if<RotateOp>(&op)) {
        const Vec2f pivot = pivotOf(rotate->pivot, rotate->pivotFallback);
        const Mat3f matrix = Mat3f::aroundPivot(Mat3f::rotation(rotate->angleDegrees), pivot);
        return runMatrix(matrix, rotate->targetRegion, rotate->sampling);
    }
    if (const auto* scale = std::get_if<ScaleOp>(&op)) {
        const Vec2f pivot = pivotOf(scale->pivot, scale->pivotFallback);
        const Mat3f matrix = Mat3f::aroundPivot(Mat3f::scaling(scale->factor), pivot);
        return runMatrix(matrix, scale->targetRegion, scale->sampling);
    }
    if (const auto* mirror = std::get_if<MirrorOp>(&op)) {
        const Vec2f pivot = pivotOf(mirror->pivot, mirror->pivotFallback);
        const Vec2f factor {
            mirror->axis == MirrorAxis::Y ? 1.f : -1.f,
            mirror->axis == MirrorAxis::X ? 1.f : -1.f
        };
        const Mat3f matrix = Mat3f::aroundPivot(Mat3f::scaling(factor), pivot);
        return runMatrix(matrix, mirror->targetRegion, SamplingPolicy::Center);
    }
    if (const auto* shear = std::get_if<ShearOp>(&op)) {
        const Vec2f pivot = pivotOf(shear->pivot, shear->pivotFallback);
        const Mat3f matrix = Mat3f::aroundPivot(Mat3f::shearing(shear->shear), pivot);
        return runMatrix(matrix, shear->targetRegion, shear->sampling);
    }
    if (const auto* skew = std::get_if<SkewOp>(&op)) {
        const Vec2f pivot = pivotOf(skew->pivot, skew->pivotFallback);
        const Vec2f shear { std::tan(skew->angleX * kPi / 180.f),
                            std::tan(skew->angleY * kPi / 180.f) };
        const Mat3f matrix = Mat3f::aroundPivot(Mat3f::shearing(shear), pivot);
        return runMatrix(matrix, skew->targetRegion, skew->sampling);
    }
    if (const auto* matrixOp = std::get_if<MatrixTransformOp>(&op)) {
        return runMatrix(matrixOp->matrix, matrixOp->targetRegion, matrixOp->sampling);
    }
    if (const auto* pivotOp = std::get_if<PivotTransformOp>(&op)) {
        const Vec2f pivot = pivotOf(pivotOp->pivot, {});
        const Mat3f matrix = Mat3f::aroundPivot(
            Mat3f::rotation(pivotOp->angleDegrees).mul(Mat3f::scaling(pivotOp->scale)), pivot);
        return runMatrix(matrix, RegionId::null(), pivotOp->sampling);
    }
    if (const auto* anchor = std::get_if<AnchorTransformOp>(&op)) {
        const SocketData* socket = env.impl->findSocket(anchor->socket);
        const PivotData* pivot = env.impl->findPivot(anchor->childPivot);
        if (socket == nullptr) {
            return source;
        }
        Mat3f matrix = Mat3f::translation(socket->desc.position)
                           .mul(Mat3f::rotation(socket->desc.angle))
                           .mul(anchor->localOffset);
        if (pivot != nullptr) {
            matrix = matrix.mul(Mat3f::translation({ -pivot->position.x, -pivot->position.y }));
        }
        return runMatrix(matrix, RegionId::null(), SamplingPolicy::Coverage);
    }

    // --- deforms: displacement fields -------------------------------------

    auto boundaryInfluence = [&](BoundaryId id, Vec2f point) -> float {
        if (!id.valid()) {
            return 1.f;
        }
        const BoundaryData* boundary = env.impl->findBoundary(id);
        if (boundary == nullptr) {
            return 1.f;
        }
        const GeometryData* shape = env.impl->findGeometry(boundary->desc.shape);
        if (shape == nullptr) {
            return 1.f;
        }
        const IntervalSet coverage = env.impl->rasterizeGeometry(*shape);
        const Vec2i pixel { static_cast<int32_t>(std::floor(point.x)),
                            static_cast<int32_t>(std::floor(point.y)) };
        return geom::contains(coverage, pixel) ? 1.f : 0.f;
    };

    // Squash and stretch preserve volume: one axis scales by the factor, the
    // other by its reciprocal. With no boundary limiting them they are a plain
    // affine transform, so they take the matrix path, which resolves coverage
    // geometrically instead of forward-scattering pixels and tearing gaps.
    auto volumeScale = [](float factor) {
        const float safe = std::max(0.01f, factor);
        return Vec2f{ 1.f / safe, safe };
    };

    if (const auto* squash = std::get_if<SquashOp>(&op)) {
        const Vec2f pivot = pivotOf(squash->pivot, squash->pivotFallback);
        const float factor = std::max(0.01f, squash->factor);
        if (!squash->boundary.valid()) {
            const Mat3f matrix = Mat3f::aroundPivot(Mat3f::scaling(volumeScale(factor)), pivot);
            return runMatrix(matrix, squash->targetRegion, SamplingPolicy::Coverage);
        }
        return displaceRaster(source, [&](Vec2f p) {
            const float influence = applyFalloff(boundaryInfluence(squash->boundary, p), squash->falloff);
            const float scaleY = 1.f + (factor - 1.f) * influence;
            const float scaleX = influence > 0.f ? 1.f + (1.f / factor - 1.f) * influence : 1.f;
            return Vec2f { pivot.x + (p.x - pivot.x) * scaleX,
                           pivot.y + (p.y - pivot.y) * scaleY };
        }, trackedPoints);
    }
    if (const auto* stretch = std::get_if<StretchOp>(&op)) {
        const Vec2f pivot = pivotOf(stretch->pivot, stretch->pivotFallback);
        const float factor = std::max(0.01f, stretch->factor);
        if (!stretch->boundary.valid()) {
            const Mat3f matrix = Mat3f::aroundPivot(Mat3f::scaling(volumeScale(factor)), pivot);
            return runMatrix(matrix, stretch->targetRegion, SamplingPolicy::Coverage);
        }
        return displaceRaster(source, [&](Vec2f p) {
            const float influence = applyFalloff(boundaryInfluence(stretch->boundary, p), stretch->falloff);
            const float scaleY = 1.f + (factor - 1.f) * influence;
            const float scaleX = influence > 0.f ? 1.f + (1.f / factor - 1.f) * influence : 1.f;
            return Vec2f { pivot.x + (p.x - pivot.x) * scaleX,
                           pivot.y + (p.y - pivot.y) * scaleY };
        }, trackedPoints);
    }
    if (const auto* bend = std::get_if<BendOp>(&op)) {
        const Rect2i box = rasterBounds(source);
        const float span = std::max(1.f, static_cast<float>(box.height()));
        const float originY = static_cast<float>(box.min.y);
        const float originX = (static_cast<float>(box.min.x) + static_cast<float>(box.max.x)) * 0.5f;
        return displaceRaster(source, [&](Vec2f p) {
            const float t = applyFalloff((p.y - originY) / span, bend->falloff);
            const float angle = bend->strength * t * kPi / 180.f;
            const float dx = p.x - originX;
            const float dy = p.y - originY;
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            return Vec2f { originX + c * dx - s * dy + bend->angle * t,
                           originY + s * dx + c * dy };
        }, trackedPoints);
    }
    if (const auto* warp = std::get_if<WarpOp>(&op)) {
        return displaceRaster(source, [&](Vec2f p) {
            Vec2f moved = p;
            const size_t count = std::min(warp->handlePoints.size(), warp->displacements.size());
            for (size_t i = 0; i < count; ++i) {
                const Vec2f handle = warp->handlePoints[i];
                const float distance = std::sqrt((p.x - handle.x) * (p.x - handle.x) +
                                                 (p.y - handle.y) * (p.y - handle.y));
                if (distance > warp->radius) {
                    continue;
                }
                const float weight = applyFalloff(1.f - distance / std::max(0.001f, warp->radius),
                                                  warp->falloff) * warp->strength;
                moved.x += warp->displacements[i].x * weight;
                moved.y += warp->displacements[i].y * weight;
            }
            return moved;
        }, trackedPoints);
    }
    if (const auto* weighted = std::get_if<WeightedDeformOp>(&op)) {
        return displaceRaster(source, [&](Vec2f p) {
            Vec2f moved = p;
            const size_t count = std::min(weighted->handles.size(), weighted->handleTargets.size());
            for (size_t i = 0; i < count; ++i) {
                const Vec2f handle = weighted->handles[i];
                const float distance = std::sqrt((p.x - handle.x) * (p.x - handle.x) +
                                                 (p.y - handle.y) * (p.y - handle.y));
                if (distance > weighted->radius) {
                    continue;
                }
                const float base = applyFalloff(1.f - distance / std::max(0.001f, weighted->radius),
                                                weighted->falloff);
                const float weight = base * (i < weighted->weights.size() ? weighted->weights[i] : 1.f);
                moved.x += (weighted->handleTargets[i].x - handle.x) * weight;
                moved.y += (weighted->handleTargets[i].y - handle.y) * weight;
            }
            return moved;
        }, trackedPoints);
    }
    if (const auto* pin = std::get_if<PinDeformOp>(&op)) {
        return displaceRaster(source, [&](Vec2f p) {
            Vec2f moved = p;
            const size_t count = std::min(pin->pins.size(), pin->pinTargets.size());
            float totalWeight = 0.f;
            Vec2f offset { 0.f, 0.f };
            for (size_t i = 0; i < count; ++i) {
                const Vec2f anchor = pin->pins[i];
                const float distanceSq = (p.x - anchor.x) * (p.x - anchor.x) +
                                         (p.y - anchor.y) * (p.y - anchor.y);
                const float weight = 1.f / (1.f + distanceSq * std::max(0.01f, pin->stiffness));
                offset.x += (pin->pinTargets[i].x - anchor.x) * weight;
                offset.y += (pin->pinTargets[i].y - anchor.y) * weight;
                totalWeight += weight;
            }
            if (totalWeight > 0.f) {
                moved.x += offset.x / totalWeight;
                moved.y += offset.y / totalWeight;
            }
            return moved;
        }, trackedPoints);
    }
    if (const auto* lattice = std::get_if<LatticeDeformOp>(&op)) {
        const Rect2i box = rasterBounds(source);
        if (box.empty() || lattice->gridW < 2 || lattice->gridH < 2 ||
            lattice->controlPoints.size() !=
                static_cast<size_t>(lattice->gridW) * lattice->gridH) {
            return source;
        }
        const float width = static_cast<float>(box.width());
        const float height = static_cast<float>(box.height());
        return displaceRaster(source, [&](Vec2f p) {
            const float u = clamp01((p.x - static_cast<float>(box.min.x)) / width) *
                            static_cast<float>(lattice->gridW - 1);
            const float v = clamp01((p.y - static_cast<float>(box.min.y)) / height) *
                            static_cast<float>(lattice->gridH - 1);
            const uint32_t x0 = std::min(static_cast<uint32_t>(u), lattice->gridW - 2);
            const uint32_t y0 = std::min(static_cast<uint32_t>(v), lattice->gridH - 2);
            const float fx = u - static_cast<float>(x0);
            const float fy = v - static_cast<float>(y0);

            auto controlPoint = [&](uint32_t gx, uint32_t gy) {
                return lattice->controlPoints[static_cast<size_t>(gy) * lattice->gridW + gx];
            };
            const Vec2f p00 = controlPoint(x0, y0);
            const Vec2f p10 = controlPoint(x0 + 1, y0);
            const Vec2f p01 = controlPoint(x0, y0 + 1);
            const Vec2f p11 = controlPoint(x0 + 1, y0 + 1);

            return Vec2f {
                (p00.x * (1 - fx) + p10.x * fx) * (1 - fy) + (p01.x * (1 - fx) + p11.x * fx) * fy,
                (p00.y * (1 - fx) + p10.y * fx) * (1 - fy) + (p01.y * (1 - fx) + p11.y * fx) * fy
            };
        }, trackedPoints);
    }
    if (const auto* path = std::get_if<PathDeformOp>(&op)) {
        const GeometryData* geometry = env.impl->findGeometry(path->path);
        if (geometry == nullptr) {
            return source;
        }
        const std::vector<Vec2f> points = env.impl->geometryPath(*geometry);
        if (points.size() < 2) {
            return source;
        }
        const Rect2i box = rasterBounds(source);
        if (box.empty()) {
            return source;
        }
        const float width = std::max(1.f, static_cast<float>(box.width()));
        const float centerY = (static_cast<float>(box.min.y) + static_cast<float>(box.max.y)) * 0.5f;

        return displaceRaster(source, [&](Vec2f p) {
            const float t = clamp01((p.x - static_cast<float>(box.min.x)) / width);
            const float position = t * static_cast<float>(points.size() - 1);
            const size_t index = std::min(static_cast<size_t>(position), points.size() - 2);
            const float local = position - static_cast<float>(index);
            const Vec2f a = points[index];
            const Vec2f b = points[index + 1];
            const Vec2f onPath { a.x + (b.x - a.x) * local, a.y + (b.y - a.y) * local };

            float offsetY = p.y - centerY + path->offset;
            if (!path->followTangent) {
                return Vec2f { onPath.x, onPath.y + offsetY };
            }
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float length = std::max(0.001f, std::sqrt(dx * dx + dy * dy));
            return Vec2f { onPath.x - dy / length * offsetY, onPath.y + dx / length * offsetY };
        }, trackedPoints);
    }
    if (const auto* envelope = std::get_if<EnvelopeDeformOp>(&op)) {
        const GeometryData* geometry = env.impl->findGeometry(envelope->envelopeCurve);
        if (geometry == nullptr) {
            return source;
        }
        const std::vector<Vec2f> points = env.impl->geometryPath(*geometry);
        if (points.size() < 2) {
            return source;
        }
        const Rect2i box = rasterBounds(source);
        if (box.empty()) {
            return source;
        }
        const float width = std::max(1.f, static_cast<float>(box.width()));
        const float top = static_cast<float>(box.min.y);
        const float height = std::max(1.f, static_cast<float>(box.height()));

        return displaceRaster(source, [&](Vec2f p) {
            const float t = clamp01((p.x - static_cast<float>(box.min.x)) / width);
            const float position = t * static_cast<float>(points.size() - 1);
            const size_t index = std::min(static_cast<size_t>(position), points.size() - 2);
            const float local = position - static_cast<float>(index);
            const float targetY = points[index].y + (points[index + 1].y - points[index].y) * local;
            const float depth = applyFalloff((p.y - top) / height, envelope->falloff);
            return Vec2f { p.x, p.y + (targetY - top) * depth * envelope->strength };
        }, trackedPoints);
    }
    if (const auto* boundaryDeform = std::get_if<BoundaryDeformOp>(&op)) {
        const GeometryData* target = env.impl->findGeometry(boundaryDeform->targetShape);
        if (target == nullptr) {
            return source;
        }
        const Rect2i from = rasterBounds(source);
        const Rect2i to = geom::bounds(env.impl->rasterizeGeometry(*target));
        if (from.empty() || to.empty()) {
            return source;
        }
        const float scaleX = static_cast<float>(to.width()) / static_cast<float>(from.width());
        const float scaleY = static_cast<float>(to.height()) / static_cast<float>(from.height());
        return displaceRaster(source, [&](Vec2f p) {
            const float u = (p.x - static_cast<float>(from.min.x));
            const float v = (p.y - static_cast<float>(from.min.y));
            const Vec2f mapped { static_cast<float>(to.min.x) + u * scaleX,
                                 static_cast<float>(to.min.y) + v * scaleY };
            const float strength = clamp01(boundaryDeform->strength);
            return Vec2f { p.x + (mapped.x - p.x) * strength,
                           p.y + (mapped.y - p.y) * strength };
        }, trackedPoints);
    }

    return source;
}

// Resolve one mark operation into coverage plus colour.
bool resolveMarkOperation(const CompileEnv& env, const Operation& op,
                          const RasterBuffer& current, Mark& out) {
    auto coverageOf = [&](RegionId region) -> const IntervalSet* {
        return regionCoverage(env, region);
    };

    if (const auto* fill = std::get_if<FillSolidOp>(&op)) {
        const IntervalSet* coverage = coverageOf(fill->targetRegion);
        if (coverage == nullptr) {
            return false;
        }
        const Color color = env.role(fill->paletteRole, fill->fallbackColor);
        out.coverage = *coverage;
        out.color = [color](int32_t, int32_t) { return color; };
        out.blend = fill->blend;
        out.opacity = fill->opacity;
        return true;
    }
    if (const auto* fill = std::get_if<FillSemanticColorOp>(&op)) {
        const IntervalSet* coverage = coverageOf(fill->targetRegion);
        if (coverage == nullptr) {
            return false;
        }
        const Color color = env.role(fill->paletteRole, Color::black());
        out.coverage = *coverage;
        out.color = [color](int32_t, int32_t) { return color; };
        out.blend = fill->blend;
        out.opacity = fill->opacity;
        return true;
    }
    if (const auto* fill = std::get_if<FillDitherOp>(&op)) {
        const IntervalSet* coverage = coverageOf(fill->targetRegion);
        if (coverage == nullptr) {
            return false;
        }
        const RampData* ramp = env.impl->findRamp(fill->ramp);
        const PatternData* pattern = env.impl->findPattern(fill->pattern);
        const float density = clamp01(fill->density);
        const float phase = fill->phase;
        const DitherModulation modulation = fill->modulation;
        const Vec2f gradientStart = fill->gradientStart;
        const Vec2f gradientEnd = fill->gradientEnd;

        out.coverage = *coverage;
        out.anchor = fill->anchor;
        out.patternOrigin = patternOriginFor(env, fill->anchor, fill->coordinateSpace, *coverage);
        out.evaluateAt = [ramp, pattern, density, phase, modulation,
                          gradientStart, gradientEnd](int32_t x, int32_t y, Vec2f origin) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float threshold = samplePattern(pattern, px, py, origin, phase).threshold;
            const float value = modulationValue(modulation, density, px, py,
                                                origin, gradientStart, gradientEnd);
            return ditherRampColor(ramp, value, threshold);
        };
        const auto evaluate = out.evaluateAt;
        const Vec2f origin = out.patternOrigin;
        out.color = [evaluate, origin](int32_t x, int32_t y) { return evaluate(x, y, origin); };
        out.blend = fill->blend;
        out.opacity = fill->opacity;
        return true;
    }
    if (const auto* fill = std::get_if<FillGradientOp>(&op)) {
        const IntervalSet* coverage = coverageOf(fill->targetRegion);
        if (coverage == nullptr) {
            return false;
        }
        const RampData* ramp = env.impl->findRamp(fill->ramp);
        const Vec2f origin = spaceOrigin(env, fill->coordinateSpace, *coverage);
        const Vec2f start { fill->startPoint.x + origin.x, fill->startPoint.y + origin.y };
        const Vec2f end { fill->endPoint.x + origin.x, fill->endPoint.y + origin.y };
        const float dx = end.x - start.x;
        const float dy = end.y - start.y;
        const float lengthSq = std::max(0.0001f, dx * dx + dy * dy);
        const bool repeat = fill->repeat;

        out.coverage = *coverage;
        out.color = [ramp, start, dx, dy, lengthSq, repeat](int32_t x, int32_t y) {
            const float px = static_cast<float>(x) + 0.5f - start.x;
            const float py = static_cast<float>(y) + 0.5f - start.y;
            float t = (px * dx + py * dy) / lengthSq;
            t = repeat ? t - std::floor(t) : clamp01(t);
            return sampleRampData(ramp, t, Color::white());
        };
        out.blend = fill->blend;
        out.opacity = fill->opacity;
        return true;
    }
    if (const auto* fill = std::get_if<FillRampOp>(&op)) {
        const IntervalSet* coverage = coverageOf(fill->targetRegion);
        if (coverage == nullptr) {
            return false;
        }
        const RampData* ramp = env.impl->findRamp(fill->ramp);
        const Rect2i box = geom::bounds(*coverage);
        const float radians = fill->angle * kPi / 180.f;
        const Vec2f axis { std::cos(radians), std::sin(radians) };
        const float span = std::max(1.f, std::fabs(axis.x) * static_cast<float>(box.width()) +
                                          std::fabs(axis.y) * static_cast<float>(box.height()));
        const Vec2f origin { static_cast<float>(box.min.x), static_cast<float>(box.min.y) };

        out.coverage = *coverage;
        out.color = [ramp, axis, span, origin](int32_t x, int32_t y) {
            const float px = static_cast<float>(x) + 0.5f - origin.x;
            const float py = static_cast<float>(y) + 0.5f - origin.y;
            const float t = clamp01((px * axis.x + py * axis.y) / span);
            return sampleRampData(ramp, t, Color::white());
        };
        out.blend = fill->blend;
        out.opacity = fill->opacity;
        return true;
    }
    if (const auto* fill = std::get_if<FillNoiseOp>(&op)) {
        const IntervalSet* coverage = coverageOf(fill->targetRegion);
        if (coverage == nullptr) {
            return false;
        }
        const RampData* ramp = env.impl->findRamp(fill->ramp);
        const uint32_t seed = static_cast<uint32_t>(fill->seed);
        const float scale = std::max(0.0001f, fill->scale);

        out.coverage = *coverage;
        out.anchor = fill->anchor;
        out.patternOrigin = patternOriginFor(env, fill->anchor, fill->coordinateSpace, *coverage);
        out.evaluateAt = [ramp, seed, scale](int32_t x, int32_t y, Vec2f origin) {
            const int32_t sx = static_cast<int32_t>(std::floor((static_cast<float>(x) - origin.x) / scale));
            const int32_t sy = static_cast<int32_t>(std::floor((static_cast<float>(y) - origin.y) / scale));
            const float t = static_cast<float>(stableHash(sx, sy, seed) % 10000u) / 10000.f;
            return sampleRampData(ramp, t, Color::white());
        };
        const auto evaluate = out.evaluateAt;
        const Vec2f origin = out.patternOrigin;
        out.color = [evaluate, origin](int32_t x, int32_t y) { return evaluate(x, y, origin); };
        out.blend = fill->blend;
        out.opacity = fill->opacity;
        return true;
    }
    if (const auto* fill = std::get_if<FillLinePatternOp>(&op)) {
        const IntervalSet* coverage = coverageOf(fill->targetRegion);
        if (coverage == nullptr) {
            return false;
        }
        const Color lineColor = env.role(fill->lineRole, Color::black());
        const Color bgColor = env.role(fill->bgRole, Color::transparent());
        const float radians = fill->angle * kPi / 180.f;
        const float nx = std::cos(radians);
        const float ny = std::sin(radians);
        const float spacing = std::max(1.f, fill->spacing);
        const float lineWidth = std::max(0.f, fill->lineWidth);

        out.coverage = *coverage;
        out.anchor = fill->anchor;
        out.patternOrigin = patternOriginFor(env, fill->anchor, fill->coordinateSpace, *coverage);
        out.evaluateAt = [lineColor, bgColor, nx, ny, spacing, lineWidth](
                             int32_t x, int32_t y, Vec2f origin) {
            const float px = static_cast<float>(x) + 0.5f - origin.x;
            const float py = static_cast<float>(y) + 0.5f - origin.y;
            const float projected = px * nx + py * ny;
            const float offset = projected - std::floor(projected / spacing) * spacing;
            return offset < lineWidth ? lineColor : bgColor;
        };
        const auto evaluate = out.evaluateAt;
        const Vec2f origin = out.patternOrigin;
        out.color = [evaluate, origin](int32_t x, int32_t y) { return evaluate(x, y, origin); };
        out.blend = fill->blend;
        out.opacity = fill->opacity;
        return true;
    }
    if (const auto* fill = std::get_if<FillTexturePatternOp>(&op)) {
        const IntervalSet* coverage = coverageOf(fill->targetRegion);
        if (coverage == nullptr) {
            return false;
        }
        const PatternData* pattern = env.impl->findPattern(fill->pattern);
        const Color foreground = env.role(fill->foregroundRole, Color::white());
        const Color background = env.role(fill->backgroundRole, Color::transparent());
        const Vec2f scale = fill->scale;
        const Vec2f offset = fill->offset;
        const float angle = fill->angle;

        out.coverage = *coverage;
        out.anchor = fill->anchor;
        const Vec2f base = patternOriginFor(env, fill->anchor, fill->coordinateSpace, *coverage);
        out.patternOrigin = { base.x + offset.x, base.y + offset.y };
        out.evaluateAt = [pattern, foreground, background, scale, angle](
                             int32_t x, int32_t y, Vec2f origin) {
            const PatternSample sample = samplePattern(pattern,
                                                       static_cast<float>(x) + 0.5f,
                                                       static_cast<float>(y) + 0.5f,
                                                       origin, 0.f, scale, angle);
            // A tile carrying colours is a texture; a bare threshold tile
            // resolves to the foreground and background roles instead.
            if (sample.hasColor) {
                return sample.color;
            }
            return sample.threshold < 0.5f ? foreground : background;
        };
        const auto evaluate = out.evaluateAt;
        const Vec2f origin = out.patternOrigin;
        out.color = [evaluate, origin](int32_t x, int32_t y) { return evaluate(x, y, origin); };
        out.blend = fill->blend;
        out.opacity = fill->opacity;
        return true;
    }

    // --- strokes ----------------------------------------------------------

    if (const auto* stroke = std::get_if<StrokePolylineOp>(&op)) {
        const GeometryData* geometry = env.impl->findGeometry(stroke->polyline);
        if (geometry == nullptr) {
            return false;
        }
        const std::vector<Vec2f> points = env.impl->geometryPath(*geometry);
        bool closed = false;
        if (const PolylineDesc* polyline = std::get_if<PolylineDesc>(&geometry->shape)) {
            closed = polyline->closed;
        }
        const Color color = env.role(stroke->paletteRole, stroke->fallbackColor);
        out.coverage = strokePath(points, closed, stroke->width, stroke->cap, stroke->taper);
        out.color = [color](int32_t, int32_t) { return color; };
        out.blend = stroke->blend;
        out.opacity = stroke->opacity;
        return true;
    }
    if (const auto* stroke = std::get_if<StrokeCurveOp>(&op)) {
        const GeometryData* geometry = env.impl->findGeometry(stroke->curve);
        if (geometry == nullptr) {
            return false;
        }
        const std::vector<Vec2f> points = env.impl->geometryPath(*geometry);
        bool closed = false;
        if (const CurveDesc* curve = std::get_if<CurveDesc>(&geometry->shape)) {
            closed = curve->closed;
        }
        const Color color = env.role(stroke->paletteRole, stroke->fallbackColor);
        out.coverage = strokePath(points, closed, stroke->width, stroke->cap, stroke->taper);
        out.color = [color](int32_t, int32_t) { return color; };
        out.blend = stroke->blend;
        out.opacity = stroke->opacity;
        return true;
    }
    if (const auto* stroke = std::get_if<StrokePixelPathOp>(&op)) {
        const GeometryData* geometry = env.impl->findGeometry(stroke->path);
        if (geometry == nullptr) {
            return false;
        }
        const Color color = env.role(stroke->paletteRole, stroke->fallbackColor);
        out.coverage = env.impl->rasterizeGeometry(*geometry);
        out.color = [color](int32_t, int32_t) { return color; };
        out.blend = stroke->blend;
        out.opacity = stroke->opacity;
        return true;
    }
    if (const auto* stroke = std::get_if<StrokeRegionBoundaryOp>(&op)) {
        const IntervalSet* coverage = coverageOf(stroke->targetRegion);
        if (coverage == nullptr) {
            return false;
        }
        const RegionData* region = env.impl->findRegion(stroke->targetRegion);
        const Color color = env.role(stroke->paletteRole, stroke->fallbackColor);
        // An authored closed loop already knows its own outline; otherwise the
        // boundary is derived from coverage.
        const IntervalSet base = (region != nullptr && !region->boundary.empty())
            ? region->boundary
            : geom::boundaryOf(*coverage);
        out.coverage = stroke->width > 1.f
            ? geom::intersectSets(geom::expand(base, stroke->width - 1.f, true), *coverage)
            : base;
        out.color = [color](int32_t, int32_t) { return color; };
        out.blend = stroke->blend;
        out.opacity = stroke->opacity;
        return true;
    }
    if (const auto* stroke = std::get_if<StrokeBrushOp>(&op)) {
        const GeometryData* geometry = env.impl->findGeometry(stroke->path);
        if (geometry == nullptr) {
            return false;
        }
        const std::vector<Vec2f> points = env.impl->geometryPath(*geometry);
        const Color color = env.role(stroke->paletteRole, stroke->fallbackColor);
        const float spacing = std::max(0.05f, stroke->spacing) * std::max(1.f, stroke->size);

        IntervalSet coverage;
        float carry = 0.f;
        uint32_t stampIndex = 0;
        for (size_t i = 0; i + 1 < points.size(); ++i) {
            const Vec2f a = points[i];
            const Vec2f b = points[i + 1];
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            for (float travelled = carry; travelled < length; travelled += spacing) {
                const float t = length > 0.f ? travelled / length : 0.f;
                Vec2f center { a.x + dx * t, a.y + dy * t };
                if (stroke->scatter > 0.f) {
                    const uint32_t noise = stableHash(static_cast<int32_t>(stampIndex), 0,
                                                      stroke->scatterSeed);
                    const float offsetX = (static_cast<float>(noise % 1000u) / 1000.f - 0.5f) * 2.f;
                    const float offsetY = (static_cast<float>((noise / 1000u) % 1000u) / 1000.f - 0.5f) * 2.f;
                    center.x += offsetX * stroke->scatter;
                    center.y += offsetY * stroke->scatter;
                }
                coverage = geom::unionSets(coverage,
                    geom::rasterizeCircle({ center, std::max(0.5f, stroke->size * 0.5f) }));
                ++stampIndex;
            }
            carry = length > 0.f ? std::fmod(spacing - std::fmod(length - carry, spacing), spacing) : carry;
        }

        out.coverage = std::move(coverage);
        out.color = [color](int32_t, int32_t) { return color; };
        out.blend = stroke->blend;
        out.opacity = stroke->opacity;
        return true;
    }

    // --- outlines ---------------------------------------------------------

    if (const auto* outline = std::get_if<GenerateSilhouetteOutlineOp>(&op)) {
        const IntervalSet silhouette = geom::maskToIntervals(current, 0.001f);
        if (silhouette.empty()) {
            return false;
        }
        IntervalSet coverage = outlineOf(silhouette, outline->thickness, outline->side,
                                         outline->diagonal);
        if (outline->limitBoundary.valid()) {
            if (const BoundaryData* boundary = env.impl->findBoundary(outline->limitBoundary)) {
                if (const GeometryData* shape = env.impl->findGeometry(boundary->desc.shape)) {
                    coverage = geom::intersectSets(coverage, env.impl->rasterizeGeometry(*shape));
                }
            }
        }
        const Color color = env.role(outline->paletteRole, outline->fallbackColor);
        out.coverage = std::move(coverage);
        out.color = [color](int32_t, int32_t) { return color; };
        out.blend = outline->blend;
        out.opacity = outline->opacity;
        return true;
    }
    if (const auto* outline = std::get_if<GenerateInnerOutlineOp>(&op)) {
        const IntervalSet* coverage = coverageOf(outline->targetRegion);
        if (coverage == nullptr) {
            return false;
        }
        const Color color = env.role(outline->paletteRole, outline->fallbackColor);
        out.coverage = outlineOf(*coverage, outline->thickness, OutlineSide::Inside,
                                 OutlineDiagonal::Include);
        out.color = [color](int32_t, int32_t) { return color; };
        out.blend = outline->blend;
        out.opacity = outline->opacity;
        return true;
    }
    if (const auto* outline = std::get_if<GenerateOuterOutlineOp>(&op)) {
        const IntervalSet* coverage = coverageOf(outline->targetRegion);
        if (coverage == nullptr) {
            return false;
        }
        const Color color = env.role(outline->paletteRole, outline->fallbackColor);
        out.coverage = outlineOf(*coverage, outline->thickness, OutlineSide::Outside,
                                 outline->diagonal);
        out.color = [color](int32_t, int32_t) { return color; };
        out.blend = outline->blend;
        out.opacity = outline->opacity;
        return true;
    }
    if (const auto* outline = std::get_if<GenerateRegionOutlineOp>(&op)) {
        const IntervalSet* coverage = coverageOf(outline->targetRegion);
        if (coverage == nullptr) {
            return false;
        }
        const Color color = env.role(outline->paletteRole, outline->fallbackColor);
        out.coverage = outlineOf(*coverage, outline->thickness, outline->side,
                                 OutlineDiagonal::Include);
        out.color = [color](int32_t, int32_t) { return color; };
        out.blend = outline->blend;
        out.opacity = outline->opacity;
        return true;
    }
    if (const auto* outline = std::get_if<GenerateMaterialBoundaryOutlineOp>(&op)) {
        // Where two different colours meet inside the current content.
        IntervalSet coverage;
        for (int32_t y = 0; y < static_cast<int32_t>(current.height); ++y) {
            for (int32_t x = 0; x < static_cast<int32_t>(current.width); ++x) {
                const Color color = getRasterPixel(current, x, y);
                if (color.a == 0) {
                    continue;
                }
                const Color right = getRasterPixel(current, x + 1, y);
                const Color below = getRasterPixel(current, x, y + 1);
                if ((right.a != 0 && right != color) || (below.a != 0 && below != color)) {
                    coverage.intervals.push_back({ y, x, x + 1 });
                }
            }
        }
        coverage = geom::normalize(std::move(coverage));
        if (outline->thickness > 1.f) {
            coverage = geom::expand(coverage, outline->thickness - 1.f, true);
        }
        const Color color = env.role(outline->paletteRole, outline->fallbackColor);
        out.coverage = std::move(coverage);
        out.color = [color](int32_t, int32_t) { return color; };
        out.blend = outline->blend;
        out.opacity = outline->opacity;
        return true;
    }

    return false;
}

} // namespace

Result<CompileResult> LSContext::compileLayer(LayerId id, const CompileProfile& profile) {
    const LayerData* layer = impl_->findLayer(id);
    if (layer == nullptr) {
        return Result<CompileResult>::err(LSError::InvalidId);
    }

    const SpriteId spriteId = layer->sprite;
    const DocumentId docId = impl_->documentOfSprite(spriteId);
    const CompileProfile resolved = impl_->resolveProfileDefaults(profile, docId);

    CacheKey key;
    key.entity = id.value;
    key.profileHash = impl_->hashProfile(resolved);
    key.resourceRevision = impl_->resourceRevision;
    key.engineVersion = LS_ENGINE_VERSION;
    auto cached = impl_->compileCache.find(key);
    if (cached != impl_->compileCache.end() && impl_->graph.dirty.count(id.value) == 0) {
        ++impl_->cacheHits;
        return Result<CompileResult>::ok(cached->second);
    }
    ++impl_->cacheMisses;

    auto allocated = allocateRaster(resolved.outputWidth, resolved.outputHeight);
    if (allocated.fail()) {
        return Result<CompileResult>::err(allocated.error);
    }

    CompileResult result;
    result.raster = std::move(allocated.value);

    CompileEnv env;
    env.impl = impl_.get();
    env.profile = resolved;
    env.doc = docId;
    env.sprite = spriteId;
    env.palette = impl_->effectivePalette(spriteId);
    env.spriteBounds = { {0, 0}, { static_cast<int32_t>(resolved.outputWidth),
                                   static_cast<int32_t>(resolved.outputHeight) } };
    env.trace = resolved.type == CompileProfileType::Debug ? &result.trace : nullptr;

    if (!resolved.samplingPolicyId.empty()) {
        auto policy = impl_->plugins.compilePolicies.find(resolved.samplingPolicyId);
        if (policy == impl_->plugins.compilePolicies.end()) {
            return Result<CompileResult>::err(LSError::PluginNotFound);
        }
        const PluginCompilePolicyDesc& desc = policy->second;
        const CompileProfile profileCopy = resolved;
        env.samplePolicy = [&desc, profileCopy](const std::vector<Color>& samples) {
            auto chosen = desc.resolve(samples, profileCopy);
            return chosen.ok() ? chosen.value : Color::transparent();
        };
    }

    // What each mark operation actually covered, so the outline post-processing
    // operations can act on the marks they name.
    struct ResolvedMark {
        IntervalSet coverage;
        Color color;
        BlendMode blend = BlendMode::Normal;
        float opacity = 1.f;
    };
    std::map<uint64_t, ResolvedMark> history;

    // Patterns anchored Global or Fixed are re-resolved once the transform
    // stack has run. Until then their pixels are marked in a tag plane so they
    // can be found again after the content has moved.
    struct DeferredFill {
        uint32_t tag = 0;
        PatternAnchor anchor = PatternAnchor::Global;
        Vec2f origin;
        std::function<Color(int32_t, int32_t, Vec2f)> evaluate;
    };
    std::vector<DeferredFill> deferred;
    RasterBuffer tagPlane;
    uint32_t nextTag = 1;

    auto ensureTagPlane = [&]() {
        if (tagPlane.empty()) {
            auto allocatedTags = allocateRaster(resolved.outputWidth, resolved.outputHeight);
            if (allocatedTags.ok()) {
                tagPlane = std::move(allocatedTags.value);
            }
        }
        return !tagPlane.empty();
    };

    // Every mark stamps the tag plane, so a later mark painting over a deferred
    // fill takes ownership of those pixels away from it.
    auto stampTags = [&](const IntervalSet& coverage, uint32_t tag) {
        if (tagPlane.empty()) {
            return;
        }
        for (const Interval& interval : coverage.intervals) {
            for (int32_t x = interval.x0; x < interval.x1; ++x) {
                const Color painted = getRasterPixel(result.raster, x, interval.y);
                setRasterPixel(tagPlane, x, interval.y,
                               painted.a == 0 ? Color::transparent() : encodeTag(tag));
            }
        }
    };

    // The tag plane must carry the same alpha as the colour plane for the two
    // to make identical coverage decisions inside a transform.
    auto syncTagAlpha = [&]() {
        if (tagPlane.empty()) {
            return;
        }
        for (uint32_t y = 0; y < tagPlane.height; ++y) {
            uint8_t* tagRow = tagPlane.row(y);
            const uint8_t* colorRow = result.raster.row(y);
            for (uint32_t x = 0; x < tagPlane.width; ++x) {
                if (colorRow[x * 4 + 3] == 0) {
                    tagRow[x * 4 + 0] = 0;
                    tagRow[x * 4 + 1] = 0;
                    tagRow[x * 4 + 2] = 0;
                    tagRow[x * 4 + 3] = 0;
                } else {
                    tagRow[x * 4 + 3] = 255;
                }
            }
        }
    };

    for (OperationId opId : layer->operations) {
        const OperationData* data = impl_->findOperation(opId);
        if (data == nullptr) {
            continue;
        }

        if (const PluginOp* plugin = std::get_if<PluginOp>(&data->op)) {
            auto operationResolver = impl_->plugins.operations.find(plugin->typeId);
            auto fillResolver      = impl_->plugins.fillResolvers.find(plugin->typeId);
            auto transformResolver = impl_->plugins.transformResolvers.find(plugin->typeId);

            const bool known = operationResolver != impl_->plugins.operations.end() ||
                               fillResolver != impl_->plugins.fillResolvers.end() ||
                               transformResolver != impl_->plugins.transformResolvers.end();
            if (!known) {
                return Result<CompileResult>::err(LSError::PluginNotFound);
            }
            if (operationResolver != impl_->plugins.operations.end() &&
                !operationResolver->second.isDeterministic &&
                resolved.type == CompileProfileType::Export) {
                // Export output must be reproducible; a self-declared
                // non-deterministic op cannot take part in it.
                return Result<CompileResult>::err(LSError::PluginDeterminismViolation);
            }

            auto pluginRaster = allocateRaster(resolved.outputWidth, resolved.outputHeight);
            if (pluginRaster.fail()) {
                return Result<CompileResult>::err(pluginRaster.error);
            }
            PluginResolveContext pluginCtx;
            pluginCtx.params = &plugin->params;
            pluginCtx.engineVersion = LS_ENGINE_VERSION;
            pluginCtx.profile = &resolved;
            pluginCtx.outputBuffer = &pluginRaster.value;
            pluginCtx.getRegion = [this](RegionId region) -> const IntervalSet* {
                const RegionData* regionData = impl_->findRegion(region);
                return regionData == nullptr ? nullptr : &regionData->coverage;
            };
            const PaletteId palette = env.palette;
            pluginCtx.resolveColor = [this, palette](ColorRole role) {
                return impl_->resolveColorRole(palette, role, Color::transparent());
            };

            if (operationResolver != impl_->plugins.operations.end()) {
                const LSError pluginError = operationResolver->second.resolve(*plugin, pluginCtx);
                if (pluginError != LSError::None) {
                    return Result<CompileResult>::err(pluginError);
                }
            } else if (fillResolver != impl_->plugins.fillResolvers.end()) {
                // A fill resolver works over a region named by the parameter
                // bag, exactly like a built-in fill.
                const IntervalSet* coverage = nullptr;
                auto regionParam = plugin->params.find("region");
                if (regionParam != plugin->params.end()) {
                    if (const uint64_t* regionId = std::get_if<uint64_t>(&regionParam->second)) {
                        coverage = pluginCtx.getRegion(RegionId{*regionId});
                    }
                }
                static const IntervalSet kEmptyCoverage;
                const LSError pluginError = fillResolver->second.resolve(
                    *plugin, coverage == nullptr ? kEmptyCoverage : *coverage, pluginCtx);
                if (pluginError != LSError::None) {
                    return Result<CompileResult>::err(pluginError);
                }
            } else {
                // A transform resolver returns a matrix; the engine applies it
                // with its own sampling rules.
                auto matrix = transformResolver->second.resolve(*plugin, pluginCtx);
                if (matrix.fail()) {
                    return Result<CompileResult>::err(matrix.error);
                }
                if (resolved.resolveTransforms) {
                    result.raster = transformRaster(result.raster, matrix.value,
                                                    resolved.sampling,
                                                    resolved.coverageThreshold,
                                                    env.samplePolicy);
                }
                env.note("op " + std::to_string(opId.value) + ": resolved plugin transform " +
                         plugin->typeId);
                continue;
            }

            for (int32_t y = 0; y < static_cast<int32_t>(result.raster.height); ++y) {
                for (int32_t x = 0; x < static_cast<int32_t>(result.raster.width); ++x) {
                    const Color color = getRasterPixel(pluginRaster.value, x, y);
                    if (color.a != 0) {
                        setRasterPixel(result.raster, x, y,
                                       blendPixel(getRasterPixel(result.raster, x, y),
                                                  color, BlendMode::Normal, 1.f));
                    }
                }
            }
            env.note("op " + std::to_string(opId.value) + ": resolved plugin " + plugin->typeId);
            continue;
        }

        if (operationIsTransform(data->op)) {
            if (!resolved.resolveTransforms) {
                env.note("op " + std::to_string(opId.value) + ": transform recorded, not resolved");
                continue;
            }
            // A Global pattern travels with its object, so its origin is moved
            // by whatever moved the content, affine or not. A Fixed pattern is
            // left out of the list and so never moves.
            std::vector<Vec2f> travellingOrigins;
            std::vector<size_t> travellingIndices;
            for (size_t i = 0; i < deferred.size(); ++i) {
                if (deferred[i].anchor == PatternAnchor::Global) {
                    travellingOrigins.push_back(deferred[i].origin);
                    travellingIndices.push_back(i);
                }
            }

            syncTagAlpha();
            result.raster = applyTransformOperation(
                env, data->op, result.raster,
                travellingOrigins.empty() ? nullptr : &travellingOrigins);

            for (size_t i = 0; i < travellingIndices.size(); ++i) {
                deferred[travellingIndices[i]].origin = travellingOrigins[i];
            }

            if (!deferred.empty() && !tagPlane.empty()) {
                // Carry provenance with the pixels. The tag pass runs without a
                // sampling policy: a policy chooses colours, not owners.
                CompileEnv tagEnv = env;
                tagEnv.samplePolicy = {};
                tagEnv.trace = nullptr;
                tagPlane = applyTransformOperation(tagEnv, data->op, tagPlane);
            }
            env.note("op " + std::to_string(opId.value) + ": resolved " +
                     std::string(operationTypeName(data->op)) + " over layer content");
            continue;
        }

        // Outline post-processing reads the marks already laid down.
        if (const auto* cleanup = std::get_if<CleanupOutlineOp>(&data->op)) {
            auto target = history.find(cleanup->targetOutlineOp.value);
            if (target == history.end()) {
                env.note("op " + std::to_string(opId.value) + ": cleanup found no target outline");
                continue;
            }
            IntervalSet coverage = target->second.coverage;
            if (cleanup->removeIsolatedPixels) {
                IntervalSet kept;
                for (const Interval& interval : coverage.intervals) {
                    for (int32_t x = interval.x0; x < interval.x1; ++x) {
                        int neighbours = 0;
                        neighbours += geom::contains(coverage, {x - 1, interval.y}) ? 1 : 0;
                        neighbours += geom::contains(coverage, {x + 1, interval.y}) ? 1 : 0;
                        neighbours += geom::contains(coverage, {x, interval.y - 1}) ? 1 : 0;
                        neighbours += geom::contains(coverage, {x, interval.y + 1}) ? 1 : 0;
                        if (neighbours >= 2) {
                            kept.intervals.push_back({interval.y, x, x + 1});
                        } else {
                            setRasterPixel(result.raster, x, interval.y, Color::transparent());
                        }
                    }
                }
                coverage = geom::normalize(std::move(kept));
            }
            if (cleanup->smoothCorners) {
                // Fill the notch left by a right-angle step in the outline.
                const IntervalSet ring =
                    geom::subtractSets(geom::expand(coverage, 1.f, true), coverage);
                IntervalSet added;
                for (const Interval& interval : ring.intervals) {
                    for (int32_t x = interval.x0; x < interval.x1; ++x) {
                        const bool horizontal = geom::contains(coverage, {x - 1, interval.y}) ||
                                                geom::contains(coverage, {x + 1, interval.y});
                        const bool vertical   = geom::contains(coverage, {x, interval.y - 1}) ||
                                                geom::contains(coverage, {x, interval.y + 1});
                        if (horizontal && vertical) {
                            added.intervals.push_back({interval.y, x, x + 1});
                            setRasterPixel(result.raster, x, interval.y, target->second.color);
                        }
                    }
                }
                coverage = geom::unionSets(coverage, geom::normalize(std::move(added)));
            }
            history[opId.value] = ResolvedMark{coverage, target->second.color,
                                               target->second.blend, target->second.opacity};
            env.note("op " + std::to_string(opId.value) + ": cleaned outline " +
                     std::to_string(cleanup->targetOutlineOp.value));
            continue;
        }

        if (const auto* join = std::get_if<JoinCornersOp>(&data->op)) {
            auto first = history.find(join->outlineA.value);
            auto second = history.find(join->outlineB.value);
            if (first == history.end() || second == history.end()) {
                env.note("op " + std::to_string(opId.value) + ": join found no outlines");
                continue;
            }
            const float radius = std::max(1.f, join->joinRadius);
            const IntervalSet both =
                geom::unionSets(first->second.coverage, second->second.coverage);
            const IntervalSet bridge = geom::subtractSets(
                geom::intersectSets(geom::expand(first->second.coverage, radius, true),
                                    geom::expand(second->second.coverage, radius, true)),
                both);
            for (const Interval& interval : bridge.intervals) {
                for (int32_t x = interval.x0; x < interval.x1; ++x) {
                    setRasterPixel(result.raster, x, interval.y, first->second.color);
                }
            }
            history[opId.value] = ResolvedMark{bridge, first->second.color,
                                               first->second.blend, first->second.opacity};
            env.note("op " + std::to_string(opId.value) + ": joined corners over " +
                     std::to_string(geom::pixelCount(bridge)) + " px");
            continue;
        }

        if (const auto* collisions = std::get_if<ResolveOutlineCollisionsOp>(&data->op)) {
            // Priority order: the first outline listed owns any shared pixel.
            IntervalSet claimed;
            for (OperationId outlineId : collisions->outlineOps) {
                auto entry = history.find(outlineId.value);
                if (entry == history.end()) {
                    continue;
                }
                const IntervalSet overlap = geom::intersectSets(entry->second.coverage, claimed);
                for (const Interval& interval : overlap.intervals) {
                    for (int32_t x = interval.x0; x < interval.x1; ++x) {
                        for (OperationId ownerId : collisions->outlineOps) {
                            auto owner = history.find(ownerId.value);
                            if (owner == history.end()) {
                                continue;
                            }
                            if (geom::contains(owner->second.coverage, {x, interval.y})) {
                                setRasterPixel(result.raster, x, interval.y, owner->second.color);
                                break;
                            }
                        }
                    }
                }
                claimed = geom::unionSets(claimed, entry->second.coverage);
            }
            env.note("op " + std::to_string(opId.value) + ": resolved outline collisions");
            continue;
        }

        Mark mark;
        if (!resolveMarkOperation(env, data->op, result.raster, mark)) {
            env.note("op " + std::to_string(opId.value) + ": skipped " +
                     std::string(operationTypeName(data->op)) + " (missing input)");
            continue;
        }
        compositeMark(result.raster, mark);
        if (!mark.coverage.empty()) {
            const Interval& sample = mark.coverage.intervals.front();
            history[opId.value] = ResolvedMark{ mark.coverage,
                                                mark.color(sample.x0, sample.y),
                                                mark.blend, mark.opacity };
        }

        const bool deferrable = mark.anchor != PatternAnchor::Local && mark.evaluateAt &&
                                !mark.coverage.empty();
        if (deferrable && ensureTagPlane()) {
            DeferredFill fill;
            fill.tag = nextTag++;
            fill.anchor = mark.anchor;
            fill.origin = mark.patternOrigin;
            fill.evaluate = mark.evaluateAt;
            deferred.push_back(std::move(fill));
            stampTags(mark.coverage, deferred.back().tag);
            env.note("op " + std::to_string(opId.value) + ": pattern anchored " +
                     (mark.anchor == PatternAnchor::Global ? "global" : "fixed") +
                     ", deferred to after transforms");
        } else if (!tagPlane.empty()) {
            stampTags(mark.coverage, 0);
        }
        env.note("op " + std::to_string(opId.value) + ": rasterized " +
                 std::string(operationTypeName(data->op)) + " over " +
                 std::to_string(geom::pixelCount(mark.coverage)) + " px");
    }

    // Re-resolve anchored patterns now that the content has finished moving.
    // Coverage, alpha and blending were settled at paint time; only the colour
    // is recomputed, in the frame the anchor asked for.
    if (!deferred.empty() && !tagPlane.empty()) {
        syncTagAlpha();
        for (const DeferredFill& fill : deferred) {
            int64_t repainted = 0;
            for (int32_t y = 0; y < static_cast<int32_t>(result.raster.height); ++y) {
                for (int32_t x = 0; x < static_cast<int32_t>(result.raster.width); ++x) {
                    if (decodeTag(getRasterPixel(tagPlane, x, y)) != fill.tag) {
                        continue;
                    }
                    const Color painted = getRasterPixel(result.raster, x, y);
                    if (painted.a == 0) {
                        continue;
                    }
                    Color resolvedColor = fill.evaluate(x, y, fill.origin);
                    resolvedColor.a = painted.a;
                    setRasterPixel(result.raster, x, y, resolvedColor);
                    ++repainted;
                }
            }
            env.note("re-resolved anchored pattern over " + std::to_string(repainted) + " px");
        }
    }

    // Layer mask: only the mask region survives.
    if (layer->mask.valid()) {
        if (const RegionData* mask = impl_->findRegion(layer->mask)) {
            for (int32_t y = 0; y < static_cast<int32_t>(result.raster.height); ++y) {
                for (int32_t x = 0; x < static_cast<int32_t>(result.raster.width); ++x) {
                    if (!geom::contains(mask->coverage, {x, y})) {
                        setRasterPixel(result.raster, x, y, Color::transparent());
                    }
                }
            }
            env.note("applied layer mask " + std::to_string(layer->mask.value));
        }
    }

    result.bounds = rasterBounds(result.raster);
    impl_->compileCache[key] = result;
    impl_->graph.dirty.erase(id.value);
    return Result<CompileResult>::ok(result);
}

Result<CompileResult> LSContext::compileSprite(SpriteId id, const CompileProfile& profile) {
    const SpriteData* sprite = impl_->findSprite(id);
    if (sprite == nullptr) {
        return Result<CompileResult>::err(LSError::InvalidId);
    }

    const CompileProfile resolved = impl_->resolveProfileDefaults(profile, sprite->document);

    CacheKey key;
    key.entity = id.value;
    key.profileHash = impl_->hashProfile(resolved);
    key.resourceRevision = impl_->resourceRevision;
    key.engineVersion = LS_ENGINE_VERSION;
    auto cached = impl_->compileCache.find(key);
    if (cached != impl_->compileCache.end() && impl_->graph.dirty.count(id.value) == 0) {
        ++impl_->cacheHits;
        return Result<CompileResult>::ok(cached->second);
    }
    ++impl_->cacheMisses;

    auto allocated = allocateRaster(resolved.outputWidth, resolved.outputHeight);
    if (allocated.fail()) {
        return Result<CompileResult>::err(allocated.error);
    }

    CompileResult result;
    result.raster = std::move(allocated.value);
    const bool debug = resolved.type == CompileProfileType::Debug;

    auto order = flattenLayersForCompile(id);
    if (order.fail()) {
        return Result<CompileResult>::err(order.error);
    }

    for (LayerId layerId : order.value) {
        auto compiled = compileLayer(layerId, resolved);
        if (compiled.fail()) {
            return Result<CompileResult>::err(compiled.error);
        }
        const LayerData* layer = impl_->findLayer(layerId);
        if (layer == nullptr) {
            continue;
        }

        // A clipping layer only paints where its base layer already has pixels.
        RasterBuffer clipMask;
        bool hasClip = false;
        if (layer->clipBase.valid()) {
            auto base = compileLayer(layer->clipBase, resolved);
            if (base.ok()) {
                clipMask = base.value.raster;
                hasClip = true;
            }
        }

        for (int32_t y = 0; y < static_cast<int32_t>(result.raster.height); ++y) {
            for (int32_t x = 0; x < static_cast<int32_t>(result.raster.width); ++x) {
                Color source = getRasterPixel(compiled.value.raster, x, y);
                if (source.a == 0) {
                    continue;
                }
                if (hasClip && getRasterPixel(clipMask, x, y).a == 0) {
                    continue;
                }
                setRasterPixel(result.raster, x, y,
                               blendPixel(getRasterPixel(result.raster, x, y), source,
                                          layer->desc.blend, layer->desc.opacity));
            }
        }

        if (debug) {
            result.trace.insert(result.trace.end(),
                                compiled.value.trace.begin(), compiled.value.trace.end());
            result.trace.push_back("composited layer " + std::to_string(layerId.value) +
                                   " (" + layer->desc.name + ")");
        }
    }

    // Palette policy.
    const PaletteId palette = impl_->effectivePalette(id);
    if (palette.valid() && resolved.palette != PalettePolicy::Unconstrained) {
        const PaletteData* paletteData = impl_->findPalette(palette);
        if (paletteData != nullptr && !paletteData->colors.empty()) {
            if (resolved.palette == PalettePolicy::ExactMatch) {
                for (uint32_t y = 0; y < result.raster.height; ++y) {
                    for (uint32_t x = 0; x < result.raster.width; ++x) {
                        const Color color = getRasterPixel(result.raster,
                                                           static_cast<int32_t>(x),
                                                           static_cast<int32_t>(y));
                        if (color.a == 0) {
                            continue;
                        }
                        bool found = false;
                        for (const auto& [role, entry] : paletteData->colors) {
                            (void)role;
                            if (entry.r == color.r && entry.g == color.g && entry.b == color.b) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            return Result<CompileResult>::err(LSError::PaletteNotBound);
                        }
                    }
                }
            } else {
                quantizeToPalette(result.raster, palette);
                if (debug) {
                    result.trace.push_back("quantized to palette " + std::to_string(palette.value));
                }
            }
        }
    }

    // Alpha policy.
    const uint8_t alphaCutoff = static_cast<uint8_t>(clamp01(resolved.alphaThreshold) * 255.f);
    for (uint32_t y = 0; y < result.raster.height; ++y) {
        uint8_t* row = result.raster.row(y);
        for (uint32_t x = 0; x < result.raster.width; ++x) {
            uint8_t* px = row + x * 4;
            switch (resolved.alpha) {
                case AlphaPolicy::Preserve:
                    break;
                case AlphaPolicy::Threshold:
                    px[3] = px[3] >= alphaCutoff ? 255 : 0;
                    break;
                case AlphaPolicy::Premultiply:
                    px[0] = static_cast<uint8_t>(px[0] * px[3] / 255);
                    px[1] = static_cast<uint8_t>(px[1] * px[3] / 255);
                    px[2] = static_cast<uint8_t>(px[2] * px[3] / 255);
                    break;
                case AlphaPolicy::ForceOpaque:
                    if (px[3] != 0) {
                        px[3] = 255;
                    }
                    break;
            }
        }
    }

    if (resolved.type == CompileProfileType::MaskOnly) {
        for (uint32_t y = 0; y < result.raster.height; ++y) {
            uint8_t* row = result.raster.row(y);
            for (uint32_t x = 0; x < result.raster.width; ++x) {
                uint8_t* px = row + x * 4;
                const uint8_t alpha = px[3];
                px[0] = px[1] = px[2] = alpha != 0 ? 255 : 0;
            }
        }
    }

    result.bounds = rasterBounds(result.raster);

    if (resolved.type == CompileProfileType::BoundsOnly) {
        result.raster = RasterBuffer{};
    }

    impl_->compileCache[key] = result;
    impl_->graph.dirty.erase(id.value);
    return Result<CompileResult>::ok(result);
}

Result<CompileResult> LSContext::compileRegion(RegionId id, const CompileProfile& profile) {
    const RegionData* region = impl_->findRegion(id);
    if (region == nullptr) {
        return Result<CompileResult>::err(LSError::InvalidId);
    }
    const CompileProfile resolved = impl_->resolveProfileDefaults(profile, region->document);

    auto allocated = allocateRaster(resolved.outputWidth, resolved.outputHeight);
    if (allocated.fail()) {
        return Result<CompileResult>::err(allocated.error);
    }

    CompileResult result;
    result.raster = std::move(allocated.value);
    for (const Interval& interval : region->coverage.intervals) {
        for (int32_t x = interval.x0; x < interval.x1; ++x) {
            setRasterPixel(result.raster, x, interval.y, Color::white());
        }
    }
    result.bounds = geom::bounds(region->coverage);
    if (resolved.type == CompileProfileType::Debug) {
        result.trace.push_back("compiled region " + std::to_string(id.value) + " with " +
                               std::to_string(geom::pixelCount(region->coverage)) + " px");
    }
    return Result<CompileResult>::ok(result);
}

Result<CompileResult> LSContext::compilePreview(SpriteId id, uint32_t maxWidth, uint32_t maxHeight) {
    const SpriteData* sprite = impl_->findSprite(id);
    if (sprite == nullptr) {
        return Result<CompileResult>::err(LSError::InvalidId);
    }
    const DocumentData* doc = impl_->findDocument(sprite->document);
    if (doc == nullptr) {
        return Result<CompileResult>::err(LSError::InvalidId);
    }

    CompileProfile profile;
    profile.type = CompileProfileType::Preview;
    profile.outputWidth = maxWidth == 0 ? doc->canvasWidth : std::min(maxWidth, doc->canvasWidth);
    profile.outputHeight = maxHeight == 0 ? doc->canvasHeight : std::min(maxHeight, doc->canvasHeight);
    return compileSprite(id, profile);
}

Result<RasterBuffer> LSContext::compileToRaster(SpriteId id, const CompileProfile& profile) {
    CompileProfile exportProfile = profile;
    exportProfile.type = CompileProfileType::Export;
    auto compiled = compileSprite(id, exportProfile);
    if (compiled.fail()) {
        return Result<RasterBuffer>::err(compiled.error);
    }
    return Result<RasterBuffer>::ok(compiled.value.raster);
}

Result<RasterBuffer> LSContext::compileToMask(SpriteId id, const CompileProfile& profile) {
    CompileProfile maskProfile = profile;
    maskProfile.type = CompileProfileType::MaskOnly;
    auto compiled = compileSprite(id, maskProfile);
    if (compiled.fail()) {
        return Result<RasterBuffer>::err(compiled.error);
    }
    return Result<RasterBuffer>::ok(compiled.value.raster);
}

Result<IntervalSet> LSContext::compileToIntervals(RegionId id, const CompileProfile& profile) {
    (void)profile;
    const RegionData* region = impl_->findRegion(id);
    if (region == nullptr) {
        return Result<IntervalSet>::err(LSError::InvalidId);
    }
    return Result<IntervalSet>::ok(region->coverage);
}

Result<GeometryId> LSContext::compileToCollisionShape(SpriteId id, const CompileProfile& profile) {
    const SpriteData* sprite = impl_->findSprite(id);
    if (sprite == nullptr) {
        return Result<GeometryId>::err(LSError::InvalidId);
    }
    auto mask = compileToMask(id, profile);
    if (mask.fail()) {
        return Result<GeometryId>::err(mask.error);
    }

    const IntervalSet coverage = geom::maskToIntervals(mask.value, 0.5f);
    const std::vector<ContourDesc> contours = geom::traceContours(coverage, false);
    if (contours.empty()) {
        return Result<GeometryId>::err(LSError::RegionEmpty);
    }

    // The largest outer contour is the collision hull.
    size_t bestIndex = 0;
    size_t bestSize = 0;
    for (size_t i = 0; i < contours.size(); ++i) {
        if (contours[i].points.size() > bestSize) {
            bestSize = contours[i].points.size();
            bestIndex = i;
        }
    }

    SimplifyParams simplify;
    simplify.epsilon = 1.f;
    PolygonDesc polygon;
    polygon.vertices = geom::simplifyPath(contours[bestIndex].points, simplify);
    if (polygon.vertices.size() < 3) {
        polygon.vertices = contours[bestIndex].points;
    }
    return createPolygon(sprite->document, polygon);
}

Result<Rect2i> LSContext::compileBoundsOnly(SpriteId id, const CompileProfile& profile) {
    CompileProfile boundsProfile = profile;
    boundsProfile.type = CompileProfileType::BoundsOnly;
    auto compiled = compileSprite(id, boundsProfile);
    if (compiled.fail()) {
        return Result<Rect2i>::err(compiled.error);
    }
    return Result<Rect2i>::ok(compiled.value.bounds);
}

Result<IntervalSet> LSContext::compileBoundaryMask(BoundaryId id, const CompileProfile& profile) const {
    (void)profile;
    const BoundaryData* boundary = impl_->findBoundary(id);
    if (boundary == nullptr) {
        return Result<IntervalSet>::err(LSError::InvalidId);
    }
    const GeometryData* shape = impl_->findGeometry(boundary->desc.shape);
    if (shape == nullptr) {
        return Result<IntervalSet>::err(LSError::InvalidId);
    }
    return Result<IntervalSet>::ok(impl_->rasterizeGeometry(*shape));
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

namespace {
SnapshotResult snapshotFrom(const CompileResult& compiled) {
    SnapshotResult snapshot;
    snapshot.raster = compiled.raster;
    snapshot.bounds = compiled.bounds;
    snapshot.mask = geom::maskToIntervals(compiled.raster, 0.001f);
    return snapshot;
}
} // namespace

Result<SnapshotResult> LSContext::snapshotCompiledSprite(SpriteId id, const CompileProfile& profile) {
    auto compiled = compileSprite(id, profile);
    if (compiled.fail()) {
        return Result<SnapshotResult>::err(compiled.error);
    }
    return Result<SnapshotResult>::ok(snapshotFrom(compiled.value));
}

Result<SnapshotResult> LSContext::snapshotCompiledLayer(LayerId id, const CompileProfile& profile) {
    auto compiled = compileLayer(id, profile);
    if (compiled.fail()) {
        return Result<SnapshotResult>::err(compiled.error);
    }
    return Result<SnapshotResult>::ok(snapshotFrom(compiled.value));
}

Result<SnapshotResult> LSContext::snapshotRegion(RegionId id, const CompileProfile& profile) {
    (void)profile;
    const RegionData* region = impl_->findRegion(id);
    if (region == nullptr) {
        return Result<SnapshotResult>::err(LSError::InvalidId);
    }
    SnapshotResult snapshot;
    snapshot.mask = region->coverage;
    snapshot.bounds = geom::bounds(region->coverage);
    return Result<SnapshotResult>::ok(std::move(snapshot));
}

Result<RegionId> LSContext::convertSnapshotToLiveRegion(DocumentId doc, const SnapshotResult& snapshot) {
    return createRegionFromCompiledSnapshot(doc, snapshot);
}

Result<IntervalSet> LSContext::convertMaskToIntervals(const RasterBuffer& mask, float alphaThreshold) const {
    if (mask.empty()) {
        return Result<IntervalSet>::err(LSError::InvalidParameter);
    }
    return Result<IntervalSet>::ok(geom::maskToIntervals(mask, alphaThreshold));
}

Result<RegionId> LSContext::convertRasterToRegionData(DocumentId doc, const RasterBuffer& raster,
                                                      const TraceBoundaryParams& params) {
    return traceBoundary(doc, raster, params);
}

} // namespace ls
