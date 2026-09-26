# Embedding the engine

How LiveSprite gets into something else. There are two doors, and which one you
want depends on whether your code is compiled next to the engine or loads it at
runtime.

```
        C++ apps  ──►  livesprite.h      rich API: Result<T>, STL, operation structs
                             │
                             ▼
                        the engine
                             ▲
                             │
  everyone else  ──►  livesprite_c.h     opaque handles, error codes, no STL
```

The C header is a translation layer, not a second implementation. Both doors
open onto the same code, the same determinism, and the same file format.

## C++: `find_package` or `FetchContent`

Against an installed copy:

```cmake
find_package(LiveSprite REQUIRED)
target_link_libraries(my_app PRIVATE livesprite::livesprite)
```

Against the source, with no install step:

```cmake
include(FetchContent)
FetchContent_Declare(livesprite GIT_REPOSITORY <url> GIT_TAG <tag>)
FetchContent_MakeAvailable(livesprite)
target_link_libraries(my_app PRIVATE livesprite::livesprite)
```

Both give you the same target name, so you can switch between them without
touching the rest of your build. Pulled in as a subproject, the engine turns off
its own tests, testbed and install rules: you get the library, not our
scaffolding.

Installing it yourself:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /where/you/want/it
```

Version compatibility is `SameMinorVersion` while the major is 0. That is
deliberate: before 1.0 the API is not settled, and promising major-level
compatibility would be promising something we cannot keep.

## Everything else: the C ABI

The C++ header passes `std::vector`, `std::string` and `std::function` through
its interface. That is fine when your code is compiled with the engine and
impossible when it is not: a shared library built with MSVC cannot safely be
called by a Clang program, and no other language can call it at all.

`livesprite_c.h` is the same engine without that constraint.

```bash
cmake -S . -B build -DLS_BUILD_SHARED_C=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

That produces `livesprite_c.dll`, `liblivesprite_c.so` or
`liblivesprite_c.dylib`, exporting the C entry points and nothing else — the
C++ symbols are hidden, so a binding cannot reach past the stable surface by
accident.

### What the C API looks like

Every call returns `ls_error`; results come back through out parameters.
Handles are opaque `uint64_t`. Nothing throws — an allocation failure that would
have thrown becomes an error code, because letting an exception unwind into C is
undefined behaviour.

```c
ls_context* ctx = ls_context_create();

ls_id doc, sprite, layer, rect, region, fill;
ls_document_create(ctx, "badge", 24, 24, &doc);
ls_sprite_create(ctx, doc, &sprite);
ls_layer_create(ctx, sprite, "body", &layer);

ls_vec2f origin = { 6.f, 6.f };
ls_geometry_rect(ctx, doc, origin, 12.f, 12.f, 0.f, &rect);
ls_region_from_geometry(ctx, rect, &region);

ls_operation_add(ctx, layer, "FillSolidOp", -1, &fill);
ls_operation_set_id(ctx, fill, "targetRegion", region);

ls_profile profile = { LS_PROFILE_EXPORT, 24, 24, LS_PALETTE_UNCONSTRAINED, {0, 0} };
ls_raster* raster = NULL;
if (ls_compile_sprite(ctx, sprite, &profile, &raster) == LS_OK) {
    const uint8_t* rgba = ls_raster_data(raster);   /* width*height*4 */
    ls_raster_release(raster);
}
ls_context_destroy(ctx);
```

### Operations are named, not structured

The C++ API has 41 operation structs. Mirroring them in C would mean 41 more
struct definitions, kept in step by hand, and a new operation would be a
breaking ABI change every time.

So the C API does not mirror them. An operation is created by type name and
configured by parameter name:

```c
ls_id turn;
ls_operation_add(ctx, layer, "RotateOp", -1, &turn);
ls_operation_set_id(ctx, turn, "targetLayer", layer);
ls_operation_set_float(ctx, turn, "angleDegrees", 45.f);
```

Those names are not a second vocabulary invented for C. They are the same field
tables the save file and the live instruction system already walk, so a
parameter cannot exist for C++ and be missing here. A new operation type becomes
callable from every binding the moment it is added to the engine, with no ABI
change at all.

The catalogue is enumerable rather than hard-coded:

```c
size_t count = ls_operation_type_count();
char name[64]; size_t needed;
ls_operation_type_name(0, name, sizeof(name), &needed);
```

and so are an operation's parameters, through `ls_operation_parameter_count` and
`ls_operation_parameter_at`, which reports each name and its type. A UI can
build itself from that.

### Memory and strings

Anything the engine allocates comes back as an opaque handle that owns its
storage, and has a matching release: `ls_raster_release`, `ls_blob_release`,
`ls_snapshot_release`, `ls_package_builder_release`. Releasing `NULL` is
harmless. Nothing hands you a pointer into a temporary.

Strings are UTF-8 and are copied into a buffer you supply. Pass `NULL` to ask
how much you need:

```c
size_t needed = 0;
ls_metadata_get(ctx, sprite, "fast.frame", NULL, 0, &needed);   /* BufferTooSmall */
char* value = malloc(needed);
ls_metadata_get(ctx, sprite, "fast.frame", value, needed, &needed);
```

A buffer that is one byte short is refused rather than truncated into.

### Loading it from another language

Nothing beyond a foreign function interface is needed. In Python:

```python
import ctypes as C

dll = C.CDLL("./liblivesprite_c.so")          # or livesprite_c.dll
dll.ls_context_create.restype = C.c_void_p
dll.ls_error_string.restype = C.c_char_p

ctx = C.c_void_p(dll.ls_context_create())
doc = C.c_uint64()
dll.ls_document_create(ctx, b"from-python", 24, 24, C.byref(doc))
```

Declare `argtypes` for anything taking a struct or a float by value, and
`restype` for anything returning a pointer or a size — ctypes assumes `int`
otherwise, which silently truncates pointers on 64-bit. The same shape applies
to C# `DllImport`, Rust `extern "C"`, and Node N-API.

A complete working version is in [`tools/ls_ctypes_smoke.py`](../tools/ls_ctypes_smoke.py),
which CI runs on every push. Copy it as the starting point for a binding:

```bash
cmake -S . -B build -DLS_BUILD_SHARED_C=ON && cmake --build build
python3 tools/ls_ctypes_smoke.py build/liblivesprite_c.so
```

### What the C API does not cover

It is the surface a binding needs to do real work, not a mirror of all 265 C++
methods. Missing on purpose:

- **Plugins.** Registering an operation type means handing the engine a callback
  that runs inside a compile. That is doable across a C boundary but it is a
  different lifetime and threading problem, and no binding has needed it yet.
- **The convenience helpers**, where the underlying call is already exposed:
  most region morphology, most pattern constructors, socket and boundary
  querying.
- **`applyInstructions` and `renderFrame`.** A binding can drive parameters one
  at a time; the batched validate-then-apply guarantee has no C form yet.

If you need one of these, it is an addition to this header rather than a reason
to abandon it for the C++ API.

## Determinism across a boundary

The engine carries its own trigonometry and pins the compiler's floating-point
contract, so a document compiles to the same bytes whichever door it came
through and whichever toolchain built the library. The determinism suite is run
on Windows, Linux and macOS in CI for exactly this reason — see
[performance and limits](performance-and-limits.md).

That matters more here than in C++. If a Python tool and a C++ editor disagree
about one pixel, the file they share stops being a shared truth.

## Verifying an embedding

Two tests in the repository exist to keep these paths honest, and both are worth
copying if you package the engine yourself:

- `tests/c_api_tests.c` is compiled by the **C** compiler, not the C++ one. A
  header can look like C and only compile as C++, and this is the only way to
  find out.
- The CI `consume` job installs the engine and builds a project that knows
  nothing about our source layout against `find_package`, so a broken config
  file fails in our repository rather than in yours.
- The CI `bindings` job loads the shared library with ctypes and compiles a
  sprite, which is the only place the "callable from other languages" claim is
  tested rather than asserted.
