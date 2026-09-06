// render_tests.cpp — the controls that used to be stored and ignored: stroke
// joins, caps, snapping and patterns; groups as compositing units; and the
// resolution policies.

#include "ls_test.h"

#include <livesprite/livesprite.h>

#include <cmath>
#include <set>
#include <utility>
#include <vector>

using namespace ls;

namespace {

CompileProfile exportProfile(uint32_t size = 40) {
    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = size;
    profile.outputHeight = size;
    profile.palette = PalettePolicy::Unconstrained;
    return profile;
}

struct Stage {
    DocumentId doc;
    SpriteId sprite;
    LayerId layer;
};

Stage makeStage(LSContext& ctx, uint32_t size = 40) {
    Stage stage;
    stage.doc = ctx.createDocument({"render", size, size}).value;
    stage.sprite = ctx.createSprite(stage.doc).value;
    stage.layer = ctx.createLayer(stage.sprite, {"main"}).value;
    return stage;
}

int opaqueCount(const RasterBuffer& raster) {
    int count = 0;
    for (uint32_t y = 0; y < raster.height; ++y) {
        for (uint32_t x = 0; x < raster.width; ++x) {
            count += readPixel(raster, static_cast<int32_t>(x), static_cast<int32_t>(y)).a != 0
                ? 1 : 0;
        }
    }
    return count;
}

// --- strokes ---------------------------------------------------------------

// A sharp corner, stroked thick enough that the join treatment is visible.
RasterBuffer strokeCorner(LSContext& ctx, StrokeJoin join, StrokeCap cap, float miterLimit,
                          SnapPolicy snap, const std::vector<Vec2f>& points, float width = 5.f) {
    const Stage stage = makeStage(ctx);
    PolylineDesc path;
    path.points = points;
    const GeometryId geometry = ctx.createPolyline(stage.doc, path).value;

    StrokePolylineOp stroke;
    stroke.polyline = geometry;
    stroke.width = width;
    stroke.join = join;
    stroke.cap = cap;
    stroke.miterLimit = miterLimit;
    stroke.snap = snap;
    stroke.fallbackColor = Color::white();
    ctx.addOperation(stage.layer, stroke);
    return ctx.compileSprite(stage.sprite, exportProfile()).value.raster;
}

void testStrokeJoins() {
    auto ctx = LSContext::create();
    const std::vector<Vec2f> corner = { {8.f, 8.f}, {28.f, 8.f}, {28.f, 30.f} };

    const RasterBuffer miter = strokeCorner(*ctx, StrokeJoin::Miter, StrokeCap::Flat, 4.f,
                                            SnapPolicy::None, corner);
    const RasterBuffer bevel = strokeCorner(*ctx, StrokeJoin::Bevel, StrokeCap::Flat, 4.f,
                                            SnapPolicy::None, corner);
    const RasterBuffer round = strokeCorner(*ctx, StrokeJoin::Round, StrokeCap::Flat, 4.f,
                                            SnapPolicy::None, corner);

    // The three joins are genuinely different marks.
    LS_CHECK(miter.pixels != bevel.pixels);
    LS_CHECK(bevel.pixels != round.pixels);
    LS_CHECK(miter.pixels != round.pixels);

    // A miter fills the outer corner, so it covers at least as much as a bevel,
    // which cuts that corner off.
    LS_CHECK(opaqueCount(miter) > opaqueCount(bevel));

    // The miter reaches further into the outer corner than the bevel does.
    auto reach = [](const RasterBuffer& raster) {
        int32_t maxX = -1;
        int32_t minY = 1000;
        for (uint32_t y = 0; y < raster.height; ++y) {
            for (uint32_t x = 0; x < raster.width; ++x) {
                if (readPixel(raster, static_cast<int32_t>(x), static_cast<int32_t>(y)).a != 0) {
                    maxX = std::max(maxX, static_cast<int32_t>(x));
                    minY = std::min(minY, static_cast<int32_t>(y));
                }
            }
        }
        return std::pair<int32_t, int32_t>{ maxX, minY };
    };
    LS_CHECK(reach(miter).first >= reach(bevel).first);
    LS_CHECK(reach(miter).second <= reach(bevel).second);

    // A very sharp turn would spike past the limit, so it falls back to a bevel.
    const std::vector<Vec2f> spike = { {8.f, 20.f}, {30.f, 20.f}, {9.f, 22.f} };
    const RasterBuffer limited = strokeCorner(*ctx, StrokeJoin::Miter, StrokeCap::Flat, 1.2f,
                                              SnapPolicy::None, spike);
    const RasterBuffer beveled = strokeCorner(*ctx, StrokeJoin::Bevel, StrokeCap::Flat, 1.2f,
                                              SnapPolicy::None, spike);
    LS_CHECK(limited.pixels == beveled.pixels);

    // A generous limit lets the same turn keep its spike.
    const RasterBuffer spiked = strokeCorner(*ctx, StrokeJoin::Miter, StrokeCap::Flat, 20.f,
                                             SnapPolicy::None, spike);
    LS_CHECK(opaqueCount(spiked) > opaqueCount(limited));
}

void testStrokeCapsAndSnap() {
    auto ctx = LSContext::create();
    const std::vector<Vec2f> line = { {10.f, 20.f}, {30.f, 20.f} };

    const RasterBuffer flat = strokeCorner(*ctx, StrokeJoin::Miter, StrokeCap::Flat, 4.f,
                                           SnapPolicy::None, line);
    const RasterBuffer square = strokeCorner(*ctx, StrokeJoin::Miter, StrokeCap::Square, 4.f,
                                             SnapPolicy::None, line);
    const RasterBuffer round = strokeCorner(*ctx, StrokeJoin::Miter, StrokeCap::Round, 4.f,
                                            SnapPolicy::None, line);

    // A square cap carries the stroke past the end; a round one bulges; a flat
    // one stops dead.
    LS_CHECK(opaqueCount(square) > opaqueCount(flat));
    LS_CHECK(opaqueCount(round) > opaqueCount(flat));
    LS_CHECK(square.pixels != round.pixels);

    auto boundsWidth = [](const RasterBuffer& raster) {
        int32_t minX = 1000, maxX = -1;
        for (uint32_t y = 0; y < raster.height; ++y) {
            for (uint32_t x = 0; x < raster.width; ++x) {
                if (readPixel(raster, static_cast<int32_t>(x), static_cast<int32_t>(y)).a != 0) {
                    minX = std::min(minX, static_cast<int32_t>(x));
                    maxX = std::max(maxX, static_cast<int32_t>(x));
                }
            }
        }
        return maxX - minX + 1;
    };
    LS_CHECK(boundsWidth(square) > boundsWidth(flat));

    // Snapping puts an off-grid path exactly where the on-grid one lands.
    const std::vector<Vec2f> offGrid = { {10.4f, 20.3f}, {30.2f, 20.4f} };
    const RasterBuffer snapped = strokeCorner(*ctx, StrokeJoin::Miter, StrokeCap::Flat, 4.f,
                                              SnapPolicy::Grid, offGrid);
    LS_CHECK(snapped.pixels == flat.pixels);

    // Without snapping it lands somewhere else.
    const RasterBuffer loose = strokeCorner(*ctx, StrokeJoin::Miter, StrokeCap::Flat, 4.f,
                                            SnapPolicy::None, offGrid);
    LS_CHECK(loose.pixels != flat.pixels);
}

void testStrokePatterns() {
    auto ctx = LSContext::create();
    const Stage stage = makeStage(*ctx);

    PolylineDesc path;
    path.points = { {6.f, 20.f}, {34.f, 20.f} };
    const GeometryId line = ctx->createPolyline(stage.doc, path).value;

    auto strokeWith = [&](PatternId pattern) {
        const Stage local = makeStage(*ctx);
        const GeometryId localLine = ctx->createPolyline(local.doc, path).value;
        StrokePolylineOp stroke;
        stroke.polyline = localLine;
        stroke.width = 3.f;
        stroke.strokePattern = pattern;
        stroke.fallbackColor = Color::white();
        ctx->addOperation(local.layer, stroke);
        return ctx->compileSprite(local.sprite, exportProfile()).value.raster;
    };
    (void)line;

    const RasterBuffer solid = strokeWith(PatternId::null());
    const PatternId dashes =
        ctx->createDitherPattern(stage.doc, DitherPatternKind::VerticalLines).value;
    const RasterBuffer dashed = strokeWith(dashes);

    // A patterned stroke marks a subset of the solid one: it thins the line, it
    // does not move it.
    LS_CHECK(opaqueCount(dashed) > 0);
    LS_CHECK(opaqueCount(dashed) < opaqueCount(solid));
    bool subset = true;
    for (uint32_t y = 0; y < solid.height; ++y) {
        for (uint32_t x = 0; x < solid.width; ++x) {
            const bool inDashed = readPixel(dashed, static_cast<int32_t>(x),
                                            static_cast<int32_t>(y)).a != 0;
            const bool inSolid = readPixel(solid, static_cast<int32_t>(x),
                                           static_cast<int32_t>(y)).a != 0;
            subset = subset && (!inDashed || inSolid);
        }
    }
    LS_CHECK(subset);

    // A brush pattern is the stamp shape, so it differs from the default disk.
    auto brushed = [&](PatternId pattern) {
        const Stage local = makeStage(*ctx);
        const GeometryId localLine = ctx->createPolyline(local.doc, path).value;
        StrokeBrushOp brush;
        brush.path = localLine;
        brush.brushPattern = pattern;
        brush.size = 5.f;
        brush.spacing = 0.5f;
        brush.fallbackColor = Color::white();
        ctx->addOperation(local.layer, brush);
        return ctx->compileSprite(local.sprite, exportProfile()).value.raster;
    };
    const RasterBuffer disk = brushed(PatternId::null());
    const PatternId checker =
        ctx->createDitherPattern(stage.doc, DitherPatternKind::Checker).value;
    const RasterBuffer stamped = brushed(checker);
    LS_CHECK(opaqueCount(stamped) > 0);
    LS_CHECK(stamped.pixels != disk.pixels);
}

// --- groups ----------------------------------------------------------------

void testGroupsComposite() {
    auto ctx = LSContext::create();
    const Stage stage = makeStage(*ctx);

    auto fillLayer = [&](LayerId layer, Vec2f origin, Color color) {
        const GeometryId rect = ctx->createRect(stage.doc, {origin, 14.f, 14.f, 0.f}).value;
        const RegionId region = ctx->createRegionFromGeometry(rect).value;
        FillSolidOp fill;
        fill.targetRegion = region;
        fill.fallbackColor = color;
        ctx->addOperation(layer, fill);
    };

    // Two overlapping layers, both inside one group.
    const LayerId lower = stage.layer;
    const LayerId upper = ctx->createLayer(stage.sprite, {"upper"}).value;
    fillLayer(lower, {6.f, 6.f}, Color::white());
    fillLayer(upper, {14.f, 14.f}, Color::white());

    auto group = ctx->createGroup(stage.sprite, GroupDesc{"pair", 0.5f, BlendMode::Normal, true});
    LS_REQUIRE(group.ok());
    LS_CHECK(ctx->addLayerToGroup(group.value, lower).ok());
    LS_CHECK(ctx->addLayerToGroup(group.value, upper).ok());

    CompileProfile profile = exportProfile();
    profile.alpha = AlphaPolicy::Preserve;
    auto compiled = ctx->compileSprite(stage.sprite, profile);
    LS_REQUIRE(compiled.ok());

    // The group is composited as one thing, so the overlap is no more opaque
    // than the rest. Applying the opacity per layer would have stacked it.
    const uint8_t single = readPixel(compiled.value.raster, 8, 8).a;
    const uint8_t overlap = readPixel(compiled.value.raster, 16, 16).a;
    LS_CHECK(single > 100 && single < 160);
    LS_CHECK(overlap == single);

    // Group opacity is live.
    LS_CHECK(ctx->setGroupOpacity(group.value, 1.f).ok());
    auto opaque = ctx->compileSprite(stage.sprite, profile);
    LS_CHECK(readPixel(opaque.value.raster, 8, 8).a == 255);

    // So is group visibility.
    LS_CHECK(ctx->setGroupVisibility(group.value, false).ok());
    auto hidden = ctx->compileSprite(stage.sprite, profile);
    LS_CHECK(opaqueCount(hidden.value.raster) == 0);
    LS_CHECK(ctx->setGroupVisibility(group.value, true).ok());

    // And group blend, which applies to the group against what is under it.
    const LayerId backdrop = ctx->createLayer(stage.sprite, {"backdrop"}).value;
    fillLayer(backdrop, {0.f, 0.f}, Color{200, 60, 60, 255});
    LS_CHECK(ctx->setLayerOrder(stage.sprite, {backdrop, lower, upper}).ok());
    LS_CHECK(ctx->setGroupBlendMode(group.value, BlendMode::Multiply).ok());
    auto blended = ctx->compileSprite(stage.sprite, profile);
    LS_REQUIRE(blended.ok());
    const Color mixed = readPixel(blended.value.raster, 8, 8);
    LS_CHECK(mixed.r == 200 && mixed.g == 60);

    auto info = ctx->getGroupInfo(group.value);
    LS_REQUIRE(info.ok());
    LS_CHECK(info.value.name == "pair");
    LS_CHECK(info.value.blend == BlendMode::Multiply);
    LS_CHECK(info.value.layers.size() == 2);
    LS_CHECK(ctx->setGroupOpacity(group.value, 4.f).fail());
}

void testGroupsSurviveSaveLoad() {
    auto ctx = LSContext::create();
    const Stage stage = makeStage(*ctx);
    const GeometryId rect = ctx->createRect(stage.doc, {{8.f, 8.f}, 12.f, 12.f, 0.f}).value;
    const RegionId region = ctx->createRegionFromGeometry(rect).value;
    FillSolidOp fill;
    fill.targetRegion = region;
    fill.fallbackColor = Color::white();
    ctx->addOperation(stage.layer, fill);

    auto group = ctx->createGroup(stage.sprite, GroupDesc{"soft", 0.25f, BlendMode::Screen, true});
    LS_REQUIRE(group.ok());
    LS_CHECK(ctx->addLayerToGroup(group.value, stage.layer).ok());

    CompileProfile profile = exportProfile();
    profile.alpha = AlphaPolicy::Preserve;
    auto before = ctx->compileSprite(stage.sprite, profile);
    LS_REQUIRE(before.ok());

    auto saved = ctx->serializeDocument(stage.doc);
    LS_REQUIRE(saved.ok());
    auto loaded = LSContext::create();
    LS_REQUIRE(loaded->deserializeDocument(saved.value).ok());

    SpriteId restored;
    for (uint64_t candidate = 1; candidate < 4096; ++candidate) {
        if (loaded->getSpriteInfo(SpriteId{candidate}).ok()) {
            restored = SpriteId{candidate};
            break;
        }
    }
    LS_REQUIRE(restored.valid());
    auto after = loaded->compileSprite(restored, profile);
    LS_REQUIRE(after.ok());
    LS_CHECK(after.value.raster.pixels == before.value.raster.pixels);
}

// --- resolution policies ---------------------------------------------------

void testRoundingPolicies() {
    auto ctx = LSContext::create();

    auto translated = [&](RoundingPolicy rounding, Vec2f delta) {
        const Stage stage = makeStage(*ctx);
        const GeometryId rect = ctx->createRect(stage.doc, {{10.f, 10.f}, 8.f, 8.f, 0.f}).value;
        const RegionId region = ctx->createRegionFromGeometry(rect).value;
        FillSolidOp fill;
        fill.targetRegion = region;
        fill.fallbackColor = Color::white();
        ctx->addOperation(stage.layer, fill);

        TranslateOp move;
        move.targetLayer = stage.layer;
        move.delta = delta;
        move.rounding = rounding;
        ctx->addOperation(stage.layer, move);
        return ctx->compileSprite(stage.sprite, exportProfile()).value.bounds;
    };

    // A fractional move resolves differently under each policy.
    LS_CHECK(translated(RoundingPolicy::Floor, {3.7f, 0.f}).min.x == 13);
    LS_CHECK(translated(RoundingPolicy::Ceil, {3.2f, 0.f}).min.x == 14);
    LS_CHECK(translated(RoundingPolicy::Nearest, {3.7f, 0.f}).min.x == 14);
    LS_CHECK(translated(RoundingPolicy::Truncate, {3.7f, 0.f}).min.x == 13);

    // Rounding reaches rotation too, not only translation.
    auto rotated = [&](RoundingPolicy rounding) {
        const Stage stage = makeStage(*ctx);
        const GeometryId rect = ctx->createRect(stage.doc, {{10.f, 10.f}, 9.f, 5.f, 0.f}).value;
        const RegionId region = ctx->createRegionFromGeometry(rect).value;
        FillSolidOp fill;
        fill.targetRegion = region;
        fill.fallbackColor = Color::white();
        ctx->addOperation(stage.layer, fill);

        RotateOp turn;
        turn.targetLayer = stage.layer;
        turn.angleDegrees = 33.f;
        turn.pivotFallback = {14.3f, 12.7f};
        turn.rounding = rounding;
        ctx->addOperation(stage.layer, turn);
        return ctx->compileSprite(stage.sprite, exportProfile()).value.raster;
    };
    LS_CHECK(rotated(RoundingPolicy::Nearest).pixels != rotated(RoundingPolicy::SubpixelHalf).pixels);
}

void testSamplingPolicies() {
    auto ctx = LSContext::create();

    auto sampled = [&](SamplingPolicy sampling) {
        const Stage stage = makeStage(*ctx);
        // Three bands, so median and majority can disagree.
        const Color bands[3] = { {40, 40, 40, 255}, {130, 130, 130, 255}, {230, 230, 230, 255} };
        for (int i = 0; i < 3; ++i) {
            const GeometryId rect = ctx->createRect(
                stage.doc, {{10.f, 10.f + static_cast<float>(i) * 4.f}, 16.f, 4.f, 0.f}).value;
            const RegionId region = ctx->createRegionFromGeometry(rect).value;
            FillSolidOp fill;
            fill.targetRegion = region;
            fill.fallbackColor = bands[i];
            ctx->addOperation(stage.layer, fill);
        }

        RotateOp turn;
        turn.targetLayer = stage.layer;
        turn.angleDegrees = 27.f;
        turn.pivotFallback = {18.f, 16.f};
        turn.sampling = sampling;
        ctx->addOperation(stage.layer, turn);
        return ctx->compileSprite(stage.sprite, exportProfile()).value.raster;
    };

    const RasterBuffer median = sampled(SamplingPolicy::Median);
    const RasterBuffer majority = sampled(SamplingPolicy::Majority);
    const RasterBuffer average = sampled(SamplingPolicy::Average);

    // Median is its own policy: it picks a sample rather than mixing, so it
    // parts company with Average even where it agrees with Majority.
    LS_CHECK(median.pixels != average.pixels);
    LS_CHECK(opaqueCount(median) > 0 && opaqueCount(majority) > 0);

    // Median only ever picks a colour that was actually there. Average mixes,
    // which is why it is the wrong default for pixel art.
    auto distinctColors = [](const RasterBuffer& raster) {
        std::set<uint32_t> seen;
        for (uint32_t y = 0; y < raster.height; ++y) {
            for (uint32_t x = 0; x < raster.width; ++x) {
                const Color color = readPixel(raster, static_cast<int32_t>(x),
                                              static_cast<int32_t>(y));
                if (color.a != 0) {
                    seen.insert((static_cast<uint32_t>(color.r) << 16) |
                                (static_cast<uint32_t>(color.g) << 8) | color.b);
                }
            }
        }
        return seen.size();
    };
    LS_CHECK(distinctColors(median) <= 3);
    LS_CHECK(distinctColors(average) > 3);

    // And it is deterministic, like every other policy.
    LS_CHECK(sampled(SamplingPolicy::Median).pixels == median.pixels);
}

} // namespace

int main() {
    testStrokeJoins();
    testStrokeCapsAndSnap();
    testStrokePatterns();
    testGroupsComposite();
    testGroupsSurviveSaveLoad();
    testRoundingPolicies();
    testSamplingPolicies();
    return lstest::report("render");
}
