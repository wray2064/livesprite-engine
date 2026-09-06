// dither_tests.cpp — the dither contract: gradients built from dithered
// colour, the prebaked pattern library plus imported tiles, and the three
// anchor modes (local, global, fixed) under motion.

#include "ls_test.h"

#include <livesprite/livesprite.h>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

using namespace ls;

namespace {

CompileProfile exportProfile(uint32_t size = 32) {
    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = size;
    profile.outputHeight = size;
    profile.palette = PalettePolicy::Unconstrained;
    return profile;
}

struct Scene {
    DocumentId doc;
    SpriteId sprite;
    LayerId layer;
    RegionId region;
};

Scene makeScene(LSContext& ctx, Vec2f origin, float size, uint32_t canvas = 32) {
    Scene scene;
    scene.doc = ctx.createDocument({"dither", canvas, canvas}).value;
    scene.sprite = ctx.createSprite(scene.doc).value;
    scene.layer = ctx.createLayer(scene.sprite, {"main"}).value;
    const GeometryId rect = ctx.createRect(scene.doc, {origin, size, size, 0.f}).value;
    scene.region = ctx.createRegionFromGeometry(rect).value;
    return scene;
}

// The pattern as the shape sees it: sampled relative to the shape origin, so a
// pattern that travels with the object reads the same wherever the object is.
std::vector<Color> patternRelativeToShape(const RasterBuffer& raster, Vec2i origin, int32_t size) {
    std::vector<Color> out;
    for (int32_t y = 0; y < size; ++y) {
        for (int32_t x = 0; x < size; ++x) {
            out.push_back(readPixel(raster, origin.x + x, origin.y + y));
        }
    }
    return out;
}

int countOf(const std::vector<Color>& pixels, Color color) {
    int count = 0;
    for (Color pixel : pixels) {
        count += pixel == color ? 1 : 0;
    }
    return count;
}

// --- gradients -------------------------------------------------------------

void testDitheredGradient() {
    auto ctx = LSContext::create();
    const Scene scene = makeScene(*ctx, {4.f, 4.f}, 24.f, 32);

    const Color dark   {30, 30, 60, 255};
    const Color mid    {120, 120, 170, 255};
    const Color light  {230, 230, 250, 255};
    const RampId ramp = ctx->createRamp(scene.doc,
        {"tone", {{0.f, dark}, {0.5f, mid}, {1.f, light}}, true}).value;
    const PatternId pattern = ctx->createDitherPattern(scene.doc, DitherPatternKind::Bayer4).value;

    FillDitherOp dither;
    dither.targetRegion = scene.region;
    dither.ramp = ramp;
    dither.pattern = pattern;
    dither.modulation = DitherModulation::Linear;
    dither.gradientStart = {0.f, 0.f};        // left edge of the shape
    dither.gradientEnd = {24.f, 0.f};         // right edge
    LS_CHECK(ctx->addOperation(scene.layer, dither).ok());

    auto compiled = ctx->compileSprite(scene.sprite, exportProfile());
    LS_REQUIRE(compiled.ok());

    // Only ramp colours appear: a dithered gradient mixes stops, it does not
    // invent intermediate colours.
    std::set<uint32_t> seen;
    for (int32_t y = 4; y < 28; ++y) {
        for (int32_t x = 4; x < 28; ++x) {
            const Color pixel = readPixel(compiled.value.raster, x, y);
            seen.insert((static_cast<uint32_t>(pixel.r) << 16) |
                        (static_cast<uint32_t>(pixel.g) << 8) | pixel.b);
        }
    }
    LS_CHECK(seen.size() == 3);

    // The mix walks along the gradient: dark dominates the left column band,
    // light dominates the right, and the middle stop appears in between.
    const std::vector<Color> leftBand = patternRelativeToShape(compiled.value.raster, {4, 4}, 6);
    const std::vector<Color> rightBand = patternRelativeToShape(compiled.value.raster, {22, 4}, 6);
    LS_CHECK(countOf(leftBand, dark) > countOf(leftBand, light));
    LS_CHECK(countOf(rightBand, light) > countOf(rightBand, dark));

    int midCount = 0;
    for (int32_t y = 4; y < 28; ++y) {
        for (int32_t x = 4; x < 28; ++x) {
            midCount += readPixel(compiled.value.raster, x, y) == mid ? 1 : 0;
        }
    }
    LS_CHECK(midCount > 0);

    // Radial and angular modulation produce different arrangements from the
    // same ramp and pattern.
    FillDitherOp radial = dither;
    radial.modulation = DitherModulation::Radial;
    radial.gradientStart = {12.f, 12.f};
    radial.gradientEnd = {24.f, 12.f};
    const Scene radialScene = makeScene(*ctx, {4.f, 4.f}, 24.f, 32);
    radial.targetRegion = radialScene.region;
    radial.ramp = ctx->createRamp(radialScene.doc,
        {"tone", {{0.f, dark}, {0.5f, mid}, {1.f, light}}, true}).value;
    radial.pattern = ctx->createDitherPattern(radialScene.doc, DitherPatternKind::Bayer4).value;
    LS_CHECK(ctx->addOperation(radialScene.layer, radial).ok());
    auto radialCompiled = ctx->compileSprite(radialScene.sprite, exportProfile());
    LS_REQUIRE(radialCompiled.ok());

    // A radial gradient is dark at its centre and light at the rim.
    LS_CHECK(readPixel(radialCompiled.value.raster, 16, 16) == dark ||
             readPixel(radialCompiled.value.raster, 16, 16) == mid);
    const std::vector<Color> centre =
        patternRelativeToShape(radialCompiled.value.raster, {14, 14}, 4);
    const std::vector<Color> rim =
        patternRelativeToShape(radialCompiled.value.raster, {4, 4}, 4);
    LS_CHECK(countOf(centre, dark) > countOf(rim, dark));

    // A constant density still behaves as classic two-tone dithering.
    const Scene flatScene = makeScene(*ctx, {8.f, 8.f}, 8.f, 32);
    FillDitherOp flat;
    flat.targetRegion = flatScene.region;
    flat.ramp = ctx->createRamp(flatScene.doc, {"two", {{0.f, dark}, {1.f, light}}, true}).value;
    flat.pattern = ctx->createDitherPattern(flatScene.doc, DitherPatternKind::Bayer4).value;
    flat.density = 0.5f;
    LS_CHECK(ctx->addOperation(flatScene.layer, flat).ok());
    auto flatCompiled = ctx->compileSprite(flatScene.sprite, exportProfile());
    LS_REQUIRE(flatCompiled.ok());
    const std::vector<Color> flatPixels =
        patternRelativeToShape(flatCompiled.value.raster, {8, 8}, 8);
    LS_CHECK(countOf(flatPixels, dark) == 32);
    LS_CHECK(countOf(flatPixels, light) == 32);
}

// --- pattern library and external tiles ------------------------------------

void testPatternLibrary() {
    auto ctx = LSContext::create();
    auto doc = ctx->createDocument({"patterns", 32, 32});
    LS_REQUIRE(doc.ok());

    const std::vector<DitherPatternKind> kinds = ctx->ditherPatternKinds();
    LS_CHECK(kinds.size() == 12);

    for (DitherPatternKind kind : kinds) {
        auto pattern = ctx->createDitherPattern(doc.value, kind);
        LS_CHECK(pattern.ok());
        if (!pattern.ok()) {
            continue;
        }
        auto tile = ctx->getPattern(pattern.value);
        LS_REQUIRE(tile.ok());
        LS_CHECK(tile.value.tileWidth > 0 && tile.value.tileHeight > 0);
        LS_CHECK(tile.value.mask.size() ==
                 static_cast<size_t>(tile.value.tileWidth) * tile.value.tileHeight);
        LS_CHECK(tile.value.levels >= 2);
        LS_CHECK(!ctx->ditherPatternName(kind).empty());

        // Every rank must be inside the declared level count, otherwise the
        // tile could never reach full coverage.
        uint32_t highest = 0;
        for (uint8_t rank : tile.value.mask) {
            highest = std::max(highest, static_cast<uint32_t>(rank));
        }
        LS_CHECK(highest < tile.value.levels);
    }

    // A line screen must vary along one axis only.
    auto horizontal = ctx->createDitherPattern(doc.value, DitherPatternKind::HorizontalLines);
    LS_REQUIRE(horizontal.ok());
    auto horizontalTile = ctx->getPattern(horizontal.value);
    LS_REQUIRE(horizontalTile.ok());
    bool rowsUniform = true;
    for (uint32_t y = 0; y < horizontalTile.value.tileHeight; ++y) {
        for (uint32_t x = 1; x < horizontalTile.value.tileWidth; ++x) {
            rowsUniform = rowsUniform &&
                horizontalTile.value.mask[static_cast<size_t>(y) * horizontalTile.value.tileWidth + x] ==
                horizontalTile.value.mask[static_cast<size_t>(y) * horizontalTile.value.tileWidth];
        }
    }
    LS_CHECK(rowsUniform);

    // Scale enlarges the tile, which thickens the lines.
    auto thick = ctx->createDitherPattern(doc.value, DitherPatternKind::HorizontalLines, 3);
    LS_REQUIRE(thick.ok());
    LS_CHECK(ctx->getPattern(thick.value).value.tileHeight ==
             horizontalTile.value.tileHeight * 3);

    // Ordered kinds are permutations: every rank appears exactly once.
    auto bayer = ctx->createDitherPattern(doc.value, DitherPatternKind::Bayer4);
    std::vector<uint8_t> ranks = ctx->getPattern(bayer.value).value.mask;
    std::sort(ranks.begin(), ranks.end());
    bool permutation = ranks.size() == 16;
    for (size_t i = 0; i < ranks.size() && permutation; ++i) {
        permutation = ranks[i] == static_cast<uint8_t>(i);
    }
    LS_CHECK(permutation);

    LS_CHECK(ctx->createDitherPattern(doc.value, DitherPatternKind::Bayer8, 0).fail());
    LS_CHECK(ctx->createDitherPattern(doc.value, DitherPatternKind::Bayer8, 4).fail());  // 32x32 > 256 cells
}

void testExternalPatterns() {
    auto ctx = LSContext::create();
    const Scene scene = makeScene(*ctx, {8.f, 8.f}, 8.f, 32);

    // A tile handed straight to the engine.
    PatternTileDesc custom;
    custom.name = "custom";
    custom.tileWidth = 2;
    custom.tileHeight = 2;
    custom.levels = 4;
    custom.mask = {0, 2, 3, 1};
    auto handMade = ctx->createPattern(scene.doc, custom);
    LS_CHECK(handMade.ok());

    // A malformed tile is refused rather than compiled into nonsense.
    PatternTileDesc broken = custom;
    broken.mask = {0, 1};
    LS_CHECK(ctx->createPattern(scene.doc, broken).fail());

    // A tile imported from a raster, as a threshold matrix.
    auto source = makeRaster(4, 4);
    for (int32_t y = 0; y < 4; ++y) {
        for (int32_t x = 0; x < 4; ++x) {
            const uint8_t level = static_cast<uint8_t>((x + y * 4) * 16);
            writePixel(source, x, y, Color{level, level, level, 255});
        }
    }
    auto imported = ctx->createPatternFromRaster(scene.doc, source, PatternImportMode::Threshold,
                                                 "imported");
    LS_REQUIRE(imported.ok());
    auto importedTile = ctx->getPattern(imported.value);
    LS_CHECK(importedTile.value.tileWidth == 4);
    LS_CHECK(importedTile.value.levels == 256);
    LS_CHECK(importedTile.value.mask[0] == 0);
    LS_CHECK(importedTile.value.mask[15] > importedTile.value.mask[0]);
    LS_CHECK(importedTile.value.colors.empty());

    // The same raster imported as a colour texture, then tiled across a fill:
    // this is the tileable texture path.
    auto texture = makeRaster(4, 4);
    const Color a {200, 40, 40, 255};
    const Color b {40, 40, 200, 255};
    for (int32_t y = 0; y < 4; ++y) {
        for (int32_t x = 0; x < 4; ++x) {
            writePixel(texture, x, y, ((x + y) % 2) == 0 ? a : b);
        }
    }
    auto colorTile = ctx->createPatternFromRaster(scene.doc, texture, PatternImportMode::Colors,
                                                  "checker texture");
    LS_REQUIRE(colorTile.ok());
    LS_CHECK(ctx->getPattern(colorTile.value).value.colors.size() == 16);

    FillTexturePatternOp fill;
    fill.targetRegion = scene.region;
    fill.pattern = colorTile.value;
    LS_CHECK(ctx->addOperation(scene.layer, fill).ok());

    auto compiled = ctx->compileSprite(scene.sprite, exportProfile());
    LS_REQUIRE(compiled.ok());

    // The texture paints its own colours and repeats on its tile period.
    LS_CHECK(readPixel(compiled.value.raster, 8, 8) == a);
    LS_CHECK(readPixel(compiled.value.raster, 9, 8) == b);
    LS_CHECK(readPixel(compiled.value.raster, 12, 8) == readPixel(compiled.value.raster, 8, 8));
    LS_CHECK(readPixel(compiled.value.raster, 8, 12) == readPixel(compiled.value.raster, 8, 8));
}

// --- anchoring -------------------------------------------------------------

// Build the same dithered square twice and compare how the pattern behaves
// under a move and under a quarter turn.
struct AnchorProbe {
    std::vector<Color> still;
    std::vector<Color> translated;
    std::vector<Color> rotated;
};

AnchorProbe probeAnchor(PatternAnchor anchor) {
    AnchorProbe probe;
    auto ctx = LSContext::create();

    const Color dark {20, 20, 20, 255};
    const Color light {240, 240, 240, 255};

    // Horizontal line screen: its orientation is visible at a glance, which is
    // what makes rotation locking testable.
    auto build = [&](Vec2f origin, float rotateDegrees) {
        const Scene scene = makeScene(*ctx, origin, 8.f, 32);
        FillDitherOp dither;
        dither.targetRegion = scene.region;
        dither.ramp = ctx->createRamp(scene.doc, {"two", {{0.f, dark}, {1.f, light}}, true}).value;
        dither.pattern = ctx->createDitherPattern(scene.doc,
                                                  DitherPatternKind::HorizontalLines).value;
        dither.density = 0.5f;
        dither.anchor = anchor;
        ctx->addOperation(scene.layer, dither);

        if (rotateDegrees != 0.f) {
            RotateOp rotate;
            rotate.targetLayer = scene.layer;
            rotate.angleDegrees = rotateDegrees;
            rotate.pivotFallback = { origin.x + 4.f, origin.y + 4.f };
            ctx->addOperation(scene.layer, rotate);
        }

        auto compiled = ctx->compileSprite(scene.sprite, exportProfile());
        return compiled.value.raster;
    };

    probe.still = patternRelativeToShape(build({8.f, 8.f}, 0.f), {8, 8}, 8);
    probe.translated = patternRelativeToShape(build({13.f, 11.f}, 0.f), {13, 11}, 8);
    probe.rotated = patternRelativeToShape(build({8.f, 8.f}, 90.f), {8, 8}, 8);
    return probe;
}

// Rows of a square sample: true when the sample is banded horizontally (every
// row uniform), which is how a horizontal line screen reads.
bool bandedByRow(const std::vector<Color>& pixels, int32_t size) {
    for (int32_t y = 0; y < size; ++y) {
        for (int32_t x = 1; x < size; ++x) {
            if (pixels[static_cast<size_t>(y) * size + x] !=
                pixels[static_cast<size_t>(y) * size]) {
                return false;
            }
        }
    }
    return true;
}

void testAnchorModes() {
    const AnchorProbe local = probeAnchor(PatternAnchor::Local);
    const AnchorProbe global = probeAnchor(PatternAnchor::Global);
    const AnchorProbe fixed = probeAnchor(PatternAnchor::Fixed);

    // Every mode starts from the same picture: horizontal bands.
    LS_CHECK(bandedByRow(local.still, 8));
    LS_CHECK(bandedByRow(global.still, 8));
    LS_CHECK(bandedByRow(fixed.still, 8));

    // Moving the object.
    //   local and global travel with it: the pattern looks unchanged.
    //   fixed stays with the canvas: the shape slides over the pattern.
    LS_CHECK(local.translated == local.still);
    LS_CHECK(global.translated == global.still);
    LS_CHECK(fixed.translated != fixed.still);

    // Rotating the object.
    //   local rotates too, so horizontal bands become vertical.
    //   global and fixed are rotation locked: bands stay horizontal.
    LS_CHECK(!bandedByRow(local.rotated, 8));
    LS_CHECK(bandedByRow(global.rotated, 8));
    LS_CHECK(bandedByRow(fixed.rotated, 8));
}

// A Global pattern must follow its object through a free-form deform, not only
// through an affine transform. A single-pin deform is a pure translation, which
// makes the expected phase exactly checkable.
void testAnchorFollowsDeform() {
    auto ctx = LSContext::create();
    const Color dark {20, 20, 20, 255};
    const Color light {240, 240, 240, 255};

    auto build = [&](PatternAnchor anchor, bool deform) {
        const Scene scene = makeScene(*ctx, {8.f, 8.f}, 8.f, 32);
        FillDitherOp dither;
        dither.targetRegion = scene.region;
        dither.ramp = ctx->createRamp(scene.doc, {"two", {{0.f, dark}, {1.f, light}}, true}).value;
        dither.pattern = ctx->createDitherPattern(scene.doc,
                                                  DitherPatternKind::HorizontalLines).value;
        dither.density = 0.5f;
        dither.anchor = anchor;
        ctx->addOperation(scene.layer, dither);

        if (deform) {
            PinDeformOp pin;
            pin.targetLayer = scene.layer;
            pin.pins = {{12.f, 12.f}};
            pin.pinTargets = {{17.f, 15.f}};    // a clean translation of (5, 3)
            ctx->addOperation(scene.layer, pin);
        }

        auto compiled = ctx->compileSprite(scene.sprite, exportProfile());
        return compiled.value.raster;
    };

    const std::vector<Color> baseline = patternRelativeToShape(build(PatternAnchor::Global, false),
                                                              {8, 8}, 8);
    const std::vector<Color> deformedGlobal =
        patternRelativeToShape(build(PatternAnchor::Global, true), {13, 11}, 8);
    const std::vector<Color> deformedFixed =
        patternRelativeToShape(build(PatternAnchor::Fixed, true), {13, 11}, 8);

    // The deform really did move the shape.
    LS_CHECK(countOf(baseline, dark) > 0 && countOf(deformedGlobal, dark) > 0);

    // Global followed the shape through the deform: same pattern phase.
    LS_CHECK(deformedGlobal == baseline);

    // Fixed stayed with the canvas, so the shape slid over the pattern.
    LS_CHECK(deformedFixed != baseline);
}

void testAnchorSurvivesSaveLoad() {
    auto ctx = LSContext::create();
    const Scene scene = makeScene(*ctx, {8.f, 8.f}, 10.f, 32);

    FillDitherOp dither;
    dither.targetRegion = scene.region;
    dither.ramp = ctx->createRamp(scene.doc,
        {"tone", {{0.f, {10, 10, 10, 255}}, {1.f, {250, 250, 250, 255}}}, true}).value;
    dither.pattern = ctx->createDitherPattern(scene.doc, DitherPatternKind::ClusteredDot).value;
    dither.modulation = DitherModulation::Linear;
    dither.gradientStart = {0.f, 0.f};
    dither.gradientEnd = {10.f, 0.f};
    dither.anchor = PatternAnchor::Global;
    LS_CHECK(ctx->addOperation(scene.layer, dither).ok());

    auto before = ctx->compileSprite(scene.sprite, exportProfile());
    LS_REQUIRE(before.ok());

    auto saved = ctx->serializeDocument(scene.doc);
    LS_REQUIRE(saved.ok());
    auto loaded = LSContext::create();
    auto restoredDoc = loaded->deserializeDocument(saved.value);
    LS_REQUIRE(restoredDoc.ok());

    SpriteId restored;
    for (uint64_t candidate = 1; candidate < 4096; ++candidate) {
        if (loaded->getSpriteInfo(SpriteId{candidate}).ok()) {
            restored = SpriteId{candidate};
            break;
        }
    }
    LS_REQUIRE(restored.valid());

    auto after = loaded->compileSprite(restored, exportProfile());
    LS_REQUIRE(after.ok());
    LS_CHECK(after.value.raster.pixels == before.value.raster.pixels);
}

} // namespace

int main() {
    testDitheredGradient();
    testPatternLibrary();
    testExternalPatterns();
    testAnchorModes();
    testAnchorFollowsDeform();
    testAnchorSurvivesSaveLoad();
    return lstest::report("dither");
}
