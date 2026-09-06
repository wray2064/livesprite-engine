// ls_dependency.cpp — Dependency graph and cache management
// LiveSprite Engine
//
// Responsibility:
//   - Track which entities depend on which other entities
//   - Propagate dirty flags when any entity changes
//   - Decide what needs recompilation on CompileDirtyOnly
//   - Manage the compiled result cache
//
// Dependency rules (register these on entity creation):
//
//   GeometryId  → RegionId(s) that reference it
//   RegionId    → OperationId(s) that target it (fills, strokes, outlines)
//   OperationId → LayerId that owns it
//   LayerId     → SpriteId that owns it
//   PaletteId   → all operations that reference its color roles
//   RampId      → FillGradientOp, FillRampOp, FillDitherOp using it
//   PatternId   → FillDitherOp, FillLinePatternOp, StrokeBrushOp using it
//   BoundaryId  → deform ops and squash/stretch ops scoped to it
//   PivotId     → RotateOp, ScaleOp, SquashOp, StretchOp referencing it
//   SocketId    → AnchorTransformOp using it

#include <livesprite/ls_api.h>

namespace ls {

// ---------------------------------------------------------------------------
// getDependencyInfo
// ---------------------------------------------------------------------------
Result<DependencyInfo> LSContext::getDependencyInfo(uint64_t entityId) const {
    auto it = impl_->dependencyGraph.find(entityId);
    if (it == impl_->dependencyGraph.end()) {
        // Entity exists but has no registered dependencies — return empty info
        return Result<DependencyInfo>::ok(DependencyInfo{});
    }
    DependencyInfo info;
    info.dependencies = it->second.dependencies;
    info.dependents   = it->second.dependents;
    return Result<DependencyInfo>::ok(std::move(info));
}

// ---------------------------------------------------------------------------
// recompute — force recompile of one entity and its dependents
// ---------------------------------------------------------------------------
VoidResult LSContext::recompute(uint64_t entityId, const CompileProfile& profile) {
    // Mark entity clean, recompile, then propagate to dependents
    // TODO: implement entity-type dispatch (sprite vs layer vs region)
    impl_->propagateDirty(entityId);
    return VoidResult::err(LSError::NotImplemented);
}

// ---------------------------------------------------------------------------
// compileDirtyOnly — recompile only dirty nodes in the graph
// ---------------------------------------------------------------------------
VoidResult LSContext::compileDirtyOnly(DocumentId doc, const CompileProfile& profile) {
    // TODO: topological sort the dependency graph, recompile dirty leaves first
    // Walk from leaves (operations) up to sprites, only where dirty = true
    return VoidResult::err(LSError::NotImplemented);
}

// ---------------------------------------------------------------------------
// Cache management
// ---------------------------------------------------------------------------
VoidResult LSContext::clearCache(DocumentId doc) {
    // TODO: clear all cached compile results for entities owned by this document
    return VoidResult::ok();
}

VoidResult LSContext::cachePreview(SpriteId id, const CompileResult& result) {
    // TODO: store in preview cache keyed on (id, profile hash)
    return VoidResult::ok();
}

VoidResult LSContext::cacheOperationResult(OperationId id, const RasterBuffer& result) {
    // TODO: store in operation result cache
    return VoidResult::ok();
}

// ---------------------------------------------------------------------------
// Internal: register a dependency edge
// Call this whenever an operation is created or updated.
// ---------------------------------------------------------------------------

// Example usage (called from addOperation / updateOperation):
//
//   void registerOperationDependencies(OperationId opId, const Operation& op) {
//       std::visit([&](auto&& o) {
//           using T = std::decay_t<decltype(o)>;
//           if constexpr (std::is_same_v<T, FillDitherOp>) {
//               addDependencyEdge(opId.value, o.targetRegion.value);
//               addDependencyEdge(opId.value, o.ramp.value);
//               addDependencyEdge(opId.value, o.pattern.value);
//           }
//           // ... etc for all op types
//       }, op);
//   }
//
// TODO: implement this for all Operation variant types.

} // namespace ls
