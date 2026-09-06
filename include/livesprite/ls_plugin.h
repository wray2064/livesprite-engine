#pragma once
// ls_plugin.h — Plugin and math extension registration interface
// LiveSprite Engine
//
// Plugins allow Fast, Pract, and third parties to register:
//   - Custom operation types
//   - Custom pattern types
//   - Custom fill resolvers
//   - Custom transform resolvers
//   - Custom compile policies
//
// Plugin operations participate in the dependency graph and serialization
// exactly like built-in operations. Each plugin must declare its contract.

#include "ls_types.h"
#include "ls_operations.h"

#include <functional>
#include <string>
#include <vector>

namespace ls {

// ---------------------------------------------------------------------------
// Plugin parameter schema (for serialization and validation)
// ---------------------------------------------------------------------------

enum class PluginParamType : uint8_t {
    Bool,
    Int,
    Float,
    String,
    Vec2f,
    Color,
    ColorRole,
    RegionId,
    GeometryId,
    PatternId,
    RampId,
    BoundaryId,
    CoordinateSpace,
    BlendMode,
};

struct PluginParamDef {
    std::string     name;
    PluginParamType type = PluginParamType::Float;
    bool            required = true;
    PluginValue     defaultValue;
    std::string     description;
};

// ---------------------------------------------------------------------------
// Plugin operation context — passed to resolver functions
// Resolvers receive this instead of the full LSContext to limit engine access.
// ---------------------------------------------------------------------------
struct PluginResolveContext {
    const PluginParams*   params        = nullptr;
    uint32_t              engineVersion = LS_ENGINE_VERSION;
    const CompileProfile* profile       = nullptr;

    // For resolvers that produce raster contributions: write here. The engine
    // composites this buffer onto the layer using the operation blend rules.
    RasterBuffer* outputBuffer = nullptr;

    // Region query: interval set for a region id (null if unknown).
    std::function<const IntervalSet*(RegionId)> getRegion;

    // Palette query: resolve a color role against the active palette.
    std::function<Color(ColorRole)> resolveColor;
};

// ---------------------------------------------------------------------------
// Plugin operation registration descriptor
// ---------------------------------------------------------------------------
struct PluginOperationDesc {
    // Globally unique type ID. Reverse-domain format: "com.myapp.myop"
    std::string                 typeId;
    std::string                 displayName;
    std::vector<PluginParamDef> params;

    uint32_t                    minEngineVersion = 0;
    uint32_t                    maxEngineVersion = UINT32_MAX;

    // true  = same params + same engine version always produce the same output
    // false = op uses time, randomness, or external state.
    // Non-deterministic ops are rejected in the Export profile.
    bool                        isDeterministic = true;

    // Entity IDs that, when changed, require this op to recompute.
    std::function<std::vector<uint64_t>(const PluginOp&)> getDependencies;

    // Resolve the operation into ctx.outputBuffer.
    std::function<LSError(const PluginOp&, PluginResolveContext&)> resolve;

    // Optional custom serialization. When absent the engine serializes the
    // parameter bag directly using the declared schema.
    std::function<Result<std::string>(const PluginOp&)> serialize;
    std::function<Result<PluginOp>(std::string_view)>   deserialize;
    std::function<Result<PluginOp>(const PluginOp&, uint32_t fromVersion)> migrate;
};

// ---------------------------------------------------------------------------
// Plugin pattern registration descriptor
// ---------------------------------------------------------------------------
struct PluginPatternDesc {
    std::string     typeId;
    std::string     displayName;
    uint32_t        tileWidth   = 4;
    uint32_t        tileHeight  = 4;

    // Generate the pattern tile as a 1-bit mask (true = foreground).
    std::function<std::vector<bool>(
        uint32_t tileW,
        uint32_t tileH,
        float density,
        float phase,
        const PluginParams& params
    )> generateTile;
};

// ---------------------------------------------------------------------------
// Plugin fill resolver registration
// Extends or overrides how a fill type is rasterized.
// ---------------------------------------------------------------------------
struct PluginFillResolverDesc {
    std::string typeId;
    std::function<LSError(
        const PluginOp& op,
        const IntervalSet& region,
        PluginResolveContext& ctx
    )> resolve;
};

// ---------------------------------------------------------------------------
// Plugin transform resolver registration
// Allows custom transform math (for example IK pose results from Practipuppets)
// ---------------------------------------------------------------------------
struct PluginTransformResolverDesc {
    std::string typeId;
    std::function<Result<Mat3f>(const PluginOp& op, PluginResolveContext& ctx)> resolve;
};

// ---------------------------------------------------------------------------
// Plugin compile policy registration
// Allows custom sampling/resolution policies.
// ---------------------------------------------------------------------------
struct PluginCompilePolicyDesc {
    std::string typeId;
    std::function<Result<Color>(
        const std::vector<Color>& samples,
        const CompileProfile& profile
    )> resolve;
};

} // namespace ls
