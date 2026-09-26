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

The library builds with warnings as errors, RTTI off, and the floating-point
contract pinned so the arithmetic cannot drift between compilers. No third-party
dependencies: not for graphics, not for JSON, not for the container format.

To use it from another project, or another language, see
[embedding](docs/embedding.md):

```bash
find_package(LiveSprite REQUIRED)        # C++
target_link_libraries(app PRIVATE livesprite::livesprite)

cmake -S . -B build -DLS_BUILD_SHARED_C=ON   # everything else: a loadable
cmake --build build                          # livesprite_c.dll / .so / .dylib
```

## Try it

```bash
build/livesprite_example        # the worked example, prints its result as text
build/livesprite_testbed --out testbed   # renders 15 scenes into a gallery page
build/livesprite_bench          # performance baseline
ctest --test-dir build          # 13 suites, plus the example
```

`livesprite_testbed` writes `testbed/index.html`, a self-contained page where
every image was compiled by the engine. Several scenes compute a verdict rather
than asking you to compare by eye.

## Documentation

| Document | What is in it |
|---|---|
| [Getting started](docs/getting-started.md) | Build a sprite in code, step by step |
| [Embedding](docs/embedding.md) | Consuming the engine from C++, and from every other language |
| [Concepts](docs/concepts.md) | The model: regions, operations, compiling, anchoring, attachment |
| [Operations](docs/operations.md) | The catalogue: all 41 operation types and their parameters |
| [File format](docs/file-format.md) | The document, the package container, metadata, versioning |
| [Performance and limits](docs/performance-and-limits.md) | Measured numbers, and what the engine does not do |
| [Licensing](LICENSING.md) | What licence applies where, and the reasoning |
| [Contributing](CONTRIBUTING.md) | The licence grant, the determinism rules, adding an operation |
| [Engine spec](LIVESPRITE_ENGINE_SPEC.md) | The implementation contract |
| [Architecture](Livesprite%20architecture.txt) | The wider design: engine, Fast, Pract |
| [Handoff](https://github.com/wray2064/sprits-fast/blob/main/docs/handoff.md) | Where both products stand, what each part is, and the traps — lives in the editor repo |

## Repository

```
include/livesprite/   the public API: one header per area, livesprite.h pulls them all in
src/                  the implementation, split the way the spec splits it
tests/                18 suites, one per area, run under ctest (one of them C)
tools/                the testbed gallery and the benchmark
examples/             the worked example the documentation quotes
```

## Status

Feature-complete against the spec, with the deviations recorded in the spec
documents themselves. Thirteen test suites cover geometry, entity storage,
compilation, dithering, anchors, rendering controls, live instructions,
serialization, packaging, editing support, plugins, cross-platform determinism,
and the C ABI.

Known limits are listed in [performance and limits](docs/performance-and-limits.md)
rather than left for you to discover.

## Licence

The engine is under the **Business Source License 1.1**, and each version becomes
Apache-2.0 four years after it is published. In short:

- **Building a sprite editor on it, including a commercial one, is explicitly
  allowed** — even one that competes with ours. That is the point.
- **Offering it to others as a sprite engine, library or service is not.**
- **Embedding it at run time in a shipped game is reserved** and available under
  a separate commercial licence. Authoring, converting and exporting are free.

`include/livesprite/livesprite_c.h`, the examples and the docs are Apache-2.0, so
a binding author never has to think about licence compatibility to copy a
declaration. Every file carries an `SPDX-License-Identifier`.

The reasoning behind the split — including why a blanket "no competing products"
clause would have been a mistake here — is in [LICENSING.md](LICENSING.md).
