// ls_testbed.cpp — render the engine, not a lookalike.
//
// Every panel in the gallery this produces comes out of ls::LSContext: the
// scenes build operation stacks, compile them through the real pipeline, and
// the resulting rasters are written as PNGs next to an index.html that shows
// them side by side with the compile trace.
//
//   livesprite_testbed [--out DIR] [--angle DEG] [--density D] [--zoom N]

#include "ls_png.h"

#include <livesprite/livesprite.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace ls;

namespace {

struct Options {
    std::string outDir = "testbed_out";
    float angle = 25.f;
    float density = 0.35f;
    int zoom = 8;
};

struct Panel {
    std::string label;
    std::string note;
    RasterBuffer raster;
    std::string file;      // PNG written next to the page
    std::string dataUri;   // the same PNG inlined, so the page stands alone
};

struct Scene {
    std::string id;
    std::string title;
    std::string blurb;
    std::vector<Panel> panels;
    std::vector<std::string> trace;
    std::string verdict;
};

// --- small helpers ---------------------------------------------------------

CompileProfile profileFor(uint32_t size, CompileProfileType type = CompileProfileType::Export) {
    CompileProfile profile;
    profile.type = type;
    profile.outputWidth = size;
    profile.outputHeight = size;
    profile.palette = PalettePolicy::Unconstrained;
    return profile;
}

int opaqueCount(const RasterBuffer& raster) {
    int count = 0;
    for (uint32_t y = 0; y < raster.height; ++y) {
        const uint8_t* row = raster.row(y);
        for (uint32_t x = 0; x < raster.width; ++x) {
            count += row[x * 4 + 3] != 0 ? 1 : 0;
        }
    }
    return count;
}

std::string pixels(const RasterBuffer& raster) {
    return std::to_string(opaqueCount(raster)) + " px";
}

struct Stage {
    DocumentId doc;
    SpriteId sprite;
    LayerId layer;
};

Stage makeStage(LSContext& ctx, uint32_t size, const char* name) {
    Stage stage;
    stage.doc = ctx.createDocument({name, size, size}).value;
    stage.sprite = ctx.createSprite(stage.doc).value;
    stage.layer = ctx.createLayer(stage.sprite, {"main"}).value;
    return stage;
}

// A hand-drawn blade outline: the kind of input an artist actually produces.
PixelRegionDesc bladeOutline() {
    PixelRegionDesc desc;
    const Color ink = Color::black();
    auto add = [&desc, ink](int x, int y) { desc.pixels.push_back({{x, y}, ink}); };

    for (int y = 6; y <= 22; ++y) {      // left edge
        add(13, y);
    }
    for (int y = 6; y <= 22; ++y) {      // right edge
        add(18, y);
    }
    add(14, 5); add(15, 4); add(16, 4); add(17, 5);    // tip
    add(14, 23); add(15, 23); add(16, 23); add(17, 23); // base
    for (int x = 10; x <= 21; ++x) {     // crossguard
        add(x, 24);
        add(x, 25);
    }
    for (int y = 26; y <= 29; ++y) {     // grip
        add(14, y); add(17, y);
    }
    add(15, 30); add(16, 30);
    return desc;
}

// --- scenes ----------------------------------------------------------------

Scene sceneAuthoredPixels(const Options& options) {
    (void)options;
    Scene scene;
    scene.id = "authored";
    scene.title = "Authored pixels become a live region";
    scene.blurb = "A hand-drawn closed loop is stored as an operation, not as pixels. The engine "
                  "seals its interior, so a bucket fill has something to fill, and the boundary "
                  "stays addressable as its own stroke.";

    auto ctx = LSContext::create();
    const uint32_t size = 36;

    {   // What the artist drew
        Stage stage = makeStage(*ctx, size, "drawn");
        PixelRegionDesc desc = bladeOutline();
        desc.closeSameColorBoundaries = false;
        const RegionId region = ctx->createRegionFromPixels(stage.doc, desc).value;
        FillSolidOp fill;
        fill.targetRegion = region;
        fill.fallbackColor = {20, 22, 30, 255};
        ctx->addOperation(stage.layer, fill);
        auto compiled = ctx->compileSprite(stage.sprite, profileFor(size));
        scene.panels.push_back({"as drawn", pixels(compiled.value.raster), compiled.value.raster, ""});
    }
    {   // The same input with the closing rules on
        Stage stage = makeStage(*ctx, size, "sealed");
        const RegionId region = ctx->createRegionFromPixels(stage.doc, bladeOutline()).value;
        FillSolidOp fill;
        fill.targetRegion = region;
        fill.fallbackColor = {150, 162, 186, 255};
        ctx->addOperation(stage.layer, fill);
        StrokeRegionBoundaryOp edge;
        edge.targetRegion = region;
        edge.fallbackColor = {20, 22, 30, 255};
        ctx->addOperation(stage.layer, edge);
        auto compiled = ctx->compileSprite(stage.sprite, profileFor(size, CompileProfileType::Debug));
        scene.panels.push_back({"sealed + boundary stroke", pixels(compiled.value.raster),
                                compiled.value.raster, ""});
        scene.trace = compiled.value.trace;
    }
    return scene;
}

Scene sceneRotation(const Options& options) {
    Scene scene;
    scene.id = "rotation";
    scene.title = "Rotation re-resolves from source truth";
    scene.blurb = "Each panel is a fresh compile of the same operation stack with one rotation "
                  "parameter changed. Nothing is rotated twice, so nothing degrades: the quarter "
                  "turn keeps the pixel count exactly.";

    auto ctx = LSContext::create();
    const uint32_t size = 36;
    const float angles[] = {0.f, options.angle, 45.f, 90.f};

    for (float angle : angles) {
        Stage stage = makeStage(*ctx, size, "rotate");
        const RegionId region = ctx->createRegionFromPixels(stage.doc, bladeOutline()).value;
        FillSolidOp fill;
        fill.targetRegion = region;
        fill.fallbackColor = {150, 162, 186, 255};
        ctx->addOperation(stage.layer, fill);
        StrokeRegionBoundaryOp edge;
        edge.targetRegion = region;
        edge.fallbackColor = {20, 22, 30, 255};
        ctx->addOperation(stage.layer, edge);

        if (angle != 0.f) {
            RotateOp rotate;
            rotate.targetLayer = stage.layer;
            rotate.angleDegrees = angle;
            rotate.pivotFallback = {18.f, 18.f};
            ctx->addOperation(stage.layer, rotate);
        }

        auto compiled = ctx->compileSprite(stage.sprite, profileFor(size));
        std::ostringstream label;
        label << static_cast<int>(angle) << " degrees";
        scene.panels.push_back({label.str(), pixels(compiled.value.raster),
                                compiled.value.raster, ""});
    }

    const int base = opaqueCount(scene.panels.front().raster);
    const int quarter = opaqueCount(scene.panels.back().raster);
    scene.verdict = base == quarter
        ? "90 degrees preserves the pixel count exactly (" + std::to_string(base) + ")"
        : "90 degrees changed the pixel count: " + std::to_string(base) + " -> " +
          std::to_string(quarter);
    return scene;
}

Scene sceneDitherSpaces(const Options& options) {
    Scene scene;
    scene.id = "dither";
    scene.title = "Dither coordinate spaces";
    scene.blurb = "The same shape is drawn at two canvas positions. In Object space the pattern is "
                  "anchored to the shape, so it travels with it. In Canvas space it is anchored to "
                  "the canvas, so moving the shape re-phases the pattern. That difference is what "
                  "stops dither shimmering when a sprite moves.";

    auto ctx = LSContext::create();
    const uint32_t size = 32;
    const Vec2f origins[] = {{4.f, 6.f}, {9.f, 12.f}};
    const CoordinateSpace spaces[] = {CoordinateSpace::Object, CoordinateSpace::Canvas};

    for (CoordinateSpace space : spaces) {
        for (Vec2f origin : origins) {
            Stage stage = makeStage(*ctx, size, "dither");
            const GeometryId rect = ctx->createRect(stage.doc, {origin, 14.f, 14.f, 3.f}).value;
            const RegionId region = ctx->createRegionFromGeometry(rect).value;
            const RampId ramp = ctx->createRamp(stage.doc,
                {"tone", {{0.f, {58, 64, 92, 255}}, {1.f, {206, 214, 240, 255}}}, true}).value;
            const PatternId pattern = ctx->createOrderedDitherPattern(stage.doc, 4).value;

            FillDitherOp dither;
            dither.targetRegion = region;
            dither.ramp = ramp;
            dither.pattern = pattern;
            dither.density = options.density;
            dither.coordinateSpace = space;
            ctx->addOperation(stage.layer, dither);

            auto compiled = ctx->compileSprite(stage.sprite, profileFor(size));
            std::ostringstream label;
            label << (space == CoordinateSpace::Object ? "object @ " : "canvas @ ")
                  << static_cast<int>(origin.x) << "," << static_cast<int>(origin.y);
            scene.panels.push_back({label.str(), pixels(compiled.value.raster),
                                    compiled.value.raster, ""});
        }
    }

    // State the result rather than asking the reader to compare by eye: lift the
    // pattern out of each panel relative to its own shape and compare.
    auto patternOf = [&](size_t panel, Vec2f origin) {
        std::vector<uint8_t> tone;
        const RasterBuffer& raster = scene.panels[panel].raster;
        for (int32_t y = 0; y < 14; ++y) {
            for (int32_t x = 0; x < 14; ++x) {
                tone.push_back(readPixel(raster, static_cast<int32_t>(origin.x) + x,
                                                 static_cast<int32_t>(origin.y) + y).r);
            }
        }
        return tone;
    };

    const bool objectStable = patternOf(0, origins[0]) == patternOf(1, origins[1]);
    const bool canvasShifts = patternOf(2, origins[0]) != patternOf(3, origins[1]);
    scene.verdict = objectStable && canvasShifts
        ? "object space: pattern identical at both positions. canvas space: pattern re-phased."
        : std::string("MISMATCH: object stable = ") + (objectStable ? "yes" : "no") +
          ", canvas shifted = " + (canvasShifts ? "yes" : "no");
    return scene;
}

Scene scenePalette(const Options& options) {
    (void)options;
    Scene scene;
    scene.id = "palette";
    scene.title = "Palette swap without touching an operation";
    scene.blurb = "One operation stack, three palettes. The fills reference colour roles, so the "
                  "engine resolves them at compile time and the artwork repaints itself.";

    auto ctx = LSContext::create();
    const uint32_t size = 36;

    struct Variant { const char* name; Color ink; Color body; Color light; };
    const Variant variants[] = {
        {"steel",  {24, 26, 36, 255},  {126, 138, 164, 255}, {214, 224, 246, 255}},
        {"bronze", {40, 22, 12, 255},  {150, 96, 44, 255},   {232, 178, 96, 255}},
        {"venom",  {14, 32, 22, 255},  {58, 138, 84, 255},   {150, 232, 140, 255}},
    };

    for (const Variant& variant : variants) {
        Stage stage = makeStage(*ctx, size, "palette");
        const PaletteId palette = ctx->createPalette(stage.doc, {variant.name, {
            {0, variant.ink,   "ink"},
            {1, variant.body,  "body"},
            {2, variant.light, "light"}
        }}).value;
        ctx->bindSpritePalette(stage.sprite, palette);

        const RegionId region = ctx->createRegionFromPixels(stage.doc, bladeOutline()).value;
        FillSemanticColorOp body;
        body.targetRegion = region;
        body.paletteRole = 1;
        ctx->addOperation(stage.layer, body);

        const GeometryId shine = ctx->createRect(stage.doc, {{14.f, 7.f}, 2.f, 15.f, 0.f}).value;
        const RegionId shineRegion = ctx->createRegionFromGeometry(shine).value;
        FillSemanticColorOp light;
        light.targetRegion = shineRegion;
        light.paletteRole = 2;
        ctx->addOperation(stage.layer, light);

        StrokeRegionBoundaryOp edge;
        edge.targetRegion = region;
        edge.paletteRole = 0;
        ctx->addOperation(stage.layer, edge);

        auto compiled = ctx->compileSprite(stage.sprite, profileFor(size));
        scene.panels.push_back({variant.name, pixels(compiled.value.raster),
                                compiled.value.raster, ""});
    }
    return scene;
}

Scene sceneOutlines(const Options& options) {
    (void)options;
    Scene scene;
    scene.id = "outlines";
    scene.title = "Outline operations";
    scene.blurb = "Inner, centre and outer outlines of the same region, plus a silhouette outline "
                  "that reads whatever the layer has drawn so far.";

    auto ctx = LSContext::create();
    const uint32_t size = 32;

    struct Variant { const char* name; OutlineSide side; bool silhouette; };
    const Variant variants[] = {
        {"inner",      OutlineSide::Inside,  false},
        {"centre",     OutlineSide::Center,  false},
        {"outer",      OutlineSide::Outside, false},
        {"silhouette", OutlineSide::Outside, true},
    };

    for (const Variant& variant : variants) {
        Stage stage = makeStage(*ctx, size, "outline");
        const GeometryId shape = ctx->createEllipse(stage.doc, {{16.f, 16.f}, 9.f, 7.f}).value;
        const RegionId region = ctx->createRegionFromGeometry(shape).value;

        FillSolidOp fill;
        fill.targetRegion = region;
        fill.fallbackColor = {96, 118, 156, 255};
        ctx->addOperation(stage.layer, fill);

        if (variant.silhouette) {
            GenerateSilhouetteOutlineOp outline;
            outline.thickness = 2.f;
            outline.fallbackColor = {236, 196, 84, 255};
            ctx->addOperation(stage.layer, outline);
        } else {
            GenerateRegionOutlineOp outline;
            outline.targetRegion = region;
            outline.side = variant.side;
            outline.thickness = 2.f;
            outline.fallbackColor = {236, 196, 84, 255};
            ctx->addOperation(stage.layer, outline);
        }

        auto compiled = ctx->compileSprite(stage.sprite, profileFor(size));
        scene.panels.push_back({variant.name, pixels(compiled.value.raster),
                                compiled.value.raster, ""});
    }
    return scene;
}

Scene sceneBlendModes(const Options& options) {
    (void)options;
    Scene scene;
    scene.id = "blend";
    scene.title = "Layer compositing";
    scene.blurb = "A warm square over a cool one, composited with different layer blend modes.";

    auto ctx = LSContext::create();
    const uint32_t size = 32;

    struct Variant { const char* name; BlendMode mode; };
    const Variant variants[] = {
        {"normal", BlendMode::Normal}, {"multiply", BlendMode::Multiply},
        {"screen", BlendMode::Screen}, {"overlay", BlendMode::Overlay},
        {"lighten", BlendMode::Lighten}, {"difference", BlendMode::Difference},
    };

    for (const Variant& variant : variants) {
        Stage stage = makeStage(*ctx, size, "blend");
        const GeometryId lower = ctx->createRect(stage.doc, {{5.f, 5.f}, 16.f, 16.f, 0.f}).value;
        const RegionId lowerRegion = ctx->createRegionFromGeometry(lower).value;
        FillSolidOp cool;
        cool.targetRegion = lowerRegion;
        cool.fallbackColor = {60, 110, 190, 255};
        ctx->addOperation(stage.layer, cool);

        const LayerId top = ctx->createLayer(stage.sprite, {"top"}).value;
        const GeometryId upper = ctx->createRect(stage.doc, {{11.f, 11.f}, 16.f, 16.f, 0.f}).value;
        const RegionId upperRegion = ctx->createRegionFromGeometry(upper).value;
        FillSolidOp warm;
        warm.targetRegion = upperRegion;
        warm.fallbackColor = {230, 150, 60, 255};
        ctx->addOperation(top, warm);
        ctx->setLayerBlendMode(top, variant.mode);

        auto compiled = ctx->compileSprite(stage.sprite, profileFor(size));
        scene.panels.push_back({variant.name, "", compiled.value.raster, ""});
    }
    return scene;
}

Scene sceneDeforms(const Options& options) {
    (void)options;
    Scene scene;
    scene.id = "deform";
    scene.title = "Deformations as math";
    scene.blurb = "Squash, stretch, bend and warp are parameters on the operation stack. Remove the "
                  "operation and the original is back, exactly.";

    auto ctx = LSContext::create();
    const uint32_t size = 36;

    enum class Kind { None, Squash, Stretch, Bend, Warp };
    struct Variant { const char* name; Kind kind; };
    const Variant variants[] = {
        {"source", Kind::None}, {"squash 0.7", Kind::Squash},
        {"stretch 1.4", Kind::Stretch}, {"bend", Kind::Bend}, {"warp", Kind::Warp},
    };

    for (const Variant& variant : variants) {
        Stage stage = makeStage(*ctx, size, "deform");
        const GeometryId body = ctx->createRect(stage.doc, {{13.f, 8.f}, 10.f, 20.f, 2.f}).value;
        const RegionId region = ctx->createRegionFromGeometry(body).value;
        FillSolidOp fill;
        fill.targetRegion = region;
        fill.fallbackColor = {132, 168, 120, 255};
        ctx->addOperation(stage.layer, fill);
        GenerateOuterOutlineOp outline;
        outline.targetRegion = region;
        outline.fallbackColor = {28, 40, 26, 255};
        ctx->addOperation(stage.layer, outline);

        switch (variant.kind) {
            case Kind::None:
                break;
            case Kind::Squash: {
                SquashOp op;
                op.targetLayer = stage.layer;
                op.factor = 0.7f;
                op.pivotFallback = {18.f, 28.f};
                ctx->addOperation(stage.layer, op);
                break;
            }
            case Kind::Stretch: {
                StretchOp op;
                op.targetLayer = stage.layer;
                op.factor = 1.4f;
                op.pivotFallback = {18.f, 28.f};
                ctx->addOperation(stage.layer, op);
                break;
            }
            case Kind::Bend: {
                BendOp op;
                op.targetLayer = stage.layer;
                op.strength = 45.f;
                op.falloff = Falloff::Smooth;
                ctx->addOperation(stage.layer, op);
                break;
            }
            case Kind::Warp: {
                WarpOp op;
                op.targetLayer = stage.layer;
                op.handlePoints = {{18.f, 10.f}};
                op.displacements = {{7.f, 0.f}};
                op.radius = 12.f;
                op.strength = 1.f;
                op.falloff = Falloff::Smooth;
                ctx->addOperation(stage.layer, op);
                break;
            }
        }

        auto compiled = ctx->compileSprite(stage.sprite, profileFor(size));
        scene.panels.push_back({variant.name, pixels(compiled.value.raster),
                                compiled.value.raster, ""});
    }
    return scene;
}

Scene sceneRegionMath(const Options& options) {
    (void)options;
    Scene scene;
    scene.id = "regions";
    scene.title = "Region boolean math";
    scene.blurb = "Two overlapping shapes combined by the engine region operations. Every higher "
                  "level tool eventually becomes one of these.";

    auto ctx = LSContext::create();
    const uint32_t size = 32;

    struct Variant { const char* name; int op; };
    const Variant variants[] = {
        {"a and b", 0}, {"union", 1}, {"subtract", 2}, {"intersect", 3}, {"xor", 4},
    };

    for (const Variant& variant : variants) {
        Stage stage = makeStage(*ctx, size, "region");
        const GeometryId circleA = ctx->createCircle(stage.doc, {{13.f, 16.f}, 8.f}).value;
        const GeometryId circleB = ctx->createCircle(stage.doc, {{20.f, 16.f}, 8.f}).value;
        const RegionId a = ctx->createRegionFromGeometry(circleA).value;
        const RegionId b = ctx->createRegionFromGeometry(circleB).value;

        if (variant.op == 0) {
            FillSolidOp fillA;
            fillA.targetRegion = a;
            fillA.fallbackColor = {70, 130, 200, 160};
            ctx->addOperation(stage.layer, fillA);
            FillSolidOp fillB;
            fillB.targetRegion = b;
            fillB.fallbackColor = {220, 120, 80, 160};
            ctx->addOperation(stage.layer, fillB);
        } else {
            RegionId combined = a;
            switch (variant.op) {
                case 1: combined = ctx->unionRegions(a, b).value; break;
                case 2: combined = ctx->subtractRegions(a, b).value; break;
                case 3: combined = ctx->intersectRegions(a, b).value; break;
                default: combined = ctx->xorRegions(a, b).value; break;
            }
            FillSolidOp fill;
            fill.targetRegion = combined;
            fill.fallbackColor = {150, 190, 235, 255};
            ctx->addOperation(stage.layer, fill);
            GenerateOuterOutlineOp outline;
            outline.targetRegion = combined;
            outline.fallbackColor = {24, 40, 60, 255};
            ctx->addOperation(stage.layer, outline);
        }

        CompileProfile profile = profileFor(size);
        profile.alpha = AlphaPolicy::Preserve;
        auto compiled = ctx->compileSprite(stage.sprite, profile);
        scene.panels.push_back({variant.name, pixels(compiled.value.raster),
                                compiled.value.raster, ""});
    }
    return scene;
}

Scene sceneRoundTrip(const Options& options) {
    Scene scene;
    scene.id = "roundtrip";
    scene.title = "Save, load, recompile";
    scene.blurb = "The left panel is compiled from a live document. The right one is compiled after "
                  "serializing that document, loading it into a second engine context, and "
                  "recompiling from the restored operations.";

    auto original = LSContext::create();
    const uint32_t size = 36;
    Stage stage = makeStage(*original, size, "roundtrip");

    const RegionId region = original->createRegionFromPixels(stage.doc, bladeOutline()).value;
    const PaletteId palette = original->createPalette(stage.doc, {"steel", {
        {0, {24, 26, 36, 255}, "ink"}, {1, {126, 138, 164, 255}, "body"}}}).value;
    original->bindSpritePalette(stage.sprite, palette);

    FillSemanticColorOp fill;
    fill.targetRegion = region;
    fill.paletteRole = 1;
    original->addOperation(stage.layer, fill);

    const RampId ramp = original->createRamp(stage.doc,
        {"tone", {{0.f, {60, 68, 92, 255}}, {1.f, {212, 220, 244, 255}}}, true}).value;
    const PatternId pattern = original->createOrderedDitherPattern(stage.doc, 4).value;
    const GeometryId blade = original->createRect(stage.doc, {{14.f, 6.f}, 4.f, 17.f, 0.f}).value;
    const RegionId bladeRegion = original->createRegionFromGeometry(blade).value;
    FillDitherOp dither;
    dither.targetRegion = bladeRegion;
    dither.ramp = ramp;
    dither.pattern = pattern;
    dither.density = options.density;
    original->addOperation(stage.layer, dither);

    StrokeRegionBoundaryOp edge;
    edge.targetRegion = region;
    edge.paletteRole = 0;
    original->addOperation(stage.layer, edge);

    RotateOp rotate;
    rotate.targetLayer = stage.layer;
    rotate.angleDegrees = options.angle;
    rotate.pivotFallback = {18.f, 18.f};
    original->addOperation(stage.layer, rotate);

    auto before = original->compileSprite(stage.sprite, profileFor(size));
    scene.panels.push_back({"live document", pixels(before.value.raster), before.value.raster, ""});

    auto saved = original->serializeDocument(stage.doc);
    auto restored = LSContext::create();
    auto restoredDoc = restored->deserializeDocument(saved.value);

    RasterBuffer afterRaster;
    if (restoredDoc.ok()) {
        for (uint64_t candidate = 1; candidate < 8192; ++candidate) {
            auto info = restored->getSpriteInfo(SpriteId{candidate});
            if (info.ok()) {
                auto after = restored->compileSprite(SpriteId{candidate}, profileFor(size));
                if (after.ok()) {
                    afterRaster = after.value.raster;
                }
                break;
            }
        }
    }
    scene.panels.push_back({"after save + load", pixels(afterRaster), afterRaster, ""});

    const bool identical = !afterRaster.empty() && afterRaster.pixels == before.value.raster.pixels;
    scene.verdict = identical
        ? "identical pixel for pixel, from " + std::to_string(saved.value.bytes.size()) +
          " bytes of document"
        : "MISMATCH: the round trip changed the compiled output";
    return scene;
}

Scene sceneDitheredGradients(const Options& options) {
    (void)options;
    Scene scene;
    scene.id = "gradients";
    scene.title = "Gradients made of dithered colour";
    scene.blurb = "A three stop ramp resolved through a dither pattern. The value being dithered "
                  "varies across the shape, so the fill walks from one ramp stop to the next in "
                  "bands of mixed pixels. No colour outside the ramp is ever produced.";

    auto ctx = LSContext::create();
    const uint32_t size = 40;

    struct Variant { const char* name; DitherModulation modulation; DitherPatternKind pattern; };
    const Variant variants[] = {
        {"linear, bayer8",   DitherModulation::Linear,  DitherPatternKind::Bayer8},
        {"radial, clustered", DitherModulation::Radial, DitherPatternKind::ClusteredDot},
        {"angular, dots",    DitherModulation::Angular, DitherPatternKind::Dots},
        {"linear, crosshatch", DitherModulation::Linear, DitherPatternKind::CrossHatch},
    };

    for (const Variant& variant : variants) {
        Stage stage = makeStage(*ctx, size, "gradient");
        const GeometryId shape = ctx->createRect(stage.doc, {{6.f, 6.f}, 28.f, 28.f, 4.f}).value;
        const RegionId region = ctx->createRegionFromGeometry(shape).value;
        const RampId ramp = ctx->createRamp(stage.doc, {"tone", {
            {0.f,  {36, 40, 72, 255}},
            {0.5f, {118, 132, 178, 255}},
            {1.f,  {226, 234, 252, 255}}}, true}).value;

        FillDitherOp dither;
        dither.targetRegion = region;
        dither.ramp = ramp;
        dither.pattern = ctx->createDitherPattern(stage.doc, variant.pattern).value;
        dither.modulation = variant.modulation;
        if (variant.modulation == DitherModulation::Linear) {
            dither.gradientStart = {0.f, 0.f};
            dither.gradientEnd = {28.f, 28.f};
        } else {
            dither.gradientStart = {14.f, 14.f};
            dither.gradientEnd = {28.f, 14.f};
        }
        ctx->addOperation(stage.layer, dither);

        auto compiled = ctx->compileSprite(stage.sprite, profileFor(size));
        scene.panels.push_back({variant.name, pixels(compiled.value.raster),
                                compiled.value.raster, "", ""});
    }

    // The claim worth checking: a dithered gradient uses only ramp colours.
    std::set<uint32_t> distinct;
    const RasterBuffer& first = scene.panels.front().raster;
    for (uint32_t y = 0; y < first.height; ++y) {
        for (uint32_t x = 0; x < first.width; ++x) {
            const Color color = readPixel(first, static_cast<int32_t>(x), static_cast<int32_t>(y));
            if (color.a == 0) {
                continue;
            }
            distinct.insert((static_cast<uint32_t>(color.r) << 16) |
                            (static_cast<uint32_t>(color.g) << 8) | color.b);
        }
    }
    scene.verdict = distinct.size() == 3
        ? "the linear panel holds exactly the 3 ramp stops, mixed by the pattern"
        : "MISMATCH: expected 3 ramp colours, found " + std::to_string(distinct.size());
    return scene;
}

Scene scenePatternLibrary(const Options& options) {
    Scene scene;
    scene.id = "library";
    scene.title = "The prebaked pattern library";
    scene.blurb = "Every kind the engine ships, filled at one density. These are threshold "
                  "matrices rather than stamps, so the same tile serves every density and every "
                  "step of a gradient. External tiles register the same way.";

    auto ctx = LSContext::create();
    const uint32_t size = 24;

    auto probe = LSContext::create();
    for (DitherPatternKind kind : probe->ditherPatternKinds()) {
        Stage stage = makeStage(*ctx, size, "library");
        const GeometryId shape = ctx->createRect(stage.doc, {{2.f, 2.f}, 20.f, 20.f, 0.f}).value;
        const RegionId region = ctx->createRegionFromGeometry(shape).value;

        FillDitherOp dither;
        dither.targetRegion = region;
        dither.ramp = ctx->createRamp(stage.doc,
            {"two", {{0.f, {32, 36, 58, 255}}, {1.f, {228, 232, 248, 255}}}, true}).value;
        dither.pattern = ctx->createDitherPattern(stage.doc, kind).value;
        dither.density = options.density;
        ctx->addOperation(stage.layer, dither);

        auto compiled = ctx->compileSprite(stage.sprite, profileFor(size));
        scene.panels.push_back({std::string(ctx->ditherPatternName(kind)), "",
                                compiled.value.raster, "", ""});
    }
    return scene;
}

Scene sceneTileableTexture(const Options& options) {
    (void)options;
    Scene scene;
    scene.id = "texture";
    scene.title = "External tiles and tileable textures";
    scene.blurb = "A tile handed to the engine from outside. Imported as a threshold matrix it "
                  "behaves as a dither screen; imported with its colours it is a tileable texture "
                  "that fills a region directly, with its own scale and rotation.";

    auto ctx = LSContext::create();
    const uint32_t size = 32;

    // A small woven tile, the kind an artist would paint by hand.
    RasterBuffer tile = makeRaster(8, 8);
    const Color warp {196, 132, 68, 255};
    const Color weft {132, 84, 44, 255};
    const Color knot {236, 200, 140, 255};
    for (int32_t y = 0; y < 8; ++y) {
        for (int32_t x = 0; x < 8; ++x) {
            Color value = ((x / 2 + y / 2) % 2) == 0 ? warp : weft;
            if (x % 4 == 0 && y % 4 == 0) {
                value = knot;
            }
            writePixel(tile, x, y, value);
        }
    }

    struct Variant { const char* name; PatternImportMode mode; Vec2f scale; float angle; };
    const Variant variants[] = {
        {"imported as texture", PatternImportMode::Colors,    {1.f, 1.f}, 0.f},
        {"texture, 2x scale",   PatternImportMode::Colors,    {2.f, 2.f}, 0.f},
        {"texture, rotated 30", PatternImportMode::Colors,    {1.f, 1.f}, 30.f},
        {"imported as screen",  PatternImportMode::Threshold, {1.f, 1.f}, 0.f},
    };

    for (const Variant& variant : variants) {
        Stage stage = makeStage(*ctx, size, "texture");
        const GeometryId shape = ctx->createEllipse(stage.doc, {{16.f, 16.f}, 13.f, 11.f}).value;
        const RegionId region = ctx->createRegionFromGeometry(shape).value;
        const PatternId pattern =
            ctx->createPatternFromRaster(stage.doc, tile, variant.mode, "weave").value;

        if (variant.mode == PatternImportMode::Colors) {
            FillTexturePatternOp fill;
            fill.targetRegion = region;
            fill.pattern = pattern;
            fill.scale = variant.scale;
            fill.angle = variant.angle;
            ctx->addOperation(stage.layer, fill);
        } else {
            // The same tile as a threshold screen: luminance becomes the rank.
            FillDitherOp dither;
            dither.targetRegion = region;
            dither.ramp = ctx->createRamp(stage.doc,
                {"tone", {{0.f, {60, 38, 20, 255}}, {1.f, {242, 214, 168, 255}}}, true}).value;
            dither.pattern = pattern;
            dither.density = 0.5f;
            ctx->addOperation(stage.layer, dither);
        }

        GenerateOuterOutlineOp outline;
        outline.targetRegion = region;
        outline.fallbackColor = {28, 18, 10, 255};
        ctx->addOperation(stage.layer, outline);

        auto compiled = ctx->compileSprite(stage.sprite, profileFor(size));
        scene.panels.push_back({variant.name, "", compiled.value.raster, "", ""});
    }
    return scene;
}

Scene sceneAnchorModes(const Options& options) {
    (void)options;
    Scene scene;
    scene.id = "anchors";
    scene.title = "Local, global and fixed anchoring";
    scene.blurb = "A horizontal line screen on a square, shown still, moved, and turned a quarter "
                  "turn. Local rides the object completely. Global travels with it but stays "
                  "rotation locked. Fixed belongs to the canvas and never moves at all.";

    auto ctx = LSContext::create();
    const uint32_t size = 32;

    struct Variant { const char* name; PatternAnchor anchor; };
    const Variant variants[] = {
        {"local", PatternAnchor::Local},
        {"global", PatternAnchor::Global},
        {"fixed", PatternAnchor::Fixed},
    };
    struct Motion { const char* name; Vec2f origin; float rotate; };
    const Motion motions[] = {
        {"still", {8.f, 8.f}, 0.f},
        {"moved", {15.f, 13.f}, 0.f},
        {"turned", {8.f, 8.f}, 90.f},
    };

    for (const Variant& variant : variants) {
        for (const Motion& motion : motions) {
            Stage stage = makeStage(*ctx, size, "anchor");
            const GeometryId shape =
                ctx->createRect(stage.doc, {motion.origin, 12.f, 12.f, 0.f}).value;
            const RegionId region = ctx->createRegionFromGeometry(shape).value;

            FillDitherOp dither;
            dither.targetRegion = region;
            dither.ramp = ctx->createRamp(stage.doc,
                {"two", {{0.f, {30, 34, 54, 255}}, {1.f, {232, 236, 250, 255}}}, true}).value;
            dither.pattern = ctx->createDitherPattern(stage.doc,
                                                      DitherPatternKind::HorizontalLines).value;
            dither.density = 0.5f;
            dither.anchor = variant.anchor;
            ctx->addOperation(stage.layer, dither);

            if (motion.rotate != 0.f) {
                RotateOp rotate;
                rotate.targetLayer = stage.layer;
                rotate.angleDegrees = motion.rotate;
                rotate.pivotFallback = { motion.origin.x + 6.f, motion.origin.y + 6.f };
                ctx->addOperation(stage.layer, rotate);
            }

            auto compiled = ctx->compileSprite(stage.sprite, profileFor(size));
            scene.panels.push_back({std::string(variant.name) + ", " + motion.name, "",
                                    compiled.value.raster, "", ""});
        }
    }

    // Read the orientation back out of the pixels rather than asserting it in
    // prose: banded by row means the screen is still horizontal.
    auto bandedByRow = [](const RasterBuffer& raster, Vec2i origin, int32_t span) {
        for (int32_t y = 0; y < span; ++y) {
            const Color first = readPixel(raster, origin.x, origin.y + y);
            for (int32_t x = 1; x < span; ++x) {
                if (readPixel(raster, origin.x + x, origin.y + y) != first) {
                    return false;
                }
            }
        }
        return true;
    };

    const bool localTurns = !bandedByRow(scene.panels[2].raster, {8, 8}, 12);
    const bool globalLocked = bandedByRow(scene.panels[5].raster, {8, 8}, 12);
    const bool fixedLocked = bandedByRow(scene.panels[8].raster, {8, 8}, 12);
    scene.verdict = (localTurns && globalLocked && fixedLocked)
        ? "under the quarter turn: local rotated with the object, global and fixed stayed level"
        : "MISMATCH: local turned = " + std::string(localTurns ? "yes" : "no") +
          ", global locked = " + (globalLocked ? "yes" : "no") +
          ", fixed locked = " + (fixedLocked ? "yes" : "no");
    return scene;
}

// --- gallery ---------------------------------------------------------------

std::string escapeHtml(const std::string& text) {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

bool writeGallery(const Options& options, const std::vector<Scene>& scenes) {
    std::ofstream page(options.outDir + "/index.html", std::ios::binary);
    if (!page) {
        return false;
    }

    page << "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
         << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
         << "<title>LiveSprite engine testbed</title>\n<style>\n"
         << ":root { --zoom: " << options.zoom << "; --bg: #12141a; --card: #1a1d26;\n"
         << "        --line: #2b3040; --text: #e6e9f2; --muted: #99a1b8; }\n"
         << "* { box-sizing: border-box; }\n"
         << "body { margin: 0; padding: 32px 24px 64px; background: var(--bg); color: var(--text);\n"
         << "       font: 14px/1.55 ui-sans-serif, system-ui, 'Segoe UI', sans-serif; }\n"
         << "header { max-width: 1100px; margin: 0 auto 28px; }\n"
         << "h1 { font-size: 22px; margin: 0 0 6px; letter-spacing: -0.01em; }\n"
         << "header p { color: var(--muted); margin: 0; max-width: 62ch; }\n"
         << ".controls { max-width: 1100px; margin: 0 auto 24px; display: flex; gap: 16px;\n"
         << "            align-items: center; color: var(--muted); }\n"
         << "input[type=range] { width: 220px; }\n"
         << "section { max-width: 1100px; margin: 0 auto 22px; background: var(--card);\n"
         << "          border: 1px solid var(--line); border-radius: 10px; padding: 20px 22px; }\n"
         << "h2 { font-size: 16px; margin: 0 0 6px; }\n"
         << "section > p { color: var(--muted); margin: 0 0 16px; max-width: 76ch; }\n"
         << ".panels { display: flex; flex-wrap: wrap; gap: 18px; }\n"
         << ".panel { display: flex; flex-direction: column; gap: 6px; }\n"
         << ".frame { background: repeating-conic-gradient(#20242e 0% 25%, #191c24 0% 50%)\n"
         << "         50% / 12px 12px; border: 1px solid var(--line); border-radius: 6px;\n"
         << "         padding: 6px; }\n"
         << "img { display: block; image-rendering: pixelated; width: calc(var(--w) * var(--zoom) * 1px);\n"
         << "      height: calc(var(--h) * var(--zoom) * 1px); }\n"
         << ".label { font-weight: 600; }\n"
         << ".note, .meta { color: var(--muted); font-size: 12px; }\n"
         << ".verdict { margin-top: 14px; font-size: 13px; color: #8fe0a5; }\n"
         << ".verdict.bad { color: #ff9a8a; }\n"
         << "details { margin-top: 14px; }\n"
         << "summary { cursor: pointer; color: var(--muted); }\n"
         << "pre { overflow-x: auto; background: #10121a; border: 1px solid var(--line);\n"
         << "      border-radius: 6px; padding: 12px; font-size: 12px; color: #c6cde0; }\n"
         << "</style>\n</head>\n<body>\n";

    page << "<header>\n<h1>LiveSprite engine testbed</h1>\n"
         << "<p>Every image below was compiled by <code>ls::LSContext</code> through the real "
         << "pipeline and written straight to PNG. Nothing here reimplements the engine.</p>\n"
         << "</header>\n";

    page << "<div class=\"controls\"><label>zoom <input id=\"zoom\" type=\"range\" min=\"1\" "
         << "max=\"16\" value=\"" << options.zoom << "\"></label><span id=\"zoomValue\">"
         << options.zoom << "x</span></div>\n";

    for (const Scene& scene : scenes) {
        page << "<section id=\"" << scene.id << "\">\n<h2>" << escapeHtml(scene.title) << "</h2>\n"
             << "<p>" << escapeHtml(scene.blurb) << "</p>\n<div class=\"panels\">\n";
        for (const Panel& panel : scene.panels) {
            page << "  <div class=\"panel\">\n    <div class=\"frame\"><img src=\"" << panel.dataUri
                 << "\" alt=\"" << escapeHtml(panel.label) << "\" style=\"--w:"
                 << panel.raster.width << "; --h:" << panel.raster.height << "\"></div>\n"
                 << "    <div class=\"label\">" << escapeHtml(panel.label) << "</div>\n";
            if (!panel.note.empty()) {
                page << "    <div class=\"meta\">" << escapeHtml(panel.note) << "</div>\n";
            }
            page << "  </div>\n";
        }
        page << "</div>\n";

        if (!scene.verdict.empty()) {
            const bool bad = scene.verdict.find("MISMATCH") != std::string::npos;
            page << "<div class=\"verdict" << (bad ? " bad" : "") << "\">"
                 << escapeHtml(scene.verdict) << "</div>\n";
        }
        if (!scene.trace.empty()) {
            page << "<details><summary>compile trace (Debug profile)</summary><pre>";
            for (const std::string& line : scene.trace) {
                page << escapeHtml(line) << "\n";
            }
            page << "</pre></details>\n";
        }
        page << "</section>\n";
    }

    page << "<script>\n"
         << "const zoom = document.getElementById('zoom');\n"
         << "const readout = document.getElementById('zoomValue');\n"
         << "zoom.addEventListener('input', () => {\n"
         << "  document.documentElement.style.setProperty('--zoom', zoom.value);\n"
         << "  readout.textContent = zoom.value + 'x';\n"
         << "});\n"
         << "</script>\n</body>\n</html>\n";

    return true;
}

bool writePanels(const Options& options, std::vector<Scene>& scenes) {
    for (Scene& scene : scenes) {
        int index = 0;
        for (Panel& panel : scene.panels) {
            if (panel.raster.empty()) {
                continue;
            }
            panel.file = scene.id + "_" + std::to_string(index++) + ".png";
            const std::vector<uint8_t> png = lstestbed::encodePng(
                panel.raster.width, panel.raster.height, panel.raster.pixels);
            if (png.empty() ||
                !lstestbed::writePng(options.outDir + "/" + panel.file,
                                     panel.raster.width, panel.raster.height,
                                     panel.raster.pixels)) {
                std::cerr << "failed to write " << panel.file << "\n";
                return false;
            }
            panel.dataUri = "data:image/png;base64," + lstestbed::base64(png);
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--out" && hasValue)          { options.outDir = argv[++i]; }
        else if (arg == "--angle" && hasValue)   { options.angle = std::strtof(argv[++i], nullptr); }
        else if (arg == "--density" && hasValue) { options.density = std::strtof(argv[++i], nullptr); }
        else if (arg == "--zoom" && hasValue)    { options.zoom = std::atoi(argv[++i]); }
        else {
            std::cout << "usage: livesprite_testbed [--out DIR] [--angle DEG] "
                      << "[--density 0..1] [--zoom N]\n";
            return arg == "--help" ? 0 : 1;
        }
    }

    std::error_code directoryError;
    std::filesystem::create_directories(options.outDir, directoryError);
    if (directoryError) {
        std::cerr << "testbed: cannot create " << options.outDir << ": "
                  << directoryError.message() << "\n";
        return 1;
    }

    std::vector<Scene> scenes;
    scenes.push_back(sceneAuthoredPixels(options));
    scenes.push_back(sceneRotation(options));
    scenes.push_back(sceneDitherSpaces(options));
    scenes.push_back(sceneDitheredGradients(options));
    scenes.push_back(scenePatternLibrary(options));
    scenes.push_back(sceneTileableTexture(options));
    scenes.push_back(sceneAnchorModes(options));
    scenes.push_back(scenePalette(options));
    scenes.push_back(sceneOutlines(options));
    scenes.push_back(sceneBlendModes(options));
    scenes.push_back(sceneDeforms(options));
    scenes.push_back(sceneRegionMath(options));
    scenes.push_back(sceneRoundTrip(options));

    if (!writePanels(options, scenes) || !writeGallery(options, scenes)) {
        std::cerr << "testbed: could not write output into " << options.outDir << "\n";
        return 1;
    }

    int panels = 0;
    for (const Scene& scene : scenes) {
        panels += static_cast<int>(scene.panels.size());
    }
    std::cout << "engine version " << LS_ENGINE_VERSION << ": wrote " << panels
              << " panels across " << scenes.size() << " scenes\n"
              << options.outDir << "/index.html\n";
    for (const Scene& scene : scenes) {
        if (!scene.verdict.empty()) {
            std::cout << "  " << scene.id << ": " << scene.verdict << "\n";
        }
    }
    return 0;
}
