// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors

// palette_tests.cpp -- colours as roles, all the way down.
//
// The model says an operation names a role and the palette turns it into a
// colour at compile time, so changing a slot recolours everything that uses
// it from the drawing rather than over it. That was true of fills and strokes
// and outlines, and not true of ramps: a ramp stop held a literal colour, so a
// dither -- which is most pixel art -- sat outside the palette entirely and a
// palette swap left it behind.
//
// Now a stop can name a role, and this file states what that has to mean.

#include "ls_test.h"

#include <livesprite/livesprite.h>

#include <memory>
#include <vector>

using namespace ls;

namespace {

constexpr uint32_t kSize = 16;
constexpr ColorRole kDark  = 1;
constexpr ColorRole kLight = 2;
const Color kDarkA  { 20, 30, 60, 255 };
const Color kLightA { 240, 200, 120, 255 };
const Color kDarkB  { 60, 20, 20, 255 };     // the "night" palette
const Color kLightB { 200, 90, 90, 255 };

struct Scene {
    std::unique_ptr<LSContext> engine = LSContext::create();
    DocumentId doc;
    SpriteId   sprite;
    PaletteId  palette;
    RegionId   region;
    LayerId    layer;

    bool build() {
        auto document = engine->createDocument({ "palette", kSize, kSize });
        if (document.fail()) { return false; }
        doc = document.value;
        auto made = engine->createSprite(doc);
        if (made.fail()) { return false; }
        sprite = made.value;

        PaletteDesc desc;
        desc.name = "day";
        desc.entries = { { kDark, kDarkA, "dark" }, { kLight, kLightA, "light" } };
        auto pal = engine->createPalette(doc, desc);
        if (pal.fail()) { return false; }
        palette = pal.value;
        if (engine->bindSpritePalette(sprite, palette).fail()) { return false; }

        auto rect = engine->createRect(doc, { { 0.f, 0.f }, 16.f, 16.f, 0.f });
        if (rect.fail()) { return false; }
        auto made2 = engine->createRegionFromGeometry(rect.value);
        if (made2.fail()) { return false; }
        region = made2.value;
        auto made3 = engine->createLayer(sprite, { "fill" });
        if (made3.fail()) { return false; }
        layer = made3.value;
        return true;
    }

    // A two-stop dither over the whole canvas, either from roles or literals.
    bool ditherWith(bool roles) {
        RampDesc ramp;
        ramp.name = "ramp";
        RampStop a; a.position = 0.f; a.color = kDarkA;  a.role = roles ? kDark  : kColorRoleNone;
        RampStop b; b.position = 1.f; b.color = kLightA; b.role = roles ? kLight : kColorRoleNone;
        ramp.stops = { a, b };
        auto made = engine->createRamp(doc, ramp);
        if (made.fail()) { return false; }
        auto pattern = engine->createDitherPattern(doc, DitherPatternKind::Bayer4);
        if (pattern.fail()) { return false; }

        FillDitherOp op;
        op.targetRegion = region;
        op.ramp = made.value;
        op.pattern = pattern.value;
        op.density = 0.5f;
        return engine->addOperation(layer, op).ok();
    }

    RasterBuffer render() {
        CompileProfile profile;
        profile.type = CompileProfileType::Export;
        profile.outputWidth = kSize;
        profile.outputHeight = kSize;
        profile.palette = PalettePolicy::Unconstrained;
        auto compiled = engine->compileSprite(sprite, profile);
        return compiled.ok() ? compiled.value.raster : RasterBuffer{};
    }

    void goNight() {
        engine->setPaletteColor(palette, kDark, kDarkB);
        engine->setPaletteColor(palette, kLight, kLightB);
    }
};

bool has(const RasterBuffer& r, Color want) {
    for (uint32_t y = 0; y < r.height; ++y) {
        for (uint32_t x = 0; x < r.width; ++x) {
            const Color c = readPixel(r, static_cast<int32_t>(x), static_cast<int32_t>(y));
            if (c.r == want.r && c.g == want.g && c.b == want.b && c.a == want.a) {
                return true;
            }
        }
    }
    return false;
}

// The whole point: a dither built from roles follows a palette change.
void testADitherFromRolesFollowsThePalette() {
    Scene s;
    LS_REQUIRE(s.build());
    LS_REQUIRE(s.ditherWith(true));

    const RasterBuffer day = s.render();
    LS_REQUIRE(!day.empty());
    LS_CHECK(has(day, kDarkA) && has(day, kLightA));    // a two-tone screen

    s.goNight();
    const RasterBuffer night = s.render();
    LS_CHECK(has(night, kDarkB) && has(night, kLightB));
    LS_CHECK(!has(night, kDarkA) && !has(night, kLightA));

    // Same screen, different colours: the pattern of which pixel got which
    // stop is unchanged, because only the resolution moved.
    LS_REQUIRE(day.pixels.size() == night.pixels.size());
    size_t sameShape = 0;
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            const Color d = readPixel(day, static_cast<int32_t>(x), static_cast<int32_t>(y));
            const Color n = readPixel(night, static_cast<int32_t>(x), static_cast<int32_t>(y));
            const bool dWasDark = d.r == kDarkA.r;
            const bool nIsDark = n.r == kDarkB.r;
            if (dWasDark == nIsDark) { ++sameShape; }
        }
    }
    LS_CHECK(sameShape == kSize * kSize);
}

// And the old behaviour is still the old behaviour: literal stops are literal,
// so a file written before roles reached ramps compiles exactly as it did.
void testADitherFromLiteralsIgnoresThePalette() {
    Scene s;
    LS_REQUIRE(s.build());
    LS_REQUIRE(s.ditherWith(false));

    const RasterBuffer day = s.render();
    LS_REQUIRE(!day.empty());
    s.goNight();
    const RasterBuffer night = s.render();
    LS_CHECK(day.pixels == night.pixels);
}

// A role that resolves to nothing falls back to the stop's literal colour, the
// same rule as every fill. Removing a slot therefore reverts, never breaks.
void testARemovedSlotFallsBackToTheLiteral() {
    Scene s;
    LS_REQUIRE(s.build());
    LS_REQUIRE(s.ditherWith(true));
    s.goNight();
    LS_CHECK(has(s.render(), kDarkB));

    LS_REQUIRE(s.engine->removePaletteColor(s.palette, kDark).ok());
    const RasterBuffer after = s.render();
    LS_CHECK(has(after, kDarkA));       // the literal the stop was made with
    LS_CHECK(!has(after, kDarkB));
    LS_CHECK(has(after, kLightB));      // the other slot is untouched

    // Removing what is not there is an error, not a no-op.
    LS_CHECK(s.engine->removePaletteColor(s.palette, 99).fail());
}

void testTheDocumentKnowsWhatUsesARole() {
    Scene s;
    LS_REQUIRE(s.build());

    // Nothing yet.
    auto used = s.engine->usesPaletteRole(s.doc, kDark);
    LS_REQUIRE(used.ok());
    LS_CHECK(!used.value);

    // A ramp stop names it.
    LS_REQUIRE(s.ditherWith(true));
    used = s.engine->usesPaletteRole(s.doc, kDark);
    LS_CHECK(used.ok() && used.value);

    // A fill on another layer names a third role through paletteRole.
    auto other = s.engine->createLayer(s.sprite, { "solid" });
    LS_REQUIRE(other.ok());
    FillSolidOp fill;
    fill.targetRegion = s.region;
    fill.paletteRole = 7;
    LS_REQUIRE(s.engine->addOperation(other.value, fill).ok());
    used = s.engine->usesPaletteRole(s.doc, 7);
    LS_CHECK(used.ok() && used.value);

    // A region's standing role counts too.
    LS_REQUIRE(s.engine->bindRegionToPaletteRole(s.region, 9).ok());
    used = s.engine->usesPaletteRole(s.doc, 9);
    LS_CHECK(used.ok() && used.value);

    LS_CHECK(s.engine->usesPaletteRole(s.doc, 42).value == false);
    LS_CHECK(s.engine->usesPaletteRole(s.doc, kColorRoleNone).value == false);
}

void testLabelsAreKeptAndCanChange() {
    Scene s;
    LS_REQUIRE(s.build());

    auto entries = s.engine->getPaletteEntries(s.palette);
    LS_REQUIRE(entries.ok() && entries.value.size() == 2);
    LS_CHECK(entries.value[0].label == "dark");

    LS_REQUIRE(s.engine->setPaletteLabel(s.palette, kDark, "outline").ok());
    entries = s.engine->getPaletteEntries(s.palette);
    LS_CHECK(entries.value[0].label == "outline");

    // Clearing, and labelling a slot that is not there.
    LS_REQUIRE(s.engine->setPaletteLabel(s.palette, kDark, "").ok());
    entries = s.engine->getPaletteEntries(s.palette);
    LS_CHECK(entries.value[0].label.empty());
    LS_CHECK(s.engine->setPaletteLabel(s.palette, 99, "ghost").fail());
}

// The role on a stop is part of the document, so it survives a save -- and a
// stop without one is written without one, so an old file reads unchanged.
void testRoleStopsSurviveARoundTrip() {
    Scene s;
    LS_REQUIRE(s.build());
    LS_REQUIRE(s.ditherWith(true));
    s.goNight();
    const RasterBuffer expected = s.render();

    auto saved = s.engine->serializeDocument(s.doc);
    LS_REQUIRE(saved.ok());
    auto reader = LSContext::create();
    auto loaded = reader->deserializeDocument(saved.value);
    LS_REQUIRE(loaded.ok());

    auto info = reader->getDocumentInfo(loaded.value);
    LS_REQUIRE(info.ok() && !info.value.sprites.empty());
    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = kSize;
    profile.outputHeight = kSize;
    profile.palette = PalettePolicy::Unconstrained;
    auto compiled = reader->compileSprite(info.value.sprites.front(), profile);
    LS_REQUIRE(compiled.ok());
    LS_CHECK(compiled.value.raster.pixels == expected.pixels);

    // And it still follows the palette after reloading: the role came back,
    // not just the colour it happened to resolve to.
    auto pal = reader->getEffectivePalette(info.value.sprites.front());
    LS_REQUIRE(pal.ok());
    LS_REQUIRE(reader->setPaletteColor(pal.value, kDark, Color{ 1, 2, 3, 255 }).ok());
    auto again = reader->compileSprite(info.value.sprites.front(), profile);
    LS_REQUIRE(again.ok());
    LS_CHECK(has(again.value.raster, Color{ 1, 2, 3, 255 }));
}

// remapRamp is the old, guessing answer for literal ramps. It must leave a
// role-bound stop alone, because that stop already follows the palette and a
// guess would only ever make it wrong.
void testRemapLeavesRoleStopsAlone() {
    Scene s;
    LS_REQUIRE(s.build());

    PaletteDesc nightDesc;
    nightDesc.name = "night";
    nightDesc.entries = { { kDark, kDarkB }, { kLight, kLightB } };
    auto night = s.engine->createPalette(s.doc, nightDesc);
    LS_REQUIRE(night.ok());

    RampDesc ramp;
    RampStop literal; literal.position = 0.f; literal.color = kDarkA;
    RampStop bound;   bound.position = 1.f;   bound.color = kLightA; bound.role = kLight;
    ramp.stops = { literal, bound };
    auto made = s.engine->createRamp(s.doc, ramp);
    LS_REQUIRE(made.ok());

    LS_REQUIRE(s.engine->remapRamp(made.value, s.palette, night.value).ok());
    auto info = s.engine->getRamp(made.value);
    LS_REQUIRE(info.ok() && info.value.stops.size() == 2);
    // The literal was matched and rewritten; the bound stop was not touched.
    LS_CHECK(info.value.stops[0].color.r == kDarkB.r);
    LS_CHECK(info.value.stops[1].color.r == kLightA.r);
    LS_CHECK(info.value.stops[1].role == kLight);
}

} // namespace

// A palette write dirties every sprite that resolves through it -- the one
// bound by name, one cloned from it, and one that was never bound and inherits
// the document's palette. An editor that compiles only what the engine calls
// dirty was showing stale frames for the last two.
void testAPaletteWriteDirtiesEverySpriteThatUsesIt() {
    Scene s;
    LS_REQUIRE(s.build());
    LS_REQUIRE(s.ditherWith(true));
    LS_REQUIRE(s.engine->bindDocumentPalette(s.doc, s.palette).ok());

    auto cloned = s.engine->cloneSprite(s.sprite);
    LS_REQUIRE(cloned.ok());
    auto later = s.engine->createSprite(s.doc);        // inherits the document's
    LS_REQUIRE(later.ok());

    const SpriteId all[] = { s.sprite, cloned.value, later.value };
    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = kSize;
    profile.outputHeight = kSize;
    for (SpriteId sprite : all) {
        s.engine->compileSprite(sprite, profile);
        auto dirty = s.engine->isDirty(sprite.value);
        LS_CHECK(dirty.ok() && !dirty.value);
    }

    s.engine->setPaletteColor(s.palette, kDark, kDarkB);
    for (SpriteId sprite : all) {
        auto dirty = s.engine->isDirty(sprite.value);
        LS_CHECK(dirty.ok() && dirty.value);
    }

    for (SpriteId sprite : all) { s.engine->compileSprite(sprite, profile); }
    LS_CHECK(s.engine->removePaletteColor(s.palette, kLight).ok());
    for (SpriteId sprite : all) {
        auto dirty = s.engine->isDirty(sprite.value);
        LS_CHECK(dirty.ok() && dirty.value);
    }

    // A sprite bound to a different palette is left alone.
    PaletteDesc other;
    other.name = "other";
    other.entries = { { kDark, kDarkA, "dark" } };
    auto otherPalette = s.engine->createPalette(s.doc, other);
    LS_REQUIRE(otherPalette.ok());
    auto apart = s.engine->createSprite(s.doc);
    LS_REQUIRE(apart.ok());
    LS_REQUIRE(s.engine->bindSpritePalette(apart.value, otherPalette.value).ok());
    s.engine->compileSprite(apart.value, profile);
    s.engine->setPaletteColor(s.palette, kDark, kDarkA);
    auto dirty = s.engine->isDirty(apart.value.value);
    LS_CHECK(dirty.ok() && !dirty.value);
}

// Several palettes in one document: the document names one, a sprite may name
// another, and a sprite that drops its own follows the document's again. This
// is what a swap is made of, and what per-frame binding is made of.
void testADocumentHoldsSeveralPalettesAndASpriteMayPickOne() {
    Scene s;
    LS_REQUIRE(s.build());
    LS_REQUIRE(s.ditherWith(true));

    PaletteDesc night;
    night.name = "night";
    night.entries = { { kDark, kDarkB, "dark" }, { kLight, kLightB, "light" } };
    auto made = s.engine->createPalette(s.doc, night);
    LS_REQUIRE(made.ok());

    auto info = s.engine->getDocumentInfo(s.doc);
    LS_REQUIRE(info.ok());
    LS_CHECK(info.value.palettes.size() == 2);
    LS_CHECK(info.value.palettes[0] == s.palette && info.value.palettes[1] == made.value);

    auto name = s.engine->getPaletteName(made.value);
    LS_CHECK(name.ok() && name.value == "night");
    LS_CHECK(s.engine->setPaletteName(made.value, "dusk").ok());
    name = s.engine->getPaletteName(made.value);
    LS_CHECK(name.ok() && name.value == "dusk");

    // The sprite was bound to "day" by name. Dropping that makes it follow
    // the document's, which is nothing yet -- then day, then night.
    LS_CHECK(s.engine->bindSpritePalette(s.sprite, PaletteId::null()).ok());
    auto own = s.engine->getSpritePalette(s.sprite);
    LS_CHECK(own.ok() && !own.value.valid());
    LS_REQUIRE(s.engine->bindDocumentPalette(s.doc, s.palette).ok());
    auto effective = s.engine->getEffectivePalette(s.sprite);
    LS_CHECK(effective.ok() && effective.value == s.palette);
    LS_CHECK(has(s.render(), kDarkA));

    LS_REQUIRE(s.engine->bindDocumentPalette(s.doc, made.value).ok());
    LS_CHECK(has(s.render(), kDarkB));
    LS_CHECK(!has(s.render(), kDarkA));

    // A sprite with its own binding ignores the document's switch, and a
    // write to the palette it left no longer reaches it.
    LS_REQUIRE(s.engine->bindSpritePalette(s.sprite, s.palette).ok());
    LS_CHECK(has(s.render(), kDarkA));
    s.engine->compileSprite(s.sprite, CompileProfile{});
    s.engine->setPaletteColor(made.value, kDark, Color{ 1, 1, 1, 255 });
    auto dirty = s.engine->isDirty(s.sprite.value);
    LS_CHECK(dirty.ok() && !dirty.value);

    // Names and bindings survive a save.
    auto bytes = s.engine->serializeDocument(s.doc);
    LS_REQUIRE(bytes.ok());
    auto again = LSContext::create();
    auto loaded = again->deserializeDocument(bytes.value);
    LS_REQUIRE(loaded.ok());
    auto info2 = again->getDocumentInfo(loaded.value);
    LS_REQUIRE(info2.ok() && info2.value.palettes.size() == 2);
    auto name2 = again->getPaletteName(info2.value.palettes[1]);
    LS_CHECK(name2.ok() && name2.value == "dusk");
    LS_CHECK(info2.value.palette == info2.value.palettes[1]);
    auto own2 = again->getSpritePalette(info2.value.sprites.front());
    LS_CHECK(own2.ok() && own2.value == info2.value.palettes[0]);
}

int main() {
    testADitherFromRolesFollowsThePalette();
    testADocumentHoldsSeveralPalettesAndASpriteMayPickOne();
    testAPaletteWriteDirtiesEverySpriteThatUsesIt();
    testADitherFromLiteralsIgnoresThePalette();
    testARemovedSlotFallsBackToTheLiteral();
    testTheDocumentKnowsWhatUsesARole();
    testLabelsAreKeptAndCanChange();
    testRoleStopsSurviveARoundTrip();
    testRemapLeavesRoleStopsAlone();
    return lstest::report("palettes");
}
