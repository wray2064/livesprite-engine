# File format

Two things can be written: a **document**, which is the engine's canonical
representation, and a **package**, which is a document plus whatever files an
app keeps with it.

## The document

`serializeDocument` produces UTF-8 JSON. It is the source of truth for a sprite;
no raster output ever is.

```json
{
  "format": "livesprite/document",
  "engineVersion": 256,
  "document":  { "id": 1, "name": "badge", "canvasWidth": 24, "canvasHeight": 24 },
  "geometry":  [ … ], "regions": [ … ], "palettes": [ … ],
  "ramps":     [ … ], "patterns": [ … ],
  "sprites":   [ { "id": 2, "transform": [ … ], "layers": [ { "operations": [ … ] } ] } ],
  "metadata":  { "2": { "fast.frame": "3" } }
}
```

Three properties matter:

**Deterministic.** Object keys are sorted and numbers use a fixed format, so
saving the same state twice produces byte-identical output. Diffs are therefore
meaningful, and a save is testable.

**Lossless.** Every operation parameter round-trips. The tests prove it by
compiling both the original and the reloaded document and comparing pixels
rather than by comparing structures.

**Forward compatible.** Fields written by a newer engine are preserved through a
load and save cycle, so an older build cannot silently strip a newer build's
data. Unknown *operation types*, by contrast, are refused: dropping an operation
would silently change the picture.

### Ids

Ids inside a file are file-local. `deserializeDocument` mints fresh ones so two
documents can be open at once. The exception is `restoreDocumentState`, which
puts ids back exactly as they were, because undo must not invalidate handles the
interface is holding.

### Versioning and migration

The engine version is a `uint32_t` in every document. Within a major version the
schema is additive, so files pass straight through. Across a major version,
migration steps rewrite a document one major at a time and compose, so a v1 file
reaches v3 by running v1→v2 then v2→v3. Loading an older major walks that chain
before the reader sees the document.

No breaking version has shipped, so the step table is empty and a major version
with no route is refused rather than half read. A *newer* major is always
refused: it can change the meaning of fields this build thinks it understands.

## The package

A package carries the document plus whole files: a thumbnail, an imported
reference, a palette from somewhere else. Bulk data belongs beside the document
rather than inside the JSON, where a 40 KB thumbnail would become 55 KB of
base64 that every load walks past.

```
livesprite.json     the document
manifest.json       what the writer claimed about each entry
fast/thumbnail.png  app entries, namespaced by owner
pract/notes.txt
```

**The engine owns the container; apps own the entries.** The engine guarantees
it carries entries it does not understand through a read and write cycle, so two
apps can share one file without erasing each other's data. An app-owned
container would mean the engine could no longer promise its own document
round-trips, and each app would invent its own box.

```cpp
PackageEntry thumb;
thumb.name        = "fast/thumbnail.png";   // first segment names the owner
thumb.contentType = "image/png";            // a claim, never a fact
thumb.data        = bytes;

auto package = ctx->writePackage(doc, {thumb});

std::vector<PackageEntry> entries;
auto reopened = other->loadPackage(package.value, &entries);
```

### Why ZIP, and why stored

It is a ZIP with **stored** (uncompressed) entries, no extra fields and fixed
timestamps. Both halves are deliberate:

- **Portable.** Any tool in any language opens it; rename it to `.zip` and it
  expands. The same state written twice produces identical bytes.
- **Safe.** With nothing compressed, a decompression bomb has nowhere to live
  rather than merely being bounded. A package containing compressed entries is
  refused rather than expanded.

### Entry names are identifiers, not paths

A name is validated, never repaired — a sanitised name can collide with a real
one. Refused: `..` segments, leading `/`, backslashes, drive letters, empty
segments, embedded nulls, names over 255 bytes, the engine's own
`livesprite.json` and `manifest.json`, and anything without a namespace segment.

### Limits

| Limit | Value |
|---|---|
| `kPackageMaxNameLength` | 255 bytes |
| `kPackageMaxEntries` | 1024 |
| `kPackageMaxEntrySize` | 32 MB |
| `kPackageMaxTotalSize` | 128 MB |

Sizes are checked before anything is allocated, every offset is bounds-checked
against the buffer, and every entry carries a CRC so corruption stops at the
reader instead of being handed on.

## App metadata

Small, entity-scoped data that belongs with the document rather than in a file
beside it: which frame an app thinks a sprite is, whether a panel row is
collapsed.

```cpp
ctx->setMetadata(sprite.value, "fast.frame", "3");
ctx->setMetadata(sprite.value, "pract.arranger.cell", "4,2");
```

Keys must be namespaced, so two apps annotating one document cannot overwrite
each other. Values are opaque bytes: the engine stores them, returns them, and
never parses or executes them. Caps are 128-byte keys, 64 KB values, 64 entries
per entity. Metadata survives save, load and undo, and is remapped onto new ids
when a document is loaded.

Use metadata for annotations, and a package entry for whole files.

## Security

Everything in a received document or package is **untrusted input**. It came
from whoever sent the file.

- Content types in a manifest are a claim by the writer, not a fact. Sniff, do
  not trust, and never execute anything from a package.
- Metadata written by another app is attacker-controlled if the file is. Validate
  before acting on it.
- Do not put in metadata anything you would not send to a stranger. A document
  travels, and the engine cannot warn you about what is inside an opaque value.
- **Undo history does not belong in a shared file.** It would hand the recipient
  every version of the sprite the author thought they had deleted. Crash
  recovery wants a local autosave using the same format, never the artefact you
  send someone.
