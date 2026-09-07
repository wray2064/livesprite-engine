# Getting started

Everything below is taken from [`examples/first_sprite.cpp`](../examples/first_sprite.cpp),
which is built and run alongside the tests, so it cannot drift from an API that
no longer exists.

```bash
./build.bat && build/livesprite_example
```

## One context, one workspace

```cpp
#include <livesprite/livesprite.h>
using namespace ls;

auto ctx = LSContext::create();
```

The context owns every entity. You hold ids, never pointers. Nothing throws:
every call returns `Result<T>` with an `error` you can check, or `VoidResult`
when there is nothing to return.

```cpp
auto doc = ctx->createDocument({"badge", 24, 24});
if (doc.fail()) {
    std::printf("%s\n", std::string(lsErrorString(doc.error)).c_str());
    return 1;
}
```

The examples below use `.value` directly for brevity. Real code checks.

## A document, a sprite, a layer

```cpp
const DocumentId doc    = ctx->createDocument({"badge", 24, 24}).value;
const SpriteId   sprite = ctx->createSprite(doc).value;
const LayerId    layer  = ctx->createLayer(sprite, {"body"}).value;
```

A document is a canvas and everything drawn on it. A sprite is one thing on that
canvas. A layer is an ordered stack of operations.

## Colours are roles

```cpp
const PaletteId palette = ctx->createPalette(doc, {"badge", {
    {0, {26, 28, 38, 255},   "ink"},
    {1, {96, 132, 196, 255}, "body"},
    {2, {206, 224, 255, 255},"light"}}}).value;
ctx->bindSpritePalette(sprite, palette);
```

Painting through a role rather than a literal colour is what makes a palette
swap repaint the artwork without touching a single operation. A region can also
carry a standing role with `bindRegionToPaletteRole`, so a fill that names no
role of its own paints whatever that shape is made of.

## A shape

Geometry, or pixels somebody drew:

```cpp
PixelRegionDesc drawn;
for (int32_t i = 0; i < 14; ++i) {
    drawn.pixels.push_back({{5 + i, 5},  ink});
    drawn.pixels.push_back({{5 + i, 18}, ink});
    drawn.pixels.push_back({{5, 5 + i},  ink});
    drawn.pixels.push_back({{18, 5 + i}, ink});
}
const RegionId badge = ctx->createRegionFromPixels(doc, drawn).value;
```

An authored closed loop **seals**: the engine works out that the ring has an
inside, so a bucket fill has something to fill, and the loop stays addressable
as its own outline. An open stroke encloses nothing, which is also correct.

The geometry route is `createRect`, `createEllipse`, `createPolygon`,
`createCurve` and friends, then `createRegionFromGeometry`. A region made that
way tracks its geometry: edit the rectangle and every fill on it follows.

## Operations, which are the actual sprite

```cpp
FillSolidOp body;
body.targetRegion = badge;
body.paletteRole = 1;
ctx->addOperation(layer, body);
```

That call does not paint anything. It records a standing instruction: *this
region takes its colour from role 1*. Nothing is rasterized until you compile.

A dither resolves a value against a threshold pattern and picks between the two
ramp stops the value falls between. Modulate that value across the shape and you
have a gradient made of dithered colour:

```cpp
FillDitherOp sheen;
sheen.targetRegion   = badge;
sheen.ramp           = ramp;
sheen.pattern        = ctx->createDitherPattern(doc, DitherPatternKind::Bayer4).value;
sheen.modulation     = DitherModulation::Linear;
sheen.gradientStart  = {0.f, 0.f};
sheen.gradientEnd    = {14.f, 14.f};
sheen.anchor         = PatternAnchor::Local;
ctx->addOperation(layer, sheen);
```

`anchor` decides what the pattern does when the sprite moves: `Local` rides the
object, `Global` travels with it but stays rotation-locked, `Fixed` belongs to
the canvas. See [concepts](concepts.md#pattern-anchoring).

## Compiling

```cpp
CompileProfile profile;
profile.type         = CompileProfileType::Export;
profile.outputWidth  = 24;
profile.outputHeight = 24;

auto compiled = ctx->compileSprite(sprite, profile);
```

This is the first moment pixels exist. `compiled.value.raster` is RGBA8;
`compiled.value.bounds` is the tight box around what was drawn. Use
`readPixel(raster, x, y)` to look at it.

The same inputs always produce the same output, on every platform and every run.

## Changing your mind costs nothing

```cpp
ctx->setOperationParameter(fillId, "density", 0.8f);
ctx->setPaletteColor(palette, 1, {176, 96, 72, 255});

RotateOp turn;
turn.targetLayer    = layer;
turn.angleDegrees   = 45.f;
turn.pivotFallback  = {12.f, 12.f};
ctx->addOperation(layer, turn);

auto turned = ctx->compileSprite(sprite, profile);
```

The rotation is a parameter, not a destructive edit. Remove it and the original
comes back exactly, because it was never overwritten. The example prints both
compiles side by side so you can see it.

## Saving

```cpp
auto document = ctx->serializeDocument(doc);           // just the document
auto package  = ctx->writePackage(doc, {thumbnail});   // document plus app files
```

A package is a ZIP: rename it and any tool opens it. Loading is
`deserializeDocument` or `loadPackage`, which hands back app entries — including
entries written by an app that is not yours, untouched.

## Where to go next

- [Concepts](concepts.md) for the model behind all of this
- [Operations](operations.md) for what else you can put in a layer
- [File format](file-format.md) before you write files users will share
- [Performance and limits](performance-and-limits.md) before you put a compile
  behind a live canvas
