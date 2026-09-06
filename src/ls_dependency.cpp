// ls_dependency.cpp — dependency graph, dirty propagation, and compile cache.
//
// Edge direction: dependencies[dependent] contains what it reads;
// dependents[dependency] contains everything that must recompile when it changes.

#include "ls_internal.h"

#include <deque>

namespace ls {

// ---------------------------------------------------------------------------
// Impl helpers
// ---------------------------------------------------------------------------

void LSContext::Impl::addDependencyEdge(uint64_t dependency, uint64_t dependent) {
    if (dependency == 0 || dependent == 0 || dependency == dependent) {
        return;
    }
    graph.dependents[dependency].insert(dependent);
    graph.dependencies[dependent].insert(dependency);
}

void LSContext::Impl::clearDependenciesOf(uint64_t dependent) {
    auto it = graph.dependencies.find(dependent);
    if (it != graph.dependencies.end()) {
        for (uint64_t dependency : it->second) {
            auto dependentsIt = graph.dependents.find(dependency);
            if (dependentsIt != graph.dependents.end()) {
                dependentsIt->second.erase(dependent);
                if (dependentsIt->second.empty()) {
                    graph.dependents.erase(dependentsIt);
                }
            }
        }
        graph.dependencies.erase(it);
    }

    // Also drop edges where this entity was the dependency: it no longer exists.
    auto asDependency = graph.dependents.find(dependent);
    if (asDependency != graph.dependents.end()) {
        for (uint64_t downstream : asDependency->second) {
            auto downstreamIt = graph.dependencies.find(downstream);
            if (downstreamIt != graph.dependencies.end()) {
                downstreamIt->second.erase(dependent);
            }
        }
        graph.dependents.erase(asDependency);
    }

    graph.dirty.erase(dependent);
}

void LSContext::Impl::registerOperationDependencies(OperationId id) {
    const OperationData* data = findOperation(id);
    if (data == nullptr) {
        return;
    }

    // The layer reads this operation. This edge is intrinsic and has to be
    // restored here, because clearDependenciesOf drops every edge touching the
    // operation before an edit re-registers it. Without it, a second edit to
    // the same operation would never reach the layer and the layer would keep
    // serving a cached compile.
    addDependencyEdge(id.value, data->layer.value);

    for (uint64_t dependency : operationDependencies(data->op)) {
        addDependencyEdge(dependency, id.value);
    }

    if (const PluginOp* plugin = std::get_if<PluginOp>(&data->op)) {
        auto registered = plugins.operations.find(plugin->typeId);
        if (registered != plugins.operations.end() && registered->second.getDependencies) {
            for (uint64_t dependency : registered->second.getDependencies(*plugin)) {
                addDependencyEdge(dependency, id.value);
            }
        }
    }

    if (data->assignedBoundary.valid()) {
        addDependencyEdge(data->assignedBoundary.value, id.value);
    }
}

void LSContext::Impl::markDirtyInternal(uint64_t entityId) {
    if (entityId == 0) {
        return;
    }

    // Visited is tracked separately from dirty on purpose: an entity can still
    // be dirty from an earlier edit while its dependents have since been
    // recompiled and cached. Stopping at an already-dirty node would leave
    // those dependents holding stale output.
    std::set<uint64_t> visited;
    std::deque<uint64_t> queue { entityId };
    while (!queue.empty()) {
        const uint64_t current = queue.front();
        queue.pop_front();
        if (!visited.insert(current).second) {
            continue;
        }
        graph.dirty.insert(current);
        invalidateCacheFor(current);

        auto it = graph.dependents.find(current);
        if (it == graph.dependents.end()) {
            continue;
        }
        for (uint64_t dependent : it->second) {
            queue.push_back(dependent);
        }
    }
}

void LSContext::Impl::invalidateCacheFor(uint64_t entityId) {
    // A boundary field is derived from geometry, so it is dropped whenever
    // either the boundary or anything it reads is dirtied.
    boundaryFields.erase(entityId);
    for (auto it = boundaryFields.begin(); it != boundaryFields.end(); ) {
        const BoundaryData* boundary = findBoundary(BoundaryId{it->first});
        it = (boundary == nullptr || boundary->desc.shape.value == entityId)
            ? boundaryFields.erase(it)
            : std::next(it);
    }

    for (auto it = compileCache.begin(); it != compileCache.end(); ) {
        it = it->first.entity == entityId ? compileCache.erase(it) : std::next(it);
    }
    operationCache.erase(entityId);
}

// ---------------------------------------------------------------------------
// Public dependency and cache API
// ---------------------------------------------------------------------------

VoidResult LSContext::markDirty(uint64_t entityId) {
    if (entityId == 0) {
        return VoidResult::err(LSError::InvalidId);
    }
    impl_->markDirtyInternal(entityId);
    return VoidResult::success();
}

Result<bool> LSContext::isDirty(uint64_t entityId) const {
    if (entityId == 0) {
        return Result<bool>::err(LSError::InvalidId);
    }
    return Result<bool>::ok(impl_->graph.dirty.count(entityId) != 0);
}

Result<DependencyInfo> LSContext::getDependencyInfo(uint64_t entityId) const {
    if (entityId == 0) {
        return Result<DependencyInfo>::err(LSError::InvalidId);
    }
    DependencyInfo info;
    auto dependencies = impl_->graph.dependencies.find(entityId);
    if (dependencies != impl_->graph.dependencies.end()) {
        info.dependencies.assign(dependencies->second.begin(), dependencies->second.end());
    }
    auto dependents = impl_->graph.dependents.find(entityId);
    if (dependents != impl_->graph.dependents.end()) {
        info.dependents.assign(dependents->second.begin(), dependents->second.end());
    }
    return Result<DependencyInfo>::ok(info);
}

VoidResult LSContext::recompute(uint64_t entityId, const CompileProfile& profile) {
    if (entityId == 0) {
        return VoidResult::err(LSError::InvalidId);
    }

    if (impl_->sprites.count(entityId) != 0) {
        auto compiled = compileSprite(SpriteId{ entityId }, profile);
        if (compiled.fail()) {
            return VoidResult::err(compiled.error);
        }
        impl_->graph.dirty.erase(entityId);
        return VoidResult::success();
    }
    if (impl_->layers.count(entityId) != 0) {
        auto compiled = compileLayer(LayerId{ entityId }, profile);
        if (compiled.fail()) {
            return VoidResult::err(compiled.error);
        }
        impl_->graph.dirty.erase(entityId);
        return VoidResult::success();
    }
    if (impl_->regions.count(entityId) != 0) {
        auto compiled = compileRegion(RegionId{ entityId }, profile);
        if (compiled.fail()) {
            return VoidResult::err(compiled.error);
        }
        impl_->graph.dirty.erase(entityId);
        return VoidResult::success();
    }

    // Anything else (geometry, palette, pattern, pivot...) has no raster of its
    // own: recomputing it means recomputing whatever reads it.
    auto dependents = impl_->graph.dependents.find(entityId);
    if (dependents != impl_->graph.dependents.end()) {
        const std::vector<uint64_t> downstream(dependents->second.begin(), dependents->second.end());
        for (uint64_t id : downstream) {
            recompute(id, profile);
        }
    }
    impl_->graph.dirty.erase(entityId);
    return VoidResult::success();
}

VoidResult LSContext::compileDirtyOnly(DocumentId doc, const CompileProfile& profile) {
    const DocumentData* document = impl_->findDocument(doc);
    if (document == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }

    LSError firstError = LSError::None;
    for (SpriteId sprite : document->sprites) {
        if (impl_->graph.dirty.count(sprite.value) == 0) {
            continue;
        }
        auto compiled = compileSprite(sprite, profile);
        if (compiled.fail() && firstError == LSError::None) {
            firstError = compiled.error;
            continue;
        }
        impl_->graph.dirty.erase(sprite.value);
        const SpriteData* data = impl_->findSprite(sprite);
        if (data == nullptr) {
            continue;
        }
        for (LayerId layer : data->layers) {
            impl_->graph.dirty.erase(layer.value);
            const LayerData* layerData = impl_->findLayer(layer);
            if (layerData == nullptr) {
                continue;
            }
            for (OperationId op : layerData->operations) {
                impl_->graph.dirty.erase(op.value);
            }
        }
    }

    return firstError == LSError::None ? VoidResult::success() : VoidResult::err(firstError);
}

VoidResult LSContext::clearCache(DocumentId doc) {
    const DocumentData* document = impl_->findDocument(doc);
    if (document == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }

    std::set<uint64_t> owned { doc.value };
    for (SpriteId sprite : document->sprites) {
        owned.insert(sprite.value);
        const SpriteData* data = impl_->findSprite(sprite);
        if (data == nullptr) {
            continue;
        }
        for (LayerId layer : data->layers) {
            owned.insert(layer.value);
            const LayerData* layerData = impl_->findLayer(layer);
            if (layerData == nullptr) {
                continue;
            }
            for (OperationId op : layerData->operations) {
                owned.insert(op.value);
            }
        }
    }
    for (RegionId region : document->regions) {
        owned.insert(region.value);
    }

    for (auto it = impl_->compileCache.begin(); it != impl_->compileCache.end(); ) {
        it = owned.count(it->first.entity) != 0 ? impl_->compileCache.erase(it) : std::next(it);
    }
    for (auto it = impl_->operationCache.begin(); it != impl_->operationCache.end(); ) {
        it = owned.count(it->first) != 0 ? impl_->operationCache.erase(it) : std::next(it);
    }
    return VoidResult::success();
}

VoidResult LSContext::cachePreview(SpriteId id, const CompileResult& result) {
    if (impl_->findSprite(id) == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    CompileProfile preview;
    preview.type = CompileProfileType::Preview;
    preview.outputWidth = result.raster.width;
    preview.outputHeight = result.raster.height;

    CacheKey key;
    key.entity = id.value;
    key.profileHash = impl_->hashProfile(preview);
    key.resourceRevision = impl_->resourceRevision;
    key.engineVersion = LS_ENGINE_VERSION;
    impl_->compileCache[key] = result;
    impl_->graph.dirty.erase(id.value);
    return VoidResult::success();
}

VoidResult LSContext::cacheOperationResult(OperationId id, const RasterBuffer& result) {
    if (impl_->findOperation(id) == nullptr) {
        return VoidResult::err(LSError::InvalidId);
    }
    impl_->operationCache[id.value] = result;
    return VoidResult::success();
}

CacheStats LSContext::cacheStats() const {
    CacheStats stats;
    stats.entries = impl_->compileCache.size() + impl_->operationCache.size();
    stats.hits = impl_->cacheHits;
    stats.misses = impl_->cacheMisses;
    stats.dirtyEntities = impl_->graph.dirty.size();
    return stats;
}

} // namespace ls
