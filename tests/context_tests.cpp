// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors
// context_tests.cpp — entity CRUD, regions, operations, palettes, patterns,
// anchors, and dirty propagation through the dependency graph.

#include "ls_test.h"

#include <livesprite/livesprite.h>

#include <algorithm>
#include <cmath>

using namespace ls;

namespace {

bool listContains(const std::vector<uint64_t>& list, uint64_t value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

void testEntities(LSContext& ctx) {
    auto doc = ctx.createDocument({"sword", 32, 32});
    LS_CHECK(doc.ok());
    LS_CHECK(ctx.getCanvasSize(doc.value).value.x == 32);
    LS_CHECK(ctx.createDocument({"bad", 0, 32}).fail());

    auto sprite = ctx.createSprite(doc.value);
    LS_CHECK(sprite.ok());
    LS_CHECK(ctx.createSprite(SpriteId::null().valid() ? DocumentId::null() : DocumentId::null()).fail());

    auto blade = ctx.createLayer(sprite.value, {"blade"});
    auto hilt  = ctx.createLayer(sprite.value, {"hilt"});
    LS_CHECK(blade.ok() && hilt.ok());

    auto info = ctx.getSpriteInfo(sprite.value);
    LS_CHECK(info.ok());
    LS_CHECK(info.value.layers.size() == 2);
    LS_CHECK(info.value.layers[0] == blade.value);

    LS_CHECK(ctx.setLayerOrder(sprite.value, {hilt.value, blade.value}).ok());
    LS_CHECK(ctx.getSpriteInfo(sprite.value).value.layers[0] == hilt.value);
    LS_CHECK(ctx.setLayerOrder(sprite.value, {hilt.value}).fail());

    LS_CHECK(ctx.setLayerOpacity(blade.value, 0.5f).ok());
    LS_CHECK(ctx.setLayerOpacity(blade.value, 2.f).fail());
    LS_CHECK(ctx.setLayerBlendMode(blade.value, BlendMode::Multiply).ok());
    LS_CHECK(ctx.getLayerInfo(blade.value).value.blend == BlendMode::Multiply);

    auto group = ctx.createGroup(sprite.value, "metal");
    LS_CHECK(group.ok());
    LS_CHECK(ctx.addLayerToGroup(group.value, blade.value).ok());
    LS_CHECK(ctx.getLayerInfo(blade.value).value.parentId == group.value);
    LS_CHECK(ctx.removeLayerFromGroup(group.value, blade.value).ok());
    LS_CHECK(!ctx.getLayerInfo(blade.value).value.parentId.valid());

    LS_CHECK(ctx.setLayerVisibility(hilt.value, false).ok());
    auto flattened = ctx.flattenLayersForCompile(sprite.value);
    LS_CHECK(flattened.ok() && flattened.value.size() == 1);
    LS_CHECK(ctx.setLayerVisibility(hilt.value, true).ok());

    LS_CHECK(ctx.deleteLayer(hilt.value).ok());
    LS_CHECK(ctx.getLayerInfo(hilt.value).fail());
    LS_CHECK(ctx.getSpriteInfo(sprite.value).value.layers.size() == 1);
}

// A geometry reads back exactly as it was last described -- corner radius
// and all -- and asking for the wrong kind is refused rather than answered.
void testGeometryReadsBack(LSContext& ctx) {
    auto doc = ctx.createDocument({"shapes", 32, 32});
    LS_REQUIRE(doc.ok());
    auto rect = ctx.createRect(doc.value, {{2.f, 3.f}, 10.f, 6.f, 2.5f});
    LS_REQUIRE(rect.ok());
    auto read = ctx.getRect(rect.value);
    LS_REQUIRE(read.ok());
    LS_CHECK(read.value.origin.x == 2.f && read.value.origin.y == 3.f);
    LS_CHECK(read.value.width == 10.f && read.value.height == 6.f);
    LS_CHECK(read.value.cornerRadius == 2.5f);

    LS_CHECK(ctx.updateRect(rect.value, {{4.f, 4.f}, 8.f, 8.f, 1.f}).ok());
    LS_CHECK(ctx.getRect(rect.value).value.cornerRadius == 1.f);
    LS_CHECK(ctx.getEllipse(rect.value).fail());
    LS_CHECK(ctx.getPolyline(rect.value).fail());

    auto oval = ctx.createEllipse(doc.value, {{16.f, 16.f}, 5.f, 3.f});
    LS_REQUIRE(oval.ok());
    LS_CHECK(ctx.getEllipse(oval.value).value.radiusX == 5.f);

    auto line = ctx.createPolyline(doc.value, {{{1.f, 1.f}, {9.f, 4.f}}, false});
    LS_REQUIRE(line.ok());
    auto points = ctx.getPolyline(line.value);
    LS_REQUIRE(points.ok());
    LS_CHECK(points.value.points.size() == 2 && points.value.points[1].x == 9.f);
    LS_CHECK(ctx.getRect(GeometryId::null()).fail());

    auto polygon = ctx.createPolygon(doc.value, {{{1.f, 1.f}, {9.f, 2.f}, {4.f, 8.f}}});
    LS_REQUIRE(polygon.ok());
    auto corners = ctx.getPolygon(polygon.value);
    LS_REQUIRE(corners.ok());
    LS_CHECK(corners.value.vertices.size() == 3 && corners.value.vertices[2].y == 8.f);
    LS_CHECK(ctx.updatePolygon(polygon.value, {{{0.f, 0.f}, {5.f, 0.f}, {5.f, 5.f}, {0.f, 5.f}}}).ok());
    LS_CHECK(ctx.getPolygon(polygon.value).value.vertices.size() == 4);
    LS_CHECK(ctx.getCurve(polygon.value).fail());

    CurveDesc s;
    s.segments.push_back({{0.f, 0.f}, {4.f, 0.f}, {8.f, 4.f}, {8.f, 8.f}});
    s.closed = true;
    auto curve = ctx.createCurve(doc.value, s);
    LS_REQUIRE(curve.ok());
    auto read2 = ctx.getCurve(curve.value);
    LS_REQUIRE(read2.ok());
    LS_CHECK(read2.value.closed && read2.value.segments.size() == 1);
    LS_CHECK(read2.value.segments[0].cp1.x == 8.f && read2.value.segments[0].cp1.y == 4.f);
    LS_CHECK(ctx.getPolygon(curve.value).fail());
}

void testRegions(LSContext& ctx) {
    auto doc = ctx.createDocument({"regions", 32, 32});
    auto rect = ctx.createRect(doc.value, {{4.f, 4.f}, 8.f, 8.f, 0.f});
    LS_CHECK(rect.ok());
    LS_CHECK(ctx.createRect(doc.value, {{0.f, 0.f}, 0.f, 4.f, 0.f}).fail());

    auto region = ctx.createRegionFromGeometry(rect.value);
    LS_CHECK(region.ok());
    LS_CHECK(ctx.getRegionIntervals(region.value).value.intervals.size() == 8);
    LS_CHECK(ctx.getRegionBounds(region.value).value.area == 64.f);
    LS_CHECK(ctx.regionContainsPoint(region.value, {5, 5}).value);
    LS_CHECK(!ctx.regionContainsPoint(region.value, {20, 20}).value);

    // A geometry edit reaches the region it created: source truth flows down.
    LS_CHECK(ctx.updateRect(rect.value, {{4.f, 4.f}, 4.f, 4.f, 0.f}).ok());
    LS_CHECK(ctx.getRegionBounds(region.value).value.area == 16.f);
    LS_CHECK(ctx.isDirty(region.value.value).value);

    auto other = ctx.createRegionFromIntervals(doc.value, [] {
        IntervalSet set;
        for (int32_t y = 6; y < 12; ++y) {
            set.intervals.push_back({y, 6, 12});
        }
        return set;
    }());
    LS_CHECK(other.ok());

    auto merged = ctx.unionRegions(region.value, other.value);
    auto overlap = ctx.intersectRegions(region.value, other.value);
    LS_CHECK(merged.ok() && overlap.ok());
    LS_CHECK(ctx.getRegionBounds(merged.value).value.area == 16.f + 36.f - 4.f);
    LS_CHECK(ctx.getRegionBounds(overlap.value).value.area == 4.f);

    auto grown = ctx.expandRegion(other.value, 1.f);
    LS_CHECK(grown.ok());
    LS_CHECK(ctx.getRegionBounds(grown.value).value.area == 64.f);

    auto split = ctx.splitRegion(merged.value);
    LS_CHECK(split.ok() && split.value.size() == 1);

    auto pixels = ctx.createRegionFromPixels(doc.value, [] {
        PixelRegionDesc desc;
        for (int y = 20; y < 24; ++y) {
            for (int x = 20; x < 24; ++x) {
                if (x == 20 || x == 23 || y == 20 || y == 23) {
                    desc.pixels.push_back({{x, y}, Color::black()});
                }
            }
        }
        return desc;
    }());
    LS_CHECK(pixels.ok());
    LS_CHECK(ctx.getRegionBounds(pixels.value).value.area == 16.f);
    LS_CHECK(!ctx.getRegionBoundaryIntervals(pixels.value).value.empty());

    LS_CHECK(ctx.deleteRegion(other.value).ok());
    LS_CHECK(ctx.getRegionIntervals(other.value).fail());
}

void testOperations(LSContext& ctx) {
    auto doc = ctx.createDocument({"ops", 32, 32});
    auto sprite = ctx.createSprite(doc.value);
    auto layer = ctx.createLayer(sprite.value, {"body"});
    auto rect = ctx.createRect(doc.value, {{8.f, 8.f}, 10.f, 10.f, 0.f});
    auto region = ctx.createRegionFromGeometry(rect.value);

    FillSolidOp fill;
    fill.targetRegion = region.value;
    fill.fallbackColor = {24, 28, 36, 255};
    auto fillOp = ctx.addOperation(layer.value, fill);
    LS_CHECK(fillOp.ok());

    RotateOp rotate;
    rotate.targetLayer = layer.value;
    rotate.angleDegrees = 25.f;
    rotate.pivotFallback = {14.f, 16.f};
    auto rotateOp = ctx.addOperation(layer.value, rotate);
    LS_CHECK(rotateOp.ok());

    auto ops = ctx.getLayerOperations(layer.value);
    LS_CHECK(ops.ok() && ops.value.size() == 2);
    LS_CHECK(ops.value[0].type == "FillSolidOp");
    LS_CHECK(ops.value[1].type == "RotateOp");
    LS_CHECK(ops.value[0].summary.find("region:") != std::string::npos);

    // Reordering keeps the same ids and rejects malformed orders.
    LS_CHECK(ctx.reorderOperations(layer.value, {rotateOp.value, fillOp.value}).ok());
    LS_CHECK(ctx.getLayerOperations(layer.value).value[0].id == rotateOp.value);
    LS_CHECK(ctx.reorderOperations(layer.value, {rotateOp.value}).fail());
    LS_CHECK(ctx.reorderOperations(layer.value, {rotateOp.value, rotateOp.value}).fail());

    // Updating an operation keeps its identity but must keep its type.
    RotateOp turned = rotate;
    turned.angleDegrees = 90.f;
    LS_CHECK(ctx.updateOperation(rotateOp.value, turned).ok());
    LS_CHECK(ctx.updateOperation(rotateOp.value, fill).fail());
    auto stored = ctx.getOperation(rotateOp.value);
    LS_CHECK(stored.ok());
    LS_CHECK(std::get<RotateOp>(stored.value).angleDegrees == 90.f);
    LS_CHECK(operationIsTransform(stored.value));
    LS_CHECK(!operationIsTransform(Operation{fill}));

    // The fill declares the region it reads.
    auto info = ctx.getDependencyInfo(fillOp.value.value);
    LS_CHECK(info.ok());
    LS_CHECK(listContains(info.value.dependencies, region.value.value));

    LS_CHECK(ctx.removeOperation(layer.value, fillOp.value).ok());
    LS_CHECK(ctx.getOperation(fillOp.value).fail());
    LS_CHECK(ctx.getLayerOperations(layer.value).value.size() == 1);

    // Inserting at an index puts the operation where it was asked to go.
    auto inserted = ctx.addOperation(layer.value, fill, 0);
    LS_CHECK(inserted.ok());
    LS_CHECK(ctx.getLayerOperations(layer.value).value[0].id == inserted.value);
}

void testPalettesAndPatterns(LSContext& ctx) {
    auto doc = ctx.createDocument({"palette", 32, 32});
    auto sprite = ctx.createSprite(doc.value);

    PaletteDesc desc;
    desc.name = "steel";
    desc.entries = {
        {0, {20, 24, 32, 255}, "shadow"},
        {1, {120, 130, 150, 255}, "base"},
        {2, {220, 230, 245, 255}, "light"}
    };
    auto palette = ctx.createPalette(doc.value, desc);
    LS_CHECK(palette.ok());
    LS_CHECK(ctx.getPaletteEntries(palette.value).value.size() == 3);
    LS_CHECK(ctx.resolveSemanticColor(palette.value, 1).value == Color{120, 130, 150, 255});
    LS_CHECK(ctx.resolveSemanticColor(palette.value, 9).fail());
    LS_CHECK(ctx.nearestPaletteColor(palette.value, {215, 225, 240, 255}).value == 2u);

    // A document palette is inherited until a sprite binds its own.
    LS_CHECK(ctx.getEffectivePalette(sprite.value).value == palette.value);
    auto alternate = ctx.createPalette(doc.value, {"rust", {{0, {60, 30, 20, 255}, ""}}});
    LS_CHECK(ctx.bindSpritePalette(sprite.value, alternate.value).ok());
    LS_CHECK(ctx.getEffectivePalette(sprite.value).value == alternate.value);
    LS_CHECK(ctx.isDirty(sprite.value.value).value);

    LS_CHECK(ctx.setPaletteColor(palette.value, 1, {130, 140, 160, 255}).ok());
    LS_CHECK(ctx.resolveSemanticColor(palette.value, 1).value == Color{130, 140, 160, 255});

    auto ramp = ctx.createRamp(doc.value, {"steel_ramp", {{0.f, {0, 0, 0, 255}}, {1.f, {255, 255, 255, 255}}}, true});
    LS_CHECK(ramp.ok());
    LS_CHECK(ctx.sampleRamp(ramp.value, 0.f).value == Color{0, 0, 0, 255});
    LS_CHECK(ctx.sampleRamp(ramp.value, 1.f).value == Color{255, 255, 255, 255});
    const Color middle = ctx.sampleRamp(ramp.value, 0.5f).value;
    LS_CHECK(middle.r > 120 && middle.r < 135);
    LS_CHECK(ctx.createRamp(doc.value, {"empty", {}, true}).fail());

    auto bayer = ctx.createOrderedDitherPattern(doc.value, 4);
    LS_CHECK(bayer.ok());
    auto tile = ctx.getPattern(bayer.value);
    LS_CHECK(tile.ok());
    LS_CHECK(tile.value.tileWidth == 4 && tile.value.mask.size() == 16);
    LS_CHECK(tile.value.levels == 16);
    LS_CHECK(tile.value.mask[0] == 0);
    // Every threshold rank appears exactly once in a Bayer matrix.
    std::vector<uint8_t> sorted = tile.value.mask;
    std::sort(sorted.begin(), sorted.end());
    bool ranksUnique = true;
    for (size_t i = 0; i < sorted.size(); ++i) {
        ranksUnique = ranksUnique && sorted[i] == static_cast<uint8_t>(i);
    }
    LS_CHECK(ranksUnique);
    LS_CHECK(ctx.createOrderedDitherPattern(doc.value, 3).fail());

    LS_CHECK(ctx.createCheckerDitherPattern(doc.value, 1).ok());
    LS_CHECK(ctx.createLineDitherPattern(doc.value, 45.f, 4.f).ok());
    LS_CHECK(ctx.createSeededNoiseDitherPattern(doc.value, 17, 8).ok());
    LS_CHECK(ctx.setPatternCoordinateSpace(bayer.value, CoordinateSpace::Canvas).ok());
    LS_CHECK(ctx.getPattern(bayer.value).value.coordinateSpace == CoordinateSpace::Canvas);
    LS_CHECK(ctx.setPatternDensity(bayer.value, 1.5f).fail());
}

void testAnchors(LSContext& ctx) {
    auto doc = ctx.createDocument({"anchors", 32, 32});
    auto sprite = ctx.createSprite(doc.value);

    auto pivot = ctx.createPivot(sprite.value, {16.f, 16.f});
    LS_CHECK(pivot.ok());
    LS_CHECK(ctx.getSpriteInfo(sprite.value).value.pivot == pivot.value);
    LS_CHECK(ctx.movePivot(pivot.value, {1.f, -1.f}).ok());
    LS_CHECK(ctx.getPivot(pivot.value).value.x == 17.f);

    auto socket = ctx.addSocket(sprite.value, {"grip", {10.f, 20.f}, 90.f});
    LS_CHECK(socket.ok());
    auto transform = ctx.getSocketTransform(socket.value);
    LS_CHECK(transform.ok());
    const Vec2f mapped = transform.value.transformPoint({1.f, 0.f});
    LS_CHECK(std::fabs(mapped.x - 10.f) < 0.001f);
    LS_CHECK(std::fabs(mapped.y - 21.f) < 0.001f);

    auto attachment = ctx.resolveSocketAttachment(socket.value, pivot.value);
    LS_CHECK(attachment.ok());
    const Vec2f attached = attachment.value.transformPoint({17.f, 15.f});
    LS_CHECK(std::fabs(attached.x - 10.f) < 0.001f);
    LS_CHECK(std::fabs(attached.y - 20.f) < 0.001f);

    auto shape = ctx.createEllipse(doc.value, {{16.f, 16.f}, 8.f, 8.f});
    auto boundary = ctx.createBoundary(sprite.value, {"squash_zone", shape.value, Falloff::Linear, 4.f});
    LS_CHECK(boundary.ok());
    LS_CHECK(ctx.getBoundaryInfluence(boundary.value, {16.f, 16.f}).value > 0.9f);
    LS_CHECK(ctx.getBoundaryInfluence(boundary.value, {30.f, 30.f}).value == 0.f);
    LS_CHECK(!ctx.compileBoundaryMask(boundary.value, {}).value.empty());
    LS_CHECK(ctx.exportBoundaryShape(boundary.value).value == shape.value);
}

void testDependencies(LSContext& ctx) {
    auto doc = ctx.createDocument({"deps", 32, 32});
    auto sprite = ctx.createSprite(doc.value);
    auto layer = ctx.createLayer(sprite.value, {"body"});
    auto rect = ctx.createRect(doc.value, {{4.f, 4.f}, 8.f, 8.f, 0.f});
    auto region = ctx.createRegionFromGeometry(rect.value);

    FillSolidOp fill;
    fill.targetRegion = region.value;
    fill.fallbackColor = Color::white();
    auto fillOp = ctx.addOperation(layer.value, fill);
    LS_CHECK(fillOp.ok());

    // Editing the geometry must dirty the whole chain up to the sprite.
    LS_CHECK(ctx.updateRect(rect.value, {{4.f, 4.f}, 10.f, 10.f, 0.f}).ok());
    LS_CHECK(ctx.isDirty(region.value.value).value);
    LS_CHECK(ctx.isDirty(fillOp.value.value).value);
    LS_CHECK(ctx.isDirty(layer.value.value).value);
    LS_CHECK(ctx.isDirty(sprite.value.value).value);

    auto regionInfo = ctx.getDependencyInfo(region.value.value);
    LS_CHECK(regionInfo.ok());
    LS_CHECK(listContains(regionInfo.value.dependencies, rect.value.value));
    LS_CHECK(listContains(regionInfo.value.dependents, fillOp.value.value));

    LS_CHECK(ctx.cacheOperationResult(fillOp.value, RasterBuffer{}).ok());
    LS_CHECK(ctx.cacheStats().entries >= 1);
    LS_CHECK(ctx.clearCache(doc.value).ok());
    LS_CHECK(ctx.markDirty(0).fail());
}

} // namespace

// A Source layer is a reference: a preview draws it, an export does not, and
// it survives a save as what it is.
void testReferenceLayers(LSContext& ctx) {
    auto doc = ctx.createDocument({"reference", 16, 16});
    auto sprite = ctx.createSprite(doc.value);
    auto art = ctx.createLayer(sprite.value, {"art"});
    auto guide = ctx.createLayer(sprite.value, {"guide"});
    auto left = ctx.createRegionFromGeometry(ctx.createRect(doc.value, {{0.f, 0.f}, 4.f, 4.f, 0.f}).value);
    auto right = ctx.createRegionFromGeometry(ctx.createRect(doc.value, {{8.f, 8.f}, 4.f, 4.f, 0.f}).value);
    FillSolidOp fill;
    fill.targetRegion = left.value;
    fill.fallbackColor = {200, 40, 40, 255};
    LS_CHECK(ctx.addOperation(art.value, fill).ok());
    fill.targetRegion = right.value;
    fill.fallbackColor = {40, 40, 200, 255};
    LS_CHECK(ctx.addOperation(guide.value, fill).ok());
    LS_CHECK(ctx.setLayerType(guide.value, LayerType::Source).ok());
    LS_CHECK(ctx.getLayerInfo(guide.value).value.type == LayerType::Source);
    LS_CHECK(ctx.setLayerType(LayerId::null(), LayerType::Source).fail());

    CompileProfile preview;
    preview.type = CompileProfileType::Preview;
    preview.outputWidth = 16;
    preview.outputHeight = 16;
    CompileProfile exporting = preview;
    exporting.type = CompileProfileType::Export;
    auto shown = ctx.compileSprite(sprite.value, preview);
    auto shipped = ctx.compileSprite(sprite.value, exporting);
    LS_REQUIRE(shown.ok() && shipped.ok());
    LS_CHECK(readPixel(shown.value.raster, 9, 9).a != 0);
    LS_CHECK(readPixel(shipped.value.raster, 9, 9).a == 0);
    LS_CHECK(readPixel(shipped.value.raster, 1, 1).a != 0);

    // Back to a drawing layer, it ships again.
    LS_CHECK(ctx.setLayerType(guide.value, LayerType::Drawing).ok());
    auto again = ctx.compileSprite(sprite.value, exporting);
    LS_REQUIRE(again.ok());
    LS_CHECK(readPixel(again.value.raster, 9, 9).a != 0);
}

// An erase: a region cleared from what the layer has drawn so far, with the
// shape under it still a shape -- widen it and the erased patch stays put --
// and a later fill not erased.
void testClearRegion(LSContext& ctx) {
    auto doc = ctx.createDocument({"erase", 16, 16});
    auto sprite = ctx.createSprite(doc.value);
    auto layer = ctx.createLayer(sprite.value, {"figure"});
    auto rect = ctx.createRect(doc.value, {{2.f, 2.f}, 6.f, 6.f, 0.f});
    auto region = ctx.createRegionFromGeometry(rect.value);
    FillSolidOp fill;
    fill.targetRegion = region.value;
    fill.fallbackColor = {200, 60, 60, 255};
    LS_CHECK(ctx.addOperation(layer.value, fill).ok());
    const auto at = [](std::vector<Vec2i> points) {
        PixelRegionDesc desc;
        for (Vec2i p : points) {
            desc.pixels.push_back({p, Color::black()});
        }
        return desc;
    };
    auto hole = ctx.createRegionFromPixels(doc.value, at({{4, 4}, {5, 4}}));
    LS_REQUIRE(hole.ok());
    ClearRegionOp clear;
    clear.targetRegion = hole.value;
    auto cleared = ctx.addOperation(layer.value, clear);
    LS_REQUIRE(cleared.ok());
    LS_CHECK(ctx.getLayerOperations(layer.value).value[1].type == "ClearRegionOp");

    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = 16;
    profile.outputHeight = 16;
    auto compiled = ctx.compileSprite(sprite.value, profile);
    LS_REQUIRE(compiled.ok());
    LS_CHECK(readPixel(compiled.value.raster, 4, 4).a == 0);
    LS_CHECK(readPixel(compiled.value.raster, 5, 4).a == 0);
    LS_CHECK(readPixel(compiled.value.raster, 6, 4).a == 255);
    LS_CHECK(readPixel(compiled.value.raster, 3, 3).a == 255);

    // The erased region grows, and the picture follows.
    LS_CHECK(ctx.addPixelsToRegion(hole.value, at({{6, 4}})).ok());
    compiled = ctx.compileSprite(sprite.value, profile);
    LS_REQUIRE(compiled.ok());
    LS_CHECK(readPixel(compiled.value.raster, 6, 4).a == 0);

    // A fill added after the erase is not erased.
    FillSolidOp over;
    over.targetRegion = ctx.createRegionFromPixels(doc.value, at({{4, 4}})).value;
    over.fallbackColor = {20, 200, 60, 255};
    LS_CHECK(ctx.addOperation(layer.value, over).ok());
    compiled = ctx.compileSprite(sprite.value, profile);
    LS_REQUIRE(compiled.ok());
    LS_CHECK(readPixel(compiled.value.raster, 4, 4).g == 200);
    LS_CHECK(readPixel(compiled.value.raster, 5, 4).a == 0);
}

// A fade: what the layer drew before it, more transparent; what comes after
// it is not faded.
void testFade(LSContext& ctx) {
    auto doc = ctx.createDocument({"fade", 8, 8});
    auto sprite = ctx.createSprite(doc.value);
    auto layer = ctx.createLayer(sprite.value, {"cel"});
    PixelRegionDesc two;
    two.pixels = { {{1, 1}, Color::black()}, {{2, 1}, Color::black()} };
    FillSolidOp fill;
    fill.targetRegion = ctx.createRegionFromPixels(doc.value, two).value;
    fill.fallbackColor = {200, 40, 40, 255};
    LS_REQUIRE(ctx.addOperation(layer.value, fill).ok());
    FadeOp fade;
    fade.opacity = 0.5f;
    auto faded = ctx.addOperation(layer.value, fade);
    LS_REQUIRE(faded.ok());
    PixelRegionDesc one;
    one.pixels = { {{2, 1}, Color::black()} };
    FillSolidOp after;
    after.targetRegion = ctx.createRegionFromPixels(doc.value, one).value;
    after.fallbackColor = {40, 200, 40, 255};
    LS_REQUIRE(ctx.addOperation(layer.value, after).ok());

    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = 8;
    profile.outputHeight = 8;
    profile.alpha = AlphaPolicy::Preserve;      // the default would threshold it away
    auto compiled = ctx.compileSprite(sprite.value, profile);
    LS_REQUIRE(compiled.ok());
    LS_CHECK(readPixel(compiled.value.raster, 1, 1).a >= 126 && readPixel(compiled.value.raster, 1, 1).a <= 129);
    LS_CHECK(readPixel(compiled.value.raster, 2, 1).a == 255 && readPixel(compiled.value.raster, 2, 1).g == 200);

    // Driven by its parameter, like any other.
    LS_REQUIRE(ctx.setOperationParameter(faded.value, "opacity", ParameterValue{1.f}).ok());
    compiled = ctx.compileSprite(sprite.value, profile);
    LS_REQUIRE(compiled.ok());
    LS_CHECK(readPixel(compiled.value.raster, 1, 1).a == 255);
}

// A tilemap: cells drawn from a tileset's layers, turned as each cell says;
// editing a tile redraws every cell naming it; a snapshot holds the grid; a
// cloned layer gets a grid of its own; saving and loading keep it all.
void testTilemap(LSContext& ctx) {
    auto doc = ctx.createDocument({"tiles", 16, 8});
    auto tileset = ctx.createSprite(doc.value);
    auto frame = ctx.createSprite(doc.value);
    const auto dot = [&](LayerId layer, Vec2i at, Color colour) {
        PixelRegionDesc desc;
        desc.pixels.push_back({at, colour});
        FillSolidOp fill;
        fill.targetRegion = ctx.createRegionFromPixels(doc.value, desc).value;
        fill.fallbackColor = colour;
        return ctx.addOperation(layer, fill).ok();
    };
    const Color red {200, 30, 30, 255};
    const Color blue {30, 30, 200, 255};
    const Color green {30, 200, 30, 255};
    auto t1 = ctx.createLayer(tileset.value, {"t1"});
    auto t2 = ctx.createLayer(tileset.value, {"t2"});
    LS_REQUIRE(dot(t1.value, {0, 0}, red));
    LS_REQUIRE(dot(t2.value, {3, 0}, blue));
    LS_REQUIRE(dot(t2.value, {1, 0}, green));

    TilemapDesc grid;
    grid.columns = 4;
    grid.rows = 2;
    grid.tileWidth = 4;
    grid.tileHeight = 4;
    grid.cells = { 1, 2 | kTileFlipX, 0, 2 | kTileFlipD,
                   0, 0, 0, 0 };
    auto map = ctx.createTilemap(doc.value, grid);
    LS_REQUIRE(map.ok());
    auto layer = ctx.createLayer(frame.value, {"map"});
    DrawTilemapOp draw;
    draw.tilemap = map.value;
    draw.tileset = tileset.value;
    LS_REQUIRE(ctx.addOperation(layer.value, draw).ok());

    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = 16;
    profile.outputHeight = 8;
    const auto at = [&](int x, int y) {
        auto compiled = ctx.compileSprite(frame.value, profile);
        return compiled.ok() ? readPixel(compiled.value.raster, x, y) : Color{};
    };
    LS_CHECK(at(0, 0).r == 200);                          // tile 1 as drawn
    LS_CHECK(at(4, 0).b == 200 && at(6, 0).g == 200);     // tile 2 mirrored: blue 3 -> 0, green 1 -> 2
    LS_CHECK(at(7, 0).a == 0);
    LS_CHECK(at(8, 0).a == 0);                            // an empty cell
    LS_CHECK(at(12, 3).b == 200 && at(12, 1).g == 200);   // diagonal: (3,0) -> (0,3), (1,0) -> (0,1)

    // One cell set; a tile edited redraws every cell that names it.
    LS_REQUIRE(ctx.setTilemapCell(map.value, 2, 0, 1).ok());
    LS_CHECK(at(8, 0).r == 200);
    LS_REQUIRE(dot(t1.value, {1, 1}, green));
    LS_CHECK(at(1, 1).g == 200 && at(9, 1).g == 200);

    // A snapshot holds the grid.
    auto before = ctx.snapshotDocumentState(doc.value);
    LS_REQUIRE(before.ok());
    LS_REQUIRE(ctx.setTilemapCell(map.value, 0, 0, 0).ok());
    LS_CHECK(at(0, 0).a == 0);
    LS_REQUIRE(ctx.restoreDocumentState(doc.value, before.value).ok());
    LS_CHECK(at(0, 0).r == 200);

    // A clone has its own grid: changing it leaves the original.
    auto copy = ctx.cloneLayer(layer.value, frame.value);
    LS_REQUIRE(copy.ok());
    auto copyOps = ctx.getLayerOperations(copy.value);
    LS_REQUIRE(copyOps.ok() && copyOps.value.size() == 1);
    auto copyMap = ctx.getOperationParameter(copyOps.value[0].id, "tilemap");
    const uint64_t* copyId = copyMap.ok() ? std::get_if<uint64_t>(&copyMap.value) : nullptr;
    LS_REQUIRE(copyId != nullptr && *copyId != map.value.value);
    LS_REQUIRE(ctx.setTilemapCell(TilemapId{*copyId}, 3, 1, 1).ok());
    auto original = ctx.getTilemap(map.value);
    LS_CHECK(original.ok() && original.value.cells[7] == 0);

    // Saved and loaded: the same picture.
    const Color beforeSave = at(12, 3);
    auto saved = ctx.serializeDocument(doc.value);
    LS_REQUIRE(saved.ok());
    auto loaded = ctx.deserializeDocument(saved.value);
    LS_REQUIRE(loaded.ok());
    auto info = ctx.getDocumentInfo(loaded.value);
    LS_REQUIRE(info.ok() && info.value.sprites.size() == 2);
    auto reloaded = ctx.compileSprite(info.value.sprites[1], profile);
    LS_REQUIRE(reloaded.ok());
    LS_CHECK(readPixel(reloaded.value.raster, 0, 0).r == 200);
    LS_CHECK(readPixel(reloaded.value.raster, 12, 3).b == beforeSave.b);
    LS_CHECK(readPixel(reloaded.value.raster, 12, 4).r == 200);   // the clone's cell
}

// A drop shadow: the layer's own drawing, moved and in one colour, only where
// the layer draws nothing -- following the drawing when it changes.
void testDropShadow(LSContext& ctx) {
    auto doc = ctx.createDocument({"shadow", 16, 16});
    auto sprite = ctx.createSprite(doc.value);
    auto layer = ctx.createLayer(sprite.value, {"figure"});
    auto rect = ctx.createRect(doc.value, {{2.f, 2.f}, 2.f, 2.f, 0.f});
    auto region = ctx.createRegionFromGeometry(rect.value);
    FillSolidOp fill;
    fill.targetRegion = region.value;
    fill.fallbackColor = {200, 60, 60, 255};
    LS_CHECK(ctx.addOperation(layer.value, fill).ok());
    GenerateDropShadowOp shadow;
    shadow.offset = {2.f, 1.f};
    shadow.fallbackColor = {0, 0, 0, 255};
    shadow.opacity = 1.f;
    auto shadowOp = ctx.addOperation(layer.value, shadow);
    LS_REQUIRE(shadowOp.ok());
    LS_CHECK(ctx.getLayerOperations(layer.value).value[1].type == "GenerateDropShadowOp");

    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = 16;
    profile.outputHeight = 16;
    auto compiled = ctx.compileSprite(sprite.value, profile);
    LS_REQUIRE(compiled.ok());
    const RasterBuffer& r = compiled.value.raster;
    LS_CHECK(readPixel(r, 2, 2).r == 200);           // the drawing, untouched
    LS_CHECK(readPixel(r, 3, 3).r == 200);
    LS_CHECK(readPixel(r, 5, 4).a == 255 && readPixel(r, 5, 4).r == 0);   // the shadow
    LS_CHECK(readPixel(r, 4, 3).a == 255 && readPixel(r, 4, 3).r == 0);
    LS_CHECK(readPixel(r, 6, 4).a == 0);

    // Its parameters are reachable like any operation's.
    LS_CHECK(ctx.setOperationParameter(shadowOp.value, "offset", ParameterValue{Vec2f{-1.f, 0.f}}).ok());
    auto moved = ctx.compileSprite(sprite.value, profile);
    LS_REQUIRE(moved.ok());
    LS_CHECK(readPixel(moved.value.raster, 1, 2).a == 255 && readPixel(moved.value.raster, 1, 2).r == 0);
    LS_CHECK(readPixel(moved.value.raster, 5, 4).a == 0);
}

// RotSprite: a rotated line stays a line of the source's own colour, with no
// colour that was not in the source and no hole where plain nearest sampling
// can leave one.
void testRotSprite(LSContext& ctx) {
    auto doc = ctx.createDocument({"rotsprite", 32, 32});
    auto sprite = ctx.createSprite(doc.value);
    auto layer = ctx.createLayer(sprite.value, {"bar"});
    auto rect = ctx.createRect(doc.value, {{6.f, 14.f}, 20.f, 3.f, 0.f});
    auto region = ctx.createRegionFromGeometry(rect.value);
    FillSolidOp fill;
    fill.targetRegion = region.value;
    fill.fallbackColor = {200, 60, 60, 255};
    LS_CHECK(ctx.addOperation(layer.value, fill).ok());
    RotateOp rotate;
    rotate.targetLayer = layer.value;
    rotate.angleDegrees = 30.f;
    rotate.pivotFallback = {16.f, 16.f};
    rotate.sampling = SamplingPolicy::RotSprite;
    LS_REQUIRE(ctx.addOperation(layer.value, rotate).ok());
    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = 32;
    profile.outputHeight = 32;
    auto compiled = ctx.compileSprite(sprite.value, profile);
    LS_REQUIRE(compiled.ok());
    int drawn = 0;
    bool foreign = false;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const Color c = readPixel(compiled.value.raster, x, y);
            if (c.a == 0) {
                continue;
            }
            ++drawn;
            foreign = foreign || c.r != 200 || c.g != 60 || c.b != 60;
        }
    }
    LS_CHECK(!foreign);
    // About the area it had: 60 pixels, give or take its edges.
    LS_CHECK(drawn > 45 && drawn < 80);
    LS_CHECK(readPixel(compiled.value.raster, 16, 16).a != 0);     // the middle stays
}

// A closed outline one pixel wide, a wobbly blob like one drawn by hand,
// turned by RotSprite at many angles: it stays closed -- nothing outside it
// reaches the middle four ways -- and a fill inside it stays inside, the
// outline drawn over the fill wherever the two meet.
void testRotatedOutlinesStayClosed(LSContext& ctx) {
    constexpr int kSize = 64;
    constexpr int kMid = 32;
    // The outline: a polar wobble, joined point to point.
    std::vector<Vec2i> ring;
    Vec2i last { -1, -1 };
    for (int i = 0; i <= 720; ++i) {
        const float t = static_cast<float>(i) / 720.f * 6.2831853f;
        const float r = 16.f + 5.f * std::sin(5.f * t) + 2.f * std::cos(3.f * t);
        const Vec2i p { static_cast<int32_t>(std::lround(kMid + r * std::cos(t))),
                        static_cast<int32_t>(std::lround(kMid + r * std::sin(t))) };
        if (p.x == last.x && p.y == last.y) {
            continue;
        }
        if (last.x >= 0) {
            // Bresenham from the last point, so the loop is 8-connected.
            int32_t x0 = last.x, y0 = last.y;
            const int32_t dx = std::abs(p.x - x0), dy = -std::abs(p.y - y0);
            const int32_t sx = x0 < p.x ? 1 : -1, sy = y0 < p.y ? 1 : -1;
            int32_t err = dx + dy;
            while (x0 != p.x || y0 != p.y) {
                const int32_t e2 = 2 * err;
                if (e2 >= dy) { err += dy; x0 += sx; }
                if (e2 <= dx) { err += dx; y0 += sy; }
                ring.push_back({x0, y0});
            }
        } else {
            ring.push_back(p);
        }
        last = p;
    }
    const Color orange {230, 140, 60, 255};
    const Color blue {30, 70, 110, 255};
    // The inside: everything the outline encloses, found by flooding from the
    // border.
    std::vector<uint8_t> wall(kSize * kSize, 0);
    for (Vec2i p : ring) { wall[p.y * kSize + p.x] = 1; }
    const auto outsideOf = [](const std::vector<uint8_t>& blocked, int size) {
        std::vector<uint8_t> outside(size * size, 0);
        std::vector<Vec2i> stack;
        for (int i = 0; i < size; ++i) {
            stack.push_back({i, 0}); stack.push_back({i, size - 1});
            stack.push_back({0, i}); stack.push_back({size - 1, i});
        }
        while (!stack.empty()) {
            const Vec2i p = stack.back();
            stack.pop_back();
            if (p.x < 0 || p.y < 0 || p.x >= size || p.y >= size) { continue; }
            const int at = p.y * size + p.x;
            if (outside[at] || blocked[at]) { continue; }
            outside[at] = 1;
            stack.push_back({p.x + 1, p.y}); stack.push_back({p.x - 1, p.y});
            stack.push_back({p.x, p.y + 1}); stack.push_back({p.x, p.y - 1});
        }
        return outside;
    };
    const std::vector<uint8_t> outsideBefore = outsideOf(wall, kSize);
    PixelRegionDesc inside;
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            if (!outsideBefore[y * kSize + x] && !wall[y * kSize + x]) {
                inside.pixels.push_back({{x, y}, Color::black()});
            }
        }
    }
    PixelRegionDesc outline;
    for (Vec2i p : ring) { outline.pixels.push_back({p, Color::black()}); }
    outline.closeSameColorBoundaries = false;
    inside.closeSameColorBoundaries = false;

    for (int filled = 0; filled < 2; ++filled) {
        auto doc = ctx.createDocument({"blob", kSize, kSize});
        auto sprite = ctx.createSprite(doc.value);
        auto layer = ctx.createLayer(sprite.value, {"blob"});
        if (filled != 0) {
            FillSolidOp fill;
            fill.targetRegion = ctx.createRegionFromPixels(doc.value, inside).value;
            fill.fallbackColor = blue;
            LS_REQUIRE(ctx.addOperation(layer.value, fill).ok());
        }
        FillSolidOp line;
        line.targetRegion = ctx.createRegionFromPixels(doc.value, outline).value;
        line.fallbackColor = orange;
        LS_REQUIRE(ctx.addOperation(layer.value, line).ok());
        RotateOp rotate;
        rotate.targetLayer = layer.value;
        rotate.pivotFallback = {static_cast<float>(kMid), static_cast<float>(kMid)};
        rotate.sampling = SamplingPolicy::RotSprite;
        auto rotation = ctx.addOperation(layer.value, rotate);
        LS_REQUIRE(rotation.ok());
        CompileProfile profile;
        profile.type = CompileProfileType::Export;
        profile.outputWidth = kSize;
        profile.outputHeight = kSize;
        int broken = 0;
        int leaked = 0;
        for (int angle = 3; angle < 360; angle += 7) {
            LS_REQUIRE(ctx.setOperationParameter(rotation.value, "angleDegrees",
                                                 ParameterValue{static_cast<float>(angle)}).ok());
            auto compiled = ctx.compileSprite(sprite.value, profile);
            LS_REQUIRE(compiled.ok());
            std::vector<uint8_t> blocked(kSize * kSize, 0);
            for (int y = 0; y < kSize; ++y) {
                for (int x = 0; x < kSize; ++x) {
                    const Color c = readPixel(compiled.value.raster, x, y);
                    blocked[y * kSize + x] = c.a != 0 && c.r == orange.r ? 1 : 0;
                }
            }
            const std::vector<uint8_t> outside = outsideOf(blocked, kSize);
            broken += outside[kMid * kSize + kMid] ? 1 : 0;
            for (int y = 0; y < kSize; ++y) {
                for (int x = 0; x < kSize; ++x) {
                    const Color c = readPixel(compiled.value.raster, x, y);
                    leaked += (outside[y * kSize + x] && c.a != 0 && c.b == blue.b) ? 1 : 0;
                }
            }
        }
        LS_CHECK(broken == 0);
        LS_CHECK(leaked == 0);
        if (broken != 0 || leaked != 0) {
            std::printf("    %s: %d angles broken open, %d fill pixels outside\n",
                        filled != 0 ? "filled" : "outline", broken, leaked);
        }
    }
}

int main() {
    auto ctx = LSContext::create();
    LS_REQUIRE_MAIN(ctx != nullptr);
    LS_CHECK(ctx->engineVersion() == LS_ENGINE_VERSION);

    testEntities(*ctx);
    testGeometryReadsBack(*ctx);
    testRegions(*ctx);
    testOperations(*ctx);
    testPalettesAndPatterns(*ctx);
    testAnchors(*ctx);
    testDependencies(*ctx);
    testReferenceLayers(*ctx);
    testDropShadow(*ctx);
    testClearRegion(*ctx);
    testTilemap(*ctx);
    testFade(*ctx);
    testRotSprite(*ctx);
    testRotatedOutlinesStayClosed(*ctx);

    return lstest::report("context");
}
