# LiveSprite engine — working here

This is the engine under Sprit's'fast (the sibling checkout `../sprits-fast`)
and Sprit's'pract. `../sprits-fast/docs/handoff.md` covers the code as Fast
uses it, and the traps. When the workspace's own `../HANDOFF.md` is there, it
covers the plan across all three projects. Read them first.

Here:

- `LIVESPRITE_ENGINE_SPEC.md` is the implementation contract;
  `Livesprite architecture.txt` is the wider design, Pract included.
  `docs/concepts.md` explains the model: operations, regions, faces,
  compiling, pivots, sockets, attachments and assemblies.
- **Operations are the truth; pixels are only output.** A defect in what a
  transform draws is fixed in the geometry and the rasterisers, never by
  moving pixels. The same sprite compiles to the same bytes on every
  compiler (`livesprite_determinism`).
- `ls::Result<T>` throughout: no exceptions across the API, no RTTI. The
  engine never calls back into an app. C ABI enums are mirrored by hand, each
  pinned by `static_assert`.
- Build and test from PowerShell in this folder: `.\build.bat test`. Commit
  to `main` with prose messages ending in the `Co-Authored-By` line, and
  push. Docs move with the code (`docs/`, and the README's tables).
- Never write code through a heredoc, `python -c` or `sed` with `\n`. After a
  header change, delete the `.obj` files in `build\`, and in Fast's build
  trees, before trusting a run.
