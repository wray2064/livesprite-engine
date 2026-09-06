// ls_serialize.cpp — Serialization, deserialization, and version migration
// LiveSprite Engine
//
// Contract:
//   - SerializedData contains the FULL source truth for a document or sprite
//   - Every operation, geometry, palette, region, pivot, socket, boundary is included
//   - Format: binary or JSON (choose one canonical format — recommend MessagePack or CBOR
//     for binary; plain JSON for human-readable debug output)
//   - Unknown fields are preserved through round-trips (forward compatibility)
//   - Engine version is embedded; MigrateVersion handles upgrades
//
// Recommended format structure (JSON outline):
//
//   {
//     "ls_version": 256,
//     "format": "livesprite/document",
//     "documents": [
//       {
//         "id": 1,
//         "defaultPalette": 5,
//         "sprites": [...]
//       }
//     ],
//     "sprites": [
//       {
//         "id": 2,
//         "pivot": 10,
//         "sockets": [...],
//         "boundaries": [...],
//         "layers": [...],
//         "boundPalette": null
//       }
//     ],
//     "layers": [ ... ],
//     "operations": [
//       {
//         "id": 20,
//         "ownerLayer": 15,
//         "type": "FillDitherOp",
//         "params": {
//           "targetRegion": 30,
//           "ramp": 7,
//           "pattern": 3,
//           "density": 0.42,
//           "coordinateSpace": "Object",
//           "blend": "Normal",
//           "opacity": 1.0
//         }
//       }
//     ],
//     "palettes": [...],
//     "ramps": [...],
//     "patterns": [...],
//     "geometry": [...],
//     "pivots": [...],
//     "sockets": [...],
//     "boundaries": [...],
//     "_unknown": { ... }   <-- preserved unknown fields from newer engine versions
//   }

#include <livesprite/ls_api.h>
#include <string>
#include <sstream>

namespace ls {

// ---------------------------------------------------------------------------
// serializeDocument
// ---------------------------------------------------------------------------
Result<SerializedData> LSContext::serializeDocument(DocumentId doc) const {
    if (!impl_->documents.count(doc))
        return Result<SerializedData>::err(LSError::InvalidId);

    SerializedData data;
    data.engineVersion = LS_ENGINE_VERSION;
    data.formatTag     = "livesprite/document";

    // TODO: serialize all entities owned by this document into data.bytes
    // Recommended approach:
    //   1. Walk document → sprites → layers → operations
    //   2. Collect all referenced geometry/palette/ramp/pattern IDs
    //   3. Serialize all collected entities in dependency order
    //   4. Use nlohmann/json or a custom binary writer

    return Result<SerializedData>::err(LSError::NotImplemented);
}

// ---------------------------------------------------------------------------
// deserializeDocument
// ---------------------------------------------------------------------------
Result<DocumentId> LSContext::deserializeDocument(const SerializedData& data) {
    if (data.formatTag != "livesprite/document")
        return Result<DocumentId>::err(LSError::DeserializationFailure);

    if (data.engineVersion > LS_ENGINE_VERSION) {
        // Newer format — can still read if migration is available
        // For now: proceed cautiously, preserving unknown fields
    }

    // TODO: parse data.bytes, reconstruct entities in the correct order:
    //   1. Geometry
    //   2. Regions
    //   3. Palettes, Ramps, Patterns
    //   4. Pivots, Sockets, Boundaries
    //   5. Layers
    //   6. Operations (in layer order)
    //   7. Sprites
    //   8. Document

    return Result<DocumentId>::err(LSError::NotImplemented);
}

// ---------------------------------------------------------------------------
// serializeOperation / deserializeOperation
// ---------------------------------------------------------------------------
Result<std::string> LSContext::serializeOperation(OperationId id) const {
    auto it = impl_->operations.find(id);
    if (it == impl_->operations.end())
        return Result<std::string>::err(LSError::InvalidId);

    std::string json;
    // TODO: use std::visit to serialize the correct Operation variant type
    // Each op type should map to a JSON object with "type" and "params" fields.
    // std::visit([&](auto&& op) { json = serializeOpImpl(op); }, it->second.op);

    return Result<std::string>::err(LSError::NotImplemented);
}

Result<OperationId> LSContext::deserializeOperation(LayerId into, std::string_view json) {
    // TODO: parse JSON, determine "type" field, construct the correct Op struct,
    // call addOperation(into, op)
    return Result<OperationId>::err(LSError::NotImplemented);
}

// ---------------------------------------------------------------------------
// migrateVersion
// ---------------------------------------------------------------------------
Result<SerializedData> LSContext::migrateVersion(const SerializedData& data, uint32_t targetVersion) {
    if (data.engineVersion == targetVersion)
        return Result<SerializedData>::ok(data);

    if (data.engineVersion > targetVersion)
        return Result<SerializedData>::err(LSError::VersionMismatch);  // downgrade not supported

    // TODO: apply sequential migration functions:
    //   migrate_0_to_1(data)
    //   migrate_1_to_2(data)
    //   ...
    // Each migration transforms the byte payload incrementally.

    return Result<SerializedData>::err(LSError::NotImplemented);
}

// ---------------------------------------------------------------------------
// Sprite-level serialize/deserialize
// ---------------------------------------------------------------------------
Result<SerializedData> LSContext::serializeSprite(SpriteId id) const {
    if (!impl_->sprites.count(id))
        return Result<SerializedData>::err(LSError::InvalidId);
    SerializedData data;
    data.engineVersion = LS_ENGINE_VERSION;
    data.formatTag     = "livesprite/sprite";
    // TODO: same as serializeDocument but scoped to one sprite
    return Result<SerializedData>::err(LSError::NotImplemented);
}

Result<SpriteId> LSContext::deserializeSprite(DocumentId into, const SerializedData& data) {
    if (data.formatTag != "livesprite/sprite")
        return Result<SpriteId>::err(LSError::DeserializationFailure);
    // TODO: reconstruct sprite entities, attach to document
    return Result<SpriteId>::err(LSError::NotImplemented);
}

} // namespace ls
