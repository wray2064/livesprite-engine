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
#include <unordered_map>

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
    PluginParamType type;
    bool            required    = true;
    std::string     defaultJson;    // JSON-encoded default value
    std::string     description;
};

// ---------------------------------------------------------------------------
// Plugin operation context — passed to resolver functions
// Resolvers receive this instead of the full LSContext to limit engine access.
// ---------------------------------------------------------------------------
struct PluginResolveContext {
    // Read-only access to named params from the PluginOp
    const std::unordered_map<std::string, std::any>& params;

    // Engine version this is being resolved against
    uint32_t engineVersion;

    // Requested compile profile
    const CompileProfile& profile;

    // For resolvers that need to produce raster contributions:
    // Write into outputBuffer. Engine composites it onto the layer.
    RasterBuffer* outputBuffer = nullptr;  // null for non-raster ops

    // Region query: get the interval set for a region
    std::function<const IntervalSet*(RegionId)> getRegion;

    // Palette query: resolve a color role to a final Color
    std::function<Color(PaletteId, ColorRole)> resolveColor;
};

// ---------------------------------------------------------------------------
// Plugin operation registration descriptor
// ---------------------------------------------------------------------------
struct PluginOperationDesc {
    // Globally unique type ID. Use reverse-domain format: "com.myapp.myop"
    std::string                 typeId;

    // Human-readable name (for debug/logging only)
    std::string                 displayName;

    // Parameter schema — defines what params this op accepts
    std::vector<PluginParamDef> params;

    // Engine version compatibility
    uint32_t                    minEngineVersion = 0;
    uint32_t                    maxEngineVersion = UINT32_MAX;

    // Determinism declaration:
    // true  = same params + same engine version always produces the same output
    // false = op uses time, randomness, or external state (not allowed in Export profile)
    bool                        isDeterministic = true;

    // What entity IDs this op reads (for dependency graph registration)
    // Return the set of IDs that, when changed, require this op to recompute.
    std::function<std::vector<uint64_t>(const PluginOp&)> getDependencies;

    // Resolve the operation: write into PluginResolveContext::outputBuffer
    std::function<LSError(const PluginOp&, PluginResolveContext&)> resolve;

    // Serialize params to JSON string
    std::function<Result<std::string>(const PluginOp&)> serialize;

    // Deserialize params from JSON string
    std::function<Result<PluginOp>(std::string_view)> deserialize;

    // Optional: migrate params from an older engine version
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

    // Generate the pattern tile as a 1-bit mask (true = foreground)
    std::function<std::vector<bool>(
        uint32_t tileW,
        uint32_t tileH,
        float density,
        float phase,
        const std::unordered_map<std::string, std::any>& params
    )> generateTile;

    std::function<Result<std::string>(const std::unordered_map<std::string, std::any>&)> serialize;
    std::function<Result<std::unordered_map<std::string, std::any>>(std::string_view)> deserialize;
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
// Allows custom transform math (e.g. IK pose results from Practipuppets)
// ---------------------------------------------------------------------------
struct PluginTransformResolverDesc {
    std::string typeId;

    // Returns the 3×3 matrix that should be applied
    std::function<Result<Mat3f>(
        const PluginOp& op,
        PluginResolveContext& ctx
    )> resolve;
};

// ---------------------------------------------------------------------------
// Plugin compile policy registration
// Allows custom rasterization/sampling policies
// ---------------------------------------------------------------------------
struct PluginCompilePolicyDesc {
    std::string typeId;

    // Given a set of sub-pixel samples and a palette, return the final pixel color
    std::function<Result<Color>(
        std::span<const Color> samples,
        PaletteId palette,
        const CompileProfile& profile
    )> resolve;
};

} // namespace ls
