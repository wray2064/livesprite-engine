#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ls {

constexpr uint32_t LS_ENGINE_VERSION_MAJOR = 0;
constexpr uint32_t LS_ENGINE_VERSION_MINOR = 1;
constexpr uint32_t LS_ENGINE_VERSION_PATCH = 0;
constexpr uint32_t LS_ENGINE_VERSION =
    (LS_ENGINE_VERSION_MAJOR << 16) |
    (LS_ENGINE_VERSION_MINOR << 8) |
    LS_ENGINE_VERSION_PATCH;

template<typename Tag>
struct TypedId {
    uint64_t value = 0;
    bool valid() const { return value != 0; }
    bool operator==(TypedId other) const { return value == other.value; }
    bool operator!=(TypedId other) const { return value != other.value; }
};

struct TagDocument {};
struct TagSprite {};
struct TagLayer {};
struct TagRegion {};
struct TagOperation {};

using DocumentId = TypedId<TagDocument>;
using SpriteId = TypedId<TagSprite>;
using LayerId = TypedId<TagLayer>;
using RegionId = TypedId<TagRegion>;
using OperationId = TypedId<TagOperation>;

enum class LSError : uint32_t {
    None = 0,
    InvalidId,
    InvalidParameter,
    RasterAllocationFailed,
};

std::string_view lsErrorString(LSError error);

template<typename T>
struct Result {
    T value {};
    LSError error = LSError::None;

    bool ok() const { return error == LSError::None; }
    bool fail() const { return error != LSError::None; }

    static Result ok(T v) { return {v, LSError::None}; }
    static Result err(LSError e) { return {T{}, e}; }
};

template<>
struct Result<void> {
    LSError error = LSError::None;

    bool ok() const { return error == LSError::None; }
    bool fail() const { return error != LSError::None; }

    static Result success() { return {LSError::None}; }
    static Result err(LSError e) { return {e}; }
};

using VoidResult = Result<void>;

struct Vec2i {
    int32_t x = 0;
    int32_t y = 0;
};

struct Vec2f {
    float x = 0.0f;
    float y = 0.0f;
};

struct Rect2i {
    Vec2i min;
    Vec2i max;

    int32_t width() const { return max.x - min.x; }
    int32_t height() const { return max.y - min.y; }
    bool empty() const { return width() <= 0 || height() <= 0; }
};

struct Color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
};

inline bool operator==(Color a, Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

inline bool operator!=(Color a, Color b) {
    return !(a == b);
}

enum class DitherSpace : uint8_t {
    Object,
    Canvas,
};

struct Interval {
    int32_t y = 0;
    int32_t x0 = 0;
    int32_t x1 = 0;
};

struct IntervalSet {
    std::vector<Interval> intervals;
    bool empty() const { return intervals.empty(); }
};

struct RasterBuffer {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    std::vector<uint8_t> pixels;

    bool empty() const { return pixels.empty(); }
    const uint8_t* row(uint32_t y) const { return pixels.data() + y * stride; }
    uint8_t* row(uint32_t y) { return pixels.data() + y * stride; }
};

struct CompileProfile {
    uint32_t outputWidth = 32;
    uint32_t outputHeight = 32;
    bool resolveTransforms = true;
};

struct CompileResult {
    RasterBuffer raster;
    Rect2i bounds;
    std::vector<std::string> trace;
};

struct RectRegionDesc {
    Rect2i rect;
};

struct EllipseRegionDesc {
    Vec2f center;
    float radiusX = 0.0f;
    float radiusY = 0.0f;
};

struct PixelInput {
    Vec2i position;
    Color color;
};

struct PixelRegionDesc {
    std::vector<PixelInput> pixels;
    bool closeSameColorBoundaries = true;
};

struct SolidFillDesc {
    RegionId region;
    Color color;
};

struct DitherFillDesc {
    RegionId region;
    Color colorA;
    Color colorB;
    float density = 0.5f;
    uint32_t seed = 1;
    DitherSpace space = DitherSpace::Object;
};

struct RotateDesc {
    float angleDegrees = 0.0f;
    Vec2f pivot;
};

struct OperationInfo {
    OperationId id;
    std::string type;
    std::string summary;
};

class LSContext {
public:
    static std::unique_ptr<LSContext> create();

    ~LSContext();
    LSContext(const LSContext&) = delete;
    LSContext& operator=(const LSContext&) = delete;

    uint32_t engineVersion() const;

    Result<DocumentId> createDocument(uint32_t canvasWidth, uint32_t canvasHeight);
    Result<SpriteId> createSprite(DocumentId document);
    Result<LayerId> createLayer(SpriteId sprite, std::string_view name);

    Result<RegionId> createRectRegion(DocumentId document, const RectRegionDesc& desc);
    Result<RegionId> createEllipseRegion(DocumentId document, const EllipseRegionDesc& desc);
    Result<RegionId> createPixelRegion(DocumentId document, const PixelRegionDesc& desc);
    Result<IntervalSet> getRegionIntervals(RegionId region) const;
    Result<IntervalSet> getRegionBoundaryIntervals(RegionId region) const;

    Result<OperationId> addSolidFill(LayerId layer, const SolidFillDesc& desc);
    Result<OperationId> addDitherFill(LayerId layer, const DitherFillDesc& desc);
    Result<OperationId> addRotate(LayerId layer, const RotateDesc& desc);

    Result<std::vector<OperationInfo>> getLayerOperations(LayerId layer) const;
    Result<CompileResult> compileSprite(SpriteId sprite, const CompileProfile& profile) const;
    Result<CompileResult> compileLayer(LayerId layer, const CompileProfile& profile) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    LSContext();
};

} // namespace ls
