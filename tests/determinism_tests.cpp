// determinism_tests.cpp — the promise that the same document produces the same
// bytes everywhere.
//
// Two halves. The first checks the engine's own trigonometry against reference
// values, because the standard library's is not specified to the last bit and
// therefore cannot be the thing we compare against. The second compiles scenes
// that lean on that trigonometry and hashes the result: if a compiler, a
// platform or an innocent-looking refactor moves one pixel, a hash changes and
// this suite says which scene.
//
// A failing golden hash is not automatically a bug in this file. It means the
// output changed. Look at what changed before touching the constant.

#include "ls_test.h"
#include "ls_math.h"

#include <livesprite/livesprite.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ls;

namespace {

// ---------------------------------------------------------------- maths ----

constexpr double kTol = 1e-12;

bool near(double a, double b, double tol = kTol) {
    const double d = a - b;
    return (d < 0 ? -d : d) <= tol;
}

void testTrigAccuracy() {
    LS_CHECK(near(math::sinDouble(0.0), 0.0));
    LS_CHECK(near(math::cosDouble(0.0), 1.0));

    LS_CHECK(near(math::sinDouble(math::kPiOver2), 1.0));
    LS_CHECK(near(math::cosDouble(math::kPiOver2), 0.0));
    LS_CHECK(near(math::sinDouble(math::kPi), 0.0));
    LS_CHECK(near(math::cosDouble(math::kPi), -1.0));

    // 30, 45 and 60 degrees, where the exact answers are known.
    LS_CHECK(near(math::sinDouble(math::kPi / 6.0), 0.5));
    LS_CHECK(near(math::cosDouble(math::kPi / 3.0), 0.5));
    LS_CHECK(near(math::sinDouble(math::kPi / 4.0), 0.70710678118654752440));
    LS_CHECK(near(math::cosDouble(math::kPi / 4.0), 0.70710678118654752440));

    // Every quadrant, positive and negative, so the reduction's quadrant
    // bookkeeping is exercised rather than assumed. The platform library is a
    // cross-check here, not the authority: it is allowed to differ in the last
    // bits, which is exactly why the engine no longer calls it.
    for (int i = -400; i <= 400; ++i) {
        const double x = static_cast<double>(i) * 0.03125;
        const double s = math::sinDouble(x);
        const double c = math::cosDouble(x);
        LS_REQUIRE(near(s * s + c * c, 1.0, 1e-14));
        LS_REQUIRE(near(s, std::sin(x), 1e-12));
        LS_REQUIRE(near(c, std::cos(x), 1e-12));
    }
}

void testAtan2Quadrants() {
    LS_CHECK(near(math::atan2Double(0.0, 1.0), 0.0));
    LS_CHECK(near(math::atan2Double(1.0, 0.0), math::kPiOver2));
    LS_CHECK(near(math::atan2Double(-1.0, 0.0), -math::kPiOver2));
    LS_CHECK(near(math::atan2Double(0.0, -1.0), math::kPi));
    LS_CHECK(near(math::atan2Double(1.0, 1.0), math::kPi / 4.0));
    LS_CHECK(near(math::atan2Double(1.0, -1.0), 3.0 * math::kPi / 4.0));
    LS_CHECK(near(math::atan2Double(-1.0, -1.0), -3.0 * math::kPi / 4.0));
    LS_CHECK(near(math::atan2Double(0.0, 0.0), 0.0));

    for (int i = -32; i <= 32; ++i) {
        for (int j = -32; j <= 32; ++j) {
            if (i == 0 && j == 0) { continue; }
            const double y = static_cast<double>(i);
            const double x = static_cast<double>(j);
            LS_REQUIRE(near(math::atan2Double(y, x), std::atan2(y, x), 1e-12));
        }
    }
}

void testHugeAnglesStayFinite() {
    // A driven parameter is under app control, so an absurd angle must produce
    // a reproducible answer rather than a platform-dependent one.
    const double huge[] = { 1e10, -1e10, 1e18, -1e18, 1e300 };
    for (double x : huge) {
        const double s = math::sinDouble(x);
        const double c = math::cosDouble(x);
        LS_CHECK(s >= -1.0000001 && s <= 1.0000001);
        LS_CHECK(c >= -1.0000001 && c <= 1.0000001);
    }
    // A vertical tangent stays finite, so a skew cannot poison a transform
    // matrix. The sign is not ours to predict: float(pi/2) rounds just past
    // pi/2, landing on the negative side of the cosine, so the tangent is large
    // and negative. Magnitude and finiteness are the contract.
    const float vertical = math::tanf(static_cast<float>(math::kPiOver2));
    const float magnitude = vertical < 0.f ? -vertical : vertical;
    LS_CHECK(magnitude > 1e6f);
    LS_CHECK(magnitude <= 1e12f);
}

// --------------------------------------------------------------- hashes ----

uint64_t hashRaster(const RasterBuffer& raster) {
    uint64_t hash = 1469598103934665603ull;              // FNV-1a 64
    const auto mix = [&hash](uint8_t byte) {
        hash ^= byte;
        hash *= 1099511628211ull;
    };
    mix(static_cast<uint8_t>(raster.width));
    mix(static_cast<uint8_t>(raster.height));
    for (uint32_t y = 0; y < raster.height; ++y) {
        for (uint32_t x = 0; x < raster.width; ++x) {
            const Color pixel = readPixel(raster, static_cast<int32_t>(x),
                                          static_cast<int32_t>(y));
            mix(pixel.r); mix(pixel.g); mix(pixel.b); mix(pixel.a);
        }
    }
    return hash;
}

CompileProfile exportProfile(uint32_t size) {
    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = size;
    profile.outputHeight = size;
    profile.palette = PalettePolicy::Unconstrained;
    return profile;
}

struct Scene {
    DocumentId doc;
    SpriteId   sprite;
    LayerId    layer;
    PaletteId  palette;
};

Scene makeScene(LSContext& ctx, uint32_t canvas) {
    Scene scene;
    scene.doc = ctx.createDocument({"golden", canvas, canvas}).value;
    scene.sprite = ctx.createSprite(scene.doc).value;
    scene.layer = ctx.createLayer(scene.sprite, {"main"}).value;
    scene.palette = ctx.createPalette(scene.doc, {"golden", {
        {0, {24, 26, 34, 255},    "ink"},
        {1, {92, 140, 200, 255},  "body"},
        {2, {214, 228, 250, 255}, "light"}}}).value;
    ctx.bindSpritePalette(scene.sprite, scene.palette);
    return scene;
}

// Each scene is chosen to depend on the trigonometry: an off-axis rotation, a
// tangent-driven skew, an atan2-driven angular dither, and a curved outline.

uint64_t sceneRotation(LSContext& ctx) {
    Scene scene = makeScene(ctx, 32);
    const GeometryId rect = ctx.createRect(scene.doc, {{8.f, 10.f}, 16.f, 10.f, 0.f}).value;
    const RegionId region = ctx.createRegionFromGeometry(rect).value;

    FillSolidOp fill;
    fill.targetRegion = region;
    fill.paletteRole = 1;
    ctx.addOperation(scene.layer, fill);

    RotateOp turn;
    turn.targetLayer = scene.layer;
    turn.angleDegrees = 37.f;                 // not a multiple of 90: real sin/cos
    turn.pivotFallback = {16.f, 16.f};
    ctx.addOperation(scene.layer, turn);

    return hashRaster(ctx.compileSprite(scene.sprite, exportProfile(32)).value.raster);
}

uint64_t sceneSkew(LSContext& ctx) {
    Scene scene = makeScene(ctx, 32);
    const GeometryId rect = ctx.createRect(scene.doc, {{10.f, 10.f}, 12.f, 12.f, 0.f}).value;
    const RegionId region = ctx.createRegionFromGeometry(rect).value;

    FillSolidOp fill;
    fill.targetRegion = region;
    fill.paletteRole = 1;
    ctx.addOperation(scene.layer, fill);

    SkewOp skew;
    skew.targetLayer = scene.layer;
    skew.angleX = 23.f;                       // tangent
    skew.pivotFallback = {16.f, 16.f};
    ctx.addOperation(scene.layer, skew);

    return hashRaster(ctx.compileSprite(scene.sprite, exportProfile(32)).value.raster);
}

uint64_t sceneAngularDither(LSContext& ctx) {
    Scene scene = makeScene(ctx, 32);
    const GeometryId disc = ctx.createCircle(scene.doc, {{16.f, 16.f}, 12.f}).value;
    const RegionId region = ctx.createRegionFromGeometry(disc).value;

    const RampId ramp = ctx.createRamp(scene.doc, {"sweep", {
        {0.f, {92, 140, 200, 255}},
        {1.f, {214, 228, 250, 255}}}, true}).value;
    const PatternId screen = ctx.createDitherPattern(scene.doc, DitherPatternKind::Bayer8).value;

    FillDitherOp sweep;
    sweep.targetRegion = region;
    sweep.ramp = ramp;
    sweep.pattern = screen;
    sweep.modulation = DitherModulation::Angular;   // atan2 per pixel
    sweep.gradientStart = {16.f, 16.f};
    sweep.gradientEnd = {28.f, 16.f};
    ctx.addOperation(scene.layer, sweep);

    return hashRaster(ctx.compileSprite(scene.sprite, exportProfile(32)).value.raster);
}

uint64_t sceneEllipseOutline(LSContext& ctx) {
    Scene scene = makeScene(ctx, 32);
    const GeometryId oval = ctx.createEllipse(scene.doc, {{16.f, 16.f}, 13.f, 8.f}).value;
    const RegionId region = ctx.createRegionFromGeometry(oval).value;

    FillSolidOp fill;
    fill.targetRegion = region;
    fill.paletteRole = 2;
    ctx.addOperation(scene.layer, fill);

    StrokeRegionBoundaryOp edge;
    edge.targetRegion = region;
    edge.paletteRole = 0;
    ctx.addOperation(scene.layer, edge);

    return hashRaster(ctx.compileSprite(scene.sprite, exportProfile(32)).value.raster);
}

struct Golden {
    const char* name;
    uint64_t (*build)(LSContext&);
    uint64_t expected;
};

const Golden kGolden[] = {
    { "rotation-37deg",   sceneRotation,       0xe05fc5357846333bull },
    { "skew-23deg",       sceneSkew,           0xf81119f97f7a953bull },
    { "angular-dither",   sceneAngularDither,  0x31b96a2ff741b453ull },
    { "ellipse-outline",  sceneEllipseOutline, 0x0c9273d10c016213ull },
};

void testGoldenHashes() {
    for (const Golden& golden : kGolden) {
        auto ctx = LSContext::create();
        const uint64_t actual = golden.build(*ctx);
        if (actual != golden.expected) {
            std::printf("  golden %-16s expected 0x%016llxull, got 0x%016llxull\n",
                        golden.name,
                        static_cast<unsigned long long>(golden.expected),
                        static_cast<unsigned long long>(actual));
        }
        LS_CHECK(actual == golden.expected);
    }
}

// A compile is a pure function of its inputs: the same scene built in two
// separate contexts must agree, so nothing leaks between compiles.
void testRepeatabilityWithinAProcess() {
    for (const Golden& golden : kGolden) {
        auto first = LSContext::create();
        auto second = LSContext::create();
        LS_CHECK(golden.build(*first) == golden.build(*second));
    }
}

} // namespace

int main() {
    testTrigAccuracy();
    testAtan2Quadrants();
    testHugeAnglesStayFinite();
    testRepeatabilityWithinAProcess();
    testGoldenHashes();
    return lstest::report("determinism");
}
