// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors

// outline_tests.cpp -- what an outline traces.
//
// An outline here is not a filter that adds pixels; it is an operation resolved
// during the compile, so it follows whatever the artwork currently is. The
// question this file settles is *whose* artwork: the layer the outline sits on,
// or everything the sprite draws.
//
// Both are wanted, and they are different pictures. A character built from a
// body layer and an arm layer wants one line around the figure; a highlight on
// one part wants a line around that part. The difference is visible where the
// two layers meet -- a per-layer outline draws a seam there, and a sprite-wide
// one does not.

#include "ls_test.h"

#include <livesprite/livesprite.h>

#include <memory>
#include <variant>
#include <vector>

using namespace ls;

namespace {

constexpr uint32_t kSize = 24;

struct Scene {
    std::unique_ptr<LSContext> engine = LSContext::create();
    DocumentId doc;
    SpriteId   sprite;

    bool build() {
        auto document = engine->createDocument({ "outline", kSize, kSize });
        if (document.fail()) { return false; }
        doc = document.value;
        auto made = engine->createSprite(doc);
        if (made.fail()) { return false; }
        sprite = made.value;
        return true;
    }

    // A filled box on its own layer.
    LayerId box(const char* name, float x, float y, float w, float h, Color colour) {
        auto rect = engine->createRect(doc, { { x, y }, w, h, 0.f });
        if (rect.fail()) { return LayerId::null(); }
        auto region = engine->createRegionFromGeometry(rect.value);
        if (region.fail()) { return LayerId::null(); }
        auto layer = engine->createLayer(sprite, { name });
        if (layer.fail()) { return LayerId::null(); }

        FillSolidOp fill;
        fill.targetRegion = region.value;
        fill.fallbackColor = colour;
        if (engine->addOperation(layer.value, fill).fail()) { return LayerId::null(); }
        return layer.value;
    }

    RasterBuffer render() {
        CompileProfile profile;
        profile.type = CompileProfileType::Export;
        profile.outputWidth = kSize;
        profile.outputHeight = kSize;
        profile.palette = PalettePolicy::Unconstrained;
        auto compiled = engine->compileSprite(sprite, profile);
        return compiled.ok() ? compiled.value.raster : RasterBuffer{};
    }
};

Color at(const RasterBuffer& raster, int32_t x, int32_t y) {
    return readPixel(raster, x, y);
}

bool isColour(Color c, Color want) {
    return c.r == want.r && c.g == want.g && c.b == want.b && c.a == want.a;
}

size_t countColour(const RasterBuffer& raster, Color want) {
    size_t total = 0;
    for (uint32_t y = 0; y < raster.height; ++y) {
        for (uint32_t x = 0; x < raster.width; ++x) {
            if (isColour(at(raster, static_cast<int32_t>(x), static_cast<int32_t>(y)), want)) {
                ++total;
            }
        }
    }
    return total;
}

const Color kInk { 10, 10, 16, 255 };
const Color kBody{ 220, 150, 60, 255 };
const Color kArm { 90, 140, 220, 255 };

// Two boxes that touch along a vertical edge. Where they meet is the pixel that
// tells the two kinds of outline apart.
struct TwoParts {
    Scene scene;
    LayerId body;
    LayerId arm;

    bool build() {
        if (!scene.build()) { return false; }
        body = scene.box("body", 6.f, 6.f, 6.f, 10.f, kBody);
        arm  = scene.box("arm", 12.f, 8.f, 5.f, 5.f, kArm);
        return body.valid() && arm.valid();
    }
};

// The old behaviour, and still the default: an outline with no sprite named
// traces the layer it sits on.
void testAnOutlineWithNoSpriteTracesItsOwnLayer() {
    TwoParts parts;
    LS_REQUIRE(parts.build());

    GenerateSilhouetteOutlineOp outline;
    outline.thickness = 1.f;
    outline.side = OutlineSide::Outside;
    outline.fallbackColor = kInk;
    LS_REQUIRE(parts.scene.engine->addOperation(parts.body, outline).ok());

    const RasterBuffer raster = parts.scene.render();
    LS_REQUIRE(!raster.empty());

    // Outside the body on the left, so there is ink.
    LS_CHECK(isColour(at(raster, 5, 10), kInk));

    // The seam: the body's outline runs down its right edge, which is inside
    // the arm. The arm is a later layer, so it paints over the seam -- but the
    // rows where the arm is not, above and below it, still show it.
    LS_CHECK(isColour(at(raster, 12, 6), kInk));    // above the arm
    LS_CHECK(isColour(at(raster, 12, 16), kInk));   // below it
}

// The thing that was missing: naming the sprite traces the whole figure.
void testNamingTheSpriteTracesTheWholeFigure() {
    TwoParts parts;
    LS_REQUIRE(parts.build());

    GenerateSilhouetteOutlineOp outline;
    outline.thickness = 1.f;
    outline.side = OutlineSide::Outside;
    outline.fallbackColor = kInk;
    outline.targetSprite = parts.scene.sprite;

    // On its own layer, above everything, which is where an artist puts it.
    auto layer = parts.scene.engine->createLayer(parts.scene.sprite, { "outline" });
    LS_REQUIRE(layer.ok());
    LS_REQUIRE(parts.scene.engine->addOperation(layer.value, outline).ok());

    const RasterBuffer raster = parts.scene.render();
    LS_REQUIRE(!raster.empty());

    // Around the outside of the whole figure.
    LS_CHECK(isColour(at(raster, 5, 10), kInk));    // left of the body
    LS_CHECK(isColour(at(raster, 17, 10), kInk));   // right of the arm
    LS_CHECK(isColour(at(raster, 9, 5), kInk));     // above the body

    // And -- the point -- no seam where the two parts meet. The pixel at the
    // join is arm, not ink: the figure has one silhouette, not two.
    LS_CHECK(isColour(at(raster, 12, 10), kArm));
}

// Stated as a difference rather than as two separate pictures, so the test
// fails if the sprite-wide mode quietly falls back to per-layer.
void testTheTwoKindsOfOutlineDiffer() {
    const auto draw = [](bool wholeSprite) {
        TwoParts parts;
        if (!parts.build()) { return RasterBuffer{}; }

        GenerateSilhouetteOutlineOp outline;
        outline.thickness = 1.f;
        outline.side = OutlineSide::Outside;
        outline.fallbackColor = kInk;
        if (wholeSprite) {
            outline.targetSprite = parts.scene.sprite;
        }
        auto layer = parts.scene.engine->createLayer(parts.scene.sprite, { "outline" });
        if (layer.fail()) { return RasterBuffer{}; }
        if (parts.scene.engine->addOperation(layer.value, outline).fail()) {
            return RasterBuffer{};
        }
        return parts.scene.render();
    };

    const RasterBuffer perLayer = draw(false);
    const RasterBuffer figure = draw(true);
    LS_REQUIRE(!perLayer.empty() && !figure.empty());

    // On its own layer with nothing drawn in it, a per-layer outline has
    // nothing to trace at all, so it draws no ink whatever.
    LS_CHECK(countColour(perLayer, kInk) == 0);
    LS_CHECK(countColour(figure, kInk) > 0);
}

// The outline must not trace itself: if it did, the second pass would find the
// first pass's ink and grow a line around the line.
void testASpriteOutlineDoesNotTraceItself() {
    TwoParts parts;
    LS_REQUIRE(parts.build());

    GenerateSilhouetteOutlineOp outline;
    outline.thickness = 1.f;
    outline.side = OutlineSide::Outside;
    outline.fallbackColor = kInk;
    outline.targetSprite = parts.scene.sprite;
    auto layer = parts.scene.engine->createLayer(parts.scene.sprite, { "outline" });
    LS_REQUIRE(layer.ok());
    LS_REQUIRE(parts.scene.engine->addOperation(layer.value, outline).ok());

    const RasterBuffer once = parts.scene.render();
    const size_t ink = countColour(once, kInk);
    LS_REQUIRE(ink > 0);

    // Two pixels out from the artwork there is nothing, which is what says the
    // line is one pixel thick rather than a line around a line.
    LS_CHECK(at(once, 4, 10).a == 0);

    // And compiling again gives the same picture: the silhouette pass is not
    // accumulating anything between compiles.
    const RasterBuffer twice = parts.scene.render();
    LS_CHECK(once.pixels == twice.pixels);
    LS_CHECK(countColour(twice, kInk) == ink);
}

// An outline follows the artwork, which is the whole reason it is an operation
// rather than a filter. Moving one part must move the figure's outline.
void testTheFigureOutlineFollowsEveryPart() {
    TwoParts parts;
    LS_REQUIRE(parts.build());

    GenerateSilhouetteOutlineOp outline;
    outline.thickness = 1.f;
    outline.side = OutlineSide::Outside;
    outline.fallbackColor = kInk;
    outline.targetSprite = parts.scene.sprite;
    auto layer = parts.scene.engine->createLayer(parts.scene.sprite, { "outline" });
    LS_REQUIRE(layer.ok());
    LS_REQUIRE(parts.scene.engine->addOperation(layer.value, outline).ok());

    const RasterBuffer before = parts.scene.render();
    LS_REQUIRE(!before.empty());
    LS_CHECK(isColour(at(before, 17, 10), kInk));    // right of the arm

    // Move the arm. Nothing touches the outline operation.
    auto info = parts.scene.engine->getLayerInfo(parts.arm);
    LS_REQUIRE(info.ok() && !info.value.operations.empty());
    auto target = parts.scene.engine->getOperationParameter(
        info.value.operations.front(), "targetRegion");
    LS_REQUIRE(target.ok());
    auto geometry = parts.scene.engine->getRegionSourceGeometry(
        RegionId{ std::get<uint64_t>(target.value) });
    LS_REQUIRE(geometry.ok());
    LS_REQUIRE(parts.scene.engine->updateRect(geometry.value,
                                              { { 12.f, 8.f }, 8.f, 5.f }).ok());

    const RasterBuffer after = parts.scene.render();
    LS_CHECK(!isColour(at(after, 17, 10), kInk));    // the old edge is artwork now
    LS_CHECK(isColour(at(after, 20, 10), kInk));     // and the line moved out
}

// The colour is a palette role like any other, so a palette swap recolours the
// outline with everything else rather than leaving it behind.
void testAnOutlineCanTakeItsColourFromThePalette() {
    TwoParts parts;
    LS_REQUIRE(parts.build());

    auto palette = parts.scene.engine->createPalette(parts.scene.doc, PaletteDesc{ "main", {} });
    LS_REQUIRE(palette.ok());
    LS_REQUIRE(parts.scene.engine->setPaletteColor(palette.value, 3,
                                                   Color{ 200, 20, 40, 255 }).ok());
    LS_REQUIRE(parts.scene.engine->bindSpritePalette(parts.scene.sprite,
                                                     palette.value).ok());

    GenerateSilhouetteOutlineOp outline;
    outline.thickness = 1.f;
    outline.side = OutlineSide::Outside;
    outline.paletteRole = 3;
    outline.fallbackColor = kInk;          // only used if the role resolves to nothing
    outline.targetSprite = parts.scene.sprite;
    auto layer = parts.scene.engine->createLayer(parts.scene.sprite, { "outline" });
    LS_REQUIRE(layer.ok());
    LS_REQUIRE(parts.scene.engine->addOperation(layer.value, outline).ok());

    RasterBuffer raster = parts.scene.render();
    LS_CHECK(isColour(at(raster, 5, 10), Color{ 200, 20, 40, 255 }));

    // Change what the slot means and the line changes with it, from the
    // drawing rather than over it.
    LS_REQUIRE(parts.scene.engine->setPaletteColor(palette.value, 3,
                                                   Color{ 20, 200, 120, 255 }).ok());
    raster = parts.scene.render();
    LS_CHECK(isColour(at(raster, 5, 10), Color{ 20, 200, 120, 255 }));
}

// Two figures' outlines in one sprite, in different colours, which is what
// "different colours" has to mean once there is more than one outline.
void testTwoOutlinesCanBeDifferentColours() {
    TwoParts parts;
    LS_REQUIRE(parts.build());

    // A line around the arm only, in one colour.
    GenerateSilhouetteOutlineOp armLine;
    armLine.thickness = 1.f;
    armLine.side = OutlineSide::Outside;
    armLine.fallbackColor = Color{ 250, 250, 250, 255 };
    LS_REQUIRE(parts.scene.engine->addOperation(parts.arm, armLine).ok());

    // And one around the whole figure, in another, underneath.
    GenerateSilhouetteOutlineOp figureLine;
    figureLine.thickness = 1.f;
    figureLine.side = OutlineSide::Outside;
    figureLine.fallbackColor = kInk;
    figureLine.targetSprite = parts.scene.sprite;
    auto layer = parts.scene.engine->createLayer(parts.scene.sprite, { "figure line" });
    LS_REQUIRE(layer.ok());
    LS_REQUIRE(parts.scene.engine->addOperation(layer.value, figureLine).ok());

    const RasterBuffer raster = parts.scene.render();
    LS_CHECK(countColour(raster, Color{ 250, 250, 250, 255 }) > 0);
    LS_CHECK(countColour(raster, kInk) > 0);
}

// A layer compiled alone still draws something rather than nothing: an
// application asking for one layer has not asked for a hole.
void testALayerCompiledAloneStillOutlinesSomething() {
    TwoParts parts;
    LS_REQUIRE(parts.build());

    GenerateSilhouetteOutlineOp outline;
    outline.thickness = 1.f;
    outline.side = OutlineSide::Outside;
    outline.fallbackColor = kInk;
    outline.targetSprite = parts.scene.sprite;
    LS_REQUIRE(parts.scene.engine->addOperation(parts.body, outline).ok());

    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = kSize;
    profile.outputHeight = kSize;
    profile.palette = PalettePolicy::Unconstrained;

    auto compiled = parts.scene.engine->compileLayer(parts.body, profile);
    LS_REQUIRE(compiled.ok());
    // Falls back to its own layer rather than vanishing.
    LS_CHECK(countColour(compiled.value.raster, kInk) > 0);
}

} // namespace

int main() {
    testAnOutlineWithNoSpriteTracesItsOwnLayer();
    testNamingTheSpriteTracesTheWholeFigure();
    testTheTwoKindsOfOutlineDiffer();
    testASpriteOutlineDoesNotTraceItself();
    testTheFigureOutlineFollowsEveryPart();
    testAnOutlineCanTakeItsColourFromThePalette();
    testTwoOutlinesCanBeDifferentColours();
    testALayerCompiledAloneStillOutlinesSomething();
    return lstest::report("outlines");
}
