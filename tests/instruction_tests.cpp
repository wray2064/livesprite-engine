// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors
// instruction_tests.cpp — the surface an animation system drives from above.
//
// The engine knows nothing about time. What these tests pin is what a timeline,
// a puppet solver or a live slider needs underneath it: parameters addressable
// by name, values that are absolute rather than accumulated, batches that land
// as one frame, and a compile that reflects them.

#include "ls_test.h"

#include <livesprite/livesprite.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace ls;

namespace {

CompileProfile exportProfile(uint32_t size = 48) {
    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = size;
    profile.outputHeight = size;
    profile.palette = PalettePolicy::Unconstrained;
    return profile;
}

struct Rig {
    DocumentId doc;
    SpriteId body;
    SpriteId arm;
    LayerId bodyLayer;
    LayerId armLayer;
    OperationId bodyFill;
    OperationId rotate;
    PaletteId palette;
    SocketId shoulder;
    PivotId armRoot;
};

// A two part figure: a body with a socket, an arm hanging off it, and a
// rotation operation inside the arm that an animation system might drive.
Rig makeRig(LSContext& ctx) {
    Rig rig;
    rig.doc = ctx.createDocument({"rig", 48, 48}).value;

    rig.palette = ctx.createPalette(rig.doc, {"figure", {
        {0, {40, 44, 60, 255}, "ink"},
        {1, {180, 190, 220, 255}, "body"}}}).value;

    rig.body = ctx.createSprite(rig.doc).value;
    ctx.bindSpritePalette(rig.body, rig.palette);
    rig.bodyLayer = ctx.createLayer(rig.body, {"torso"}).value;
    const GeometryId torso = ctx.createRect(rig.doc, {{20.f, 18.f}, 8.f, 14.f, 0.f}).value;
    const RegionId torsoRegion = ctx.createRegionFromGeometry(torso).value;
    FillSemanticColorOp bodyFill;
    bodyFill.targetRegion = torsoRegion;
    bodyFill.paletteRole = 1;
    rig.bodyFill = ctx.addOperation(rig.bodyLayer, bodyFill).value;
    rig.shoulder = ctx.addSocket(rig.body, {"shoulder", {28.f, 21.f}, 0.f}).value;

    rig.arm = ctx.createSprite(rig.doc).value;
    ctx.bindSpritePalette(rig.arm, rig.palette);
    rig.armLayer = ctx.createLayer(rig.arm, {"arm"}).value;
    const GeometryId armShape = ctx.createRect(rig.doc, {{0.f, 0.f}, 4.f, 14.f, 0.f}).value;
    const RegionId armRegion = ctx.createRegionFromGeometry(armShape).value;
    FillSemanticColorOp armFill;
    armFill.targetRegion = armRegion;
    armFill.paletteRole = 1;
    ctx.addOperation(rig.armLayer, armFill);

    RotateOp rotate;
    rotate.targetLayer = rig.armLayer;
    rotate.angleDegrees = 0.f;
    rotate.pivotFallback = {2.f, 1.f};
    rig.rotate = ctx.addOperation(rig.armLayer, rotate).value;

    rig.armRoot = ctx.createPivot(rig.arm, PivotDesc{"root", {2.f, 1.f}}).value;
    ctx.attachSprite(rig.arm, rig.shoulder);
    return rig;
}

bool hasParameter(const std::vector<ParameterInfo>& parameters, const std::string& name,
                  ParameterType type) {
    for (const ParameterInfo& info : parameters) {
        if (info.name == name) {
            return info.type == type;
        }
    }
    return false;
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

// --- addressing ------------------------------------------------------------

void testDescribeAndAddress() {
    auto ctx = LSContext::create();
    const Rig rig = makeRig(*ctx);

    auto described = ctx->describeOperation(rig.rotate);
    LS_REQUIRE(described.ok());
    LS_CHECK(hasParameter(described.value, "angleDegrees", ParameterType::Float));
    LS_CHECK(hasParameter(described.value, "pivotFallback", ParameterType::Vec2));
    LS_CHECK(hasParameter(described.value, "sampling", ParameterType::Int));
    LS_CHECK(hasParameter(described.value, "targetLayer", ParameterType::EntityId));
    LS_CHECK(ctx->describeOperation(OperationId{999999}).fail());

    // The names an app drives are the names the save file uses: one table.
    auto serialized = ctx->serializeOperation(rig.rotate);
    LS_REQUIRE(serialized.ok());
    LS_CHECK(serialized.value.find("angleDegrees") != std::string::npos);

    // Read, write, read back.
    auto initial = ctx->getOperationParameter(rig.rotate, "angleDegrees");
    LS_REQUIRE(initial.ok());
    LS_CHECK(std::get<float>(initial.value) == 0.f);

    LS_CHECK(ctx->setOperationParameter(rig.rotate, "angleDegrees", 42.5f).ok());
    LS_CHECK(std::get<float>(ctx->getOperationParameter(rig.rotate, "angleDegrees").value) == 42.5f);

    // An integer for a float parameter is accepted: a caller driving from a
    // slider that yields whole numbers should not have to care.
    LS_CHECK(ctx->setOperationParameter(rig.rotate, "angleDegrees", int64_t{30}).ok());
    LS_CHECK(std::get<float>(ctx->getOperationParameter(rig.rotate, "angleDegrees").value) == 30.f);

    // Enumerations address as integers.
    LS_CHECK(ctx->setOperationParameter(rig.rotate, "sampling",
                                        static_cast<int64_t>(SamplingPolicy::Center)).ok());
    LS_CHECK(std::get<RotateOp>(ctx->getOperation(rig.rotate).value).sampling ==
             SamplingPolicy::Center);

    // Vectors and colours address as themselves.
    LS_CHECK(ctx->setOperationParameter(rig.rotate, "pivotFallback", Vec2f{3.f, 4.f}).ok());
    LS_CHECK(std::get<Vec2f>(ctx->getOperationParameter(rig.rotate, "pivotFallback").value).x == 3.f);

    // Nonsense is refused rather than guessed at.
    LS_CHECK(ctx->setOperationParameter(rig.rotate, "noSuchField", 1.f).fail());
    LS_CHECK(ctx->getOperationParameter(rig.rotate, "noSuchField").fail());
    LS_CHECK(ctx->setOperationParameter(rig.rotate, "angleDegrees", std::string("sideways")).fail());
    LS_CHECK(ctx->setOperationParameter(OperationId{999999}, "angleDegrees", 1.f).fail());
}

// --- absolute values -------------------------------------------------------

void testValuesAreAbsolute() {
    auto ctx = LSContext::create();
    const Rig rig = makeRig(*ctx);

    // Scrubbing a timeline has to be order independent: the picture at angle 20
    // must be the same whether it was reached going forwards or backwards.
    auto renderAt = [&](float angle) {
        std::vector<Instruction> frame;
        SetOperationParameter set;
        set.operation = rig.rotate;
        set.parameter = "angleDegrees";
        set.value = angle;
        frame.push_back(set);
        return ctx->renderFrame(rig.body, frame, exportProfile()).value.raster;
    };

    const RasterBuffer forwards = renderAt(20.f);
    renderAt(50.f);
    renderAt(-35.f);
    const RasterBuffer backwards = renderAt(20.f);
    LS_CHECK(forwards.pixels == backwards.pixels);

    // And a different value really does change the picture, so the check above
    // is not passing on a frozen render.
    const RasterBuffer other = renderAt(70.f);
    LS_CHECK(other.pixels != forwards.pixels);
}

// --- frames ----------------------------------------------------------------

void testFrameBatch() {
    auto ctx = LSContext::create();
    const Rig rig = makeRig(*ctx);

    // One frame: swing the arm, place the body, dim a layer, and recolour a
    // palette role. An animation system sends this and gets a picture back.
    std::vector<Instruction> frame;

    SetOperationParameter swing;
    swing.operation = rig.rotate;
    swing.parameter = "angleDegrees";
    swing.value = 35.f;
    frame.push_back(swing);

    SetSpriteTransformInstruction place;
    place.sprite = rig.body;
    place.transform = Mat3f::translation({2.f, 0.f});
    frame.push_back(place);

    SetPaletteColorInstruction recolor;
    recolor.palette = rig.palette;
    recolor.role = 1;
    recolor.color = {220, 90, 90, 255};
    frame.push_back(recolor);

    auto rendered = ctx->renderFrame(rig.body, frame, exportProfile());
    LS_REQUIRE(rendered.ok());
    LS_CHECK(opaqueCount(rendered.value.raster) > 0);
    LS_CHECK(std::get<float>(ctx->getOperationParameter(rig.rotate, "angleDegrees").value) == 35.f);
    LS_CHECK(ctx->getSpriteTransform(rig.body).value.m[2] == 2.f);
    LS_CHECK(ctx->resolveSemanticColor(rig.palette, 1).value == Color{220, 90, 90, 255});

    // The recolour reached the pixels: the body is drawn through the role.
    bool foundNewColor = false;
    for (uint32_t y = 0; y < rendered.value.raster.height && !foundNewColor; ++y) {
        for (uint32_t x = 0; x < rendered.value.raster.width; ++x) {
            if (readPixel(rendered.value.raster, static_cast<int32_t>(x),
                          static_cast<int32_t>(y)) == Color{220, 90, 90, 255}) {
                foundNewColor = true;
                break;
            }
        }
    }
    LS_CHECK(foundNewColor);

    // Layer visibility and opacity are drivable too.
    SetLayerVisibilityInstruction hide;
    hide.layer = rig.armLayer;
    hide.visible = false;
    const int before = opaqueCount(rendered.value.raster);
    auto hidden = ctx->renderFrame(rig.body, {hide}, exportProfile());
    LS_REQUIRE(hidden.ok());
    LS_CHECK(opaqueCount(hidden.value.raster) < before);

    SetLayerOpacityInstruction fade;
    fade.layer = rig.bodyLayer;
    fade.opacity = 0.5f;
    LS_CHECK(ctx->applyInstructions({fade}).ok());
    LS_CHECK(ctx->getLayerInfo(rig.bodyLayer).value.opacity == 0.5f);
}

void testBatchIsAllOrNothing() {
    auto ctx = LSContext::create();
    const Rig rig = makeRig(*ctx);

    LS_CHECK(ctx->setOperationParameter(rig.rotate, "angleDegrees", 10.f).ok());

    // A batch whose second instruction is bad must leave the first unapplied:
    // a half posed frame is worse than a refused one.
    SetOperationParameter good;
    good.operation = rig.rotate;
    good.parameter = "angleDegrees";
    good.value = 80.f;

    SetOperationParameter bad;
    bad.operation = rig.rotate;
    bad.parameter = "noSuchField";
    bad.value = 1.f;

    LS_CHECK(ctx->applyInstructions({good, bad}).fail());
    LS_CHECK(std::get<float>(ctx->getOperationParameter(rig.rotate, "angleDegrees").value) == 10.f);

    // The same batch without the bad instruction goes through.
    LS_CHECK(ctx->applyInstructions({good}).ok());
    LS_CHECK(std::get<float>(ctx->getOperationParameter(rig.rotate, "angleDegrees").value) == 80.f);

    // Bad ids and out of range values are caught in validation.
    SetLayerOpacityInstruction tooBright;
    tooBright.layer = rig.bodyLayer;
    tooBright.opacity = 4.f;
    LS_CHECK(ctx->applyInstructions({tooBright}).fail());

    DetachInstruction detachUnattached;
    detachUnattached.child = rig.body;      // the root hangs off nothing
    LS_CHECK(ctx->applyInstructions({detachUnattached}).fail());
}

void testAttachmentInstructions() {
    auto ctx = LSContext::create();
    const Rig rig = makeRig(*ctx);

    // Swapping what is held is a frame instruction like any other.
    const SpriteId sword = ctx->createSprite(rig.doc).value;
    const LayerId swordLayer = ctx->createLayer(sword, {"blade"}).value;
    const GeometryId blade = ctx->createRect(rig.doc, {{0.f, 0.f}, 3.f, 12.f, 0.f}).value;
    const RegionId bladeRegion = ctx->createRegionFromGeometry(blade).value;
    FillSolidOp fill;
    fill.targetRegion = bladeRegion;
    fill.fallbackColor = {230, 140, 60, 255};
    ctx->addOperation(swordLayer, fill);
    ctx->createPivot(sword, PivotDesc{"hilt", {1.f, 11.f}});
    const SocketId grip = ctx->addSocket(rig.arm, {"grip", {2.f, 13.f}, 0.f}).value;

    AttachInstruction equip;
    equip.child = sword;
    equip.attachment.socket = grip;
    auto armed = ctx->renderFrame(rig.body, {equip}, exportProfile());
    LS_REQUIRE(armed.ok());
    LS_CHECK(ctx->getAttachment(sword).value.parent == rig.arm);

    DetachInstruction drop;
    drop.child = sword;
    auto disarmed = ctx->renderFrame(rig.body, {drop}, exportProfile());
    LS_REQUIRE(disarmed.ok());
    LS_CHECK(ctx->getAttachment(sword).fail());
    // The blade sits inside the arm footprint, so count its own colour rather
    // than total coverage.
    const Color bladeColor {230, 140, 60, 255};
    LS_CHECK(countColor(armed.value.raster, bladeColor) > 0);
    LS_CHECK(countColor(disarmed.value.raster, bladeColor) == 0);

    // An attachment that would close a cycle is refused at apply time.
    const SocketId swordSocket = ctx->addSocket(sword, {"pommel", {1.f, 12.f}, 0.f}).value;
    ctx->attachSprite(sword, grip);
    AttachInstruction loop;
    loop.child = rig.arm;
    loop.attachment.socket = swordSocket;
    LS_CHECK(ctx->applyInstructions({loop}).error == LSError::DependencyCycle);
}

void testFrameSequenceIsCheap() {
    auto ctx = LSContext::create();
    const Rig rig = makeRig(*ctx);

    // Re-rendering an unchanged frame should come from the cache rather than
    // recompiling: this is what makes live playback affordable.
    ctx->renderFrame(rig.body, {}, exportProfile());
    const size_t hitsBefore = ctx->cacheStats().hits;
    ctx->renderFrame(rig.body, {}, exportProfile());
    LS_CHECK(ctx->cacheStats().hits > hitsBefore);

    // Driving one parameter dirties that operation and its layer, and the next
    // render reflects the change.
    SetOperationParameter swing;
    swing.operation = rig.rotate;
    swing.parameter = "angleDegrees";
    swing.value = 45.f;
    LS_CHECK(ctx->applyInstructions({swing}).ok());
    LS_CHECK(ctx->isDirty(rig.rotate.value).value);
    LS_CHECK(ctx->isDirty(rig.armLayer.value).value);

    auto swung = ctx->renderFrame(rig.body, {}, exportProfile());
    LS_REQUIRE(swung.ok());
    LS_CHECK(!ctx->isDirty(rig.armLayer.value).value);
}

} // namespace

int main() {
    testDescribeAndAddress();
    testValuesAreAbsolute();
    testFrameBatch();
    testBatchIsAllOrNothing();
    testAttachmentInstructions();
    testFrameSequenceIsCheap();
    return lstest::report("instructions");
}
