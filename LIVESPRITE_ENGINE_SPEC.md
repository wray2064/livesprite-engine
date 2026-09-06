# LiveSprite Engine — Implementation Specification
> **For Codex:** This document defines the complete scope, data model, and behavioral contract of the LiveSprite Engine. Read this before touching any file.

---

## 1. What This Is

The LiveSprite Engine (`ls::LSContext`) is a **deterministic, mathematical sprite substrate** written in C++17.

It is not:
- An editor
- An animation system
- A UI framework
- A file format parser (GLB, video, etc.)
- An export pipeline

It is exactly:
> A system that stores sprite instructions as mathematical operations, resolves their dependencies, and compiles them into deterministic raster output.

Two applications will be built on top of this engine later:
- **Sprit's'fast** — lightweight open-source sprite editor (plugs into this engine)
- **Sprit's'pract** — full production suite (also plugs into this engine)

The engine must never know about either. It exposes math. Apps provide intent.

---

## 2. Foundational Principle: Operations as Source Truth

**The engine never stores final pixels as source truth.**

Instead it stores:
- Region definitions (geometric shapes as interval sets / contours)
- Fill operations (what color/pattern fills a region, and how)
- Stroke operations (what outlines exist)
- Transform operations (rotation, scale, shear, squash, stretch — as math)
- Deform operations (bend, warp, lattice — as math)
- Palette bindings (semantic color roles)
- Spatial metadata (pivot, sockets, boundaries)

Pixels are a **compiled artifact** — they are derived from the operation stack on demand.

The first time final raster pixels exist is at `CompileToRaster()`. Everywhere else is math.

### Why This Matters for Implementation

- Every operation must be **serializable** (it is the save file)
- Every operation must be **deterministic** (same input + same engine version = same output)
- Operations form a **dependency graph** — changing a region invalidates dependent fills
- Recompilation must be **incremental** — only dirty nodes recompile
- Transforms must be **non-destructive** — rotation does not modify source regions

---

## 3. Project Structure

```
livesprite_engine/
├── LIVESPRITE_ENGINE_SPEC.md       ← this file
├── Livesprite architecture.txt     ← the wider design: engine, Fast, Pract
├── CMakeLists.txt
├── build.bat                       ← configure and build through the VS toolchain
├── include/
│   └── livesprite/
│       ├── livesprite.h            ← master include (include this, get everything)
│       ├── ls_types.h              ← IDs, Result<T>, enums, math primitives
│       ├── ls_geometry.h           ← geometry primitives and boolean ops
│       ├── ls_operations.h         ← all operation data types (the Op structs)
│       ├── ls_api.h                ← LSContext — the main engine API
│       └── ls_plugin.h             ← plugin/extension registration interface
├── src/
│   ├── ls_internal.h               ← engine-private state (LSContext::Impl)
│   ├── ls_reflect.h                ← one field table per operation
│   ├── ls_context.cpp              ← LSContext implementation (PIMPL)
│   ├── ls_geometry.cpp             ← geometry math
│   ├── ls_compile.cpp              ← compilation pipeline
│   ├── ls_dependency.cpp           ← dependency graph and cache
│   ├── ls_instructions.cpp         ← addressable parameters, live frames
│   ├── ls_serialize.cpp            ← serialization / migration
│   └── ls_json.cpp                 ← deterministic JSON for the save format
├── tests/                          ← one suite per area, run under ctest
└── tools/
    └── ls_testbed.cpp              ← renders the engine into a gallery page
```

Three files are not in the original plan and earn their place:

- `ls_internal.h` holds `LSContext::Impl`, so the implementation files share one
  definition of engine state without exposing it in a public header.
- `ls_reflect.h` holds one field table per operation. Serialization walks it,
  and so does live parameter addressing, so a field cannot be saveable but not
  drivable, or the reverse.
- `ls_json.cpp` is a small deterministic JSON writer and parser. The save format
  is text, and two saves of the same document must be byte identical, which
  rules out a hash-ordered library.

---

## 4. Core Data Model

### 4.1 Entity Hierarchy

```
LSContext
└── Document(s)
    └── Sprite(s)
        ├── Pivot
        ├── Socket(s)
        ├── Boundary(ies)
        └── Layer(s)          ← ordered, with compositing rules
            └── OperationId(s) ← ordered operation stack
```

All entities are identified by typed opaque `uint64_t` handles. The engine owns all storage. Apps hold IDs.

### 4.2 Geometry Storage

Geometry objects (polylines, curves, regions, etc.) are stored separately in the document and referenced by `GeometryId`. They are **not** embedded inline in operations. This allows:
- Multiple operations to share the same geometry
- Geometry edits to propagate to all dependent operations via the dependency graph

### 4.3 The Operation Stack

Each layer holds an ordered list of `OperationId` values. An `Operation` is a `std::variant` over all concrete operation types (FillSolidOp, RotateOp, etc.). Operations are:
- Stored in a flat map: `OperationId → Operation`
- Ordered within a layer
- Serializable as their full parameter set
- Dependency-tracked (each op declares what it reads)

### 4.4 Palettes and Color Roles

The engine owns palette resolution. A `ColorRole` is an index into a palette. Operations reference `ColorRole`, not raw colors. This enables palette swapping without touching operations.

---

## 5. Compilation Pipeline

```
Operations + Geometry + Palette
        ↓
  Dependency Resolution
        ↓
  Transform Stack Resolution
        ↓
  Region Compilation (contours → interval sets → pixel coverage)
        ↓
  Fill/Stroke Rasterization (with coordinate space normalization)
        ↓
  Layer Compositing (blend modes, opacity, masks)
        ↓
  Raster Sampling (sampling policy → pixel values)
        ↓
  Palette Quantization (NearestPaletteResolve, alpha policy)
        ↓
  CompileResult (raster buffer + metadata)
```

### Compilation Profiles

Every compile call takes a `CompileProfile` specifying:
- Output resolution (width × height)
- Sampling method
- Coverage threshold
- Rounding policy
- Palette policy
- Alpha policy
- Profile type: `Preview | Export | Debug | MaskOnly | BoundsOnly`

**Determinism guarantee:** `CompileSprite(id, profile)` must return identical output for identical inputs across platforms and runs, for the same engine version.

### Dither Coordinate Spaces

Dither patterns must be evaluated in a stable coordinate space. The engine must support:
- `Object` — stable relative to the sprite's own geometry (default; prevents shimmer on motion)
- `Sprite` — relative to the sprite's bounding box
- `Canvas` — relative to the document canvas
- `Export` — relative to the export frame

This is critical. If not implemented correctly, dither patterns will shimmer when a sprite moves.

---

## 6. Dependency System

The engine must maintain a directed dependency graph over all entities.

### Dependency Rules
| When this changes... | These must recompile... |
|---|---|
| A `GeometryId` | All operations referencing it |
| A `RegionId` | All fills/strokes/outlines targeting it |
| A `PaletteId` | All operations bound to that palette |
| A `RampId` | All fill operations using it |
| A `PatternId` | All dither/pattern fill operations using it |
| A `BoundaryId` | All deforms/fills scoped to it |
| A layer's operation order | The layer's compiled output |
| A layer's blend mode/opacity | The parent sprite's compiled output |
| A pivot | All transforms using that pivot |
| A socket position | All sprite-attachment results |

### Dirty Propagation
- `MarkDirty(id)` propagates dirty state to all dependents
- `CompileDirtyOnly()` recompiles only the dirty subgraph
- Cache is keyed on `(OperationId, CompileProfile, engine_version)`

---

## 7. Serialization Contract

The engine owns the canonical LiveSprite binary/JSON representation.

### Requirements
- All operations must round-trip through serialize/deserialize with zero loss
- Unknown fields from newer engine versions must be preserved (forward compatibility)
- `MigrateVersion(data, from_version, to_version)` must handle version upgrades
- The serialized format is the **source of truth** — not any raster output

### Versioning
- Engine version is a `uint32_t` embedded in every serialized document
- Breaking changes increment the major version
- Migration functions live in `ls_serialize.cpp`

---

## 8. Plugin Interface

Third-party operations (and Fast/Pract's own extended ops) are registered via the plugin interface.

Each plugin operation must declare:
- `type_id` — globally unique string
- Input and output types
- Parameter schema (for serialization)
- A `resolve()` function: parameters → compile contribution
- Engine version compatibility range
- Determinism declaration

Plugin operations participate in the dependency graph like built-in operations.

---

## 9. What the Engine Must NOT Contain

These belong to Fast, Pract, or their toolkits — never to the engine:

| Forbidden in engine | Lives in |
|---|---|
| `ImportGLB`, `ScrubVideo`, `ExtractPose` | Pract References Toolkit |
| `CreateAnimationClip`, `PlayTimeline` | Pract Animotron |
| `CreatePractipuppet`, `SolveIK` | Pract Practipuppets |
| `ArrangeSpriteSheet`, `ExportGodotProject` | Pract Arranger |
| `BakeAnimationTrack` | Pract Animation Toolkit |
| `ExtractPaletteFromPhoto` | Pract References Toolkit |
| Any UI, canvas, viewport, file browser | Fast / Pract apps |
| Undo/redo presentation | App layer (engine exposes snapshots) |
| Naming conventions for layers/sprites | App layer |

The engine exposes `SnapshotSprite` and `SerializeOperation` — apps build undo systems on top.

---

## 10. Implementation Priorities (Build Order)

Build in this order. Each layer depends on the previous.

1. **`ls_types.h`** — IDs, Result\<T\>, enums, Vec2i/f, Mat3f, Color (no dependencies)
2. **`ls_geometry.cpp`** — Point, Line, Rect, Ellipse, Polygon, Polyline, Curve, IntervalSet, Contour, Bounds, Region, boolean ops
3. **`ls_context.cpp`** — Document/Sprite/Layer/Group CRUD, operation storage, palette storage
4. **`ls_dependency.cpp`** — Dirty marking, dependency graph, cache
5. **`ls_compile.cpp`** — Compilation pipeline, sampling policies, raster output
6. **`ls_serialize.cpp`** — Serialize/deserialize, version migration

Plugin interface can come last.

---

## 11. C++ Conventions

- **Standard:** C++17
- **Error handling:** `ls::Result<T>` — no exceptions thrown from engine code
- **Memory:** Engine owns all entity storage. Apps hold IDs. No raw pointers cross the API boundary.
- **Thread safety:** `LSContext` is not thread-safe. Caller is responsible for locking if needed. (Single-threaded compilation is fine for v1.)
- **PIMPL:** `LSContext` uses PIMPL to keep implementation details out of headers.
- **Naming:** `lowerCamelCase` for methods, `UpperCamelCase` for types, `ls_` prefix on all public headers, `ls::` namespace for all public symbols.
- **No RTTI**: enforced by the build (`/GR-` on MSVC, `-fno-rtti` elsewhere).
- **No exceptions across the API boundary**: every entry point returns
  `ls::Result<T>`. Raster allocation, the one operation that can fail for
  reasons outside the document, catches for itself and returns
  `LSError::RasterAllocationFailed`.

---

## 12. Fast/Pract Integration Points

When Fast or Pract are built, they will:

1. Call `ls::LSContext::create()` to get an engine instance
2. Call document/sprite/layer/operation APIs to build sprite state
3. Call `CompilePreview()` to drive their canvas display
4. Call `CompileToRaster()` / `CompileToMask()` for export
5. Call `Serialize()` / `Deserialize()` for save/load
6. Register plugin operations for any app-specific op types

The engine never calls back into the app. Data flows one way: app → engine → compiled output.
