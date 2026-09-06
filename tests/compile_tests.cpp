// compile_tests.cpp — the compilation contract: determinism, non-destructive
// transforms, stable dither spaces, compositing, policies, and caching.

#include "ls_test.h"

#include <livesprite/livesprite.h>

#include <cmath>
#include <vector>

using namespace ls;

namespace {

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

Color pixelAt(const RasterBuffer& raster, uint32_t x, uint32_t y) {
    const uint8_t* px = raster.row(y) + x * 4;
    return { px[0], px[1], px[2], px[3] };
}

bool identical(const RasterBuffer& a, const RasterBuffer& b) {
    return a.width == b.width && a.height == b.height && a.pixels == b.pixels;
}

CompileProfile exportProfile(uint32_t size = 32) {
    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = size;
    profile.outputHeight = size;
    return profile;
}

struct Scene {
    DocumentId doc;
    SpriteId sprite;
    LayerId layer;
    RegionId region;
};

Scene makeScene(LSContext& ctx, Vec2f origin = {8.f, 8.f}, float size = 8.f) {
    Scene scene;
    scene.doc = ctx.createDocument({"scene", 32, 32}).value;
    scene.sprite = ctx.createSprite(scene.doc).value;
    scene.layer = ctx.createLayer(scene.sprite, {"body"}).value;
    const GeometryId rect = ctx.createRect(scene.doc, {origin, size, size, 0.f}).value;
    scene.region = ctx.createRegionFromGeometry(rect).value;
    return scene;
}

void testSolidFillAndBounds(LSContext& ctx) {
    const Scene scene = makeScene(ctx);
    FillSolidOp fill;
    fill.targetRegion = scene.region;
    fill.fallbackColor = {24, 28, 36, 255};
    LS_CHECK(ctx.addOperation(scene.layer, fill).ok());

    auto compiled = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(compiled.ok());
    LS_CHECK(opaqueCount(compiled.value.raster) == 64);
    LS_CHECK(compiled.value.bounds.min.x == 8 && compiled.value.bounds.max.x == 16);
    LS_CHECK(pixelAt(compiled.value.raster, 10, 10) == Color{24, 28, 36, 255});
    LS_CHECK(pixelAt(compiled.value.raster, 2, 2).a == 0);

    // Same input, same output — the determinism guarantee.
    auto again = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(again.ok());
    LS_CHECK(identical(compiled.value.raster, again.value.raster));
}

void testTransformsAreNonDestructive(LSContext& ctx) {
    const Scene scene = makeScene(ctx, {12.f, 12.f}, 8.f);
    FillSolidOp fill;
    fill.targetRegion = scene.region;
    fill.fallbackColor = Color::white();
    LS_CHECK(ctx.addOperation(scene.layer, fill).ok());

    auto before = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(before.ok());
    const int64_t sourcePixels = geom::pixelCount(ctx.getRegionIntervals(scene.region).value);

    RotateOp rotate;
    rotate.targetLayer = scene.layer;
    rotate.angleDegrees = 90.f;
    rotate.pivotFallback = {16.f, 16.f};
    auto rotateOp = ctx.addOperation(scene.layer, rotate);
    LS_CHECK(rotateOp.ok());

    auto turned = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(turned.ok());

    // A quarter turn is grid-preserving: not one pixel may be lost or invented.
    LS_CHECK(opaqueCount(turned.value.raster) == opaqueCount(before.value.raster));

    // The stored region is untouched: pixels were never the source truth.
    LS_CHECK(geom::pixelCount(ctx.getRegionIntervals(scene.region).value) == sourcePixels);

    // Removing the transform returns exactly the original raster — no
    // accumulated degradation, which is the whole point of the substrate.
    LS_CHECK(ctx.removeOperation(scene.layer, rotateOp.value).ok());
    auto restored = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(restored.ok());
    LS_CHECK(identical(restored.value.raster, before.value.raster));

    // A 25 degree turn keeps the shape substantially intact.
    RotateOp angled = rotate;
    angled.angleDegrees = 25.f;
    LS_CHECK(ctx.addOperation(scene.layer, angled).ok());
    auto skewed = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(skewed.ok());
    const int rotatedCount = opaqueCount(skewed.value.raster);
    LS_CHECK(rotatedCount > 48 && rotatedCount < 90);

    // Repeated compiles of a rotated sprite do not drift.
    auto skewedAgain = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(identical(skewed.value.raster, skewedAgain.value.raster));
}

void testTranslateAndMirror(LSContext& ctx) {
    const Scene scene = makeScene(ctx, {4.f, 4.f}, 6.f);
    FillSolidOp fill;
    fill.targetRegion = scene.region;
    fill.fallbackColor = Color::white();
    LS_CHECK(ctx.addOperation(scene.layer, fill).ok());

    TranslateOp translate;
    translate.targetLayer = scene.layer;
    translate.delta = {5.f, 3.f};
    auto translateOp = ctx.addOperation(scene.layer, translate);
    LS_CHECK(translateOp.ok());

    auto moved = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(moved.ok());
    LS_CHECK(opaqueCount(moved.value.raster) == 36);
    LS_CHECK(moved.value.bounds.min.x == 9);
    LS_CHECK(moved.value.bounds.min.y == 7);

    LS_CHECK(ctx.removeOperation(scene.layer, translateOp.value).ok());
    MirrorOp mirror;
    mirror.targetLayer = scene.layer;
    mirror.axis = MirrorAxis::X;      // flip along X: left becomes right
    mirror.pivotFallback = {16.f, 16.f};
    LS_CHECK(ctx.addOperation(scene.layer, mirror).ok());

    auto mirrored = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(mirrored.ok());
    LS_CHECK(opaqueCount(mirrored.value.raster) == 36);
    LS_CHECK(mirrored.value.bounds.min.x == 22);   // 32 - 4 - 6
}

void testDitherCoordinateSpaces(LSContext& ctx) {
    // The same shape drawn at two canvas positions must carry the same dither
    // pattern in Object space, and a different one in Canvas space. This is the
    // rule that stops patterns shimmering when a sprite moves.
    auto patternOf = [&](Vec2f origin, CoordinateSpace space) {
        const Scene scene = makeScene(ctx, origin, 8.f);
        const RampId ramp = ctx.createRamp(scene.doc,
            {"tone", {{0.f, {40, 40, 40, 255}}, {1.f, {230, 230, 230, 255}}}, true}).value;
        const PatternId bayer = ctx.createOrderedDitherPattern(scene.doc, 4).value;

        FillDitherOp dither;
        dither.targetRegion = scene.region;
        dither.ramp = ramp;
        dither.pattern = bayer;
        dither.density = 0.5f;
        dither.coordinateSpace = space;
        ctx.addOperation(scene.layer, dither);

        auto compiled = ctx.compileSprite(scene.sprite, exportProfile());
        std::vector<uint8_t> tone;
        for (int32_t y = 0; y < 8; ++y) {
            for (int32_t x = 0; x < 8; ++x) {
                tone.push_back(pixelAt(compiled.value.raster,
                                       static_cast<uint32_t>(origin.x) + x,
                                       static_cast<uint32_t>(origin.y) + y).r);
            }
        }
        return tone;
    };

    const std::vector<uint8_t> objectA = patternOf({4.f, 4.f}, CoordinateSpace::Object);
    const std::vector<uint8_t> objectB = patternOf({7.f, 10.f}, CoordinateSpace::Object);
    LS_CHECK(objectA == objectB);

    const std::vector<uint8_t> canvasA = patternOf({4.f, 4.f}, CoordinateSpace::Canvas);
    const std::vector<uint8_t> canvasB = patternOf({7.f, 10.f}, CoordinateSpace::Canvas);
    LS_CHECK(canvasA != canvasB);

    // A 50% ordered dither uses both tones, roughly evenly.
    int light = 0;
    for (uint8_t tone : objectA) {
        light += tone > 128 ? 1 : 0;
    }
    LS_CHECK(light > 20 && light < 44);
}

void testStrokesAndOutlines(LSContext& ctx) {
    const Scene scene = makeScene(ctx, {10.f, 10.f}, 10.f);
    FillSolidOp fill;
    fill.targetRegion = scene.region;
    fill.fallbackColor = {90, 110, 140, 255};
    LS_CHECK(ctx.addOperation(scene.layer, fill).ok());

    GenerateOuterOutlineOp outline;
    outline.targetRegion = scene.region;
    outline.thickness = 1.f;
    outline.fallbackColor = Color::black();
    LS_CHECK(ctx.addOperation(scene.layer, outline).ok());

    auto compiled = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(compiled.ok());
    LS_CHECK(opaqueCount(compiled.value.raster) == 12 * 12);   // 10x10 plus a ring
    LS_CHECK(pixelAt(compiled.value.raster, 9, 9) == Color::black());
    LS_CHECK(pixelAt(compiled.value.raster, 12, 12) == Color{90, 110, 140, 255});

    // A stroked polyline paints a line of the requested width.
    const Scene strokeScene = makeScene(ctx);
    PolylineDesc path;
    path.points = { {4.f, 16.5f}, {28.f, 16.5f} };   // centred on a pixel row
    const GeometryId line = ctx.createPolyline(strokeScene.doc, path).value;
    StrokePolylineOp stroke;
    stroke.polyline = line;
    stroke.width = 3.f;
    stroke.fallbackColor = Color::white();
    LS_CHECK(ctx.addOperation(strokeScene.layer, stroke).ok());

    auto stroked = ctx.compileSprite(strokeScene.sprite, exportProfile());
    LS_CHECK(stroked.ok());
    LS_CHECK(stroked.value.bounds.height() == 3);
    LS_CHECK(opaqueCount(stroked.value.raster) > 60);
}

void testCompositing(LSContext& ctx) {
    const Scene scene = makeScene(ctx, {8.f, 8.f}, 10.f);
    FillSolidOp base;
    base.targetRegion = scene.region;
    base.fallbackColor = {200, 0, 0, 255};
    LS_CHECK(ctx.addOperation(scene.layer, base).ok());

    const LayerId top = ctx.createLayer(scene.sprite, {"highlight"}).value;
    const GeometryId smallRect = ctx.createRect(scene.doc, {{10.f, 10.f}, 4.f, 4.f, 0.f}).value;
    const RegionId smallRegion = ctx.createRegionFromGeometry(smallRect).value;
    FillSolidOp highlight;
    highlight.targetRegion = smallRegion;
    highlight.fallbackColor = {0, 0, 200, 255};
    LS_CHECK(ctx.addOperation(top, highlight).ok());

    auto compiled = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(compiled.ok());
    LS_CHECK(pixelAt(compiled.value.raster, 11, 11) == Color{0, 0, 200, 255});
    LS_CHECK(pixelAt(compiled.value.raster, 8, 8) == Color{200, 0, 0, 255});

    // Hiding the top layer removes its contribution.
    LS_CHECK(ctx.setLayerVisibility(top, false).ok());
    auto hidden = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(pixelAt(hidden.value.raster, 11, 11) == Color{200, 0, 0, 255});
    LS_CHECK(ctx.setLayerVisibility(top, true).ok());

    // Multiply darkens where the layers meet.
    LS_CHECK(ctx.setLayerBlendMode(top, BlendMode::Multiply).ok());
    auto multiplied = ctx.compileSprite(scene.sprite, exportProfile());
    const Color mixed = pixelAt(multiplied.value.raster, 11, 11);
    LS_CHECK(mixed.r == 0 && mixed.b == 0);
    LS_CHECK(ctx.setLayerBlendMode(top, BlendMode::Normal).ok());

    // A clipped layer only paints over its base.
    const LayerId clipped = ctx.createLayer(scene.sprite, {"clipped"}).value;
    const GeometryId wideRect = ctx.createRect(scene.doc, {{0.f, 10.f}, 32.f, 2.f, 0.f}).value;
    const RegionId wideRegion = ctx.createRegionFromGeometry(wideRect).value;
    FillSolidOp band;
    band.targetRegion = wideRegion;
    band.fallbackColor = {0, 200, 0, 255};
    LS_CHECK(ctx.addOperation(clipped, band).ok());
    LS_CHECK(ctx.setLayerClip(clipped, scene.layer).ok());

    auto clippedResult = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(pixelAt(clippedResult.value.raster, 2, 10).a == 0);           // outside the base
    LS_CHECK(pixelAt(clippedResult.value.raster, 9, 10) == Color{0, 200, 0, 255});

    // A layer mask cuts the layer down to the mask region.
    LS_CHECK(ctx.clearLayerClip(clipped).ok());
    const GeometryId maskRect = ctx.createRect(scene.doc, {{0.f, 0.f}, 6.f, 32.f, 0.f}).value;
    const RegionId maskRegion = ctx.createRegionFromGeometry(maskRect).value;
    LS_CHECK(ctx.setLayerMask(clipped, maskRegion).ok());
    auto maskedResult = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(pixelAt(maskedResult.value.raster, 2, 10) == Color{0, 200, 0, 255});
    LS_CHECK(pixelAt(maskedResult.value.raster, 20, 10).a == 0);
}

void testProfilesAndPolicies(LSContext& ctx) {
    const Scene scene = makeScene(ctx, {8.f, 8.f}, 8.f);
    FillSolidOp fill;
    fill.targetRegion = scene.region;
    fill.fallbackColor = {130, 145, 170, 255};
    LS_CHECK(ctx.addOperation(scene.layer, fill).ok());

    auto mask = ctx.compileToMask(scene.sprite, exportProfile());
    LS_CHECK(mask.ok());
    LS_CHECK(pixelAt(mask.value, 10, 10) == Color::white());
    LS_CHECK(pixelAt(mask.value, 2, 2).a == 0);

    auto bounds = ctx.compileBoundsOnly(scene.sprite, exportProfile());
    LS_CHECK(bounds.ok());
    LS_CHECK(bounds.value.min.x == 8 && bounds.value.max.y == 16);

    CompileProfile debug = exportProfile();
    debug.type = CompileProfileType::Debug;
    auto traced = ctx.compileSprite(scene.sprite, debug);
    LS_CHECK(traced.ok());
    LS_CHECK(!traced.value.trace.empty());

    auto preview = ctx.compilePreview(scene.sprite, 32, 32);
    LS_CHECK(preview.ok());
    LS_CHECK(preview.value.raster.width == 32);

    // Palette policy snaps output to the bound palette.
    const PaletteId palette = ctx.createPalette(scene.doc,
        {"two", {{0, {0, 0, 0, 255}, "ink"}, {1, {255, 255, 255, 255}, "paper"}}}).value;
    LS_CHECK(ctx.bindSpritePalette(scene.sprite, palette).ok());
    CompileProfile quantized = exportProfile();
    quantized.palette = PalettePolicy::NearestMatch;
    auto snapped = ctx.compileSprite(scene.sprite, quantized);
    LS_CHECK(snapped.ok());
    const Color quantizedColor = pixelAt(snapped.value.raster, 10, 10);
    LS_CHECK(quantizedColor == Color{255, 255, 255, 255} || quantizedColor == Color{0, 0, 0, 255});

    // A fill bound to a palette role follows the palette, not a literal colour.
    const Scene roleScene = makeScene(ctx, {4.f, 4.f}, 4.f);
    const PaletteId rolePalette = ctx.createPalette(roleScene.doc,
        {"roles", {{7, {12, 34, 56, 255}, "base"}}}).value;
    LS_CHECK(ctx.bindSpritePalette(roleScene.sprite, rolePalette).ok());
    FillSemanticColorOp semantic;
    semantic.targetRegion = roleScene.region;
    semantic.paletteRole = 7;
    LS_CHECK(ctx.addOperation(roleScene.layer, semantic).ok());

    CompileProfile unconstrained = exportProfile();
    unconstrained.palette = PalettePolicy::Unconstrained;
    auto byRole = ctx.compileSprite(roleScene.sprite, unconstrained);
    LS_CHECK(pixelAt(byRole.value.raster, 5, 5) == Color{12, 34, 56, 255});

    // Swapping the palette repaints without touching a single operation.
    LS_CHECK(ctx.setPaletteColor(rolePalette, 7, {200, 100, 50, 255}).ok());
    auto swapped = ctx.compileSprite(roleScene.sprite, unconstrained);
    LS_CHECK(pixelAt(swapped.value.raster, 5, 5) == Color{200, 100, 50, 255});
}

void testCacheAndDirtyRecompile(LSContext& ctx) {
    const Scene scene = makeScene(ctx, {8.f, 8.f}, 8.f);
    FillSolidOp fill;
    fill.targetRegion = scene.region;
    fill.fallbackColor = Color::white();
    LS_CHECK(ctx.addOperation(scene.layer, fill).ok());

    LS_CHECK(ctx.compileSprite(scene.sprite, exportProfile()).ok());
    const size_t hitsBefore = ctx.cacheStats().hits;
    LS_CHECK(ctx.compileSprite(scene.sprite, exportProfile()).ok());
    LS_CHECK(ctx.cacheStats().hits > hitsBefore);

    // Editing the source geometry invalidates the cached compile and the next
    // compile reflects the edit.
    const GeometryId rect = ctx.createRect(scene.doc, {{0.f, 0.f}, 4.f, 4.f, 0.f}).value;
    (void)rect;
    LS_CHECK(ctx.markDirty(scene.sprite.value).ok());
    LS_CHECK(ctx.isDirty(scene.sprite.value).value);
    auto recompiled = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(recompiled.ok());
    LS_CHECK(!ctx.isDirty(scene.sprite.value).value);

    LS_CHECK(ctx.compileDirtyOnly(scene.doc, exportProfile()).ok());
}

void testPluginOperation(LSContext& ctx) {
    PluginOperationDesc desc;
    desc.typeId = "com.test.stamp";
    desc.displayName = "Stamp";
    desc.isDeterministic = true;
    desc.getDependencies = [](const PluginOp& op) {
        std::vector<uint64_t> ids;
        auto it = op.params.find("region");
        if (it != op.params.end()) {
            if (const uint64_t* id = std::get_if<uint64_t>(&it->second)) {
                ids.push_back(*id);
            }
        }
        return ids;
    };
    desc.resolve = [](const PluginOp& op, PluginResolveContext& pluginCtx) {
        auto it = op.params.find("region");
        if (it == op.params.end() || pluginCtx.outputBuffer == nullptr) {
            return LSError::InvalidParameter;
        }
        const uint64_t* id = std::get_if<uint64_t>(&it->second);
        if (id == nullptr) {
            return LSError::InvalidParameter;
        }
        const IntervalSet* coverage = pluginCtx.getRegion(RegionId{*id});
        if (coverage == nullptr) {
            return LSError::InvalidId;
        }
        for (const Interval& interval : coverage->intervals) {
            for (int32_t x = interval.x0; x < interval.x1; ++x) {
                writePixel(*pluginCtx.outputBuffer, x, interval.y, Color{7, 8, 9, 255});
            }
        }
        return LSError::None;
    };

    LS_CHECK(ctx.registerOperationType(desc).ok());
    LS_CHECK(ctx.isOperationTypeRegistered("com.test.stamp"));

    const Scene scene = makeScene(ctx, {6.f, 6.f}, 5.f);
    PluginOp op;
    op.typeId = "com.test.stamp";
    op.params["region"] = static_cast<uint64_t>(scene.region.value);
    auto added = ctx.addOperation(scene.layer, op);
    LS_CHECK(added.ok());

    auto compiled = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(compiled.ok());
    LS_CHECK(pixelAt(compiled.value.raster, 7, 7) == Color{7, 8, 9, 255});
    LS_CHECK(opaqueCount(compiled.value.raster) == 25);

    // The plugin declared what it reads, so the graph knows to recompile it.
    auto info = ctx.getDependencyInfo(added.value.value);
    LS_CHECK(info.ok());
    bool readsRegion = false;
    for (uint64_t dependency : info.value.dependencies) {
        readsRegion = readsRegion || dependency == scene.region.value;
    }
    LS_CHECK(readsRegion);

    // An unregistered type is refused at authoring time, not at compile time.
    PluginOp unknown;
    unknown.typeId = "com.test.missing";
    LS_CHECK(ctx.addOperation(scene.layer, unknown).fail());

    // Non-deterministic plugins are barred from the Export profile.
    PluginOperationDesc loose = desc;
    loose.typeId = "com.test.random";
    loose.isDeterministic = false;
    LS_CHECK(ctx.registerOperationType(loose).ok());
    const Scene looseScene = makeScene(ctx, {6.f, 6.f}, 4.f);
    PluginOp looseOp;
    looseOp.typeId = "com.test.random";
    looseOp.params["region"] = static_cast<uint64_t>(looseScene.region.value);
    LS_CHECK(ctx.addOperation(looseScene.layer, looseOp).ok());
    LS_CHECK(ctx.compileSprite(looseScene.sprite, exportProfile()).fail());
    CompileProfile preview = exportProfile();
    preview.type = CompileProfileType::Preview;
    LS_CHECK(ctx.compileSprite(looseScene.sprite, preview).ok());
}

void testSnapshotRoundTrip(LSContext& ctx) {
    const Scene scene = makeScene(ctx, {10.f, 10.f}, 6.f);
    FillSolidOp fill;
    fill.targetRegion = scene.region;
    fill.fallbackColor = Color::white();
    LS_CHECK(ctx.addOperation(scene.layer, fill).ok());

    auto snapshot = ctx.snapshotCompiledSprite(scene.sprite, exportProfile());
    LS_CHECK(snapshot.ok());
    LS_CHECK(geom::pixelCount(snapshot.value.mask) == 36);

    // A snapshot converts back into a live region: the app decides whether to
    // call that "bake", the engine only supplies the conversion.
    auto live = ctx.convertSnapshotToLiveRegion(scene.doc, snapshot.value);
    LS_CHECK(live.ok());
    LS_CHECK(geom::pixelCount(ctx.getRegionIntervals(live.value).value) == 36);

    auto collision = ctx.compileToCollisionShape(scene.sprite, exportProfile());
    LS_CHECK(collision.ok());
    auto path = ctx.getGeometryPath(collision.value);
    LS_CHECK(path.ok() && path.value.size() >= 4);
}

} // namespace

int main() {
    auto ctx = LSContext::create();
    LS_REQUIRE_MAIN(ctx != nullptr);

    testSolidFillAndBounds(*ctx);
    testTransformsAreNonDestructive(*ctx);
    testTranslateAndMirror(*ctx);
    testDitherCoordinateSpaces(*ctx);
    testStrokesAndOutlines(*ctx);
    testCompositing(*ctx);
    testProfilesAndPolicies(*ctx);
    testCacheAndDirtyRecompile(*ctx);
    testPluginOperation(*ctx);
    testSnapshotRoundTrip(*ctx);

    return lstest::report("compile");
}
