# LiveSprite Engine

A deterministic sprite substrate in C++17. It stores sprites as mathematical
operations, resolves their dependencies, and compiles them into pixels on
demand.

It is not an editor, an animation system, or a file importer. Two applications
are built on top of it: **Sprit's'fast**, a lightweight editor that introduces
the paradigm, and **Sprit's'pract**, a full production suite. The engine knows
about neither. It exposes maths; the apps supply intent.

## The idea in one paragraph

A traditional sprite pipeline treats pixels as the truth: every rotation
resamples them, every fill bakes a colour in, and quality drains away with each
edit. LiveSprite treats the *instructions* as the truth. A fill is a standing
rule about how a region gets its colour, a rotation is a parameter, and pixels
are a compiled artefact rebuilt from source every time. Rotate a sprite forty
times and the fortieth compile is exactly as clean as the first, because the
thirty-nine before it were never stored.

## Build

Requires CMake 3.20 and a C++17 compiler. On Windows, `build.bat` finds the
Visual Studio toolchain and does the rest:

```bash
./build.bat
```

Or directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build
```

The library builds with warnings as errors and RTTI off. No third-party
dependencies: not for graphics, not for JSON, not for the container format.

## Try it

```bash
build/livesprite_example        # the worked example, prints its result as text
build/livesprite_testbed --out testbed   # renders 15 scenes into a gallery page
build/livesprite_bench          # performance baseline
ctest --test-dir build          # 11 suites, plus the example
```

`livesprite_testbed` writes `testbed/index.html`, a self-contained page where
every image was compiled by the engine. Several scenes compute a verdict rather
than asking you to compare by eye.

## Documentation

| Document | What is in it |
|---|---|
| [Getting started](docs/getting-started.md) | Build a sprite in code, step by step |
| [Concepts](docs/concepts.md) | The model: regions, operations, compiling, anchoring, attachment |
| [Operations](docs/operations.md) | The catalogue: all 39 operation types and their parameters |
| [File format](docs/file-format.md) | The document, the package container, metadata, versioning |
| [Performance and limits](docs/performance-and-limits.md) | Measured numbers, and what the engine does not do |
| [Engine spec](LIVESPRITE_ENGINE_SPEC.md) | The implementation contract |
| [Architecture](Livesprite%20architecture.txt) | The wider design: engine, Fast, Pract |

## Repository

```
include/livesprite/   the public API: one header per area, livesprite.h pulls them all in
src/                  the implementation, split the way the spec splits it
tests/                11 suites, one per area, run under ctest
tools/                the testbed gallery and the benchmark
examples/             the worked example the documentation quotes
```

## Status

Feature-complete against the spec, with the deviations recorded in the spec
documents themselves. Eleven test suites cover geometry, entity storage,
compilation, dithering, anchors, rendering controls, live instructions,
serialization, packaging, editing support, and plugins.

Known limits are listed in [performance and limits](docs/performance-and-limits.md)
rather than left for you to discover.
