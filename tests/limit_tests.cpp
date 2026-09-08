// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors

// limit_tests.cpp — what the engine refuses, and why.
//
// A canvas size arrives from an application or from a file somebody sent.
// Without a bound the second of those is a memory-safety problem rather than a
// performance one, so most of what is here is about the refusing.

#include "ls_test.h"

#include <livesprite/livesprite.h>

using namespace ls;

namespace {

CompileProfile profileOf(uint32_t width, uint32_t height) {
    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = width;
    profile.outputHeight = height;
    profile.palette = PalettePolicy::Unconstrained;
    return profile;
}

// The bug these limits exist for: past 2^30 the 32-bit stride overflows, and the
// raster came back reporting a width while holding no storage at all. Every
// bounds check in writePixel then passed on a buffer that was not there.
void testARasterNeverLiesAboutItsSize() {
    const uint32_t sizes[] = { 1u << 30, 1u << 31, 0xFFFFFFFFu, 65535, 40000 };
    for (uint32_t size : sizes) {
        RasterBuffer raster = makeRaster(size, 4);
        const uint64_t claimed =
            static_cast<uint64_t>(raster.width) * raster.height * 4;
        LS_CHECK(raster.pixels.size() >= claimed);
        LS_CHECK(raster.empty() || raster.stride >= raster.width * 4u);
    }

    // A size within the limits is still built normally.
    RasterBuffer fine = makeRaster(256, 256);
    LS_CHECK(fine.width == 256);
    LS_CHECK(fine.pixels.size() == 256u * 256u * 4u);
}

void testCanvasSizesAreBounded() {
    auto ctx = LSContext::create();

    LS_CHECK(ctx->createDocument({"ok", 4096, 4096}).ok());
    LS_CHECK(ctx->createDocument({"ok", 16384, 1024}).ok());   // wide, small area
    LS_CHECK(ctx->createDocument({"ok", 1, 1}).ok());

    LS_CHECK(ctx->createDocument({"no", 0, 32}).fail());
    LS_CHECK(ctx->createDocument({"no", 32, 0}).fail());

    // Past the per-side cap.
    LS_CHECK(ctx->createDocument({"no", 16385, 8}).fail());
    LS_CHECK(ctx->createDocument({"no", 65535, 65535}).fail());
    LS_CHECK(ctx->createDocument({"no", 0xFFFFFFFFu, 1}).fail());

    // Within the per-side cap but far past the area cap. This is the case a
    // dimension-only limit would let through: 1 GB of raster.
    LS_CHECK(ctx->createDocument({"no", 16384, 16384}).fail());
    LS_CHECK(ctx->createDocument({"no", 8192, 8192}).fail());

    // Exactly at the area cap is allowed; one row past it is not.
    LS_CHECK(ctx->createDocument({"edge", 4096, 4096}).ok());
    LS_CHECK(ctx->createDocument({"edge", 4096, 4097}).fail());
}

void testResizeIsBoundedToo() {
    auto ctx = LSContext::create();
    const DocumentId doc = ctx->createDocument({"resize", 32, 32}).value;

    LS_CHECK(ctx->setCanvasSize(doc, 512, 512).ok());
    LS_CHECK(ctx->setCanvasSize(doc, 65535, 65535).fail());
    LS_CHECK(ctx->setCanvasSize(doc, 0, 10).fail());

    // A refused resize leaves the canvas as it was rather than half applied.
    auto size = ctx->getCanvasSize(doc);
    LS_REQUIRE(size.ok());
    LS_CHECK(size.value.x == 512);
    LS_CHECK(size.value.y == 512);
}

// A compile profile carries its own output size, so it can ask for a raster
// without going near a document.
void testCompileOutputIsBounded() {
    auto ctx = LSContext::create();
    const DocumentId doc = ctx->createDocument({"compile", 32, 32}).value;
    const SpriteId sprite = ctx->createSprite(doc).value;
    const LayerId layer = ctx->createLayer(sprite, {"main"}).value;
    const GeometryId rect = ctx->createRect(doc, {{4.f, 4.f}, 8.f, 8.f, 0.f}).value;
    FillSolidOp fill;
    fill.targetRegion = ctx->createRegionFromGeometry(rect).value;
    ctx->addOperation(layer, fill);

    LS_CHECK(ctx->compileSprite(sprite, profileOf(32, 32)).ok());
    LS_CHECK(ctx->compileSprite(sprite, profileOf(1u << 30, 4)).fail());
    LS_CHECK(ctx->compileSprite(sprite, profileOf(65535, 65535)).fail());
    LS_CHECK(ctx->compileSprite(sprite, profileOf(16384, 16384)).fail());

    // Refusing one compile does not damage the sprite.
    LS_CHECK(ctx->compileSprite(sprite, profileOf(32, 32)).ok());
}

// The path that actually matters: a document that arrived from elsewhere.
void testAFileCannotDeclareAnAbsurdCanvas() {
    auto ctx = LSContext::create();
    const DocumentId doc = ctx->createDocument({"honest", 32, 32}).value;
    auto saved = ctx->serializeDocument(doc);
    LS_REQUIRE(saved.ok());

    std::string text(saved.value.bytes.begin(), saved.value.bytes.end());
    const std::string before = "\"canvasWidth\":32";
    LS_REQUIRE(text.find(before) != std::string::npos);

    // Rewrite the file the way an attacker or a broken exporter would.
    const std::string tampered =
        text.substr(0, text.find(before)) + "\"canvasWidth\":65535" +
        text.substr(text.find(before) + before.size());

    SerializedData data;
    data.bytes.assign(tampered.begin(), tampered.end());
    data.formatTag = "livesprite/document";

    auto reader = LSContext::create();
    auto loaded = reader->deserializeDocument(data);
    LS_CHECK(loaded.fail());

    // The untampered original still loads, so the check is not simply refusing
    // everything.
    auto honest = reader->deserializeDocument(saved.value);
    LS_CHECK(honest.ok());
}

// The point of separating the two: the default is a policy an application may
// change, while the ceiling is a property of the engine that it may not.
void testAnApplicationSetsItsOwnPolicy() {
    auto ctx = LSContext::create();

    LS_CHECK(ctx->canvasLimits().maxPixels == kDefaultMaxCanvasPixels);
    LS_CHECK(ctx->createDocument({"default", 8192, 8192}).fail());

    // A production tool willing to wait raises it.
    CanvasLimits generous;
    generous.maxDimension = 16384;
    generous.maxPixels = 8192ull * 8192ull;
    LS_REQUIRE(ctx->setCanvasLimits(generous).ok());
    LS_CHECK(ctx->canvasLimits().maxPixels == 8192ull * 8192ull);
    LS_CHECK(ctx->createDocument({"raised", 8192, 8192}).ok());

    // An editor for small sprites tightens it, and the tighter bound applies to
    // documents it has yet to create.
    auto small = LSContext::create();
    CanvasLimits strict;
    strict.maxDimension = 512;
    strict.maxPixels = 512ull * 512ull;
    LS_REQUIRE(small->setCanvasLimits(strict).ok());
    LS_CHECK(small->createDocument({"fine", 512, 512}).ok());
    LS_CHECK(small->createDocument({"no", 1024, 1024}).fail());

    // The ceiling is not a policy. No application may raise past what the engine
    // can represent, whatever it asks for.
    CanvasLimits absurd;
    absurd.maxDimension = kCanvasDimensionCeiling;
    LS_CHECK(ctx->setCanvasLimits(absurd).fail());

    absurd.maxDimension = 0xFFFFFFFFu;
    LS_CHECK(ctx->setCanvasLimits(absurd).fail());

    CanvasLimits zero;
    zero.maxDimension = 0;
    LS_CHECK(ctx->setCanvasLimits(zero).fail());

    // A refused change leaves the previous policy in place.
    LS_CHECK(ctx->canvasLimits().maxPixels == 8192ull * 8192ull);
}

// A file is read against the reader's policy, not the writer's, so a strict
// application is never handed a document it cannot display.
void testTheReaderUsesTheReadersPolicy() {
    auto generous = LSContext::create();
    CanvasLimits wide;
    wide.maxDimension = 16384;
    wide.maxPixels = 4096ull * 4096ull;
    LS_REQUIRE(generous->setCanvasLimits(wide).ok());

    const DocumentId doc = generous->createDocument({"big", 2048, 2048}).value;
    auto saved = generous->serializeDocument(doc);
    LS_REQUIRE(saved.ok());

    // The same file, opened by something that only works small.
    auto strict = LSContext::create();
    CanvasLimits small;
    small.maxDimension = 256;
    small.maxPixels = 256ull * 256ull;
    LS_REQUIRE(strict->setCanvasLimits(small).ok());
    LS_CHECK(strict->deserializeDocument(saved.value).fail());

    // And by something that can take it.
    auto peer = LSContext::create();
    LS_REQUIRE(peer->setCanvasLimits(wide).ok());
    LS_CHECK(peer->deserializeDocument(saved.value).ok());
}

} // namespace

int main() {
    testARasterNeverLiesAboutItsSize();
    testCanvasSizesAreBounded();
    testResizeIsBoundedToo();
    testCompileOutputIsBounded();
    testAFileCannotDeclareAnAbsurdCanvas();
    testAnApplicationSetsItsOwnPolicy();
    testTheReaderUsesTheReadersPolicy();
    return lstest::report("limits");
}
