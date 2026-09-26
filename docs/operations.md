# Operations

The 43 operation types, what each one does, and the parameters worth knowing.
Full field lists are in [`ls_operations.h`](../include/livesprite/ls_operations.h);
at runtime, `describeOperation(id)` reports every drivable parameter and its
type.

An operation is added to a layer and resolved in order:

```cpp
FillSolidOp fill;
fill.targetRegion  = region;
fill.paletteRole   = 1;
const OperationId id = ctx->addOperation(layer, fill).value;
```

Nearly every operation carries `blend` and `opacity`. Fills and outlines carry a
`paletteRole` with a `fallbackColor` for when nothing resolves.

## Fills

| Operation | What it does |
|---|---|
| `FillSolidOp` | One colour across a region, from a role or a literal |
| `FillSemanticColorOp` | A colour role, and nothing else: the palette decides everything |
| `FillGradientOp` | A ramp along an axis between two points, optionally repeating |
| `FillRampOp` | A ramp across the region bounds at an angle |
| `FillDitherOp` | A value resolved against a threshold pattern, picking between ramp stops |
| `FillNoiseOp` | Hashed value per cell through a ramp: deterministic, seeded |
| `FillLinePatternOp` | Analytic line screen: spacing, angle, line width, two roles |
| `FillTexturePatternOp` | Tiles a pattern, with scale, offset and rotation. A tile carrying colours paints them directly, which is how a tileable texture fill works |

`FillDitherOp` is the one to understand, since it covers both classic dithering
and gradients:

```cpp
FillDitherOp dither;
dither.targetRegion   = region;
dither.ramp           = ramp;          // 2 stops = two-tone, more = banded gradient
dither.pattern        = pattern;       // a threshold matrix
dither.density        = 0.5f;          // used when modulation is Constant
dither.modulation     = DitherModulation::Linear;   // or Constant, Radial, Angular
dither.gradientStart  = {0.f, 0.f};
dither.gradientEnd    = {24.f, 0.f};
dither.anchor         = PatternAnchor::Local;       // Local, Global or Fixed
dither.coordinateSpace = CoordinateSpace::Object;   // frame for the gradient points
```

`anchor` also applies to `FillNoiseOp`, `FillLinePatternOp` and
`FillTexturePatternOp`.

## Strokes

| Operation | What it does |
|---|---|
| `StrokePolylineOp` | Strokes a polyline: width, cap, join, miter limit, taper, snap, optional pattern |
| `StrokeCurveOp` | The same for a flattened curve |
| `StrokeRegionBoundaryOp` | Strokes the edge of a region. An authored loop uses its own recorded outline |
| `StrokeBrushOp` | Stamps along a path: size, spacing, deterministic scatter, optional pattern as the stamp shape |
| `StrokePixelPathOp` | Paints exactly the pixels a path covers, no width |

Strokes are geometry: one quad per segment, a join shape at every interior
vertex, a cap at each open end. `StrokeJoin::Miter` falls back to a bevel when a
turn is too sharp for `miterLimit`. `SnapPolicy::Grid` puts vertices on pixel
corners and `HalfGrid` on pixel centres, which is what keeps a one-pixel line
from straddling two columns. A `strokePattern` thins the mark along its length.

## Erasing

| Operation | What it does |
|---|---|
| `ClearRegionOp` | Clears what the layer has drawn so far inside `targetRegion`. Shapes and fills before it stay live, anything after it is not cleared, and outlines or shadows after it follow the erased result |

An eraser that works on shapes without baking them: the erased pixels are a
region like any other, grown and shrunk by the same calls.

## Fading

| Operation | What it does |
|---|---|
| `FadeOp` | Multiplies the alpha of everything the layer has drawn so far by `opacity`; what comes after it is not faded. A cel's own opacity under the layer's, where an editor keeps layers the same across frames |

## Tilemaps

| Operation | What it does |
|---|---|
| `DrawTilemapOp` | Draws a tilemap -- a grid of cells made with `createTilemap` -- from a tileset sprite whose layers are the tiles (tile 1 its first layer, taken from its top-left `tileWidth` x `tileHeight`), each cell turned by its flip bits, the grid's corner at `origin`. Editing a tile's layer redraws every cell that names it; a cloned layer gets a grid of its own |

A cell is 0 for none or a tile number from 1 with `kTileFlipD` (x and y
swapped, done first), `kTileFlipX` and `kTileFlipY` above it; a quarter turn
clockwise is D and X together. `setTilemapCell` changes one cell.

## Outlines

| Operation | What it does |
|---|---|
| `GenerateSilhouetteOutlineOp` | Outlines whatever the layer has drawn so far, optionally limited by a boundary |
| `GenerateInnerOutlineOp` | Inside the region edge |
| `GenerateOuterOutlineOp` | Outside it; `OutlineDiagonal` decides how corners are treated |
| `GenerateRegionOutlineOp` | Inside, centre or outside, by `OutlineSide` |
| `GenerateMaterialBoundaryOutlineOp` | Where two different colours meet inside a layer |
| `CleanupOutlineOp` | Post-processes an earlier outline: drops isolated pixels, optionally fills notches |
| `JoinCornersOp` | Bridges the gap between two outlines within a radius |
| `ResolveOutlineCollisionsOp` | Where outlines overlap, the first one listed owns the pixel |
| `GenerateDropShadowOp` | What the layer has drawn so far -- or the whole sprite, naming it -- moved by `offset` and drawn in one colour where nothing else is. Never part of the silhouette a whole-sprite outline traces |

The last three name earlier operations by id, so order matters: they act on
what has already been resolved in the same layer.

## Transforms

| Operation | Notes |
|---|---|
| `TranslateOp` | `delta`, plus a rounding policy |
| `RotateOp` | `angleDegrees`, `pivot` or `pivotFallback`, sampling, rounding. `SamplingPolicy::RotSprite` enlarges the source 8x with Scale2x and samples that, for pixel art that keeps clean diagonals when turned |
| `ScaleOp` | `factor`, pivot, sampling, rounding |
| `MirrorOp` | `MirrorAxis::X` flips left to right |
| `ShearOp` | Shear factors per axis |
| `SkewOp` | Skew angles per axis |
| `SquashOp` / `StretchOp` | Volume-preserving: one axis by the factor, the other by its reciprocal. With a boundary, the falloff is applied per pixel |
| `MatrixTransformOp` | An arbitrary `Mat3f`, for anything the named ones do not cover |

**Targeting.** Every transform carries `targetRegion`, `targetLayer` and
`target`. The narrowest one set wins: a region if named, else the layer, else
the sprite owning the operation.

There is deliberately no operation for rotate-and-scale about a pivot (that is
`RotateOp` then `ScaleOp`, both of which take pivots), none for attaching to a
socket (that is `attachSprite`), and none for placing a whole sprite (that is
the sprite transform).

## Deforms

| Operation | What it does |
|---|---|
| `BendOp` | Progressive rotation along an axis, with falloff |
| `WarpOp` | Handle points with displacements, within a radius |
| `LatticeDeformOp` | A grid of control points, bilinear between them |
| `EnvelopeDeformOp` | Follows a curve as an envelope |
| `PinDeformOp` | Pins and their targets, weighted by distance and stiffness |
| `WeightedDeformOp` | Handles with explicit per-handle weights |
| `BoundaryDeformOp` | Maps a region toward a target shape |
| `PathDeformOp` | Bends content along a path, optionally following its tangent |

A deform moves geometry, as the affine transforms do: every shape drawn before
it is carried through its map point by point -- edges cut into half-pixel steps
first, so a straight edge bends -- and rasterized where it lands, so a bent
line is still a closed line and the fill inside it still meets it. A bend,
lattice, path, envelope or boundary deform lays itself over the bounds of what
the layer has drawn by then, measured from those shapes. A layer that still
holds pixels, or an effect before the deform, is forward-mapped as a picture
with hole repair instead.

A `boundary` scopes a deform and supplies a falloff; the influence a boundary
reports through `getBoundaryInfluence` is exactly the influence the compiler
applies. What lies where that influence is nothing stays put -- a shape whose
outline is the boundary keeps its outline, and what is inside it moves.

## Plugin operations

`PluginOp` carries a `typeId` and a parameter bag, and is resolved by whatever
was registered under that id — an operation resolver, a fill resolver, or a
transform resolver. Registration declares the parameter schema, what the
operation reads (so it joins the dependency graph), and whether it is
deterministic. Non-deterministic operations are refused in the `Export` profile.

```cpp
PluginOperationDesc desc;
desc.typeId          = "com.myapp.myop";
desc.isDeterministic = true;
desc.getDependencies = [](const PluginOp& op) { /* ids this op reads */ };
desc.resolve         = [](const PluginOp& op, PluginResolveContext& ctx) {
    writePixel(*ctx.outputBuffer, x, y, ctx.resolveColor(role));
    return LSError::None;
};
ctx->registerOperationType(desc);
```

Adding a `PluginOp` whose type is not registered fails at authoring time rather
than at compile time.
