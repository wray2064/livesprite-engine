// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors
// anchor_tests.cpp — pivots, sockets, and the attachment graph: the maths that
// makes one sprite hang off another and follow it.

#include "ls_test.h"

#include <livesprite/livesprite.h>

#include <cmath>
#include <vector>

using namespace ls;

namespace {

const float kPi = 3.14159265358979323846f;

CompileProfile exportProfile(uint32_t size = 48) {
    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = size;
    profile.outputHeight = size;
    profile.palette = PalettePolicy::Unconstrained;
    return profile;
}

bool near(float a, float b, float tolerance = 0.01f) {
    return std::fabs(a - b) <= tolerance;
}

bool nearPoint(Vec2f point, float x, float y, float tolerance = 0.01f) {
    return near(point.x, x, tolerance) && near(point.y, y, tolerance);
}

// A sprite that draws one solid block, so its position is readable in pixels.
struct Part {
    DocumentId doc;
    SpriteId sprite;
    LayerId layer;
    RegionId region;
};

Part makePart(LSContext& ctx, DocumentId doc, Vec2f origin, Vec2f size, Color color) {
    Part part;
    part.doc = doc;
    part.sprite = ctx.createSprite(doc).value;
    part.layer = ctx.createLayer(part.sprite, {"body"}).value;
    const GeometryId rect = ctx.createRect(doc, {origin, size.x, size.y, 0.f}).value;
    part.region = ctx.createRegionFromGeometry(rect).value;
    FillSolidOp fill;
    fill.targetRegion = part.region;
    fill.fallbackColor = color;
    ctx.addOperation(part.layer, fill);
    return part;
}

int countColor(const RasterBuffer& raster, Color color) {
    int count = 0;
    for (uint32_t y = 0; y < raster.height; ++y) {
        for (uint32_t x = 0; x < raster.width; ++x) {
            count += readPixel(raster, static_cast<int32_t>(x), static_cast<int32_t>(y)) == color
                ? 1 : 0;
        }
    }
    return count;
}

Vec2f centroidOf(const RasterBuffer& raster, Color color) {
    double sumX = 0.0, sumY = 0.0;
    int count = 0;
    for (uint32_t y = 0; y < raster.height; ++y) {
        for (uint32_t x = 0; x < raster.width; ++x) {
            if (readPixel(raster, static_cast<int32_t>(x), static_cast<int32_t>(y)) == color) {
                sumX += x;
                sumY += y;
                ++count;
            }
        }
    }
    if (count == 0) {
        return { -1.f, -1.f };
    }
    return { static_cast<float>(sumX / count), static_cast<float>(sumY / count) };
}

// --- pivots ----------------------------------------------------------------

void testPivotBasics() {
    auto ctx = LSContext::create();
    auto doc = ctx->createDocument({"pivots", 48, 48});
    LS_REQUIRE(doc.ok());
    const Part part = makePart(*ctx, doc.value, {10.f, 12.f}, {8.f, 6.f}, Color::white());

    auto named = ctx->createPivot(part.sprite, PivotDesc{"grip", {14.f, 15.f}});
    LS_REQUIRE(named.ok());
    LS_CHECK(ctx->getPivotName(named.value).value == "grip");
    LS_CHECK(ctx->findPivot(part.sprite, "grip").value == named.value);
    LS_CHECK(ctx->findPivot(part.sprite, "missing").fail());

    // The first pivot becomes the sprite pivot.
    LS_CHECK(ctx->getSpriteInfo(part.sprite).value.pivot == named.value);

    // Placement reads the compiled bounds, so it follows the artwork.
    LS_CHECK(ctx->placePivot(named.value, PivotPlacement::ContentCenter, exportProfile()).ok());
    LS_CHECK(nearPoint(ctx->getPivot(named.value).value, 14.f, 15.f));

    LS_CHECK(ctx->placePivot(named.value, PivotPlacement::ContentTopLeft, exportProfile()).ok());
    LS_CHECK(nearPoint(ctx->getPivot(named.value).value, 10.f, 12.f));

    LS_CHECK(ctx->placePivot(named.value, PivotPlacement::ContentBottom, exportProfile()).ok());
    LS_CHECK(nearPoint(ctx->getPivot(named.value).value, 14.f, 18.f));

    LS_CHECK(ctx->placePivot(named.value, PivotPlacement::CanvasCenter, exportProfile()).ok());
    LS_CHECK(nearPoint(ctx->getPivot(named.value).value, 24.f, 24.f));

    // An empty sprite has no content to centre on, and says so.
    const SpriteId empty = ctx->createSprite(doc.value).value;
    auto emptyPivot = ctx->createPivot(empty, {0.f, 0.f});
    LS_CHECK(ctx->placePivot(emptyPivot.value, PivotPlacement::ContentCenter, exportProfile())
                 .error == LSError::RegionEmpty);
}

// --- socket frames ---------------------------------------------------------

void testSocketFollowsSpriteTransform() {
    auto ctx = LSContext::create();
    auto doc = ctx->createDocument({"sockets", 48, 48});
    const Part arm = makePart(*ctx, doc.value, {20.f, 10.f}, {6.f, 20.f}, Color::white());

    auto socket = ctx->addSocket(arm.sprite, {"hand", {23.f, 30.f}, 0.f, {1.f, 1.f}});
    LS_REQUIRE(socket.ok());
    LS_CHECK(ctx->findSocket(arm.sprite, "hand").value == socket.value);
    LS_CHECK(ctx->getSocket(socket.value).value.name == "hand");

    // With no sprite transform, world equals local.
    LS_CHECK(nearPoint(ctx->getSocketWorldPosition(socket.value).value, 23.f, 30.f));

    // The core fix: turning the sprite must carry its sockets round with it.
    // A quarter turn about (24,24) sends (23,30) to (18,23).
    const Mat3f turn = Mat3f::aroundPivot(Mat3f::rotation(90.f), {24.f, 24.f});
    LS_CHECK(ctx->setSpriteTransform(arm.sprite, turn).ok());
    const Vec2f turned = ctx->getSocketWorldPosition(socket.value).value;
    LS_CHECK(nearPoint(turned, 18.f, 23.f));

    // The socket frame is turned too, not just moved: its x axis now points
    // down the canvas.
    const Mat3f world = ctx->getSocketWorldTransform(socket.value).value;
    const Vec2f axis = world.transformVector({1.f, 0.f});
    LS_CHECK(nearPoint(axis, 0.f, 1.f));

    // A pivot resolves through the same chain.
    auto pivot = ctx->createPivot(arm.sprite, {23.f, 30.f});
    LS_CHECK(nearPoint(ctx->getPivotWorldPosition(pivot.value).value, 18.f, 23.f));

    // Socket scale is part of the frame it imposes on whatever hangs from it.
    LS_CHECK(ctx->setSpriteTransform(arm.sprite, Mat3f::identity()).ok());
    LS_CHECK(ctx->setSocketScale(socket.value, {2.f, 2.f}).ok());
    const Mat3f scaled = ctx->getSocketWorldTransform(socket.value).value;
    LS_CHECK(nearPoint(scaled.transformVector({1.f, 0.f}), 2.f, 0.f));
    LS_CHECK(ctx->setSocketScale(socket.value, {0.f, 1.f}).fail());
}

// --- the attachment graph --------------------------------------------------

void testAttachmentChain() {
    auto ctx = LSContext::create();
    auto doc = ctx->createDocument({"chain", 48, 48});

    const Part body = makePart(*ctx, doc.value, {20.f, 20.f}, {8.f, 8.f}, Color::white());
    const Part arm  = makePart(*ctx, doc.value, {0.f, 0.f}, {4.f, 12.f}, Color::white());
    const Part hand = makePart(*ctx, doc.value, {0.f, 0.f}, {4.f, 4.f}, Color::white());

    const SocketId shoulder = ctx->addSocket(body.sprite, {"shoulder", {28.f, 22.f}, 0.f}).value;
    const PivotId armRoot = ctx->createPivot(arm.sprite, PivotDesc{"root", {2.f, 0.f}}).value;
    const SocketId wrist = ctx->addSocket(arm.sprite, {"wrist", {2.f, 12.f}, 0.f}).value;
    const PivotId handRoot = ctx->createPivot(hand.sprite, PivotDesc{"root", {2.f, 0.f}}).value;

    AttachmentDesc armToBody;
    armToBody.socket = shoulder;
    armToBody.childPivot = armRoot;
    LS_CHECK(ctx->attachSprite(arm.sprite, armToBody).ok());

    AttachmentDesc handToArm;
    handToArm.socket = wrist;
    handToArm.childPivot = handRoot;
    LS_CHECK(ctx->attachSprite(hand.sprite, handToArm).ok());

    // Two levels of chain: the hand ends up a full arm length below the
    // shoulder, at (28, 22) + (0, 12).
    LS_CHECK(nearPoint(ctx->getSpriteWorldTransform(arm.sprite).value.transformPoint({2.f, 0.f}),
                       28.f, 22.f));
    LS_CHECK(nearPoint(ctx->getPivotWorldPosition(handRoot).value, 28.f, 34.f));

    // Turning the arm swings the hand with it: rotate the arm 90 degrees about
    // its own root, and the hand lands on the swung wrist.
    LS_CHECK(ctx->setSpriteTransform(
        arm.sprite, Mat3f::aroundPivot(Mat3f::rotation(90.f), {2.f, 0.f})).ok());
    LS_CHECK(nearPoint(ctx->getPivotWorldPosition(handRoot).value, 16.f, 22.f));

    // The graph reports itself.
    auto info = ctx->getAttachment(hand.sprite);
    LS_REQUIRE(info.ok());
    LS_CHECK(info.value.parent == arm.sprite);
    LS_CHECK(info.value.socket == wrist);
    LS_CHECK(ctx->getAttachedSprites(shoulder).value.size() == 1);
    LS_CHECK(ctx->getAttachedSprites(wrist).value.front() == hand.sprite);

    auto order = ctx->assemblyOrder(body.sprite);
    LS_REQUIRE(order.ok());
    LS_CHECK(order.value.size() == 3);
    LS_CHECK(order.value[0] == body.sprite);
    LS_CHECK(order.value[1] == arm.sprite);
    LS_CHECK(order.value[2] == hand.sprite);

    // Detaching returns the sprite to its own frame.
    LS_CHECK(ctx->detachSprite(hand.sprite).ok());
    LS_CHECK(ctx->getAttachment(hand.sprite).fail());
    LS_CHECK(ctx->assemblyOrder(body.sprite).value.size() == 2);
    LS_CHECK(ctx->detachSprite(hand.sprite).fail());
}

void testAttachmentRejectsCycles() {
    auto ctx = LSContext::create();
    auto doc = ctx->createDocument({"cycles", 32, 32});
    const Part a = makePart(*ctx, doc.value, {0.f, 0.f}, {4.f, 4.f}, Color::white());
    const Part b = makePart(*ctx, doc.value, {0.f, 0.f}, {4.f, 4.f}, Color::white());

    const SocketId socketA = ctx->addSocket(a.sprite, {"mount", {2.f, 2.f}, 0.f}).value;
    const SocketId socketB = ctx->addSocket(b.sprite, {"mount", {2.f, 2.f}, 0.f}).value;
    const PivotId pivotA = ctx->createPivot(a.sprite, {0.f, 0.f}).value;
    const PivotId pivotB = ctx->createPivot(b.sprite, {0.f, 0.f}).value;

    // A sprite cannot hang off its own socket.
    AttachmentDesc selfAttach;
    selfAttach.socket = socketA;
    selfAttach.childPivot = pivotA;
    LS_CHECK(ctx->attachSprite(a.sprite, selfAttach).error == LSError::DependencyCycle);

    AttachmentDesc bOntoA;
    bOntoA.socket = socketA;
    bOntoA.childPivot = pivotB;
    LS_CHECK(ctx->attachSprite(b.sprite, bOntoA).ok());

    // Closing the loop the other way is refused rather than built.
    AttachmentDesc aOntoB;
    aOntoB.socket = socketB;
    aOntoB.childPivot = pivotA;
    LS_CHECK(ctx->attachSprite(a.sprite, aOntoB).error == LSError::DependencyCycle);

    // The pivot offered to a socket has to belong to the child.
    AttachmentDesc foreignPivot;
    foreignPivot.socket = socketA;
    foreignPivot.childPivot = pivotA;
    LS_CHECK(ctx->attachSprite(b.sprite, foreignPivot).error == LSError::InvalidParameter);
}

// --- assembly compile ------------------------------------------------------

void testAssemblyCompile() {
    auto ctx = LSContext::create();
    auto doc = ctx->createDocument({"assembly", 48, 48});

    const Color bodyColor {200, 200, 220, 255};
    const Color swordColor {220, 120, 60, 255};

    const Part body = makePart(*ctx, doc.value, {20.f, 18.f}, {8.f, 14.f}, bodyColor);
    const Part sword = makePart(*ctx, doc.value, {0.f, 0.f}, {3.f, 10.f}, swordColor);

    const SocketId grip = ctx->addSocket(body.sprite, {"grip", {30.f, 24.f}, 0.f}).value;
    const PivotId hilt = ctx->createPivot(sword.sprite, PivotDesc{"hilt", {1.f, 9.f}}).value;

    AttachmentDesc attach;
    attach.socket = grip;
    attach.childPivot = hilt;
    LS_CHECK(ctx->attachSprite(sword.sprite, attach).ok());

    auto assembled = ctx->compileAssembly(body.sprite, exportProfile());
    LS_REQUIRE(assembled.ok());

    // Both parts are present, and the sword sits at the grip: its hilt pixel
    // lands on the socket.
    LS_CHECK(countColor(assembled.value.raster, bodyColor) == 8 * 14);
    LS_CHECK(countColor(assembled.value.raster, swordColor) == 3 * 10);
    LS_CHECK(readPixel(assembled.value.raster, 30, 24) == swordColor);
    LS_CHECK(readPixel(assembled.value.raster, 30, 16) == swordColor);   // blade reaches up
    LS_CHECK(readPixel(assembled.value.raster, 30, 26).a == 0);          // and not below

    // Compiling the body alone leaves the sword out: assembly is the only thing
    // that pulls children in.
    auto alone = ctx->compileSprite(body.sprite, exportProfile());
    LS_CHECK(countColor(alone.value.raster, swordColor) == 0);

    // Turning the parent swings the child: the sword centroid moves with it.
    const Vec2f before = centroidOf(assembled.value.raster, swordColor);
    LS_CHECK(ctx->setSpriteTransform(
        body.sprite, Mat3f::aroundPivot(Mat3f::rotation(90.f), {24.f, 24.f})).ok());
    auto turned = ctx->compileAssembly(body.sprite, exportProfile());
    LS_REQUIRE(turned.ok());
    const Vec2f after = centroidOf(turned.value.raster, swordColor);
    LS_CHECK(countColor(turned.value.raster, swordColor) > 0);
    LS_CHECK(!nearPoint(after, before.x, before.y, 1.f));

    // The sword stayed on the grip through the turn.
    const Vec2f gripWorld = ctx->getSocketWorldPosition(grip).value;
    LS_CHECK(readPixel(turned.value.raster,
                       static_cast<int32_t>(std::floor(gripWorld.x)),
                       static_cast<int32_t>(std::floor(gripWorld.y))) == swordColor);
    LS_CHECK(ctx->setSpriteTransform(body.sprite, Mat3f::identity()).ok());

    // A child can be composited under its parent instead of over it.
    const Part pack = makePart(*ctx, doc.value, {0.f, 0.f}, {10.f, 10.f}, Color{60, 90, 60, 255});
    const SocketId backMount = ctx->addSocket(body.sprite, {"back", {24.f, 24.f}, 0.f}).value;
    const PivotId packRoot = ctx->createPivot(pack.sprite, PivotDesc{"root", {5.f, 5.f}}).value;
    AttachmentDesc behind;
    behind.socket = backMount;
    behind.childPivot = packRoot;
    behind.behindParent = true;
    LS_CHECK(ctx->attachSprite(pack.sprite, behind).ok());

    auto layered = ctx->compileAssembly(body.sprite, exportProfile());
    LS_REQUIRE(layered.ok());
    LS_CHECK(layered.value.raster.width == 48);
    // The body still wins where they overlap.
    LS_CHECK(readPixel(layered.value.raster, 24, 24) == bodyColor);
    // and the pack shows where the body does not cover it: the body starts at
    // x = 20, the pack reaches back to x = 19.
    LS_CHECK(readPixel(layered.value.raster, 19, 24) == Color{60, 90, 60, 255});

    auto order = ctx->assemblyOrder(body.sprite);
    LS_CHECK(order.value.front() == pack.sprite);   // drawn before the parent
}

// One way to place a sprite. The convenience helpers compose the sprite
// transform, so they move sockets and attached children too: an app never has
// to choose between two mechanisms that disagree.
void testStreamlinedPlacement() {
    auto ctx = LSContext::create();
    auto doc = ctx->createDocument({"placement", 48, 48});
    const Color bodyColor {200, 200, 220, 255};
    const Color swordColor {220, 120, 60, 255};

    const Part body = makePart(*ctx, doc.value, {20.f, 18.f}, {8.f, 14.f}, bodyColor);
    const Part sword = makePart(*ctx, doc.value, {0.f, 0.f}, {3.f, 10.f}, swordColor);
    const SocketId grip = ctx->addSocket(body.sprite, {"grip", {30.f, 24.f}, 0.f}).value;
    const PivotId bodyPivot = ctx->createPivot(body.sprite, PivotDesc{"root", {24.f, 24.f}}).value;
    ctx->createPivot(sword.sprite, PivotDesc{"hilt", {1.f, 9.f}});

    // The shorthand: the child offers its own pivot, no desc needed.
    LS_CHECK(ctx->attachSprite(sword.sprite, grip).ok());
    LS_CHECK(ctx->getAttachment(sword.sprite).value.parent == body.sprite);

    // Placing through the helper is the same thing as setting the transform:
    // the socket moves, so the attached sword moves with it.
    const Vec2f gripBefore = ctx->getSocketWorldPosition(grip).value;
    LS_CHECK(ctx->translateSprite(body.sprite, {4.f, 2.f}).ok());
    const Vec2f gripAfter = ctx->getSocketWorldPosition(grip).value;
    LS_CHECK(nearPoint(gripAfter, gripBefore.x + 4.f, gripBefore.y + 2.f));
    LS_CHECK(nearPoint(ctx->getPivotWorldPosition(
        ctx->getAttachment(sword.sprite).value.childPivot).value, gripAfter.x, gripAfter.y));

    // Helpers compose, and the composition is visible in one place.
    LS_CHECK(ctx->resetSpriteTransform(body.sprite).ok());
    LS_CHECK(ctx->rotateSprite(body.sprite, 90.f, bodyPivot).ok());
    const Mat3f expected = Mat3f::aroundPivot(Mat3f::rotation(90.f), {24.f, 24.f});
    const Mat3f actual = ctx->getSpriteTransform(body.sprite).value;
    bool sameMatrix = true;
    for (int i = 0; i < 9; ++i) {
        sameMatrix = sameMatrix && near(actual.m[i], expected.m[i]);
    }
    LS_CHECK(sameMatrix);

    // A quarter turn about (24,24) sends the grip at (30,24) to (24,30).
    LS_CHECK(nearPoint(ctx->getSocketWorldPosition(grip).value, 24.f, 30.f));

    // The rotation reached the compiled picture as well as the maths.
    auto assembled = ctx->compileAssembly(body.sprite, exportProfile());
    LS_REQUIRE(assembled.ok());
    LS_CHECK(readPixel(assembled.value.raster, 24, 30) == swordColor);

    // With no pivot named, a sprite turns about its own pivot.
    LS_CHECK(ctx->resetSpriteTransform(body.sprite).ok());
    LS_CHECK(ctx->rotateSprite(body.sprite, 90.f).ok());
    LS_CHECK(nearPoint(ctx->getSocketWorldPosition(grip).value, 24.f, 30.f));

    LS_CHECK(ctx->scaleSprite(body.sprite, {0.f, 1.f}).fail());
    LS_CHECK(ctx->resetSpriteTransform(body.sprite).ok());
    LS_CHECK(ctx->mirrorSprite(body.sprite, MirrorAxis::X, bodyPivot).ok());
    LS_CHECK(nearPoint(ctx->getSocketWorldPosition(grip).value, 18.f, 24.f));
}

// The removed operations must stay removed: a file naming one is refused rather
// than half loaded, so a stale document cannot resurrect a second mechanism.
void testRetiredOperationsAreRejected() {
    auto ctx = LSContext::create();
    auto doc = ctx->createDocument({"retired", 32, 32});
    const Part part = makePart(*ctx, doc.value, {4.f, 4.f}, {4.f, 4.f}, Color::white());

    LS_CHECK(ctx->deserializeOperation(part.layer,
        "{\"type\":\"AnchorTransformOp\",\"child\":0,\"socket\":0}").fail());
    LS_CHECK(ctx->deserializeOperation(part.layer,
        "{\"type\":\"PivotTransformOp\",\"angleDegrees\":90}").fail());

    // The op that replaced them is still there and still works.
    LS_CHECK(ctx->deserializeOperation(part.layer,
        "{\"type\":\"RotateOp\",\"angleDegrees\":90}").ok());
}

void testAssemblySurvivesSaveLoad() {
    auto ctx = LSContext::create();
    auto doc = ctx->createDocument({"persist", 48, 48});
    const Color bodyColor {180, 190, 210, 255};
    const Color swordColor {210, 130, 70, 255};

    const Part body = makePart(*ctx, doc.value, {20.f, 18.f}, {8.f, 14.f}, bodyColor);
    const Part sword = makePart(*ctx, doc.value, {0.f, 0.f}, {3.f, 10.f}, swordColor);
    const SocketId grip = ctx->addSocket(body.sprite, {"grip", {30.f, 24.f}, 15.f, {1.f, 1.f}}).value;
    const PivotId hilt = ctx->createPivot(sword.sprite, PivotDesc{"hilt", {1.f, 9.f}}).value;

    AttachmentDesc attach;
    attach.socket = grip;
    attach.childPivot = hilt;
    attach.localOffset = Mat3f::translation({0.f, -1.f});
    LS_CHECK(ctx->attachSprite(sword.sprite, attach).ok());
    LS_CHECK(ctx->setSpriteTransform(body.sprite, Mat3f::translation({2.f, 1.f})).ok());

    auto before = ctx->compileAssembly(body.sprite, exportProfile());
    LS_REQUIRE(before.ok());

    auto saved = ctx->serializeDocument(doc.value);
    LS_REQUIRE(saved.ok());
    auto loaded = LSContext::create();
    auto restoredDoc = loaded->deserializeDocument(saved.value);
    LS_REQUIRE(restoredDoc.ok());

    // Find the restored root: the sprite that carries a socket named "grip".
    SpriteId restoredBody;
    for (uint64_t candidate = 1; candidate < 4096; ++candidate) {
        const SpriteId sprite { candidate };
        if (loaded->getSpriteInfo(sprite).ok() && loaded->findSocket(sprite, "grip").ok()) {
            restoredBody = sprite;
            break;
        }
    }
    LS_REQUIRE(restoredBody.valid());

    auto after = loaded->compileAssembly(restoredBody, exportProfile());
    LS_REQUIRE(after.ok());
    LS_CHECK(after.value.raster.pixels == before.value.raster.pixels);

    // The graph itself came back, not just the picture.
    auto restoredSocket = loaded->findSocket(restoredBody, "grip");
    LS_REQUIRE(restoredSocket.ok());
    LS_CHECK(near(loaded->getSocket(restoredSocket.value).value.angle, 15.f));
    auto children = loaded->getAttachedSprites(restoredSocket.value);
    LS_REQUIRE(children.ok());
    LS_CHECK(children.value.size() == 1);
    auto restoredAttachment = loaded->getAttachment(children.value.front());
    LS_CHECK(restoredAttachment.ok());
    LS_CHECK(restoredAttachment.value.parent == restoredBody);
    LS_CHECK(loaded->getPivotName(restoredAttachment.value.childPivot).value == "hilt");
}

// Deleting or unsocketing must not leave a child holding a handle to something
// that is gone.
void testTeardownDetaches() {
    auto ctx = LSContext::create();
    auto doc = ctx->createDocument({"teardown", 48, 48});

    const Part body = makePart(*ctx, doc.value, {20.f, 20.f}, {8.f, 8.f}, Color::white());
    const Part arm = makePart(*ctx, doc.value, {0.f, 0.f}, {4.f, 10.f}, Color::white());
    const Part tool = makePart(*ctx, doc.value, {0.f, 0.f}, {3.f, 6.f}, Color::white());

    const SocketId shoulder = ctx->addSocket(body.sprite, {"shoulder", {28.f, 22.f}, 0.f}).value;
    ctx->createPivot(arm.sprite, PivotDesc{"root", {2.f, 0.f}});
    const SocketId grip = ctx->addSocket(arm.sprite, {"grip", {2.f, 10.f}, 0.f}).value;
    ctx->createPivot(tool.sprite, PivotDesc{"root", {1.f, 5.f}});

    LS_CHECK(ctx->attachSprite(arm.sprite, shoulder).ok());
    LS_CHECK(ctx->attachSprite(tool.sprite, grip).ok());

    // Removing a socket releases whatever hung from it.
    LS_CHECK(ctx->removeSocket(grip).ok());
    LS_CHECK(ctx->getAttachment(tool.sprite).fail());
    LS_CHECK(ctx->assemblyOrder(body.sprite).value.size() == 2);

    // Deleting a parent releases its children, and they are still usable.
    LS_CHECK(ctx->deleteSprite(body.sprite).ok());
    LS_CHECK(ctx->getAttachment(arm.sprite).fail());
    LS_CHECK(ctx->getSpriteInfo(arm.sprite).ok());
    auto orphan = ctx->compileSprite(arm.sprite, exportProfile());
    LS_CHECK(orphan.ok());
    LS_CHECK(ctx->assemblyOrder(arm.sprite).value.size() == 1);
}

// A clone is a copy of the whole sprite: where it sits, and what it hangs from.
void testCloneCarriesPlacement() {
    auto ctx = LSContext::create();
    auto doc = ctx->createDocument({"clone", 48, 48});

    const Part body = makePart(*ctx, doc.value, {20.f, 18.f}, {8.f, 14.f}, Color::white());
    const Part sword = makePart(*ctx, doc.value, {0.f, 0.f}, {3.f, 10.f}, Color{220, 120, 60, 255});
    const SocketId grip = ctx->addSocket(body.sprite, {"grip", {30.f, 24.f}, 0.f}).value;
    ctx->createPivot(sword.sprite, PivotDesc{"hilt", {1.f, 9.f}});

    LS_CHECK(ctx->attachSprite(sword.sprite, grip).ok());
    LS_CHECK(ctx->setSpriteTransform(sword.sprite, Mat3f::translation({1.f, 2.f})).ok());

    auto clone = ctx->cloneSprite(sword.sprite);
    LS_REQUIRE(clone.ok());

    // The clone kept its own placement.
    LS_CHECK(ctx->getSpriteTransform(clone.value).value.m[2] == 1.f);
    LS_CHECK(ctx->getSpriteTransform(clone.value).value.m[5] == 2.f);

    // And hangs from the same socket, through its own copy of the pivot.
    auto attachment = ctx->getAttachment(clone.value);
    LS_REQUIRE(attachment.ok());
    LS_CHECK(attachment.value.socket == grip);
    LS_CHECK(attachment.value.childPivot != ctx->getAttachment(sword.sprite).value.childPivot);
    LS_CHECK(ctx->getPivotName(attachment.value.childPivot).value == "hilt");

    // Both land in the same place, because they describe the same placement.
    LS_CHECK(nearPoint(ctx->getSpriteWorldTransform(clone.value).value.transformPoint({0.f, 0.f}),
                       ctx->getSpriteWorldTransform(sword.sprite).value.transformPoint({0.f, 0.f}).x,
                       ctx->getSpriteWorldTransform(sword.sprite).value.transformPoint({0.f, 0.f}).y));
    LS_CHECK(ctx->getAttachedSprites(grip).value.size() == 2);
}

// The influence a boundary reports is the influence the compiler applies.
void testBoundaryFalloffIsShared() {
    auto ctx = LSContext::create();
    auto doc = ctx->createDocument({"falloff", 48, 48});
    const Part blob = makePart(*ctx, doc.value, {14.f, 14.f}, {20.f, 20.f}, Color::white());

    const GeometryId zone = ctx->createRect(doc.value, {{14.f, 14.f}, 20.f, 20.f, 0.f}).value;
    const BoundaryId boundary =
        ctx->createBoundary(blob.sprite, {"zone", zone, Falloff::Linear, 6.f}).value;

    // A soft boundary reports a gradient, not an on/off answer.
    const float edge = ctx->getBoundaryInfluence(boundary, {14.5f, 24.f}).value;
    const float middle = ctx->getBoundaryInfluence(boundary, {19.f, 24.f}).value;
    const float core = ctx->getBoundaryInfluence(boundary, {24.f, 24.f}).value;
    LS_CHECK(near(edge, 0.f, 0.001f));
    LS_CHECK(middle > edge && middle < core);
    LS_CHECK(near(core, 1.f, 0.001f));
    LS_CHECK(ctx->getBoundaryInfluence(boundary, {2.f, 2.f}).value == 0.f);

    // A squash scoped to that boundary moves interior pixels further than edge
    // pixels: the compiler is reading the same gradient.
    auto flat = ctx->compileSprite(blob.sprite, exportProfile());
    LS_REQUIRE(flat.ok());

    SquashOp squash;
    squash.targetLayer = blob.layer;
    squash.boundary = boundary;
    squash.factor = 0.5f;
    squash.pivotFallback = {24.f, 24.f};
    squash.falloff = Falloff::Linear;
    LS_CHECK(ctx->addOperation(blob.layer, squash).ok());

    auto squashed = ctx->compileSprite(blob.sprite, exportProfile());
    LS_REQUIRE(squashed.ok());
    LS_CHECK(squashed.value.raster.pixels != flat.value.raster.pixels);

    // A hard boundary of the same shape squashes differently from a soft one,
    // which is only true if the falloff reaches the compiler at all.
    LS_CHECK(ctx->createBoundary(blob.sprite, {"hard", zone, Falloff::Linear, 0.f}).ok());
    const BoundaryId hard = ctx->findSocket(blob.sprite, "nothing").ok()
        ? BoundaryId::null()
        : ctx->getSpriteInfo(blob.sprite).value.boundaries.back();
    auto stored = ctx->getOperation(ctx->getLayerOperations(blob.layer).value.back().id);
    LS_REQUIRE(stored.ok());
    SquashOp hardSquash = std::get<SquashOp>(stored.value);
    hardSquash.boundary = hard;
    LS_CHECK(ctx->updateOperation(ctx->getLayerOperations(blob.layer).value.back().id,
                                  hardSquash).ok());
    auto hardResult = ctx->compileSprite(blob.sprite, exportProfile());
    LS_REQUIRE(hardResult.ok());
    LS_CHECK(hardResult.value.raster.pixels != squashed.value.raster.pixels);
}

} // namespace

int main() {
    testPivotBasics();
    testSocketFollowsSpriteTransform();
    testAttachmentChain();
    testAttachmentRejectsCycles();
    testAssemblyCompile();
    testStreamlinedPlacement();
    testRetiredOperationsAreRejected();
    testAssemblySurvivesSaveLoad();
    testTeardownDetaches();
    testCloneCarriesPlacement();
    testBoundaryFalloffIsShared();
    return lstest::report("anchors");
}
