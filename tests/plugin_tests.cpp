// plugin_tests.cpp — the extension surface: registered pattern types, fill and
// transform resolvers, compile policies, and the outline post-processing ops.

#include "ls_test.h"

#include <livesprite/livesprite.h>

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

CompileProfile exportProfile() {
    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = 32;
    profile.outputHeight = 32;
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

void testPluginPatternType(LSContext& ctx) {
    PluginPatternDesc desc;
    desc.typeId = "com.test.stripes";
    desc.displayName = "Stripes";
    desc.tileWidth = 4;
    desc.tileHeight = 4;
    desc.generateTile = [](uint32_t w, uint32_t h, float density, float phase,
                           const PluginParams& params) {
        (void)density; (void)phase;
        uint32_t period = 2;
        auto it = params.find("period");
        if (it != params.end()) {
            if (const int64_t* value = std::get_if<int64_t>(&it->second)) {
                period = static_cast<uint32_t>(*value);
            }
        }
        std::vector<bool> tile(static_cast<size_t>(w) * h, false);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                tile[static_cast<size_t>(y) * w + x] = (x % period) == 0;
            }
        }
        return tile;
    };
    LS_CHECK(ctx.registerPatternType(desc).ok());

    const Scene scene = makeScene(ctx);
    PluginParams params;
    params["period"] = static_cast<int64_t>(2);
    auto pattern = ctx.createPluginPattern(scene.doc, "com.test.stripes", 1.f, 0.f, params);
    LS_REQUIRE(pattern.ok());

    auto tile = ctx.getPattern(pattern.value);
    LS_CHECK(tile.ok());
    LS_CHECK(tile.value.tileWidth == 4 && tile.value.mask.size() == 16);
    LS_CHECK(tile.value.mask[0] == 0);   // foreground on the stripe
    LS_CHECK(tile.value.mask[1] == 1);   // background between stripes

    // The baked tile drives a texture fill like any built-in pattern.
    FillTexturePatternOp fill;
    fill.targetRegion = scene.region;
    fill.pattern = pattern.value;
    fill.foregroundRole = kColorRoleNone;
    LS_CHECK(ctx.addOperation(scene.layer, fill).ok());
    auto compiled = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(compiled.ok());
    LS_CHECK(opaqueCount(compiled.value.raster) == 32);   // half of an 8x8 region

    LS_CHECK(ctx.createPluginPattern(scene.doc, "com.test.missing").fail());
}

void testPluginFillResolver(LSContext& ctx) {
    PluginFillResolverDesc desc;
    desc.typeId = "com.test.checker";
    desc.resolve = [](const PluginOp& op, const IntervalSet& region,
                      PluginResolveContext& pluginCtx) {
        (void)op;
        if (pluginCtx.outputBuffer == nullptr) {
            return LSError::InvalidParameter;
        }
        if (region.empty()) {
            return LSError::RegionEmpty;
        }
        for (const Interval& interval : region.intervals) {
            for (int32_t x = interval.x0; x < interval.x1; ++x) {
                if (((x + interval.y) % 2) == 0) {
                    writePixel(*pluginCtx.outputBuffer, x, interval.y, Color{10, 200, 90, 255});
                }
            }
        }
        return LSError::None;
    };
    LS_CHECK(ctx.registerFillResolver(desc).ok());

    const Scene scene = makeScene(ctx, {10.f, 10.f}, 6.f);
    PluginOp op;
    op.typeId = "com.test.checker";
    op.params["region"] = static_cast<uint64_t>(scene.region.value);
    LS_CHECK(ctx.addOperation(scene.layer, op).ok());

    auto compiled = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(compiled.ok());
    LS_CHECK(opaqueCount(compiled.value.raster) == 18);   // half of a 6x6 region
    LS_CHECK(readPixel(compiled.value.raster, 10, 10) == Color{10, 200, 90, 255});
    LS_CHECK(readPixel(compiled.value.raster, 11, 10).a == 0);

    // A resolver that reports an error fails the compile rather than producing
    // half-drawn output.
    const Scene broken = makeScene(ctx, {4.f, 4.f}, 4.f);
    PluginOp missingRegion;
    missingRegion.typeId = "com.test.checker";
    LS_CHECK(ctx.addOperation(broken.layer, missingRegion).ok());
    LS_CHECK(ctx.compileSprite(broken.sprite, exportProfile()).fail());
}

void testPluginTransformResolver(LSContext& ctx) {
    PluginTransformResolverDesc desc;
    desc.typeId = "com.test.nudge";
    desc.resolve = [](const PluginOp& op, PluginResolveContext& pluginCtx) {
        (void)pluginCtx;
        Vec2f delta {0.f, 0.f};
        auto it = op.params.find("delta");
        if (it != op.params.end()) {
            if (const Vec2f* value = std::get_if<Vec2f>(&it->second)) {
                delta = *value;
            }
        }
        return Result<Mat3f>::ok(Mat3f::translation(delta));
    };
    LS_CHECK(ctx.registerTransformResolver(desc).ok());

    const Scene scene = makeScene(ctx, {4.f, 4.f}, 6.f);
    FillSolidOp fill;
    fill.targetRegion = scene.region;
    fill.fallbackColor = Color::white();
    LS_CHECK(ctx.addOperation(scene.layer, fill).ok());

    PluginOp nudge;
    nudge.typeId = "com.test.nudge";
    nudge.params["delta"] = Vec2f{6.f, 2.f};
    LS_CHECK(ctx.addOperation(scene.layer, nudge).ok());

    auto compiled = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(compiled.ok());
    LS_CHECK(opaqueCount(compiled.value.raster) == 36);
    LS_CHECK(compiled.value.bounds.min.x == 10);
    LS_CHECK(compiled.value.bounds.min.y == 6);
}

void testPluginCompilePolicy(LSContext& ctx) {
    PluginCompilePolicyDesc desc;
    desc.typeId = "com.test.firstsample";
    desc.resolve = [](const std::vector<Color>& samples, const CompileProfile& profile) {
        // Only accept a pixel that is almost fully covered, then paint it flat.
        if (samples.size() < 7) {
            return Result<Color>::ok(Color::transparent());
        }
        (void)profile;
        return Result<Color>::ok(Color{255, 0, 255, 255});
    };
    LS_CHECK(ctx.registerCompilePolicy(desc).ok());

    const Scene scene = makeScene(ctx, {10.f, 10.f}, 10.f);
    FillSolidOp fill;
    fill.targetRegion = scene.region;
    fill.fallbackColor = Color::white();
    LS_CHECK(ctx.addOperation(scene.layer, fill).ok());
    RotateOp rotate;
    rotate.targetLayer = scene.layer;
    rotate.angleDegrees = 30.f;
    rotate.pivotFallback = {16.f, 16.f};
    LS_CHECK(ctx.addOperation(scene.layer, rotate).ok());

    CompileProfile profile = exportProfile();
    profile.samplingPolicyId = "com.test.firstsample";
    auto compiled = ctx.compileSprite(scene.sprite, profile);
    LS_REQUIRE(compiled.ok());

    // The policy repainted every surviving pixel in its own colour.
    bool allPolicyColored = true;
    int painted = 0;
    for (uint32_t y = 0; y < compiled.value.raster.height; ++y) {
        for (uint32_t x = 0; x < compiled.value.raster.width; ++x) {
            const Color color = readPixel(compiled.value.raster,
                                          static_cast<int32_t>(x), static_cast<int32_t>(y));
            if (color.a == 0) {
                continue;
            }
            ++painted;
            allPolicyColored = allPolicyColored && color == Color{255, 0, 255, 255};
        }
    }
    LS_CHECK(painted > 0);
    LS_CHECK(allPolicyColored);

    // A profile naming an unregistered policy is an error, not a silent
    // fallback: the caller asked for output this build cannot produce.
    CompileProfile unknown = exportProfile();
    unknown.samplingPolicyId = "com.test.nosuch";
    LS_CHECK(ctx.compileSprite(scene.sprite, unknown).fail());
}

void testOutlinePostProcessing(LSContext& ctx) {
    const Scene scene = makeScene(ctx, {10.f, 10.f}, 8.f);
    FillSolidOp fill;
    fill.targetRegion = scene.region;
    fill.fallbackColor = {80, 90, 110, 255};
    LS_CHECK(ctx.addOperation(scene.layer, fill).ok());

    GenerateOuterOutlineOp outline;
    outline.targetRegion = scene.region;
    outline.thickness = 1.f;
    outline.fallbackColor = Color::black();
    auto outlineOp = ctx.addOperation(scene.layer, outline);
    LS_REQUIRE(outlineOp.ok());

    auto before = ctx.compileSprite(scene.sprite, exportProfile());
    LS_REQUIRE(before.ok());

    // Cleanup keeps a solid ring intact: nothing in it is isolated.
    CleanupOutlineOp cleanup;
    cleanup.targetOutlineOp = outlineOp.value;
    cleanup.removeIsolatedPixels = true;
    auto cleanupOp = ctx.addOperation(scene.layer, cleanup);
    LS_REQUIRE(cleanupOp.ok());
    auto cleaned = ctx.compileSprite(scene.sprite, exportProfile());
    LS_REQUIRE(cleaned.ok());
    LS_CHECK(opaqueCount(cleaned.value.raster) == opaqueCount(before.value.raster));

    // A separate outline plus a join fills the gap between the two.
    const GeometryId farRect = ctx.createRect(scene.doc, {{22.f, 10.f}, 4.f, 8.f, 0.f}).value;
    const RegionId farRegion = ctx.createRegionFromGeometry(farRect).value;
    GenerateOuterOutlineOp second;
    second.targetRegion = farRegion;
    second.thickness = 1.f;
    second.fallbackColor = {200, 40, 40, 255};
    auto secondOp = ctx.addOperation(scene.layer, second);
    LS_REQUIRE(secondOp.ok());

    auto beforeJoin = ctx.compileSprite(scene.sprite, exportProfile());
    LS_REQUIRE(beforeJoin.ok());

    JoinCornersOp join;
    join.outlineA = outlineOp.value;
    join.outlineB = secondOp.value;
    join.joinRadius = 2.f;
    LS_CHECK(ctx.addOperation(scene.layer, join).ok());

    auto joined = ctx.compileSprite(scene.sprite, exportProfile());
    LS_REQUIRE(joined.ok());
    LS_CHECK(opaqueCount(joined.value.raster) > opaqueCount(beforeJoin.value.raster));

    // Collision resolution hands shared pixels to the first outline listed.
    ResolveOutlineCollisionsOp collisions;
    collisions.outlineOps = { secondOp.value, outlineOp.value };
    LS_CHECK(ctx.addOperation(scene.layer, collisions).ok());
    auto resolved = ctx.compileSprite(scene.sprite, exportProfile());
    LS_CHECK(resolved.ok());

    // A cleanup naming an operation that produced nothing is skipped, not fatal.
    const Scene empty = makeScene(ctx, {2.f, 2.f}, 2.f);
    CleanupOutlineOp orphan;
    orphan.targetOutlineOp = OperationId{999999};
    LS_CHECK(ctx.addOperation(empty.layer, orphan).ok());
    LS_CHECK(ctx.compileSprite(empty.sprite, exportProfile()).ok());
}

} // namespace

int main() {
    auto ctx = LSContext::create();
    LS_REQUIRE_MAIN(ctx != nullptr);

    testPluginPatternType(*ctx);
    testPluginFillResolver(*ctx);
    testPluginTransformResolver(*ctx);
    testPluginCompilePolicy(*ctx);
    testOutlinePostProcessing(*ctx);

    return lstest::report("plugin");
}
