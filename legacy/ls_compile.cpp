// ls_compile.cpp — Compilation pipeline
// LiveSprite Engine
//
// This is the most critical subsystem. Implements the deterministic
// operation → pixel resolution pipeline.
//
// Pipeline stages (in order):
//   1. Collect operations for the sprite/layer (from dependency-resolved order)
//   2. Resolve transforms (apply MatrixTransform to all geometry references)
//   3. Compile regions (geometry → IntervalSet, applying rounding policy)
//   4. Rasterize fills and strokes into layer buffers
//   5. Apply dither patterns in the correct coordinate space
//   6. Composite layers (blend modes, opacity, masks)
//   7. Quantize to palette (NearestPaletteResolve or ExactMatch)
//   8. Apply alpha policy
//   9. Return CompileResult

#include <livesprite/ls_api.h>
#include <vector>
#include <cstring>

namespace ls {

// ---------------------------------------------------------------------------
// Internal: allocate an RGBA8 raster buffer
// ---------------------------------------------------------------------------
static Result<RasterBuffer> allocRaster(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return Result<RasterBuffer>::err(LSError::InvalidParameter);
    RasterBuffer buf;
    buf.width  = w;
    buf.height = h;
    buf.stride = w * 4;
    buf.pixels.assign(buf.stride * h, 0);
    return Result<RasterBuffer>::ok(std::move(buf));
}

// ---------------------------------------------------------------------------
// Internal: composite src onto dst using a blend mode
// ---------------------------------------------------------------------------
static void compositeBuffer(
    RasterBuffer& dst,
    const RasterBuffer& src,
    BlendMode mode,
    float opacity
) {
    // TODO: implement per-mode compositing (Normal, Multiply, Screen, etc.)
    // For now: simple alpha over (Normal mode)
    assert(dst.width == src.width && dst.height == src.height);
    for (uint32_t y = 0; y < dst.height; ++y) {
        const uint8_t* s = src.row(y);
        uint8_t*       d = dst.row(y);
        for (uint32_t x = 0; x < dst.width; ++x) {
            float sa = (s[3] / 255.f) * opacity;
            float da = d[3] / 255.f;
            float oa = sa + da * (1.f - sa);
            if (oa > 0.f) {
                d[0] = uint8_t((s[0]*sa + d[0]*da*(1.f-sa)) / oa);
                d[1] = uint8_t((s[1]*sa + d[1]*da*(1.f-sa)) / oa);
                d[2] = uint8_t((s[2]*sa + d[2]*da*(1.f-sa)) / oa);
                d[3] = uint8_t(oa * 255.f);
            }
            s += 4; d += 4;
        }
    }
}

// ---------------------------------------------------------------------------
// Internal: rasterize a single operation into a buffer
// ---------------------------------------------------------------------------
static LSError rasterizeOperation(
    const Operation& op,
    RasterBuffer& out,
    const CompileProfile& profile,
    LSContext::Impl* impl
) {
    // TODO: implement per-operation-type rasterization
    // Each op type should have a dedicated rasterizer:
    //   FillSolidOp     → flood-fill region with resolved color
    //   FillDitherOp    → apply dither pattern in correct coordinate space
    //   StrokePolylineOp → rasterize stroke along geometry
    //   RotateOp        → apply rotation matrix to region/geometry
    //   ... etc.
    return LSError::NotImplemented;
}

// ---------------------------------------------------------------------------
// compileLayer
// ---------------------------------------------------------------------------
Result<CompileResult> LSContext::compileLayer(LayerId id, const CompileProfile& profile) {
    if (!impl_->layers.count(id)) return Result<CompileResult>::err(LSError::InvalidId);
    if (profile.outputWidth == 0 || profile.outputHeight == 0)
        return Result<CompileResult>::err(LSError::InvalidParameter);

    auto bufResult = allocRaster(profile.outputWidth, profile.outputHeight);
    if (bufResult.fail()) return Result<CompileResult>::err(bufResult.error);
    RasterBuffer layerBuf = std::move(bufResult.value);

    const LayerData& layer = impl_->layers[id];
    if (!layer.visible) {
        // Invisible layer contributes nothing
        CompileResult res;
        res.raster = std::move(layerBuf);
        res.bounds = {};
        return Result<CompileResult>::ok(std::move(res));
    }

    // Process operations in order
    for (OperationId opId : layer.operations) {
        auto opIt = impl_->operations.find(opId);
        if (opIt == impl_->operations.end()) continue;

        // Each operation gets its own scratch buffer, then composited
        auto opBufResult = allocRaster(profile.outputWidth, profile.outputHeight);
        if (opBufResult.fail()) continue;
        RasterBuffer opBuf = std::move(opBufResult.value);

        LSError err = rasterizeOperation(opIt->second.op, opBuf, profile, impl_.get());
        if (err != LSError::None) {
            // TODO: in debug profile, annotate the failed op
            continue;
        }

        // Composite op result onto layer buffer
        compositeBuffer(layerBuf, opBuf, BlendMode::Normal, 1.f);
    }

    // Apply layer opacity
    if (layer.opacity < 1.f) {
        for (uint8_t& a : layerBuf.pixels) {
            // Only modify alpha channel (every 4th byte starting at index 3)
            // TODO: apply opacity correctly per-pixel
        }
    }

    // TODO: apply layer mask if set (layer.mask)

    // Compute tight bounds of non-transparent content
    Rect2i bounds = {};
    // TODO: scan pixels for non-zero alpha and compute bounds

    CompileResult res;
    res.raster = std::move(layerBuf);
    res.bounds = bounds;
    return Result<CompileResult>::ok(std::move(res));
}

// ---------------------------------------------------------------------------
// compileSprite — composites all visible layers
// ---------------------------------------------------------------------------
Result<CompileResult> LSContext::compileSprite(SpriteId id, const CompileProfile& profile) {
    if (!impl_->sprites.count(id)) return Result<CompileResult>::err(LSError::InvalidId);
    if (profile.outputWidth == 0 || profile.outputHeight == 0)
        return Result<CompileResult>::err(LSError::InvalidParameter);

    auto bufResult = allocRaster(profile.outputWidth, profile.outputHeight);
    if (bufResult.fail()) return Result<CompileResult>::err(bufResult.error);
    RasterBuffer spriteBuf = std::move(bufResult.value);

    const SpriteData& sprite = impl_->sprites[id];

    // Compile layers in compositing order (bottom to top)
    for (auto it = sprite.layers.rbegin(); it != sprite.layers.rend(); ++it) {
        LayerId layerId = *it;
        auto layerResult = compileLayer(layerId, profile);
        if (layerResult.fail()) continue;

        const LayerData& layer = impl_->layers[layerId];
        compositeBuffer(spriteBuf, layerResult.value.raster, layer.blend, layer.opacity);
    }

    // Apply palette quantization
    PaletteId palId = impl_->resolveEffectivePalette(id, sprite.ownerDocument);
    if (palId.valid() && profile.palette != PalettePolicy::Unconstrained) {
        // TODO: quantizeToPalette(spriteBuf, palId)
    }

    // Apply alpha policy
    // TODO: apply profile.alpha policy

    Rect2i bounds = {};
    // TODO: compute tight bounds

    CompileResult res;
    res.raster = std::move(spriteBuf);
    res.bounds = bounds;
    return Result<CompileResult>::ok(std::move(res));
}

// ---------------------------------------------------------------------------
// compilePreview — fast preview at capped resolution
// ---------------------------------------------------------------------------
Result<CompileResult> LSContext::compilePreview(SpriteId id, uint32_t maxW, uint32_t maxH) {
    CompileProfile preview;
    preview.outputWidth  = maxW;
    preview.outputHeight = maxH;
    preview.type         = CompileProfileType::Preview;
    preview.sampling     = SamplingPolicy::Center;    // fast, lower quality
    preview.palette      = PalettePolicy::Unconstrained;
    return compileSprite(id, preview);
}

// ---------------------------------------------------------------------------
// compileToRaster / compileToMask / compileBoundsOnly
// ---------------------------------------------------------------------------
Result<RasterBuffer> LSContext::compileToRaster(SpriteId id, const CompileProfile& profile) {
    auto res = compileSprite(id, profile);
    if (res.fail()) return Result<RasterBuffer>::err(res.error);
    return Result<RasterBuffer>::ok(std::move(res.value.raster));
}

Result<RasterBuffer> LSContext::compileToMask(SpriteId id, const CompileProfile& profile) {
    CompileProfile maskProfile = profile;
    maskProfile.type = CompileProfileType::MaskOnly;
    auto res = compileSprite(id, maskProfile);
    if (res.fail()) return Result<RasterBuffer>::err(res.error);
    // TODO: collapse to single-channel alpha mask
    return Result<RasterBuffer>::ok(std::move(res.value.raster));
}

Result<Rect2i> LSContext::compileBoundsOnly(SpriteId id, const CompileProfile& profile) {
    CompileProfile boundsProfile = profile;
    boundsProfile.type = CompileProfileType::BoundsOnly;
    auto res = compileSprite(id, boundsProfile);
    if (res.fail()) return Result<Rect2i>::err(res.error);
    return Result<Rect2i>::ok(res.value.bounds);
}

// ---------------------------------------------------------------------------
// Palette helpers
// ---------------------------------------------------------------------------
PaletteId LSContext::Impl::resolveEffectivePalette(SpriteId sprite, DocumentId doc) const {
    // Sprite palette overrides document palette
    auto sit = sprites.find(sprite);
    if (sit != sprites.end() && sit->second.boundPalette.valid())
        return sit->second.boundPalette;
    auto dit = documents.find(doc);
    if (dit != documents.end())
        return dit->second.defaultPalette;
    return PaletteId::null();
}

} // namespace ls
