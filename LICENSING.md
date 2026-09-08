# Licensing

Not every file here is under the same licence, and the split is deliberate. This
document says what applies where, and why — so that anyone deciding whether to
build on LiveSprite can answer that question without a lawyer.

**This is a description of intent, not legal advice.** The licence texts are what
bind; this explains the reasoning behind them.

## What applies to what

| Path | Licence |
|---|---|
| `src/`, `include/livesprite/ls_*.h`, `include/livesprite/livesprite.h` | Business Source License 1.1 → Apache-2.0 on the Change Date |
| `include/livesprite/livesprite_c.h` | Apache-2.0 |
| `examples/`, `tools/ls_ctypes_smoke.py` | Apache-2.0 |
| `tools/` (testbed, benchmark), `tests/` | Business Source License 1.1 |
| `docs/`, `README.md` | Apache-2.0 |

Every source file carries an `SPDX-License-Identifier` line, so a scanner can
answer this without reading this table.

## The engine: source-available, not open source

The engine is under the **Business Source License 1.1**. Read the source, modify
it, build on it, ship commercial products with it. Two things are carved out,
and each converts to Apache-2.0 four years after that version is published.

**You may not offer the engine to third parties as an engine.** Building a sprite
compilation library, SDK or hosted service whose substance is this engine is the
one thing the licence stops. That is the moat, and it is the only thing being
protected.

**You may not embed it at run time in a shipped game.** Those rights are reserved
and available commercially. Authoring, converting and exporting are free; a game
that ships the engine and compiles sprites live on a player's machine is a
different arrangement.

Everything else is granted explicitly, commercial use included.

### Why not a "no competing products" licence

The obvious choice for source-available-with-a-moat is PolyForm Shield or
Perimeter: "you may not use this to compete with the licensor." That would be a
mistake here.

We ship sprite editors. Under a broad anti-compete clause, anyone building a
sprite editor is arguably competing, which is exactly the thing this project
wants to encourage — and worse, they could not tell in advance whether they were
allowed. An ambiguous prohibition does not protect anything; it just makes
careful people walk away.

BSL lets the line be drawn precisely instead. Editors: yes, explicitly, including
ones that compete with ours and win. A rival engine: no.

### Why there is a Change Date

Every version becomes Apache-2.0 four years after it is published. That is not a
concession, it is the point.

Nobody sensible builds a product on a foundation the vendor can pull. A
proprietary engine, controlled by a company that also ships two editors, is a
frightening thing to depend on: terms can change, prices can rise, your best
feature can turn up in their premium product. The Change Date is the answer. If
this project turns hostile, stagnates, or disappears, the floor does not vanish —
it becomes Apache-2.0 on a published schedule.

The cost is real and worth stating plainly: four years after publication, any
given version can be used for anything, **including run-time embedding in
games**. The reserved run-time rights are therefore a four-year lead on each
version, not a permanent exclusion. That is an acceptable trade for the trust it
buys, and it is the reason to keep the current version meaningfully ahead of the
one that just converted.

## The C ABI, examples and docs: Apache-2.0

`livesprite_c.h` is Apache-2.0 rather than BSL, because a binding author has to
copy declarations out of it into Python, C#, Rust or whatever else. Making them
think about licence compatibility to do that would cost far more than the header
protects — it is a few hundred lines of declarations, and the documentation
describes the same surface anyway.

The examples are Apache-2.0 for the same reason: `examples/first_sprite.cpp`
exists to be copied into someone else's project. An example you may not copy is
not an example.

Apache-2.0 rather than MIT, throughout, for its explicit patent grant. Some of
the technique here — the anchored-pattern re-resolution, the deterministic dither
modulation — is the kind of thing patents get written about. Apache-2.0 settles
that in both directions; MIT is silent on it.

## The applications

**Sprit's'fast** will be Apache-2.0 when it exists. Fork it, gut it, ship
something better — that is what it is for. It is the on-ramp to the engine, not a
product to be defended.

Note the shape honestly: Fast's own source is open, and its core dependency is
not. Anyone evaluating it should know that up front rather than discover it
later.

**Sprit's'pract** is proprietary and its source is not published.

## Trademarks

"LiveSprite", "Sprit's'fast" and "Sprit's'pract" are trademarks. No licence here
grants any right to use them.

This matters more than it looks. Build a competing editor on the engine, and you
are welcome to. Call it LiveSprite, or describe your game engine integration as
"official", and you are not. Trademark is what makes "official" mean something
once third-party bridges exist.

## Contributing

The engine's licence can only be changed by whoever holds the copyright. Merging
an outside contribution without a licence grant means that person's lines cannot
be relicensed without asking them — and in five years, they may be unreachable.

So contributions require the sign-off described in [CONTRIBUTING.md](CONTRIBUTING.md).

## Commercial licensing

For run-time embedding in shipped games, for building products the Additional Use
Grant does not cover, or for terms that do not depend on a Change Date, a
commercial licence is available. Contact details are in [LICENSE](LICENSE).
