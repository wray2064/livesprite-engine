#include <livesprite/livesprite.h>

#include <cassert>
#include <iostream>

static int count_opaque(const ls::RasterBuffer& raster) {
    int count = 0;
    for (unsigned int y = 0; y < raster.height; ++y) {
        const unsigned char* row = raster.row(y);
        for (unsigned int x = 0; x < raster.width; ++x) {
            if (row[x * 4 + 3] != 0) {
                ++count;
            }
        }
    }
    return count;
}

static int count_pixels(const ls::IntervalSet& intervals) {
    int count = 0;
    for (const ls::Interval& interval : intervals.intervals) {
        count += interval.x1 - interval.x0;
    }
    return count;
}

int main() {
    auto ctx = ls::LSContext::create();
    assert(ctx);
    assert(ctx->engineVersion() == ls::LS_ENGINE_VERSION);

    auto doc = ctx->createDocument(32, 32);
    assert(doc.ok());

    auto sprite = ctx->createSprite(doc.value);
    assert(sprite.ok());

    auto layer = ctx->createLayer(sprite.value, "body");
    assert(layer.ok());

    auto region = ctx->createRectRegion(doc.value, {{{9, 8}, {19, 25}}});
    assert(region.ok());

    auto ellipse = ctx->createEllipseRegion(doc.value, {{22.0f, 12.0f}, 4.0f, 3.0f});
    assert(ellipse.ok());
    auto ellipseIntervals = ctx->getRegionIntervals(ellipse.value);
    assert(ellipseIntervals.ok());
    assert(!ellipseIntervals.value.empty());
    auto ellipseBoundary = ctx->getRegionBoundaryIntervals(ellipse.value);
    assert(ellipseBoundary.ok());
    assert(!ellipseBoundary.value.empty());
    assert(ellipseBoundary.value.intervals.size() >= 2);

    std::vector<ls::PixelInput> loop;
    const ls::Color black{0, 0, 0, 255};
    for (int y = 4; y < 8; ++y) {
        for (int x = 4; x < 8; ++x) {
            if (x == 4 || x == 7 || y == 4 || y == 7) {
                loop.push_back({{x, y}, black});
            }
        }
    }
    auto pixelRegion = ctx->createPixelRegion(doc.value, {loop, true});
    assert(pixelRegion.ok());
    auto pixelIntervals = ctx->getRegionIntervals(pixelRegion.value);
    assert(pixelIntervals.ok());
    auto pixelBoundary = ctx->getRegionBoundaryIntervals(pixelRegion.value);
    assert(pixelBoundary.ok());
    assert(!pixelBoundary.value.empty());
    assert(count_pixels(pixelIntervals.value) == 16);
    assert(count_pixels(pixelBoundary.value) == 12);

    std::vector<ls::PixelInput> openStroke = {
        {{14, 8}, black}, {{14, 9}, black}, {{14, 10}, black},
        {{14, 11}, black}, {{15, 11}, black}, {{16, 11}, black},
        {{17, 11}, black},
    };
    auto openPixelRegion = ctx->createPixelRegion(doc.value, {openStroke, true});
    assert(openPixelRegion.ok());
    auto openIntervals = ctx->getRegionIntervals(openPixelRegion.value);
    auto openBoundary = ctx->getRegionBoundaryIntervals(openPixelRegion.value);
    assert(openIntervals.ok());
    assert(openBoundary.ok());
    assert(openBoundary.value.empty());
    assert(count_pixels(openIntervals.value) == static_cast<int>(openStroke.size()));

    std::vector<ls::PixelInput> cornerTouchLoop = {
        {{14, 8}, black}, {{15, 8}, black},
        {{13, 9}, black}, {{16, 9}, black},
        {{12, 10}, black}, {{17, 10}, black},
        {{13, 11}, black}, {{16, 11}, black},
        {{14, 12}, black}, {{15, 12}, black},
    };
    auto cornerTouchRegion = ctx->createPixelRegion(doc.value, {cornerTouchLoop, true});
    assert(cornerTouchRegion.ok());
    auto cornerTouchIntervals = ctx->getRegionIntervals(cornerTouchRegion.value);
    auto cornerTouchBoundary = ctx->getRegionBoundaryIntervals(cornerTouchRegion.value);
    assert(cornerTouchIntervals.ok());
    assert(cornerTouchBoundary.ok());
    assert(!cornerTouchBoundary.value.empty());
    assert(count_pixels(cornerTouchIntervals.value) > static_cast<int>(cornerTouchLoop.size()));

    auto fill = ctx->addSolidFill(layer.value, {region.value, {24, 28, 36, 255}});
    assert(fill.ok());

    auto dither = ctx->addDitherFill(layer.value, {region.value, {239, 76, 82, 255}, {255, 190, 57, 255}, 0.5f, 17});
    assert(dither.ok());

    auto rotate = ctx->addRotate(layer.value, {25.0f, {14.0f, 16.0f}});
    assert(rotate.ok());

    auto ops = ctx->getLayerOperations(layer.value);
    assert(ops.ok());
    assert(ops.value.size() == 3);

    auto compiled = ctx->compileSprite(sprite.value, {32, 32, true});
    assert(compiled.ok());
    assert(!compiled.value.raster.empty());
    assert(!compiled.value.bounds.empty());
    assert(compiled.value.trace.size() >= 3);

    auto layer90 = ctx->createLayer(sprite.value, "quarter_turn");
    assert(layer90.ok());
    auto circle = ctx->createEllipseRegion(doc.value, {{16.0f, 16.0f}, 5.0f, 5.0f});
    assert(circle.ok());
    assert(ctx->addSolidFill(layer90.value, {circle.value, {255, 255, 255, 255}}).ok());

    auto unrotated = ctx->compileLayer(layer90.value, {32, 32, false});
    assert(unrotated.ok());
    assert(ctx->addRotate(layer90.value, {90.0f, {16.0f, 16.0f}}).ok());
    auto rotated = ctx->compileLayer(layer90.value, {32, 32, true});
    assert(rotated.ok());
    assert(count_opaque(unrotated.value.raster) == count_opaque(rotated.value.raster));

    auto protectedPixelLayer = ctx->createLayer(sprite.value, "protected_pixel_rotate");
    assert(protectedPixelLayer.ok());
    std::vector<ls::PixelInput> closedPixelShape = {
        {{14, 12}, black}, {{15, 11}, black}, {{16, 11}, black}, {{17, 12}, black},
        {{18, 13}, black}, {{18, 14}, black}, {{17, 15}, black}, {{16, 16}, black},
        {{15, 16}, black}, {{14, 15}, black}, {{13, 14}, black}, {{13, 13}, black}
    };
    auto protectedPixelRegion = ctx->createPixelRegion(doc.value, {closedPixelShape, true});
    assert(protectedPixelRegion.ok());
    assert(ctx->addSolidFill(protectedPixelLayer.value, {protectedPixelRegion.value, {255, 255, 255, 255}}).ok());
    auto unrotatedProtectedPixels = ctx->compileLayer(protectedPixelLayer.value, {32, 32, false});
    assert(unrotatedProtectedPixels.ok());
    assert(ctx->addRotate(protectedPixelLayer.value, {25.0f, {12.5f, 17.5f}}).ok());
    auto rotatedProtectedPixels = ctx->compileLayer(protectedPixelLayer.value, {32, 32, true});
    assert(rotatedProtectedPixels.ok());
    assert(count_opaque(rotatedProtectedPixels.value.raster) >= count_opaque(unrotatedProtectedPixels.value.raster));

    auto pixelLayer = ctx->createLayer(sprite.value, "loose_pixels");
    assert(pixelLayer.ok());
    std::vector<ls::PixelInput> loosePixels = {
        {{22, 8}, black}, {{23, 8}, black}, {{24, 8}, black},
        {{24, 9}, black}, {{24, 10}, black}, {{25, 10}, black}
    };
    auto looseRegion = ctx->createPixelRegion(doc.value, {loosePixels, true});
    assert(looseRegion.ok());
    assert(ctx->addSolidFill(pixelLayer.value, {looseRegion.value, {255, 255, 255, 255}}).ok());
    assert(ctx->addRotate(pixelLayer.value, {25.0f, {16.0f, 16.0f}}).ok());
    auto looseRotated = ctx->compileLayer(pixelLayer.value, {32, 32, true});
    assert(looseRotated.ok());
    assert(count_opaque(looseRotated.value.raster) > 0);

    std::cout << "LiveSprite smoke test passed\n";
    std::cout << "Compiled bounds: x:" << compiled.value.bounds.min.x << ".." << compiled.value.bounds.max.x
              << " y:" << compiled.value.bounds.min.y << ".." << compiled.value.bounds.max.y << "\n";
    return 0;
}
