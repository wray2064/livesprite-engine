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
