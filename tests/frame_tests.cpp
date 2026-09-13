// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors

// frame_tests.cpp -- what an application needs before it can hold frames.
//
// A document already holds many sprites, so an animation frame is a sprite and
// the engine needs no new concept for one. What it does need is for the central
// gesture of animation -- duplicate the last frame, then change one thing -- to
// leave two frames rather than two views of one.
//
// That gesture is cloneSprite, and it is the only engine call a timeline cannot
// work without.

#include "ls_test.h"

#include <livesprite/livesprite.h>

#include <memory>
#include <variant>
#include <vector>
#include <algorithm>

using namespace ls;

namespace {

struct Fixture {
    std::unique_ptr<LSContext> engine = LSContext::create();
    DocumentId doc;
    SpriteId   sprite;
    GeometryId geometry;
    RegionId   region;
    LayerId    layer;
    OperationId fill;

    bool build() {
        auto document = engine->createDocument({ "frames", 16, 16 });
        if (document.fail()) { return false; }
        doc = document.value;

        auto made0 = engine->createSprite(doc);
        if (made0.fail()) { return false; }
        sprite = made0.value;

        auto rect = engine->createRect(doc, { { 2.f, 2.f }, 6.f, 6.f, 0.f });
        if (rect.fail()) { return false; }
        geometry = rect.value;

        auto made = engine->createRegionFromGeometry(geometry);
        if (made.fail()) { return false; }
        region = made.value;

        auto made2 = engine->createLayer(sprite, { "Body" });
        if (made2.fail()) { return false; }
        layer = made2.value;

        FillSolidOp op;
        op.targetRegion = region;
        op.fallbackColor = { 255, 160, 40, 255 };
        auto added = engine->addOperation(layer, op);
        if (added.fail()) { return false; }
        fill = added.value;
        return true;
    }

    RasterBuffer render(SpriteId which) {
        CompileProfile profile;
        profile.type = CompileProfileType::Export;
        profile.outputWidth = 16;
        profile.outputHeight = 16;
        profile.palette = PalettePolicy::Unconstrained;
        auto compiled = engine->compileSprite(which, profile);
        return compiled.ok() ? compiled.value.raster : RasterBuffer{};
    }
};

bool sameBytes(const RasterBuffer& a, const RasterBuffer& b) {
    return a.width == b.width && a.height == b.height && a.pixels == b.pixels;
}

// The one that matters. Duplicate a frame, move the shape in the copy, and the
// original must not move with it.
void testACloneIsItsOwnFrame() {
    Fixture f;
    LS_REQUIRE(f.build());

    auto clone = f.engine->cloneSprite(f.sprite);
    LS_REQUIRE(clone.ok());

    const RasterBuffer before = f.render(f.sprite);
    LS_REQUIRE(!before.empty());
    LS_CHECK(sameBytes(before, f.render(clone.value)));   // a copy starts identical

    // Now find the clone's own fill and move what it draws.
    auto layers = f.engine->getSpriteInfo(clone.value);
    LS_REQUIRE(layers.ok() && layers.value.layers.size() == 1);
    auto operations = f.engine->getLayerOperations(layers.value.layers.front());
    LS_REQUIRE(operations.ok() && operations.value.size() == 1);

    auto target = f.engine->getOperationParameter(operations.value.front().id, "targetRegion");
    LS_REQUIRE(target.ok());
    const uint64_t clonedRegion = std::get<uint64_t>(target.value);

    // A cloned frame must not be pointing at the original frame's region.
    LS_CHECK(clonedRegion != f.region.value);

    // And editing it must leave the original alone, which is the whole promise.
    auto geometryOf = f.engine->getRegionSourceGeometry(RegionId{ clonedRegion });
    LS_REQUIRE(geometryOf.ok());
    LS_CHECK(geometryOf.value.value != f.geometry.value);

    LS_REQUIRE(f.engine->updateRect(geometryOf.value,
                                    { { 8.f, 8.f }, 6.f, 6.f, 0.f }).ok());

    LS_CHECK(sameBytes(before, f.render(f.sprite)));           // untouched
    LS_CHECK(!sameBytes(before, f.render(clone.value)));       // and the copy moved
}

// A layer copied on its own: same picture, its own drawing, placed where it
// was asked for, and at home in another frame of the same document.
void testAClonedLayerIsItsOwn() {
    Fixture f;
    LS_REQUIRE(f.build());
    const RasterBuffer before = f.render(f.sprite);

    // Right above the original: index 1 of what becomes two.
    auto copy = f.engine->cloneLayer(f.layer, f.sprite, 1);
    LS_REQUIRE(copy.ok());
    auto info = f.engine->getSpriteInfo(f.sprite);
    LS_REQUIRE(info.ok() && info.value.layers.size() == 2);
    LS_CHECK(info.value.layers[0] == f.layer && info.value.layers[1] == copy.value);
    LS_CHECK(sameBytes(before, f.render(f.sprite)));      // covers the same pixels exactly

    // Its own region and geometry.
    auto operations = f.engine->getLayerOperations(copy.value);
    LS_REQUIRE(operations.ok() && operations.value.size() == 1);
    auto target = f.engine->getOperationParameter(operations.value.front().id, "targetRegion");
    LS_REQUIRE(target.ok());
    const RegionId copiedRegion{ std::get<uint64_t>(target.value) };
    LS_CHECK(copiedRegion != f.region);
    auto geometryOf = f.engine->getRegionSourceGeometry(copiedRegion);
    LS_REQUIRE(geometryOf.ok());
    LS_REQUIRE(f.engine->updateRect(geometryOf.value, { { 8.f, 8.f }, 6.f, 6.f, 0.f }).ok());
    LS_CHECK(!sameBytes(before, f.render(f.sprite)));
    LS_REQUIRE(f.engine->setLayerVisibility(copy.value, false).ok());
    LS_CHECK(sameBytes(before, f.render(f.sprite)));      // the original is untouched

    // Into another frame, at the bottom of its stack.
    auto other = f.engine->createSprite(f.doc);
    LS_REQUIRE(other.ok());
    auto moved = f.engine->cloneLayer(f.layer, other.value, 0);
    LS_REQUIRE(moved.ok());
    LS_CHECK(sameBytes(before, f.render(other.value)));
    auto ownerOf = f.engine->getLayerInfo(moved.value);
    LS_CHECK(ownerOf.ok() && ownerOf.value.sprite == other.value);

    // -1 is the top, and a layer of another document is refused.
    auto top = f.engine->cloneLayer(f.layer, f.sprite, -1);
    LS_REQUIRE(top.ok());
    info = f.engine->getSpriteInfo(f.sprite);
    LS_CHECK(info.ok() && info.value.layers.back() == top.value);
    auto elsewhere = f.engine->createDocument({ "other", 16, 16 });
    LS_REQUIRE(elsewhere.ok());
    auto foreign = f.engine->createSprite(elsewhere.value);
    LS_REQUIRE(foreign.ok());
    LS_CHECK(f.engine->cloneLayer(f.layer, foreign.value).fail());
}

// The other half: deleting a frame must not take the frames beside it with it.
void testDeletingAFrameLeavesTheOthersDrawable() {
    Fixture f;
    LS_REQUIRE(f.build());

    auto clone = f.engine->cloneSprite(f.sprite);
    LS_REQUIRE(clone.ok());

    const RasterBuffer expected = f.render(clone.value);
    LS_REQUIRE(!expected.empty());

    LS_REQUIRE(f.engine->deleteSprite(f.sprite).ok());
    LS_CHECK(sameBytes(expected, f.render(clone.value)));
}

// A stroke names a polyline rather than a region, so it is the second way an
// operation can hold on to something the clone should have got its own copy of.
void testAClonedStrokeHasItsOwnPath() {
    Fixture f;
    LS_REQUIRE(f.build());

    PolylineDesc path;
    path.points = { { 1.f, 1.f }, { 14.f, 14.f } };
    path.closed = false;
    auto polyline = f.engine->createPolyline(f.doc, path);
    LS_REQUIRE(polyline.ok());

    StrokePolylineOp stroke;
    stroke.polyline = polyline.value;
    stroke.width = 1.f;
    stroke.fallbackColor = { 40, 200, 255, 255 };
    auto strokeLayer = f.engine->createLayer(f.sprite, { "Line" });
    LS_REQUIRE(strokeLayer.ok());
    LS_REQUIRE(f.engine->addOperation(strokeLayer.value, stroke).ok());

    auto clone = f.engine->cloneSprite(f.sprite);
    LS_REQUIRE(clone.ok());

    const RasterBuffer before = f.render(f.sprite);
    LS_REQUIRE(!before.empty());

    auto layers = f.engine->getSpriteInfo(clone.value);
    LS_REQUIRE(layers.ok() && layers.value.layers.size() == 2);
    auto operations = f.engine->getLayerOperations(layers.value.layers.back());
    LS_REQUIRE(operations.ok() && operations.value.size() == 1);

    auto held = f.engine->getOperationParameter(operations.value.front().id, "polyline");
    LS_REQUIRE(held.ok());
    const uint64_t clonedPath = std::get<uint64_t>(held.value);
    LS_CHECK(clonedPath != polyline.value.value);

    PolylineDesc moved;
    moved.points = { { 1.f, 14.f }, { 14.f, 1.f } };
    moved.closed = false;
    LS_REQUIRE(f.engine->updatePolyline(GeometryId{ clonedPath }, moved).ok());

    LS_CHECK(sameBytes(before, f.render(f.sprite)));
    LS_CHECK(!sameBytes(before, f.render(clone.value)));
}


// A document with frames in it has to survive being a file, which means the
// sprites come back in the order they were authored -- an animation whose frames
// reorder on load is not an animation.
void testFramesSurviveARoundTrip() {
    Fixture f;
    LS_REQUIRE(f.build());

    std::vector<RasterBuffer> expected;
    std::vector<SpriteId> frames{ f.sprite };
    for (int i = 1; i < 4; ++i) {
        auto clone = f.engine->cloneSprite(frames.back());
        LS_REQUIRE(clone.ok());
        auto source = f.engine->getSpriteInfo(clone.value);
        LS_REQUIRE(source.ok() && !source.value.layers.empty());
        auto operations = f.engine->getLayerOperations(source.value.layers.front());
        LS_REQUIRE(operations.ok() && !operations.value.empty());
        auto target = f.engine->getOperationParameter(operations.value.front().id,
                                                      "targetRegion");
        LS_REQUIRE(target.ok());
        auto geometryOf =
            f.engine->getRegionSourceGeometry(RegionId{ std::get<uint64_t>(target.value) });
        LS_REQUIRE(geometryOf.ok());
        // Each frame puts the square somewhere else, so they are told apart by
        // what they draw rather than by an id.
        LS_REQUIRE(f.engine->updateRect(geometryOf.value,
            { { static_cast<float>(i) * 2.f, 2.f }, 6.f, 6.f, 0.f }).ok());
        frames.push_back(clone.value);
    }
    for (SpriteId frame : frames) {
        expected.push_back(f.render(frame));
    }

    auto saved = f.engine->serializeDocument(f.doc);
    LS_REQUIRE(saved.ok());

    auto reader = LSContext::create();
    auto loaded = reader->deserializeDocument(saved.value);
    LS_REQUIRE(loaded.ok());

    auto info = reader->getDocumentInfo(loaded.value);
    LS_REQUIRE(info.ok());
    LS_REQUIRE(info.value.sprites.size() == frames.size());

    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = 16;
    profile.outputHeight = 16;
    profile.palette = PalettePolicy::Unconstrained;

    for (size_t i = 0; i < info.value.sprites.size(); ++i) {
        auto compiled = reader->compileSprite(info.value.sprites[i], profile);
        LS_REQUIRE(compiled.ok());
        LS_CHECK(sameBytes(expected[i], compiled.value.raster));
    }
}


// Dragging frame 3 in front of frame 2 is a timeline's most ordinary gesture,
// and the order it produces has to be what the file says.
void testFramesCanBeReordered() {
    Fixture f;
    LS_REQUIRE(f.build());

    auto second = f.engine->cloneSprite(f.sprite);
    auto third  = f.engine->cloneSprite(f.sprite);
    LS_REQUIRE(second.ok() && third.ok());

    const std::vector<SpriteId> wanted{ third.value, f.sprite, second.value };
    LS_REQUIRE(f.engine->setSpriteOrder(f.doc, wanted).ok());

    auto info = f.engine->getDocumentInfo(f.doc);
    LS_REQUIRE(info.ok());
    LS_CHECK(info.value.sprites == wanted);

    // Order is what a file carries, so it has to come back the same way.
    auto saved = f.engine->serializeDocument(f.doc);
    LS_REQUIRE(saved.ok());
    auto reader = LSContext::create();
    auto loaded = reader->deserializeDocument(saved.value);
    LS_REQUIRE(loaded.ok());
    auto reloaded = reader->getDocumentInfo(loaded.value);
    LS_REQUIRE(reloaded.ok());
    LS_REQUIRE(reloaded.value.sprites.size() == 3);

    // Ids are minted fresh by the reader, so what is checked is the shape of
    // the order rather than the numbers: the third frame is now first.
    auto position = [&](SpriteId id) {
        const auto& list = info.value.sprites;
        return std::find(list.begin(), list.end(), id) - list.begin();
    };
    LS_CHECK(position(third.value) == 0);
    LS_CHECK(position(second.value) == 2);
}

// A reorder that is not a permutation is refused rather than half applied: a
// document that disagrees with itself about what it contains is worse than an
// error code.
void testABadOrderIsRefused() {
    Fixture f;
    LS_REQUIRE(f.build());
    auto second = f.engine->cloneSprite(f.sprite);
    LS_REQUIRE(second.ok());

    const std::vector<SpriteId> before = f.engine->getDocumentInfo(f.doc).value.sprites;

    LS_CHECK(f.engine->setSpriteOrder(f.doc, { f.sprite }).fail());               // too short
    LS_CHECK(f.engine->setSpriteOrder(f.doc, { f.sprite, f.sprite }).fail());     // one twice
    LS_CHECK(f.engine->setSpriteOrder(f.doc, { f.sprite, SpriteId{ 99999 } }).fail());

    // And none of those moved anything.
    LS_CHECK(f.engine->getDocumentInfo(f.doc).value.sprites == before);
}


// An editor showing many frames at once -- a timeline strip, an onion skin --
// cannot afford to recompile every frame when one of them changes. It needs to
// ask which ones actually changed, and this is that question.
//
// The promises: a fresh sprite is dirty, compiling it makes it clean, editing
// it makes it dirty again, and editing one frame does not dirty its neighbours.
void testTheEngineSaysWhichFrameChanged() {
    Fixture f;
    LS_REQUIRE(f.build());

    auto other = f.engine->cloneSprite(f.sprite);
    LS_REQUIRE(other.ok());

    CompileProfile profile;
    profile.type = CompileProfileType::Preview;
    profile.outputWidth = 16;
    profile.outputHeight = 16;
    profile.palette = PalettePolicy::Unconstrained;

    auto dirty = [&](SpriteId id) {
        auto state = f.engine->isDirty(id.value);
        return state.ok() && state.value;
    };

    LS_REQUIRE(f.engine->compileSprite(f.sprite, profile).ok());
    LS_REQUIRE(f.engine->compileSprite(other.value, profile).ok());
    LS_CHECK(!dirty(f.sprite));                 // a compiled sprite is clean
    LS_CHECK(!dirty(other.value));

    // Change what one frame draws.
    auto layers = f.engine->getSpriteInfo(f.sprite);
    LS_REQUIRE(layers.ok() && !layers.value.layers.empty());
    auto operations = f.engine->getLayerOperations(layers.value.layers.front());
    LS_REQUIRE(operations.ok() && !operations.value.empty());
    LS_REQUIRE(f.engine->setOperationParameter(operations.value.front().id,
                                               "opacity", 0.5f).ok());

    LS_CHECK(dirty(f.sprite));
    // The one that matters for a timeline: the frame beside it did not change.
    LS_CHECK(!dirty(other.value));

    LS_REQUIRE(f.engine->compileSprite(f.sprite, profile).ok());
    LS_CHECK(!dirty(f.sprite));

    // Moving the geometry a region tracks has to reach the sprite that draws it,
    // or an editor caching by this flag would show a stale picture.
    LS_REQUIRE(f.engine->updateRect(f.geometry, { { 6.f, 6.f }, 6.f, 6.f, 0.f }).ok());
    LS_CHECK(dirty(f.sprite));
}


// A sheet packs many frames into one image, and every cell is compiled on its
// own and then composited. The question that decides how an editor writes one
// is what a pattern does at the cell boundary, and exportOrigin is the control:
// it says where this output frame sits inside something larger.
void testExportOriginMovesAPatternLattice() {
    Fixture f;
    LS_REQUIRE(f.build());

    // A dithered fill anchored Local in Export space -- the one combination
    // that reads exportOrigin at all.
    RampDesc ramp;
    ramp.stops = { { 0.f, Color{ 20, 30, 60, 255 } },
                   { 1.f, Color{ 250, 180, 90, 255 } } };
    auto made = f.engine->createRamp(f.doc, ramp);
    LS_REQUIRE(made.ok());
    auto pattern = f.engine->createDitherPattern(f.doc, DitherPatternKind::Bayer4);
    LS_REQUIRE(pattern.ok());

    FillDitherOp dither;
    dither.targetRegion = f.region;
    dither.ramp = made.value;
    dither.pattern = pattern.value;
    dither.density = 0.5f;
    dither.anchor = PatternAnchor::Local;
    dither.coordinateSpace = CoordinateSpace::Export;

    auto layer = f.engine->createLayer(f.sprite, { "Dithered" });
    LS_REQUIRE(layer.ok());
    LS_REQUIRE(f.engine->addOperation(layer.value, dither).ok());

    const auto at = [&](int32_t x, int32_t y) {
        CompileProfile profile;
        profile.type = CompileProfileType::Export;
        profile.outputWidth = 16;
        profile.outputHeight = 16;
        profile.palette = PalettePolicy::Unconstrained;
        profile.exportOrigin = { x, y };
        auto compiled = f.engine->compileSprite(f.sprite, profile);
        return compiled.ok() ? compiled.value.raster : RasterBuffer{};
    };

    const RasterBuffer origin = at(0, 0);
    LS_REQUIRE(!origin.empty());

    // Moving the frame by one pixel moves the lattice under it, so the same
    // drawing resolves to different pixels. This is the whole mechanism.
    LS_CHECK(!sameBytes(origin, at(1, 0)));

    // Moving it by a whole tile puts the lattice back where it was: a 4x4
    // pattern has period 4.
    LS_CHECK(sameBytes(origin, at(4, 0)));
    LS_CHECK(sameBytes(origin, at(0, 4)));
    LS_CHECK(sameBytes(origin, at(16, 32)));

    // And a frame that ignores the export frame is unmoved by any of it, which
    // is what makes a cell reproduce a single-frame export exactly.
    auto operations = f.engine->getLayerOperations(layer.value);
    LS_REQUIRE(operations.ok() && !operations.value.empty());
    LS_REQUIRE(f.engine->setOperationParameter(
        operations.value.front().id, "coordinateSpace",
        static_cast<int64_t>(CoordinateSpace::Canvas)).ok());

    const RasterBuffer fixed = at(0, 0);
    LS_CHECK(sameBytes(fixed, at(1, 0)));
    LS_CHECK(sameBytes(fixed, at(37, 91)));
}

} // namespace

int main() {
    testACloneIsItsOwnFrame();
    testAClonedLayerIsItsOwn();
    testDeletingAFrameLeavesTheOthersDrawable();
    testAClonedStrokeHasItsOwnPath();
    testFramesSurviveARoundTrip();
    testFramesCanBeReordered();
    testABadOrderIsRefused();
    testTheEngineSaysWhichFrameChanged();
    testExportOriginMovesAPatternLattice();
    return lstest::report("frames");
}
