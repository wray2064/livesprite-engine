# Performance and limits

Measured numbers, and an honest list of what the engine does not do. Both exist
so you find out here rather than three weeks into building on it.

## Baseline

`build/livesprite_bench` measures the paths an editor leans on. Numbers below
are from a desktop Windows build (RelWithDebInfo, MSVC); run it on your own
target rather than trusting these.

At **128×128, 8 layers**, each layer a dithered gradient fill plus an outline:

| Path | Cost |
|---|---|
| Compile, everything dirty | 3.5 ms |
| Compile, cached | 0.002 ms |
| Compile, one parameter driven | 4.1 ms |
| Draw a stroke, then compile | 4.3 ms |
| Region union | 1.6 ms |
| Assembly of 4 sprites | 4.3 ms |
| Snapshot document state | 0.085 ms |
| Restore document state | 0.139 ms |
| Serialize document (a file save) | 49 ms, 141 KB |

At **256×256, 20 layers**:

| Path | Cost |
|---|---|
| Compile, everything dirty | 37 ms |
| Compile, one parameter driven | 31 ms |
| Assembly of 4 sprites | 32 ms |
| Snapshot / restore | 0.14 / 0.41 ms |
| Serialize document | 101 ms, 275 KB |

### What that means for an app

- **Undo is free.** Snapshot and restore are in-memory state copies; take one
  per action without thinking about it. Do not use `serializeDocument` for undo
   — it is two orders of magnitude slower, and it is for files.
- **Cached compiles are free.** Idling redraws cost microseconds, so a canvas
  that recompiles on every paint event is fine as long as nothing changed.
- **Above roughly 256×256 with many layers, compile on a background thread** or
  narrow what you recompile. A 37 ms compile in the middle of a mouse-move
  handler is a visibly stuttering canvas.
- **Saving is not free.** 100 ms for a large document means autosave belongs off
  the interaction path.

Compile cost scales with canvas area × layer count, because each layer resolves
over the output frame. It does not scale meaningfully with operation count
within a layer.

## Hard limits

These are refused, not merely discouraged. A canvas size arrives from an
application or from a file somebody sent, and without a bound the second of those
is a memory-safety problem rather than a performance one: past a width of 2^30
the 32-bit stride overflows and a raster comes back reporting a size its storage
does not have, with every bounds check in `writePixel` passing on a buffer that
is not there.

| Limit | Value | Why |
|---|---|---|
| `kMaxCanvasDimension` | 16384 per side | A long sprite sheet is cheap; a long one is not the problem |
| `kMaxCanvasPixels` | 16,777,216 (4096x4096) | Holds one raster to 64 MB whatever the aspect |
| `kMaxLayersPerSprite` | 1024 | Compile cost is linear in layers on top of area |

**Area, not just a side.** A dimension-only cap would admit 16384x16384 -- a
gigabyte per raster, minutes per compile -- while refusing a 12000x200 sheet that
costs almost nothing. Either side may reach the dimension cap; the two together
may not exceed the area cap.

The numbers come from measuring this engine. Compile cost tracks canvas area
times layer count:

| Canvas | 1 layer | 8 layers | One raster |
|---|---|---|---|
| 256x256 | 3.2 ms | 135 ms | 0.2 MB |
| 512x512 | 64 ms | 178 ms | 1 MB |
| 1024x1024 | 103 ms | 839 ms | 4 MB |
| 2048x2048 | 297 ms | 1.6 s | 16 MB |
| 4096x4096 | 1.1 s | 7.7 s | 64 MB |

So the area cap sits where a single compile is seconds rather than minutes. Past
roughly 512x512 nothing here is interactive, which is a fact about the design
rather than a bug: this engine compiles from operations instead of blitting a
stored bitmap.

For comparison, Aseprite's 65535x65535 ceiling is the range of the 16-bit field
in its file header rather than a considered working size -- its own author puts
the practical figure near 9000, and users report trouble well below that. Piskel,
being browser-based, started at 100x100 and describes itself as being for small
sprites. Krita and Pixelorama impose no hard pixel limit and are bounded by
memory. Ours is deliberately stated rather than discovered.

## Limits worth knowing before you build on it

### By design

- **No anti-aliasing.** This is a pixel-art substrate: coverage decisions are
  binary and colours come from source pixels, never from blending several. If
  you want soft edges, this is the wrong engine.
- **Single-threaded.** `LSContext` is not thread-safe. One thread at a time, or
  lock outside. Compiles do not spawn threads.
- **No time.** No frames, no keyframes, no playback. An app drives parameters
  through instructions; the engine renders whatever it is told, whenever it is
  told.
- **No image codecs.** Compiles hand back an RGBA buffer. PNG and GIF encoding
  are the app's business.

### Known gaps

- **A sprite has one parent.** A fan-in rig, where one part hangs off two
  sockets, needs explicit duplication.
- **Free-form deforms are approximate for coverage.** Bend, warp, lattice,
  envelope, pin, weighted, boundary and path deforms forward-map with hole
  repair, so a heavy deform can leave artefacts an affine transform would not.
- **`compilePreview` clamps rather than downscales.** Asking for a smaller
  preview of a large canvas gives you a crop of the canvas size, not a scaled
  image. Scale on your side.
- **No cross-major migration steps exist.** The chain is implemented and tested;
  the table is empty because no breaking version has shipped.
- **Group nesting is flat.** A layer belongs to at most one group, and groups do
  not contain groups.
- **A layer cannot move between sprites.** Recreate it.
- **Interleaved groups flush more than once.** If a sprite's layer order is
  A, B, A where A and B are groups, group A composites in two passes. The result
  is deterministic and matches the declared order, but it is not one buffer.

### Deviations from the spec documents

Recorded in the spec files themselves, repeated here so they are in one place:

- `AnchorTransform` and `PivotTransform` are not operations. Attachment is
  sprite-level state; rotate-and-scale about a pivot is two operations that
  already take pivots.
- Stroke "coverage policy" does not exist. Strokes mark a pixel when its centre
  falls inside the stroke geometry, the same rule regions use, and `SnapPolicy`
  aligns them to the grid.
- `BindColorRole` is `setPaletteColor`, which already binds a role to a colour.
  `BindRegionToPaletteRole` exists as `bindRegionToPaletteRole`.

## Determinism

Same input, same engine version, same profile produces the same bytes, on any
platform and every run. That covers compiled rasters, serialized documents and
written packages. Ordered containers are used wherever iteration order could
reach the output, hashes are explicit rather than library-defined, and packages
carry fixed timestamps.

Two things that would otherwise break it across platforms are dealt with
directly. The engine carries its own sine, cosine, tangent and atan2 rather than
calling the platform's, because IEEE-754 does not specify those to the last bit
and the major C libraries genuinely disagree there — one bit in an angle moves a
coverage decision, which moves a pixel. And the build pins `/fp:precise` and
`-ffp-contract=off`, because GCC and Clang otherwise fuse `a*b+c` into an FMA
and do different arithmetic from MSVC on identical source.

The `livesprite_determinism` suite locks four scenes chosen to lean on that
trigonometry to golden hashes, and CI runs it under MSVC, GCC, Clang and Apple
Clang. A differing hash names a portability bug rather than a flaky test.

The one way to break it is a plugin that declares `isDeterministic = false`.
Those are refused in the `Export` profile for exactly that reason.

## Error handling

Nothing throws across the API. Every call returns `Result<T>` or `VoidResult`
carrying an `LSError`, and `lsErrorString` names it. The only operation that can
fail for reasons outside the document is raster allocation, which catches for
itself and returns `RasterAllocationFailed`.

RTTI is off in the library build (`/GR-`, `-fno-rtti`).
