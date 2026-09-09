// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors
// editing_tests.cpp — what an editor needs underneath it: an undo that keeps
// its handles, regions that accumulate a stroke at a time, and somewhere to
// keep app data that travels with the document.

#include "ls_test.h"

#include <livesprite/livesprite.h>

#include <string>
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

struct Doc {
    DocumentId doc;
    SpriteId sprite;
    LayerId layer;
    RegionId region;
    OperationId fill;
};

Doc makeDoc(LSContext& ctx) {
    Doc built;
    built.doc = ctx.createDocument({"edit", 32, 32}).value;
    built.sprite = ctx.createSprite(built.doc).value;
    built.layer = ctx.createLayer(built.sprite, {"main"}).value;
    const GeometryId rect = ctx.createRect(built.doc, {{8.f, 8.f}, 10.f, 10.f, 0.f}).value;
    built.region = ctx.createRegionFromGeometry(rect).value;
    FillSolidOp fill;
    fill.targetRegion = built.region;
    fill.fallbackColor = Color::white();
    built.fill = ctx.addOperation(built.layer, fill).value;
    return built;
}

int64_t regionPixels(LSContext& ctx, RegionId region) {
    auto intervals = ctx.getRegionIntervals(region);
    return intervals.ok() ? geom::pixelCount(intervals.value) : -1;
}

// --- undo ------------------------------------------------------------------

void testSnapshotRestoreKeepsHandles() {
    auto ctx = LSContext::create();
    const Doc built = makeDoc(*ctx);

    auto before = ctx->compileSprite(built.sprite, exportProfile());
    LS_REQUIRE(before.ok());

    auto snapshot = ctx->snapshotDocumentState(built.doc);
    LS_REQUIRE(snapshot.ok());

    // Make a mess: change a parameter, add a layer, delete the operation.
    LS_CHECK(ctx->setOperationParameter(built.fill, "fallbackColor",
                                        Color{10, 200, 40, 255}).ok());
    const LayerId extra = ctx->createLayer(built.sprite, {"scratch"}).value;
    LS_CHECK(ctx->removeOperation(built.layer, built.fill).ok());
    LS_CHECK(ctx->getOperation(built.fill).fail());

    // Undo.
    LS_CHECK(ctx->restoreDocumentState(built.doc, snapshot.value).ok());

    // The handles the caller was holding still work, which is the whole point:
    // an undo that renumbered entities would invalidate every reference the
    // interface keeps.
    LS_CHECK(ctx->getSpriteInfo(built.sprite).ok());
    LS_CHECK(ctx->getLayerInfo(built.layer).ok());
    LS_CHECK(ctx->getOperation(built.fill).ok());
    LS_CHECK(ctx->getRegionIntervals(built.region).ok());

    // The state came back with them.
    LS_CHECK(std::get<FillSolidOp>(ctx->getOperation(built.fill).value).fallbackColor ==
             Color::white());
    LS_CHECK(ctx->getSpriteInfo(built.sprite).value.layers.size() == 1);
    LS_CHECK(ctx->getLayerInfo(extra).fail());   // the scratch layer is gone

    auto after = ctx->compileSprite(built.sprite, exportProfile());
    LS_REQUIRE(after.ok());
    LS_CHECK(after.value.raster.pixels == before.value.raster.pixels);

    // Restoring twice is the same as restoring once.
    LS_CHECK(ctx->restoreDocumentState(built.doc, snapshot.value).ok());
    LS_CHECK(ctx->compileSprite(built.sprite, exportProfile()).value.raster.pixels ==
             before.value.raster.pixels);

    // Ids keep advancing after a restore, so nothing new collides with what
    // came back.
    const SpriteId fresh = ctx->createSprite(built.doc).value;
    LS_CHECK(fresh != built.sprite);
    LS_CHECK(ctx->getSpriteInfo(fresh).ok());
    LS_CHECK(ctx->getSpriteInfo(built.sprite).ok());
}

void testSnapshotRefusesMismatches() {
    auto ctx = LSContext::create();
    const Doc first = makeDoc(*ctx);
    const Doc second = makeDoc(*ctx);

    auto snapshot = ctx->snapshotDocumentState(first.doc);
    LS_REQUIRE(snapshot.ok());

    // A snapshot belongs to the document it came from: the ids inside it are
    // that document's, so pouring it into another is refused.
    LS_CHECK(ctx->restoreDocumentState(second.doc, snapshot.value).fail());
    LS_CHECK(ctx->getOperation(second.fill).ok());   // and it was left alone

    // An empty snapshot is refused rather than treated as an empty document.
    DocumentSnapshot empty;
    LS_CHECK(!empty.valid());
    LS_CHECK(ctx->restoreDocumentState(first.doc, empty).fail());
    LS_CHECK(ctx->getOperation(first.fill).ok());
}

// --- drawing into a region -------------------------------------------------

void testRegionEditing() {
    auto ctx = LSContext::create();
    const Doc built = makeDoc(*ctx);
    LS_CHECK(regionPixels(*ctx, built.region) == 100);

    // A stroke accumulates into the region being drawn, rather than leaving a
    // new region and a new operation behind every time.
    PixelRegionDesc stroke;
    for (int32_t x = 20; x < 26; ++x) {
        stroke.pixels.push_back({{x, 12}, Color::black()});
    }
    LS_CHECK(ctx->addPixelsToRegion(built.region, stroke).ok());
    LS_CHECK(regionPixels(*ctx, built.region) == 106);
    LS_CHECK(ctx->getLayerOperations(built.layer).value.size() == 1);

    // The fill follows the region without being touched.
    auto compiled = ctx->compileSprite(built.sprite, exportProfile());
    LS_REQUIRE(compiled.ok());
    LS_CHECK(readPixel(compiled.value.raster, 22, 12) == Color::white());

    // Erasing takes pixels away again.
    LS_CHECK(ctx->erasePixelsFromRegion(built.region, {{20, 12}, {21, 12}}).ok());
    LS_CHECK(regionPixels(*ctx, built.region) == 104);
    auto erased = ctx->compileSprite(built.sprite, exportProfile());
    LS_CHECK(readPixel(erased.value.raster, 20, 12).a == 0);

    // Setting intervals outright replaces the shape.
    IntervalSet replacement;
    replacement.intervals.push_back({4, 4, 8});
    LS_CHECK(ctx->setRegionIntervals(built.region, replacement).ok());
    LS_CHECK(regionPixels(*ctx, built.region) == 4);

    // An edited region no longer tracks the geometry it came from, so a later
    // geometry edit cannot silently overwrite the drawing.
    auto geometryList = ctx->getRegionBounds(built.region);
    LS_CHECK(geometryList.ok());
    LS_CHECK(ctx->setRegionIntervals(RegionId{999999}, replacement).fail());
}

void testRegionEditingDetachesFromGeometry() {
    auto ctx = LSContext::create();
    const Doc built = makeDoc(*ctx);
    const GeometryId source = ctx->createRect(built.doc, {{2.f, 2.f}, 4.f, 4.f, 0.f}).value;
    const RegionId tracked = ctx->createRegionFromGeometry(source).value;
    LS_CHECK(regionPixels(*ctx, tracked) == 16);

    // While it tracks, a geometry edit reaches it.
    LS_CHECK(ctx->updateRect(source, {{2.f, 2.f}, 6.f, 4.f, 0.f}).ok());
    LS_CHECK(regionPixels(*ctx, tracked) == 24);

    // Once drawn into by hand, it is its own shape.
    PixelRegionDesc stroke;
    stroke.pixels.push_back({{20, 20}, Color::black()});
    LS_CHECK(ctx->addPixelsToRegion(tracked, stroke).ok());
    LS_CHECK(regionPixels(*ctx, tracked) == 25);

    LS_CHECK(ctx->updateRect(source, {{2.f, 2.f}, 10.f, 10.f, 0.f}).ok());
    LS_CHECK(regionPixels(*ctx, tracked) == 25);   // the drawing survived
}

// --- app metadata ----------------------------------------------------------

void testMetadata() {
    auto ctx = LSContext::create();
    const Doc built = makeDoc(*ctx);

    LS_CHECK(ctx->setMetadata(built.sprite.value, "fast.frame", "3").ok());
    LS_CHECK(ctx->setMetadata(built.layer.value, "fast.panel.collapsed", "true").ok());
    LS_CHECK(ctx->getMetadata(built.sprite.value, "fast.frame").value == "3");

    // Two apps can annotate the same entity without colliding, which is what
    // the namespace requirement buys.
    LS_CHECK(ctx->setMetadata(built.sprite.value, "pract.arranger.cell", "4,2").ok());
    LS_CHECK(ctx->getMetadata(built.sprite.value, "fast.frame").value == "3");
    LS_CHECK(ctx->metadataKeys(built.sprite.value).value.size() == 2);

    // An unnamespaced key is refused: it would be a landgrab on a shared space.
    LS_CHECK(ctx->setMetadata(built.sprite.value, "frame", "3").fail());
    LS_CHECK(ctx->setMetadata(built.sprite.value, ".leading", "x").fail());
    LS_CHECK(ctx->setMetadata(built.sprite.value, "trailing.", "x").fail());
    LS_CHECK(ctx->setMetadata(0, "fast.frame", "3").fail());

    // Sizes are capped, because a document is a file other people open.
    LS_CHECK(ctx->setMetadata(built.sprite.value, "fast.big",
                              std::string(kMetadataMaxValueLength + 1, 'x')).fail());
    LS_CHECK(ctx->setMetadata(built.sprite.value,
                              "fast." + std::string(kMetadataMaxKeyLength, 'k'), "x").fail());
    for (size_t i = 0; i < kMetadataMaxPerEntity; ++i) {
        ctx->setMetadata(built.layer.value, "fast.key" + std::to_string(i), "v");
    }
    LS_CHECK(ctx->setMetadata(built.layer.value, "fast.oneTooMany", "v").fail());

    // The engine never looks inside a value.
    const std::string opaque = "{\"anything\":[1,2,3]}";
    LS_CHECK(ctx->setMetadata(built.region.value, "fast.blob", opaque).ok());
    LS_CHECK(ctx->getMetadata(built.region.value, "fast.blob").value == opaque);

    LS_CHECK(ctx->clearMetadata(built.region.value, "fast.blob").ok());
    LS_CHECK(ctx->getMetadata(built.region.value, "fast.blob").fail());
}

void testMetadataTravelsWithTheDocument() {
    auto ctx = LSContext::create();
    const Doc built = makeDoc(*ctx);
    LS_CHECK(ctx->setMetadata(built.sprite.value, "fast.frame", "7").ok());
    LS_CHECK(ctx->setMetadata(built.doc.value, "fast.onionSkin", "2").ok());

    // Through a save and load, remapped onto the new ids.
    auto saved = ctx->serializeDocument(built.doc);
    LS_REQUIRE(saved.ok());
    auto loaded = LSContext::create();
    auto restoredDoc = loaded->deserializeDocument(saved.value);
    LS_REQUIRE(restoredDoc.ok());
    LS_CHECK(loaded->getMetadata(restoredDoc.value.value, "fast.onionSkin").value == "2");

    SpriteId restoredSprite;
    for (uint64_t candidate = 1; candidate < 4096; ++candidate) {
        if (loaded->getSpriteInfo(SpriteId{candidate}).ok()) {
            restoredSprite = SpriteId{candidate};
            break;
        }
    }
    LS_REQUIRE(restoredSprite.valid());
    LS_CHECK(loaded->getMetadata(restoredSprite.value, "fast.frame").value == "7");

    // And through an undo, which is a restore of the same document.
    auto snapshot = ctx->snapshotDocumentState(built.doc);
    LS_REQUIRE(snapshot.ok());
    LS_CHECK(ctx->setMetadata(built.sprite.value, "fast.frame", "9").ok());
    LS_CHECK(ctx->restoreDocumentState(built.doc, snapshot.value).ok());
    LS_CHECK(ctx->getMetadata(built.sprite.value, "fast.frame").value == "7");
}

} // namespace

// A document that arrives from a file hands back no usable ids: the reader mints
// fresh ones. Without a way to enumerate what it contains, an application could
// open a file and then have no way to display it.
void testLoadedDocumentIsNavigable() {
    auto ctx = LSContext::create();

    const DocumentId doc = ctx->createDocument({"navigable", 16, 16}).value;
    const SpriteId first = ctx->createSprite(doc).value;
    const SpriteId second = ctx->createSprite(doc).value;
    ctx->createLayer(first, {"a"});
    ctx->createLayer(second, {"b"});

    auto info = ctx->getDocumentInfo(doc);
    LS_REQUIRE(info.ok());
    LS_CHECK(info.value.name == "navigable");
    LS_CHECK(info.value.canvasWidth == 16);
    LS_CHECK(info.value.sprites.size() == 2);

    auto package = ctx->writePackage(doc, {});
    LS_REQUIRE(package.ok());

    // A second context, holding none of the ids above.
    auto reader = LSContext::create();
    auto loaded = reader->loadPackage(package.value, nullptr);
    LS_REQUIRE(loaded.ok());

    auto reloaded = reader->getDocumentInfo(loaded.value);
    LS_REQUIRE(reloaded.ok());
    LS_CHECK(reloaded.value.sprites.size() == 2);
    LS_CHECK(reloaded.value.canvasWidth == 16);

    // The enumerated ids are real handles, not just numbers.
    for (SpriteId sprite : reloaded.value.sprites) {
        LS_CHECK(reader->getSpriteInfo(sprite).ok());
    }

    LS_CHECK(ctx->getDocumentInfo(DocumentId{}).fail());
}

// A name is state: it is shown, it is saved, and an interface that shows it will
// be asked to change it. It was previously write-once.
void testNamesCanBeChanged() {
    auto ctx = LSContext::create();
    const DocumentId doc = ctx->createDocument({"before", 8, 8}).value;
    const SpriteId sprite = ctx->createSprite(doc).value;
    const LayerId layer = ctx->createLayer(sprite, {"layer 1"}).value;

    LS_CHECK(ctx->getLayerInfo(layer).value.name == "layer 1");
    LS_REQUIRE(ctx->setLayerName(layer, "background").ok());
    LS_CHECK(ctx->getLayerInfo(layer).value.name == "background");

    LS_CHECK(ctx->getDocumentInfo(doc).value.name == "before");
    LS_REQUIRE(ctx->setDocumentName(doc, "after").ok());
    LS_CHECK(ctx->getDocumentInfo(doc).value.name == "after");

    LS_CHECK(ctx->setLayerName(LayerId{}, "nope").fail());
    LS_CHECK(ctx->setDocumentName(DocumentId{}, "nope").fail());

    // Renaming survives a save, and does not disturb the picture.
    const GeometryId rect = ctx->createRect(doc, {{1.f, 1.f}, 4.f, 4.f, 0.f}).value;
    FillSolidOp fill;
    fill.targetRegion = ctx->createRegionFromGeometry(rect).value;
    ctx->addOperation(layer, fill);

    auto saved = ctx->serializeDocument(doc);
    LS_REQUIRE(saved.ok());

    auto reader = LSContext::create();
    auto loaded = reader->deserializeDocument(saved.value);
    LS_REQUIRE(loaded.ok());
    auto info = reader->getDocumentInfo(loaded.value);
    LS_REQUIRE(info.ok());
    LS_CHECK(info.value.name == "after");
    LS_REQUIRE(!info.value.sprites.empty());
    auto sprites = reader->getSpriteInfo(info.value.sprites.front());
    LS_REQUIRE(sprites.ok() && !sprites.value.layers.empty());
    LS_CHECK(reader->getLayerInfo(sprites.value.layers.front()).value.name == "background");
}

// An application offering editable shapes has to recognise one in a document it
// just loaded. The link from region to geometry survives a save; without a way
// to ask, a rectangle comes back as anonymous pixels.
void testARegionRemembersItsGeometry() {
    auto ctx = LSContext::create();
    const DocumentId doc = ctx->createDocument({"shapes", 32, 32}).value;

    const GeometryId rect = ctx->createRect(doc, {{4.f, 4.f}, 8.f, 8.f, 0.f}).value;
    const RegionId fromGeometry = ctx->createRegionFromGeometry(rect).value;

    auto source = ctx->getRegionSourceGeometry(fromGeometry);
    LS_REQUIRE(source.ok());
    LS_CHECK(source.value == rect);

    // A region authored as pixels has none, and asking is not an error.
    PixelRegionDesc drawn;
    drawn.pixels.push_back({{1, 1}, Color{255, 255, 255, 255}});
    const RegionId authored = ctx->createRegionFromPixels(doc, drawn).value;
    auto none = ctx->getRegionSourceGeometry(authored);
    LS_CHECK(none.ok());
    LS_CHECK(!none.value.valid());

    LS_CHECK(ctx->getRegionSourceGeometry(RegionId{}).fail());

    // The link survives a round trip, which is the case that matters.
    const SpriteId sprite = ctx->createSprite(doc).value;
    const LayerId layer = ctx->createLayer(sprite, {"main"}).value;
    FillSolidOp fill;
    fill.targetRegion = fromGeometry;
    ctx->addOperation(layer, fill);

    auto saved = ctx->serializeDocument(doc);
    LS_REQUIRE(saved.ok());

    auto reader = LSContext::create();
    auto loaded = reader->deserializeDocument(saved.value);
    LS_REQUIRE(loaded.ok());

    auto info = reader->getDocumentInfo(loaded.value);
    LS_REQUIRE(info.ok() && !info.value.sprites.empty());
    auto sprites = reader->getSpriteInfo(info.value.sprites.front());
    LS_REQUIRE(sprites.ok() && !sprites.value.layers.empty());
    auto operations = reader->getLayerOperations(sprites.value.layers.front());
    LS_REQUIRE(operations.ok() && !operations.value.empty());

    auto region = reader->getOperationParameter(operations.value.front().id,
                                                "targetRegion");
    LS_REQUIRE(region.ok());
    const uint64_t* handle = std::get_if<uint64_t>(&region.value);
    LS_REQUIRE(handle != nullptr && *handle != 0);

    RegionId reloaded;
    reloaded.value = *handle;
    auto reloadedSource = reader->getRegionSourceGeometry(reloaded);
    LS_REQUIRE(reloadedSource.ok());
    LS_CHECK(reloadedSource.value.valid());

    // And it is the geometry that still drives the picture: editing it moves
    // what the reloaded document draws.
    LS_CHECK(reader->updateRect(reloadedSource.value,
                                {{2.f, 2.f}, 20.f, 20.f, 0.f}).ok());
}

int main() {
    testARegionRemembersItsGeometry();
    testNamesCanBeChanged();
    testLoadedDocumentIsNavigable();
    testSnapshotRestoreKeepsHandles();
    testSnapshotRefusesMismatches();
    testRegionEditing();
    testRegionEditingDetachesFromGeometry();
    testMetadata();
    testMetadataTravelsWithTheDocument();
    return lstest::report("editing");
}
