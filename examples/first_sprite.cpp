// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the LiveSprite authors
// first_sprite.cpp — the shortest path from nothing to a compiled sprite.
//
// This file is the worked example the documentation quotes, and it is built and
// run by the test target, so it cannot drift away from the API it describes.
//
//   build/livesprite_example

#include <livesprite/livesprite.h>

#include <cstdio>
#include <vector>

using namespace ls;

namespace {

// Print the compiled raster as text, so the example shows its work without
// needing an image viewer.
void printRaster(const RasterBuffer& raster, Color ink) {
    for (uint32_t y = 0; y < raster.height; ++y) {
        std::string row;
        for (uint32_t x = 0; x < raster.width; ++x) {
            const Color pixel = readPixel(raster, static_cast<int32_t>(x),
                                          static_cast<int32_t>(y));
            if (pixel.a == 0)        { row += '.'; }
            else if (pixel == ink)   { row += '#'; }
            else                     { row += 'o'; }
        }
        std::printf("  %s\n", row.c_str());
    }
}

} // namespace

int main() {
    auto ctx = LSContext::create();

    // 1. A document is a canvas and everything drawn on it. A sprite is one
    //    thing on that canvas; a layer is an ordered stack of operations.
    const DocumentId doc = ctx->createDocument({"badge", 24, 24}).value;
    const SpriteId sprite = ctx->createSprite(doc).value;
    const LayerId layer = ctx->createLayer(sprite, {"body"}).value;

    // 2. Colours are roles, not literals. Painting through a role means a
    //    palette swap repaints the artwork without touching an operation.
    const Color ink { 26, 28, 38, 255 };
    const PaletteId palette = ctx->createPalette(doc, {"badge", {
        {0, ink,                    "ink"},
        {1, {96, 132, 196, 255},    "body"},
        {2, {206, 224, 255, 255},   "light"}}}).value;
    ctx->bindSpritePalette(sprite, palette);

    // 3. A shape can come from geometry, or from pixels somebody drew. An
    //    authored closed loop seals: the engine works out that the ring has an
    //    inside, so there is something to fill.
    PixelRegionDesc drawn;
    for (int32_t i = 0; i < 14; ++i) {
        drawn.pixels.push_back({{5 + i, 5},  ink});   // top
        drawn.pixels.push_back({{5 + i, 18}, ink});   // bottom
        drawn.pixels.push_back({{5, 5 + i},  ink});   // left
        drawn.pixels.push_back({{18, 5 + i}, ink});   // right
    }
    const RegionId badge = ctx->createRegionFromPixels(doc, drawn).value;

    // 4. Operations are the source truth. A fill is not pixels: it is a
    //    standing instruction about how a region gets its colour.
    FillSolidOp body;
    body.targetRegion = badge;
    body.paletteRole = 1;
    ctx->addOperation(layer, body);

    // 5. A dither resolves a value against a threshold pattern and picks
    //    between the two ramp stops it falls between. Modulated across the
    //    shape, that is a gradient made of dithered colour.
    const RampId ramp = ctx->createRamp(doc, {"sheen", {
        {0.f, {96, 132, 196, 255}},
        {1.f, {206, 224, 255, 255}}}, true}).value;
    const PatternId screen = ctx->createDitherPattern(doc, DitherPatternKind::Bayer4).value;

    FillDitherOp sheen;
    sheen.targetRegion = badge;
    sheen.ramp = ramp;
    sheen.pattern = screen;
    sheen.modulation = DitherModulation::Linear;
    sheen.gradientStart = {0.f, 0.f};
    sheen.gradientEnd = {14.f, 14.f};
    sheen.anchor = PatternAnchor::Local;   // the pattern rides the shape
    ctx->addOperation(layer, sheen);

    // 6. The authored loop is still addressable as its own outline.
    StrokeRegionBoundaryOp edge;
    edge.targetRegion = badge;
    edge.paletteRole = 0;
    ctx->addOperation(layer, edge);

    // 7. Compiling turns the operation stack into pixels. This is the first
    //    moment pixels exist at all.
    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = 24;
    profile.outputHeight = 24;

    auto compiled = ctx->compileSprite(sprite, profile);
    if (compiled.fail()) {
        std::printf("compile failed: %s\n", std::string(lsErrorString(compiled.error)).c_str());
        return 1;
    }
    std::printf("compiled badge:\n");
    printRaster(compiled.value.raster, ink);

    // 8. Nothing above is spent. Change a parameter and compile again: the
    //    engine rebuilds from the same operations, so nothing degrades.
    ctx->setOperationParameter(ctx->getLayerOperations(layer).value[1].id, "density", 0.8f);
    ctx->setPaletteColor(palette, 1, {176, 96, 72, 255});

    RotateOp turn;
    turn.targetLayer = layer;
    turn.angleDegrees = 45.f;
    turn.pivotFallback = {12.f, 12.f};
    ctx->addOperation(layer, turn);

    auto turned = ctx->compileSprite(sprite, profile);
    std::printf("\nsame operations, recoloured and turned 45 degrees:\n");
    printRaster(turned.value.raster, ink);

    // 9. Saving. A package carries the document plus whatever an app keeps
    //    beside it; the document alone is enough if there is nothing else.
    PackageEntry note;
    note.name = "example/readme.txt";
    note.contentType = "text/plain";
    const char* text = "written by the LiveSprite example";
    note.data.assign(text, text + 33);

    auto package = ctx->writePackage(doc, {note});
    std::printf("\npackage: %zu bytes, opens as a zip\n", package.value.bytes.size());

    // 10. And back again, into a different engine context entirely.
    auto reopened = LSContext::create();
    std::vector<PackageEntry> entries;
    auto restored = reopened->loadPackage(package.value, &entries);
    std::printf("reloaded document %llu with %zu app entries\n",
                static_cast<unsigned long long>(restored.value.value), entries.size());
    return 0;
}
