# Concepts

The model, in the order it makes sense to learn it.

## Operations are the source truth

The engine never stores final pixels as truth. It stores region definitions,
fill rules, stroke rules, transforms, deforms, palette bindings and spatial
metadata. Pixels are derived from that stack on demand, and the first moment
they exist is `compileSprite`.

Three consequences follow, and most of the design is downstream of them:

- **Everything is serializable**, because the operation stack *is* the save
  file. There is no second representation to keep in sync.
- **Everything is deterministic.** Same input, same engine version, same
  profile, same pixels — on any platform, every run.
- **Nothing degrades.** A transform is a parameter that participates in the next
  compile, not a resampling of what the last compile produced. Remove it and the
  original returns exactly.

## The entity hierarchy

```
Document                 canvas size, palettes, ramps, patterns, geometry, regions
└── Sprite               its own transform, pivots, sockets, boundaries
    ├── Group            a compositing unit: opacity, blend, visibility
    └── Layer            an ordered stack of operations
        └── Operation    a fill, stroke, outline, transform or deform
```

Everything is addressed by a typed opaque id (`SpriteId`, `LayerId`, …). The
engine owns all storage; ids are your handles and no raw pointer ever crosses
the API.

## Regions

A region is a shape, stored as horizontal intervals: sorted, non-overlapping,
non-abutting spans per row. It is the engine's universal answer to "which pixels
does this affect", and every fill, stroke and mask names one.

Regions come from three places:

- **Geometry** — `createRegionFromGeometry`. The region tracks its source, so
  editing the rectangle updates every fill on it.
- **Authored pixels** — `createRegionFromPixels`. A closed same-colour loop
  seals its interior; diagonal-only contacts are bridged so a 1px diagonal
  outline still counts as closed; an open stroke encloses nothing.
- **Boolean maths** — union, subtract, intersect, xor, clip, mask, invert,
  merge, split, connected components, simplify, inset, outset, expand, contract.

A region built from geometry can also carry what was erased from it
(`setRegionErase`: the strokes that erased it) and a clip (`setRegionClip`):
other regions or shapes that add to, take from or narrow where it may draw,
read from what they draw now and moved with it. That is how a pencil stroke
kept to what lies under it -- a lock-alpha or replace ink, a shading pass, a
stroke inside a selection -- stays a stroke. A freehand stroke's brush can be a
shape of its own (`PenStroke::tip`), a custom brush stamped along the path and
turned with it. All of these are shapes, so the region is still its geometry
wherever it is turned.

Regions can also be edited in place with `addPixelsToRegion`,
`erasePixelsFromRegion` and `setRegionIntervals`, which is how a pencil
accumulates without leaving one region per stroke. A region edited by hand stops
tracking its geometry, so a later geometry edit cannot overwrite the drawing.

## Compiling

`compileSprite` runs the pipeline: dependency and cache lookup, operation
resolution, region compilation, fill and stroke rasterization, transform and
deform resolution, layer and group compositing, sampling, palette quantization,
alpha policy.

**Geometry first.** A layer whose marks are shapes compiles by moving each
shape through every transform and deform after it -- and then through the
sprite's own transform and its place in an assembly -- and rasterizing it
where it lands. That is the only place pixels enter. A layer that holds pixels,
or has an effect (an outline, a shadow, a plugin) before a move, takes the
older path: drawn, then moved as one picture.

A `CompileProfile` decides what kind of answer you want:

| Profile type | For |
|---|---|
| `Preview` | Fast, for a canvas |
| `Export` | Full quality; non-deterministic plugins are refused here |
| `Debug` | Full quality plus a populated `trace` explaining what happened |
| `MaskOnly` | Alpha only, no colour |
| `BoundsOnly` | Just the bounds, no raster |

The profile also carries sampling, rounding, alpha and palette policies, and
`exportOrigin`, which matters only for patterns in Export space.

Related entry points: `compileLayer`, `compileRegion`, `compileToRaster`,
`compileToMask`, `compileBoundsOnly`, `compileToCollisionShape`, and
`compileAssembly` for a sprite with things attached to it.

## Transforms happen in the compile, not to the pixels

A transform operation resolves geometrically: coverage is decided by
inverse-mapped sub-samples, and each output pixel takes the **colour of the
source pixel it sampled** rather than an interpolation of several. Thin features
that fall between samples are rescued by forward projection, then pixel-art
cleanup closes notches and drops orphans, and interior gaps are repaired.

That is why a quarter turn preserves the pixel count exactly, and why a 25°
turn looks like a pixel artist rotated it rather than like a photograph of one.

**Placing a whole sprite is not an operation.** A sprite carries its own
transform (`setSpriteTransform`, or the composing helpers `translateSprite`,
`rotateSprite`, `scaleSprite`, `mirrorSprite`). That transform is the single
frame that sockets, pivots and attached children all resolve through, so moving
a sprite brings everything hanging off it. Operations transform content *inside*
a sprite; the sprite transform places the sprite itself.

## Palettes and roles

The engine owns colour resolution. Operations reference a `ColorRole`, and the
palette turns it into a `Color` at compile time. A role can be bound:

- on the operation (`paletteRole`), which always wins;
- on the region (`bindRegionToPaletteRole`), so a fill naming no role paints
  whatever that shape is made of;
- and falls back to a literal `fallbackColor` if nothing resolves.

Sprites inherit the document palette unless they bind their own.

## Dithering

A dither takes a **value**, compares it against a **threshold pattern**, and
picks between the two ramp stops the value falls between. That single mechanism
covers both classic two-tone dithering (two stops, constant density) and
dithered gradients (more stops, a modulated value: `Linear`, `Radial`,
`Angular`).

Patterns are threshold *matrices*, not one-bit stamps, which is why the same
tile works at any density and at every step of a gradient. Twelve are built in
— `Bayer2/4/8`, `Checker`, `HorizontalLines`, `VerticalLines`, `DiagonalLines`,
`CrossHatch`, `Dots`, `ClusteredDot`, `Noise`, `Grid` — and you can supply your
own tile, or import one from a raster as either a threshold screen or a tileable
colour texture.

## Pattern anchoring

What a pattern does while its object moves. This is the difference between
dither that reads as part of the artwork and dither that shimmers.

| Anchor | Under a move | Under a rotation |
|---|---|---|
| `Local` | travels with the object | rotates with it |
| `Global` | travels with the object | stays level with the canvas |
| `Fixed` | stays put | stays level |

`Local` resolves at paint time. `Global` and `Fixed` are re-resolved after the
transform stack, using a provenance plane that tracks which pixels the fill
owns, so the pattern keeps its own frame instead of inheriting the rotation.

Separately, `CoordinateSpace` says where *gradient geometry* is measured:
`Object` (this shape), `Sprite` (everything the sprite draws), `Canvas`, or
`Export` (the frame being written, for sheet cells).

## Pivots, sockets and attachment

- A **pivot** is a named point in sprite space: where rotation and scale
  originate, and the point a child presents when it attaches. It can be placed
  from the compiled bounds so it follows the artwork.
- A **socket** is a named frame — position, angle, scale — on a sprite. Its
  *world* transform includes the sprite's own transform and every attachment
  above it, so a socket on a turning arm turns with the arm.
- An **attachment** hangs one sprite off another's socket, presenting one of its
  own pivots, with an optional joint offset and a flag to composite behind the
  parent. Chains resolve to any depth; a cycle is refused rather than built.

`compileAssembly` compiles every sprite in the tree from its own operations,
where the chain puts it, so an articulated figure renders in one call. The
placement is the last move each of its marks takes, so a sprite its parent
turns is drawn turned rather than drawn and then turned as a picture.

## Dependencies and caching

The engine maintains a dependency graph: change a geometry and the regions built
from it rebuild, the fills on those regions recompile, their layers recompile,
and their sprites recompile. `markDirty` propagates; `compileDirtyOnly`
recompiles just what is stale.

Compiles are cached per entity, per profile, per resource revision, per engine
version. A cached compile costs microseconds — see
[performance](performance-and-limits.md).

## Live instructions

The engine knows nothing about time, keyframes, easing or tracks, and should
not. What it offers an animation system is narrower:

- **Every operation parameter is addressable by name.** `describeOperation`
  reports what can be driven and with what type. The names are the same ones the
  save file uses, because both walk one field table.
- **Instructions carry absolute values, never deltas**, so scrubbing a timeline
  backwards reproduces the same pixels.
- **A batch lands as one frame.** `applyInstructions` validates everything
  before applying anything, so a bad instruction leaves the document untouched
  rather than half posed. `renderFrame` applies a frame and compiles.

## Editing support

- `snapshotDocumentState` / `restoreDocumentState` capture and restore a
  document **with every id intact**, which is what undo needs: renumbering would
  invalidate every handle the interface holds. It is an in-memory capture, fast
  enough to take on every action.
- **Metadata** is namespaced key/value data on any entity, opaque to the engine,
  carried through save, load and undo.
- **Packages** carry whole files beside the document. See
  [file format](file-format.md).

## Plugins

Apps and third parties register operation types, pattern types, fill resolvers,
transform resolvers and compile policies. A plugin operation declares what it
reads, so it takes part in the dependency graph like anything else, and declares
whether it is deterministic — non-deterministic operations are refused in the
Export profile.
