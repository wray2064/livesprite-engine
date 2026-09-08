# Contributing

## The licence grant, and why it is not optional

The engine is under the Business Source License 1.1, and each version becomes
Apache-2.0 on its Change Date. Both of those are promises the copyright holder
makes, and only the copyright holder can make them.

If a contribution is merged without a licence grant, that contributor keeps the
copyright in their lines. They have not agreed to the Change Date, and they have
not agreed to any future commercial licence. Relicensing then means finding every
past contributor and asking — which is unpleasant after one year and effectively
impossible after five.

So every contribution needs this in each commit message:

```
Signed-off-by: Your Name <your@email>
```

`git commit -s` adds it. It certifies the [Developer Certificate of
Origin](https://developercertificate.org/) — that you wrote the change, or have
the right to submit it — and, for this project, that you grant the maintainers
the right to license your contribution under the terms in
[LICENSING.md](LICENSING.md), including the Change License and any commercial
licence offered alongside it.

Substantial contributions may additionally be asked to sign a Contributor
Licence Agreement. That is not distrust; it is the difference between being able
to keep the promises in that document and not.

## What the engine is, and is not

Read [`docs/concepts.md`](docs/concepts.md) before a first change. Two rules
shape most decisions:

**Operations are the source truth.** The engine stores rules and compiles pixels
from them on demand. A change that caches pixels as truth, or that resamples a
previous compile instead of resolving from the operation stack, is working
against the design even when it produces the right picture today.

**Determinism is a hard requirement, not a quality goal.** The same document,
engine version and profile must produce identical bytes on every platform. In
practice:

- Never call `std::sin`, `cos`, `tan` or `atan2`. Use `src/ls_math.h`. Those
  library functions are not specified to the last bit and the major C libraries
  disagree; one bit in an angle moves a pixel. `sqrt`, `floor`, `round`, `fabs`
  and `fmod` are exact and fine.
- Never introduce iteration over an unordered container where the order can reach
  the output.
- Never add a dependency on wall-clock time, a random source, or an address.

`tests/determinism_tests.cpp` locks four scenes to golden hashes. If one changes,
find out what moved before touching the constant — a changed hash is the test
doing its job.

## Building and testing

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLS_BUILD_SHARED_C=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows, `build.bat` finds the toolchain for you.

The library builds with warnings as errors, RTTI off, and the floating-point
contract pinned. A warning is a build failure on purpose.

## What a change should come with

- **A test that fails without it.** For anything touching compilation, prefer a
  test that compares compiled pixels over one that inspects structures: the
  picture is what has to be right.
- **No new dependencies.** The engine has none — not for graphics, not for JSON,
  not for the container format — and that is a feature. It is what makes the
  engine embeddable anywhere and auditable by one person.
- **Nothing thrown across the API boundary.** Return `Result<T>` or `VoidResult`.
- **A commit message that explains why.** The what is in the diff.

## Adding an operation

Four places, and the compiler will find three of them for you:

1. The struct in `include/livesprite/ls_operations.h`, added to the `Operation`
   variant and to the `LS_OPERATION_TYPES` list beside it. A `static_assert`
   fails if you do one and not the other.
2. A field table in `src/ls_reflect.h`. Serialization, live parameter addressing
   and the C ABI all walk this one table, so a field left out is missing from
   saving, from animation and from every language binding at once.
3. Resolution in `src/ls_compile.cpp`.
4. A test.

Nothing needs to change in the C ABI: operations are created by type name there,
so a new type is callable from every binding as soon as it exists.

## Reporting a security issue

Do not open a public issue. The engine parses untrusted input — documents and
packages arrive from whoever sent the file — so parsing bugs are security bugs.
Contact the address in [LICENSE](LICENSE).
