// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors
// assembly_tests.cpp — pivots, sockets and attachments under pressure: every
// edit to the graph reaching an assembly already compiled, the graph's edges
// kept when things are deleted, moved, undone or re-parented, quarter turns
// and mirrors of a whole assembly landing exactly, and a sprite turned about
// its pivot turning about where that pivot is.

#include "ls_test.h"

#include <livesprite/livesprite.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ls;

namespace {

constexpr uint32_t kSize = 48;
const Color kBody   { 200, 200, 220, 255 };
const Color kNotch  { 90, 40, 40, 255 };
const Color kSword  { 220, 120, 60, 255 };
const Color kTip    { 250, 240, 120, 255 };
const Color kGem    { 60, 160, 220, 255 };

CompileProfile profile() {
    CompileProfile p;
    p.type = CompileProfileType::Export;
    p.outputWidth = kSize;
    p.outputHeight = kSize;
    p.palette = PalettePolicy::Unconstrained;
    return p;
}

bool near(float a, float b, float tolerance = 0.001f) {
    return std::fabs(a - b) <= tolerance;
}

bool nearPoint(Vec2f p, Vec2f q, float tolerance = 0.001f) {
    return near(p.x, q.x, tolerance) && near(p.y, q.y, tolerance);
}

void fill(LSContext& ctx, DocumentId doc, LayerId layer, Vec2f origin, Vec2f size, Color colour) {
    const GeometryId rect = ctx.createRect(doc, { origin, size.x, size.y, 0.f }).value;
    FillSolidOp op;
    op.targetRegion = ctx.createRegionFromGeometry(rect).value;
    op.fallbackColor = colour;
    ctx.addOperation(layer, op);
}

int count(const RasterBuffer& raster, Color colour) {
    int n = 0;
    for (uint32_t y = 0; y < raster.height; ++y) {
        for (uint32_t x = 0; x < raster.width; ++x) {
            n += readPixel(raster, static_cast<int32_t>(x), static_cast<int32_t>(y)) == colour ? 1 : 0;
        }
    }
    return n;
}

// A body with a notch (so no turn of it looks like another), a sword hung
// from its grip -- turned a quarter by the socket -- with a tip, and a gem
// hung from the sword's pommel. Every corner, socket and pivot on the grid
// of pixel corners, so a quarter turn about one sends each pixel to a pixel.
struct Rig {
    DocumentId doc;
    SpriteId body, sword, gem;
    LayerId bodyLayer, swordLayer, gemLayer;
    SocketId grip, pommel;
    PivotId hilt, gemRoot;
};

Rig makeRig(LSContext& ctx) {
    Rig r;
    r.doc = ctx.createDocument({ "rig", kSize, kSize }).value;
    r.body = ctx.createSprite(r.doc).value;
    r.bodyLayer = ctx.createLayer(r.body, { "body" }).value;
    fill(ctx, r.doc, r.bodyLayer, { 16.f, 14.f }, { 10.f, 18.f }, kBody);
    fill(ctx, r.doc, r.bodyLayer, { 16.f, 14.f }, { 3.f, 2.f }, kNotch);
    ctx.addSocket(r.body, { "root", { 21.f, 23.f }, 0.f });      // marks the body in a copy
    r.grip = ctx.addSocket(r.body, { "grip", { 26.f, 20.f }, 90.f }).value;

    r.sword = ctx.createSprite(r.doc).value;
    r.swordLayer = ctx.createLayer(r.sword, { "sword" }).value;
    fill(ctx, r.doc, r.swordLayer, { 0.f, 0.f }, { 3.f, 12.f }, kSword);
    fill(ctx, r.doc, r.swordLayer, { 1.f, 0.f }, { 1.f, 2.f }, kTip);
    r.hilt = ctx.createPivot(r.sword, PivotDesc{ "hilt", { 1.f, 11.f } }).value;
    r.pommel = ctx.addSocket(r.sword, { "pommel", { 3.f, 11.f }, 0.f }).value;

    r.gem = ctx.createSprite(r.doc).value;
    r.gemLayer = ctx.createLayer(r.gem, { "gem" }).value;
    fill(ctx, r.doc, r.gemLayer, { 0.f, 0.f }, { 2.f, 3.f }, kGem);
    r.gemRoot = ctx.createPivot(r.gem, PivotDesc{ "root", { 0.f, 1.f } }).value;

    AttachmentDesc sword;
    sword.socket = r.grip;
    sword.childPivot = r.hilt;
    ctx.attachSprite(r.sword, sword);
    AttachmentDesc gem;
    gem.socket = r.pommel;
    gem.childPivot = r.gemRoot;
    ctx.attachSprite(r.gem, gem);
    return r;
}

// The same document compiled from scratch: saved, loaded into a context that
// has never compiled anything, its body found by its "root" socket. What a
// cached assembly must match.
RasterBuffer fresh(LSContext& ctx, DocumentId doc) {
    auto saved = ctx.serializeDocument(doc);
    auto other = LSContext::create();
    if (saved.fail() || other->deserializeDocument(saved.value).fail()) {
        return {};
    }
    for (uint64_t id = 1; id < 8192; ++id) {
        const SpriteId sprite { id };
        if (other->getSpriteInfo(sprite).ok() && other->findSocket(sprite, "root").ok()) {
            auto compiled = other->compileAssembly(sprite, profile());
            return compiled.ok() ? compiled.value.raster : RasterBuffer{};
        }
    }
    return {};
}

RasterBuffer assembled(LSContext& ctx, SpriteId root) {
    auto compiled = ctx.compileAssembly(root, profile());
    return compiled.ok() ? compiled.value.raster : RasterBuffer{};
}

bool agrees(LSContext& ctx, const Rig& r, const char* after) {
    const RasterBuffer cached = assembled(ctx, r.body);
    const RasterBuffer scratch = fresh(ctx, r.doc);
    const bool same = !cached.empty() && cached.pixels == scratch.pixels;
    if (!same) {
        std::printf("    after %s: the assembly compiled before is still what it gives\n", after);
    }
    return same;
}

// --- every edit reaches an assembly already compiled -----------------------

void testEditsReachACompiledAssembly() {
    auto ctx = LSContext::create();
    const Rig r = makeRig(*ctx);
    LS_CHECK(agrees(*ctx, r, "building it"));

    const struct { const char* name; std::function<void()> edit; } edits[] = {
        { "moving the grip",         [&] { ctx->moveSocket(r.grip, { 27.f, 22.f }); } },
        { "turning the grip",        [&] { ctx->rotateSocket(r.grip, 180.f); } },
        { "scaling the grip",        [&] { ctx->setSocketScale(r.grip, { 2.f, 1.f }); } },
        { "moving the hilt pivot",   [&] { ctx->setPivot(r.hilt, { 1.f, 6.f }); } },
        { "nudging the hilt pivot",  [&] { ctx->movePivot(r.hilt, { 1.f, 0.f }); } },
        { "placing the hilt pivot",  [&] { ctx->placePivot(r.hilt, PivotPlacement::ContentTop, profile()); } },
        { "moving the pommel",       [&] { ctx->moveSocket(r.pommel, { 0.f, 0.f }); } },
        { "moving the gem's pivot",  [&] { ctx->setPivot(r.gemRoot, { 1.f, 2.f }); } },
        { "placing the sword",       [&] { ctx->setSpriteTransform(r.sword, Mat3f::translation({ 1.f, 1.f })); } },
        { "placing the body",        [&] { ctx->translateSprite(r.body, { 2.f, 0.f }); } },
        { "a joint offset",          [&] {
              AttachmentDesc again;
              again.socket = r.grip;
              again.childPivot = r.hilt;
              again.localOffset = Mat3f::translation({ 0.f, -3.f });
              ctx->attachSprite(r.sword, again);
          } },
        { "hanging it behind",       [&] {
              AttachmentDesc behind;
              behind.socket = r.grip;
              behind.childPivot = r.hilt;
              behind.behindParent = true;
              ctx->attachSprite(r.sword, behind);
          } },
        { "detaching the gem",       [&] { ctx->detachSprite(r.gem); } },
        { "removing the grip",       [&] { ctx->removeSocket(r.grip); } },
    };
    for (const auto& e : edits) {
        assembled(*ctx, r.body);             // compiled, and cached, before the edit
        e.edit();
        LS_CHECK(agrees(*ctx, r, e.name));
    }
}

// A child hung from another socket leaves the assembly it was in.
void testReattachingLeavesTheOldParent() {
    auto ctx = LSContext::create();
    const Rig r = makeRig(*ctx);
    const SpriteId stand = ctx->createSprite(r.doc).value;
    const LayerId standLayer = ctx->createLayer(stand, { "stand" }).value;
    fill(*ctx, r.doc, standLayer, { 2.f, 40.f }, { 10.f, 4.f }, kNotch);
    const SocketId rack = ctx->addSocket(stand, { "rack", { 6.f, 40.f }, 0.f }).value;

    LS_CHECK(count(assembled(*ctx, r.body), kSword) > 0);
    AttachmentDesc onRack;
    onRack.socket = rack;
    onRack.childPivot = r.hilt;
    LS_REQUIRE(ctx->attachSprite(r.sword, onRack).ok());
    LS_CHECK(count(assembled(*ctx, r.body), kSword) == 0);
    LS_CHECK(count(assembled(*ctx, r.body), kGem) == 0);        // the gem went with the sword
    LS_CHECK(count(assembled(*ctx, stand), kSword) == 3 * 12 - 2);
    LS_CHECK(count(assembled(*ctx, stand), kGem) == 6);
}

// --- the graph's edges when things go away ----------------------------------

// The pivot a child presents is how it hangs: deleting it lets the child go,
// as removing the socket does, rather than leaving it half attached.
void testDeletingAPresentedPivotDetaches() {
    auto ctx = LSContext::create();
    const Rig r = makeRig(*ctx);
    assembled(*ctx, r.body);
    LS_REQUIRE(ctx->deletePivot(r.hilt).ok());
    LS_CHECK(ctx->getAttachment(r.sword).fail());
    LS_CHECK(count(assembled(*ctx, r.body), kSword) == 0);
    LS_CHECK(agrees(*ctx, r, "deleting the hilt"));
}

// Deleting a sprite in the middle of a chain releases what hung from it.
void testDeletingAMiddleSpriteReleasesItsChildren() {
    auto ctx = LSContext::create();
    const Rig r = makeRig(*ctx);
    assembled(*ctx, r.body);
    LS_REQUIRE(ctx->deleteSprite(r.sword).ok());
    LS_CHECK(ctx->getAttachment(r.gem).fail());
    LS_CHECK(count(assembled(*ctx, r.body), kGem) == 0);
    LS_CHECK(ctx->assemblyOrder(r.body).value.size() == 1);
    LS_CHECK(count(assembled(*ctx, r.gem), kGem) == 6);         // still a sprite of its own
}

// A socket and a child in different documents cannot make one assembly.
void testAttachingAcrossDocumentsIsRefused() {
    auto ctx = LSContext::create();
    const Rig r = makeRig(*ctx);
    const DocumentId other = ctx->createDocument({ "other", kSize, kSize }).value;
    const SpriteId stray = ctx->createSprite(other).value;
    const PivotId strayRoot = ctx->createPivot(stray, { 0.f, 0.f }).value;
    AttachmentDesc across;
    across.socket = r.grip;
    across.childPivot = strayRoot;
    LS_CHECK(ctx->attachSprite(stray, across).fail());
    LS_CHECK(ctx->getAttachment(stray).fail());
}

// A loop is refused however long the chain it would close.
void testALongChainCannotCloseALoop() {
    auto ctx = LSContext::create();
    const DocumentId doc = ctx->createDocument({ "chain", kSize, kSize }).value;
    constexpr int kLinks = 1100;
    std::vector<SpriteId> links;
    std::vector<SocketId> ends;
    for (int i = 0; i < kLinks; ++i) {
        links.push_back(ctx->createSprite(doc).value);
        ctx->createPivot(links.back(), { 0.f, 0.f });
        ends.push_back(ctx->addSocket(links.back(), { "end", { 1.f, 0.f }, 0.f }).value);
        if (i > 0) {
            LS_REQUIRE(ctx->attachSprite(links[i], ends[i - 1]).ok());
        }
    }
    // The first link onto the last one's end: round in a circle.
    LS_CHECK(ctx->attachSprite(links.front(), ends.back()).error == LSError::DependencyCycle);
    LS_CHECK(ctx->getAttachment(links.front()).fail());
    // And the chain still resolves: the last link is kLinks - 1 along.
    auto last = ctx->getSpriteWorldTransform(links.back());
    LS_REQUIRE(last.ok());
    LS_CHECK(nearPoint(last.value.transformPoint({ 0.f, 0.f }), { kLinks - 1.f, 0.f }));
}

// --- undo, and live instructions --------------------------------------------

void testSnapshotsCarryTheGraph() {
    auto ctx = LSContext::create();
    const Rig r = makeRig(*ctx);
    const RasterBuffer before = assembled(*ctx, r.body);
    auto snapshot = ctx->snapshotDocumentState(r.doc);
    LS_REQUIRE(snapshot.ok());

    // An action: the gem off, the grip turned, the body moved.
    ctx->detachSprite(r.gem);
    ctx->rotateSocket(r.grip, 0.f);
    ctx->translateSprite(r.body, { 3.f, 3.f });
    const RasterBuffer during = assembled(*ctx, r.body);
    LS_CHECK(during.pixels != before.pixels);
    auto after = ctx->snapshotDocumentState(r.doc);
    LS_REQUIRE(after.ok());

    // Undone: the graph, and the assembly already compiled, as they were.
    LS_REQUIRE(ctx->restoreDocumentState(r.doc, snapshot.value).ok());
    LS_CHECK(ctx->getAttachment(r.gem).ok());
    LS_CHECK(near(ctx->getSocket(r.grip).value.angle, 90.f));
    LS_CHECK(assembled(*ctx, r.body).pixels == before.pixels);

    // Redone.
    LS_REQUIRE(ctx->restoreDocumentState(r.doc, after.value).ok());
    LS_CHECK(ctx->getAttachment(r.gem).fail());
    LS_CHECK(assembled(*ctx, r.body).pixels == during.pixels);
}

void testInstructionsAttachAndDetach() {
    auto ctx = LSContext::create();
    const Rig r = makeRig(*ctx);
    LS_REQUIRE(ctx->detachSprite(r.sword).ok());
    LS_CHECK(count(assembled(*ctx, r.body), kSword) == 0);

    AttachInstruction attach;
    attach.child = r.sword;
    attach.attachment.socket = r.grip;
    attach.attachment.childPivot = r.hilt;
    LS_REQUIRE(ctx->applyInstructions({ Instruction{ attach } }).ok());
    LS_CHECK(ctx->getAttachment(r.sword).ok());
    LS_CHECK(count(assembled(*ctx, r.body), kSword) == 3 * 12 - 2);

    DetachInstruction detach;
    detach.child = r.sword;
    LS_REQUIRE(ctx->applyInstructions({ Instruction{ detach } }).ok());
    LS_CHECK(count(assembled(*ctx, r.body), kSword) == 0);
}

// --- a whole assembly turned and mirrored lands exactly ----------------------

// Where the pixel (x, y) of the canvas lands.
using Landing = Vec2i (*)(int, int);
Vec2i quarter(int x, int y)      { return { static_cast<int>(kSize) - 1 - y, x }; }
Vec2i half(int x, int y)         { return { static_cast<int>(kSize) - 1 - x, static_cast<int>(kSize) - 1 - y }; }
Vec2i threeQuarters(int x, int y) { return { y, static_cast<int>(kSize) - 1 - x }; }
Vec2i across(int x, int y)       { return { static_cast<int>(kSize) - 1 - x, y }; }

int offBy(const RasterBuffer& before, const RasterBuffer& after, Landing land) {
    int off = 0;
    for (int y = 0; y < static_cast<int>(kSize); ++y) {
        for (int x = 0; x < static_cast<int>(kSize); ++x) {
            const Vec2i to = land(x, y);
            off += readPixel(before, x, y) == readPixel(after, to.x, to.y) ? 0 : 1;
        }
    }
    return off;
}

void testTurnedAndMirroredAssembliesAreExact() {
    auto ctx = LSContext::create();
    const Rig r = makeRig(*ctx);
    const RasterBuffer still = assembled(*ctx, r.body);
    LS_REQUIRE(count(still, kSword) == 3 * 12 - 2 && count(still, kGem) == 6);
    const Vec2f middle { kSize * 0.5f, kSize * 0.5f };

    const struct { const char* name; Mat3f move; Landing land; } moves[] = {
        { "90",     Mat3f::aroundPivot(Mat3f::rotation(90.f), middle),  quarter },
        { "180",    Mat3f::aroundPivot(Mat3f::rotation(180.f), middle), half },
        { "270",    Mat3f::aroundPivot(Mat3f::rotation(270.f), middle), threeQuarters },
        { "-90",    Mat3f::aroundPivot(Mat3f::rotation(-90.f), middle), threeQuarters },
        { "mirror", Mat3f::aroundPivot(Mat3f::scaling({ -1.f, 1.f }), middle), across },
    };
    for (const auto& m : moves) {
        LS_REQUIRE(ctx->setSpriteTransform(r.body, m.move).ok());
        const int off = offBy(still, assembled(*ctx, r.body), m.land);
        if (off != 0) {
            std::printf("    body turned %s: %d pixel(s) of the assembly off\n", m.name, off);
        }
        LS_CHECK(off == 0);
    }
    LS_REQUIRE(ctx->setSpriteTransform(r.body, Mat3f::identity()).ok());
    LS_CHECK(assembled(*ctx, r.body).pixels == still.pixels);
}

// Turning the socket a further quarter turns the sword about the grip.
void testATurnedSocketTurnsItsChildExactly() {
    auto ctx = LSContext::create();
    const Rig r = makeRig(*ctx);
    ctx->detachSprite(r.gem);
    const RasterBuffer before = assembled(*ctx, r.body);
    LS_REQUIRE(ctx->rotateSocket(r.grip, 180.f).ok());
    const RasterBuffer after = assembled(*ctx, r.body);
    // The grip is the corner (26, 20): a quarter turn about it sends the
    // pixel (x, y) to (26 + 20 - 1 - y, 20 - 26 + x) = (45 - y, x - 6).
    int swordBefore = 0, landed = 0;
    for (int y = 0; y < static_cast<int>(kSize); ++y) {
        for (int x = 0; x < static_cast<int>(kSize); ++x) {
            const Color c = readPixel(before, x, y);
            if (c == kSword || c == kTip) {
                ++swordBefore;
                landed += readPixel(after, 45 - y, x - 6) == c ? 1 : 0;
            }
        }
    }
    LS_CHECK(swordBefore == 3 * 12);
    LS_CHECK(landed == swordBefore);
}

// --- turning a sprite about its pivot ---------------------------------------

// A sprite turned, scaled or mirrored about a pivot keeps that pivot where it
// is -- wherever the sprite already stands.
void testTurningAboutAPivotKeepsThePivot() {
    auto ctx = LSContext::create();
    const Rig r = makeRig(*ctx);
    const PivotId centre = ctx->createPivot(r.body, PivotDesc{ "centre", { 21.f, 23.f } }).value;
    LS_REQUIRE(ctx->translateSprite(r.body, { 10.f, -4.f }).ok());
    const Vec2f at = ctx->getPivotWorldPosition(centre).value;
    LS_CHECK(nearPoint(at, { 31.f, 19.f }));

    LS_REQUIRE(ctx->rotateSprite(r.body, 90.f, centre).ok());
    LS_CHECK(nearPoint(ctx->getPivotWorldPosition(centre).value, at));
    LS_REQUIRE(ctx->scaleSprite(r.body, { 2.f, 2.f }, centre).ok());
    LS_CHECK(nearPoint(ctx->getPivotWorldPosition(centre).value, at));
    LS_REQUIRE(ctx->mirrorSprite(r.body, MirrorAxis::X, centre).ok());
    LS_CHECK(nearPoint(ctx->getPivotWorldPosition(centre).value, at));
}

// A pivot placed from what the sprite draws is placed in the sprite's own
// space: a sprite standing elsewhere gets the same pivot on the same drawing.
void testPlacingAPivotOnAMovedSprite() {
    auto ctx = LSContext::create();
    const Rig r = makeRig(*ctx);
    const PivotId centre = ctx->createPivot(r.body, { 0.f, 0.f }).value;
    LS_REQUIRE(ctx->placePivot(centre, PivotPlacement::ContentCenter, profile()).ok());
    const Vec2f unmoved = ctx->getPivot(centre).value;
    LS_CHECK(nearPoint(unmoved, { 21.f, 23.f }));

    LS_REQUIRE(ctx->setSpriteTransform(r.body, Mat3f::translation({ 8.f, 2.f })).ok());
    LS_REQUIRE(ctx->placePivot(centre, PivotPlacement::ContentCenter, profile()).ok());
    LS_CHECK(nearPoint(ctx->getPivot(centre).value, unmoved));
    LS_CHECK(nearPoint(ctx->getPivotWorldPosition(centre).value, { 29.f, 25.f }));
}

// --- order, and the pieces of the graph that ride along ---------------------

// Behind a parent means the whole branch behind it; in front of the parent in
// that branch still means in front.
void testBehindIsABranchOrder() {
    auto ctx = LSContext::create();
    const DocumentId doc = ctx->createDocument({ "order", kSize, kSize }).value;
    const auto part = [&](const char* name) {
        const SpriteId s = ctx->createSprite(doc).value;
        ctx->createPivot(s, PivotDesc{ name, { 0.f, 0.f } });
        return s;
    };
    const SpriteId body = part("body"), cape = part("cape"), clasp = part("clasp"),
                   lining = part("lining"), hat = part("hat");
    const SocketId back = ctx->addSocket(body, { "back", { 0.f, 0.f }, 0.f }).value;
    const SocketId head = ctx->addSocket(body, { "head", { 0.f, 0.f }, 0.f }).value;
    const SocketId neck = ctx->addSocket(cape, { "neck", { 0.f, 0.f }, 0.f }).value;
    const auto hang = [&](SpriteId child, SocketId socket, bool behind) {
        AttachmentDesc a;
        a.socket = socket;
        a.behindParent = behind;
        return ctx->attachSprite(child, a).ok();
    };
    LS_REQUIRE(hang(cape, back, true));
    LS_REQUIRE(hang(clasp, neck, false));
    LS_REQUIRE(hang(lining, neck, true));
    LS_REQUIRE(hang(hat, head, false));
    const std::vector<SpriteId> expected { lining, cape, clasp, body, hat };
    LS_CHECK(ctx->assemblyOrder(body).value == expected);
}

// A pivot chosen as the sprite's own, socket scale and angle, a joint offset
// and hanging behind all survive a save and a load.
void testTheGraphSurvivesSaveAndLoad() {
    auto ctx = LSContext::create();
    Rig r = makeRig(*ctx);
    const PivotId second = ctx->createPivot(r.gem, PivotDesc{ "second", { 2.f, 3.f } }).value;
    LS_REQUIRE(ctx->setSpritePivot(r.gem, second).ok());
    LS_REQUIRE(ctx->setSocketScale(r.pommel, { -1.f, 1.f }).ok());
    AttachmentDesc again;
    again.socket = r.grip;
    again.childPivot = r.hilt;
    again.localOffset = Mat3f::translation({ 0.f, -2.f });
    again.behindParent = true;
    LS_REQUIRE(ctx->attachSprite(r.sword, again).ok());
    const RasterBuffer before = assembled(*ctx, r.body);
    LS_CHECK(fresh(*ctx, r.doc).pixels == before.pixels);

    auto saved = ctx->serializeDocument(r.doc);
    auto other = LSContext::create();
    LS_REQUIRE(saved.ok() && other->deserializeDocument(saved.value).ok());
    SpriteId gem;
    for (uint64_t id = 1; id < 8192 && !gem.valid(); ++id) {
        const SpriteId s { id };
        if (other->getSpriteInfo(s).ok() && other->findPivot(s, "second").ok()) {
            gem = s;
        }
    }
    LS_REQUIRE(gem.valid());
    LS_CHECK(other->getSpriteInfo(gem).value.pivot == other->findPivot(gem, "second").value);
    const SocketId pommel = other->getAttachment(gem).value.socket;
    LS_CHECK(near(other->getSocket(pommel).value.scale.x, -1.f));
    const SpriteId sword = other->getAttachment(gem).value.parent;
    auto swordHang = other->getAttachment(sword);
    LS_REQUIRE(swordHang.ok());
    LS_CHECK(swordHang.value.behindParent);
    LS_CHECK(near(swordHang.value.localOffset.m[5], -2.f));
}

// A copy of a sprite is a copy of its sockets and pivots, with nothing yet
// hung from its sockets: what hangs from the original stays there.
void testACloneCarriesItsAnchors() {
    auto ctx = LSContext::create();
    const Rig r = makeRig(*ctx);
    auto copy = ctx->cloneSprite(r.sword);
    LS_REQUIRE(copy.ok());
    auto pommel = ctx->findSocket(copy.value, "pommel");
    LS_REQUIRE(pommel.ok());
    LS_CHECK(pommel.value != r.pommel);
    LS_CHECK(ctx->getAttachedSprites(pommel.value).value.empty());
    LS_CHECK(ctx->getAttachedSprites(r.pommel).value.size() == 1);
    LS_CHECK(ctx->findPivot(copy.value, "hilt").ok());
}

} // namespace

int main() {
    // Unbuffered: a failure printed before a crash is still read.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testEditsReachACompiledAssembly();
    testReattachingLeavesTheOldParent();
    testDeletingAPresentedPivotDetaches();
    testDeletingAMiddleSpriteReleasesItsChildren();
    testAttachingAcrossDocumentsIsRefused();
    testSnapshotsCarryTheGraph();
    testInstructionsAttachAndDetach();
    testTurnedAndMirroredAssembliesAreExact();
    testATurnedSocketTurnsItsChildExactly();
    testTurningAboutAPivotKeepsThePivot();
    testPlacingAPivotOnAMovedSprite();
    testBehindIsABranchOrder();
    testTheGraphSurvivesSaveAndLoad();
    testACloneCarriesItsAnchors();
    // Last: where a loop is not caught, what follows it never finishes.
    testALongChainCannotCloseALoop();
    return lstest::report("assembly");
}
