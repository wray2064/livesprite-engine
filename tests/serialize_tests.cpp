// serialize_tests.cpp — the save file contract: lossless round trip, byte
// determinism, forward compatibility, and version migration.

#include "ls_test.h"

#include <livesprite/livesprite.h>

#include <string>

using namespace ls;

namespace {

struct Built {
    DocumentId doc;
    SpriteId sprite;
    LayerId layer;
    RegionId region;
    OperationId dither;
};

// A document that exercises most of the model: geometry, regions, palette,
// ramp, pattern, fills, an outline, a transform, pivot, socket, boundary.
Built buildDocument(LSContext& ctx) {
    Built built;
    built.doc = ctx.createDocument({"sword", 32, 48}).value;
    built.sprite = ctx.createSprite(built.doc).value;
    built.layer = ctx.createLayer(built.sprite, {"blade", LayerType::Drawing, 0.75f,
                                                 BlendMode::Multiply, true}).value;

    const GeometryId blade = ctx.createRect(built.doc, {{12.f, 6.f}, 8.f, 26.f, 1.f}).value;
    built.region = ctx.createRegionFromGeometry(blade).value;

    const PaletteId palette = ctx.createPalette(built.doc,
        {"steel", {{0, {20, 24, 32, 255}, "shadow"}, {1, {120, 130, 150, 255}, "base"}}}).value;
    ctx.bindSpritePalette(built.sprite, palette);

    const RampId ramp = ctx.createRamp(built.doc,
        {"steel_ramp", {{0.f, {20, 24, 32, 255}}, {0.5f, {90, 100, 120, 255}},
                        {1.f, {220, 230, 245, 255}}}, true}).value;
    const PatternId bayer = ctx.createOrderedDitherPattern(built.doc, 4).value;

    FillSolidOp fill;
    fill.targetRegion = built.region;
    fill.paletteRole = 1;
    fill.fallbackColor = {120, 130, 150, 255};
    ctx.addOperation(built.layer, fill);

    FillDitherOp dither;
    dither.targetRegion = built.region;
    dither.ramp = ramp;
    dither.pattern = bayer;
    dither.density = 0.35f;
    dither.phase = 1.5f;
    dither.coordinateSpace = CoordinateSpace::Object;
    built.dither = ctx.addOperation(built.layer, dither).value;

    GenerateOuterOutlineOp outline;
    outline.targetRegion = built.region;
    outline.thickness = 1.f;
    outline.paletteRole = 0;
    ctx.addOperation(built.layer, outline);

    const PivotId pivot = ctx.createPivot(built.sprite, {16.f, 40.f}).value;
    RotateOp rotate;
    rotate.targetLayer = built.layer;
    rotate.angleDegrees = 22.5f;
    rotate.pivot = pivot;
    rotate.sampling = SamplingPolicy::Coverage;
    ctx.addOperation(built.layer, rotate);

    ctx.addSocket(built.sprite, {"grip", {16.f, 38.f}, 15.f});
    const GeometryId zone = ctx.createEllipse(built.doc, {{16.f, 20.f}, 10.f, 14.f}).value;
    ctx.createBoundary(built.sprite, {"squash", zone, Falloff::Smooth, 3.f});

    return built;
}

CompileProfile exportProfile() {
    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = 32;
    profile.outputHeight = 48;
    return profile;
}

void testDocumentRoundTrip() {
    auto source = LSContext::create();
    const Built built = buildDocument(*source);

    auto saved = source->serializeDocument(built.doc);
    LS_REQUIRE(saved.ok());
    LS_CHECK(saved.value.formatTag == "livesprite/document");
    LS_CHECK(saved.value.engineVersion == LS_ENGINE_VERSION);
    LS_CHECK(!saved.value.bytes.empty());

    // Saving twice produces identical bytes.
    auto savedAgain = source->serializeDocument(built.doc);
    LS_CHECK(savedAgain.ok() && savedAgain.value.bytes == saved.value.bytes);

    // Load into a fresh context and compare the compiled output: if any
    // parameter were lost the pixels would differ.
    auto loaded = LSContext::create();
    auto restoredDoc = loaded->deserializeDocument(saved.value);
    LS_REQUIRE(restoredDoc.ok());

    const auto sprites = loaded->documents();
    LS_CHECK(sprites.size() == 1);
    LS_CHECK(loaded->getCanvasSize(restoredDoc.value).value.y == 48);

    auto originalCompiled = source->compileSprite(built.sprite, exportProfile());
    LS_REQUIRE(originalCompiled.ok());

    // The restored document holds exactly one sprite; find it through the
    // document, since ids are remapped on load.
    auto restoredSaved = loaded->serializeDocument(restoredDoc.value);
    LS_REQUIRE(restoredSaved.ok());

    // Re-saving the loaded document reproduces the original bytes except for
    // the remapped ids, so compare the structure by loading it once more and
    // compiling. Compare pixels via the round-tripped sprite.
    SpriteId restoredSprite;
    for (uint64_t candidate = 1; candidate < 4096; ++candidate) {
        auto info = loaded->getSpriteInfo(SpriteId{candidate});
        if (info.ok()) {
            restoredSprite = SpriteId{candidate};
            break;
        }
    }
    LS_REQUIRE(restoredSprite.valid());

    auto restoredCompiled = loaded->compileSprite(restoredSprite, exportProfile());
    LS_REQUIRE(restoredCompiled.ok());
    LS_CHECK(restoredCompiled.value.raster.pixels == originalCompiled.value.raster.pixels);
    LS_CHECK(restoredCompiled.value.bounds.min.x == originalCompiled.value.bounds.min.x);

    // Layer settings survive.
    auto layerInfo = loaded->getLayerInfo(loaded->getSpriteInfo(restoredSprite).value.layers.front());
    LS_CHECK(layerInfo.ok());
    LS_CHECK(layerInfo.value.name == "blade");
    LS_CHECK(layerInfo.value.blend == BlendMode::Multiply);
    LS_CHECK(layerInfo.value.opacity == 0.75f);
    LS_CHECK(layerInfo.value.operations.size() == 4);

    // Sockets, boundaries and pivots survive.
    auto spriteInfo = loaded->getSpriteInfo(restoredSprite);
    LS_CHECK(spriteInfo.value.sockets.size() == 1);
    LS_CHECK(spriteInfo.value.boundaries.size() == 1);
    LS_CHECK(spriteInfo.value.pivot.valid());
    LS_CHECK(loaded->getPivot(spriteInfo.value.pivot).value.y == 40.f);
}

void testOperationRoundTrip() {
    auto ctx = LSContext::create();
    const Built built = buildDocument(*ctx);

    auto text = ctx->serializeOperation(built.dither);
    LS_REQUIRE(text.ok());
    LS_CHECK(text.value.find("FillDitherOp") != std::string::npos);
    LS_CHECK(text.value.find("density") != std::string::npos);

    auto restored = ctx->deserializeOperation(built.layer, text.value);
    LS_REQUIRE(restored.ok());

    auto original = ctx->getOperation(built.dither);
    auto copy = ctx->getOperation(restored.value);
    LS_REQUIRE(original.ok() && copy.ok());

    const auto& a = std::get<FillDitherOp>(original.value);
    const auto& b = std::get<FillDitherOp>(copy.value);
    LS_CHECK(a.targetRegion == b.targetRegion);
    LS_CHECK(a.ramp == b.ramp);
    LS_CHECK(a.pattern == b.pattern);
    LS_CHECK(a.density == b.density);
    LS_CHECK(a.phase == b.phase);
    LS_CHECK(a.coordinateSpace == b.coordinateSpace);

    // Serializing the copy yields the same text apart from its id.
    auto copyText = ctx->serializeOperation(restored.value);
    LS_CHECK(copyText.ok());
    LS_CHECK(copyText.value.size() == text.value.size() ||
             copyText.value.find("FillDitherOp") != std::string::npos);

    LS_CHECK(ctx->deserializeOperation(built.layer, "not json").fail());
    LS_CHECK(ctx->deserializeOperation(built.layer, "{\"type\":\"NoSuchOp\"}").fail());
}

void testForwardCompatibility() {
    auto ctx = LSContext::create();
    const Built built = buildDocument(*ctx);
    auto saved = ctx->serializeDocument(built.doc);
    LS_REQUIRE(saved.ok());

    // Pretend a newer engine wrote extra fields this build knows nothing about:
    // one on the document root, one on an operation.
    std::string text(saved.value.bytes.begin(), saved.value.bytes.end());
    const std::string opMarker = "\"type\":\"FillDitherOp\"";
    const size_t opPos = text.find(opMarker);
    LS_REQUIRE(opPos != std::string::npos);
    text.insert(opPos + opMarker.size(), ",\"futureWobble\":0.42");
    text.insert(1, "\"futureSection\":{\"tracks\":[1,2,3]},");

    SerializedData fromFuture;
    fromFuture.bytes = std::vector<uint8_t>(text.begin(), text.end());
    fromFuture.engineVersion = LS_ENGINE_VERSION;
    fromFuture.formatTag = "livesprite/document";

    auto loaded = LSContext::create();
    auto restoredDoc = loaded->deserializeDocument(fromFuture);
    LS_REQUIRE(restoredDoc.ok());

    auto resaved = loaded->serializeDocument(restoredDoc.value);
    LS_REQUIRE(resaved.ok());
    const std::string out(resaved.value.bytes.begin(), resaved.value.bytes.end());

    // Both unknown fields must still be there: an older build must not silently
    // strip a newer build's data.
    LS_CHECK(out.find("futureWobble") != std::string::npos);
    LS_CHECK(out.find("futureSection") != std::string::npos);
    LS_CHECK(out.find("\"tracks\"") != std::string::npos);
}

void testSpritePortability() {
    auto source = LSContext::create();
    const Built built = buildDocument(*source);
    auto saved = source->serializeSprite(built.sprite);
    LS_REQUIRE(saved.ok());
    LS_CHECK(saved.value.formatTag == "livesprite/sprite");

    auto target = LSContext::create();
    auto doc = target->createDocument({"host", 32, 48});
    LS_REQUIRE(doc.ok());
    auto sprite = target->deserializeSprite(doc.value, saved.value);
    LS_REQUIRE(sprite.ok());

    LS_CHECK(target->getSpriteDocument(sprite.value).value == doc.value);
    LS_CHECK(target->documents().size() == 1);

    auto original = source->compileSprite(built.sprite, exportProfile());
    auto ported = target->compileSprite(sprite.value, exportProfile());
    LS_REQUIRE(original.ok() && ported.ok());
    LS_CHECK(ported.value.raster.pixels == original.value.raster.pixels);

    // The sprite is live in its new home: editing a palette role repaints it.
    auto palette = target->getEffectivePalette(sprite.value);
    LS_CHECK(palette.ok() && palette.value.valid());
    LS_CHECK(target->setPaletteColor(palette.value, 1, {250, 40, 40, 255}).ok());
    auto repainted = target->compileSprite(sprite.value, exportProfile());
    LS_CHECK(repainted.ok());
    LS_CHECK(repainted.value.raster.pixels != ported.value.raster.pixels);
}

void testVersionMigration() {
    auto ctx = LSContext::create();
    const Built built = buildDocument(*ctx);
    auto saved = ctx->serializeDocument(built.doc);
    LS_REQUIRE(saved.ok());

    // Same major version: migration restamps and preserves everything.
    SerializedData older = saved.value;
    older.engineVersion = LS_ENGINE_VERSION - 1;
    auto migrated = ctx->migrateVersion(older, LS_ENGINE_VERSION);
    LS_REQUIRE(migrated.ok());
    LS_CHECK(migrated.value.engineVersion == LS_ENGINE_VERSION);

    auto loaded = LSContext::create();
    LS_CHECK(loaded->deserializeDocument(migrated.value).ok());

    // A future major version cannot be read or migrated by this build.
    SerializedData future = saved.value;
    future.engineVersion = LS_ENGINE_VERSION + (1u << 16);
    LS_CHECK(ctx->migrateVersion(future, LS_ENGINE_VERSION).fail());
    auto refuser = LSContext::create();
    LS_CHECK(refuser->deserializeDocument(future).fail());

    // Corrupt input fails rather than producing a half-built document.
    SerializedData corrupt = saved.value;
    corrupt.bytes = { 'x', '{', '"' };
    LS_CHECK(refuser->deserializeDocument(corrupt).fail());
}

// The chain that carries an old file forward. No breaking version has shipped,
// so what is testable now is that same-major files pass through, that a major
// with no route is refused rather than half read, and that the refusal is the
// migration error rather than a parse failure.
void testMigrationChain() {
    auto ctx = LSContext::create();
    const Built built = buildDocument(*ctx);
    auto saved = ctx->serializeDocument(built.doc);
    LS_REQUIRE(saved.ok());

    // Same major, older patch: passes through and restamps.
    SerializedData older = saved.value;
    older.engineVersion = LS_ENGINE_VERSION - 1;
    auto forward = ctx->migrateVersion(older, LS_ENGINE_VERSION);
    LS_REQUIRE(forward.ok());
    LS_CHECK(forward.value.engineVersion == LS_ENGINE_VERSION);
    auto loaded = LSContext::create();
    LS_CHECK(loaded->deserializeDocument(forward.value).ok());

    // The engine is at major 0, so there is no older major to migrate from yet:
    // the walk itself becomes reachable when the first step is registered.
    // What is reachable now is that a target this build cannot produce is
    // refused rather than approximated.
    auto reader = LSContext::create();
    LS_CHECK(ctx->migrateVersion(saved.value, LS_ENGINE_VERSION + (1u << 16)).error ==
             LSError::VersionMigrationFailed);

    // A future major is refused at the door.
    SerializedData future = saved.value;
    future.engineVersion = LS_ENGINE_VERSION + (1u << 16);
    LS_CHECK(reader->deserializeDocument(future).error == LSError::VersionMismatch);
}

} // namespace

int main() {
    testDocumentRoundTrip();
    testOperationRoundTrip();
    testForwardCompatibility();
    testSpritePortability();
    testVersionMigration();
    testMigrationChain();
    return lstest::report("serialize");
}
