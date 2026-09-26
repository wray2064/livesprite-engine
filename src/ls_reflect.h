// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors
#pragma once
// ls_reflect.h — one field table per operation, shared by everything that needs
// to walk an operation generically.
//
// Serialization reads and writes through these tables, and so does live
// parameter addressing: an app that wants to drive `angleDegrees` on an
// operation names the same field the save file names. One table, so a field
// cannot exist for one purpose and be missing for the other.
//
// An archive supplies `field(name, value)` for every member. It decides what to
// do with each one: write it, read it, list it, or match a single name.

#include "livesprite/livesprite.h"

namespace ls {
namespace reflect {

#define F(name) ar.field(#name, op.name);


#define F(name) ar.field(#name, op.name);

template<typename Ar> void mapOp(Ar& ar, FillSolidOp& op) {
    F(targetRegion) F(paletteRole) F(fallbackColor) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, FillGradientOp& op) {
    F(targetRegion) F(ramp) F(startPoint) F(endPoint) F(coordinateSpace) F(repeat) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, FillRampOp& op) {
    F(targetRegion) F(ramp) F(angle) F(coordinateSpace) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, FillDitherOp& op) {
    F(targetRegion) F(ramp) F(pattern) F(density) F(modulation) F(gradientStart) F(gradientEnd)
    F(phase) F(anchor) F(coordinateSpace) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, FillNoiseOp& op) {
    F(targetRegion) F(ramp) F(scale) F(seed) F(anchor) F(coordinateSpace) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, FillLinePatternOp& op) {
    F(targetRegion) F(lineRole) F(bgRole) F(spacing) F(angle) F(lineWidth) F(anchor)
    F(coordinateSpace) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, FillTexturePatternOp& op) {
    F(targetRegion) F(pattern) F(foregroundRole) F(backgroundRole) F(scale) F(offset) F(angle)
    F(anchor) F(coordinateSpace) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, FillSemanticColorOp& op) {
    F(targetRegion) F(paletteRole) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, StrokePolylineOp& op) {
    F(polyline) F(width) F(cap) F(join) F(miterLimit) F(taper) F(strokePattern) F(paletteRole)
    F(fallbackColor) F(snap) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, StrokeCurveOp& op) {
    F(curve) F(width) F(cap) F(join) F(miterLimit) F(taper) F(paletteRole) F(fallbackColor)
    F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, StrokeRegionBoundaryOp& op) {
    F(targetRegion) F(width) F(cap) F(join) F(miterLimit) F(paletteRole) F(fallbackColor)
    F(snap) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, StrokeBrushOp& op) {
    F(path) F(brushPattern) F(size) F(spacing) F(scatter) F(scatterSeed) F(paletteRole)
    F(fallbackColor) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, StrokePixelPathOp& op) {
    F(path) F(paletteRole) F(fallbackColor) F(snap) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, GenerateSilhouetteOutlineOp& op) {
    F(targetSprite) F(thickness) F(side) F(corner) F(diagonal) F(paletteRole) F(fallbackColor)
    F(limitBoundary) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, GenerateDropShadowOp& op) {
    F(targetSprite) F(offset) F(paletteRole) F(fallbackColor) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, ClearRegionOp& op) {
    F(targetRegion)
}
template<typename Ar> void mapOp(Ar& ar, FadeOp& op) {
    F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, DrawTilemapOp& op) {
    F(tilemap) F(tileset) F(origin) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, GenerateInnerOutlineOp& op) {
    F(targetRegion) F(thickness) F(corner) F(paletteRole) F(fallbackColor) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, GenerateOuterOutlineOp& op) {
    F(targetRegion) F(thickness) F(corner) F(diagonal) F(paletteRole) F(fallbackColor) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, GenerateRegionOutlineOp& op) {
    F(targetRegion) F(thickness) F(side) F(corner) F(paletteRole) F(fallbackColor) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, GenerateMaterialBoundaryOutlineOp& op) {
    F(sourceLayer) F(thickness) F(paletteRole) F(fallbackColor) F(blend) F(opacity)
}
template<typename Ar> void mapOp(Ar& ar, CleanupOutlineOp& op) {
    F(targetOutlineOp) F(removeIsolatedPixels) F(smoothCorners)
}
template<typename Ar> void mapOp(Ar& ar, JoinCornersOp& op) {
    F(outlineA) F(outlineB) F(joinRadius)
}
template<typename Ar> void mapOp(Ar& ar, ResolveOutlineCollisionsOp& op) {
    F(outlineOps)
}
template<typename Ar> void mapOp(Ar& ar, TranslateOp& op) {
    F(target) F(targetLayer) F(targetRegion) F(delta) F(pivot) F(coordinateSpace) F(rounding)
}
template<typename Ar> void mapOp(Ar& ar, RotateOp& op) {
    F(target) F(targetLayer) F(targetRegion) F(angleDegrees) F(pivot) F(pivotFallback)
    F(coordinateSpace) F(rounding) F(sampling)
}
template<typename Ar> void mapOp(Ar& ar, ScaleOp& op) {
    F(target) F(targetLayer) F(targetRegion) F(factor) F(pivot) F(pivotFallback)
    F(coordinateSpace) F(rounding) F(sampling)
}
template<typename Ar> void mapOp(Ar& ar, MirrorOp& op) {
    F(target) F(targetLayer) F(targetRegion) F(axis) F(pivot) F(pivotFallback) F(coordinateSpace)
}
template<typename Ar> void mapOp(Ar& ar, ShearOp& op) {
    F(target) F(targetLayer) F(targetRegion) F(shear) F(pivot) F(pivotFallback)
    F(coordinateSpace) F(sampling)
}
template<typename Ar> void mapOp(Ar& ar, SkewOp& op) {
    F(target) F(targetLayer) F(targetRegion) F(angleX) F(angleY) F(pivot) F(pivotFallback)
    F(coordinateSpace) F(sampling)
}
template<typename Ar> void mapOp(Ar& ar, SquashOp& op) {
    F(target) F(targetLayer) F(targetRegion) F(boundary) F(factor) F(pivot) F(pivotFallback) F(falloff)
}
template<typename Ar> void mapOp(Ar& ar, StretchOp& op) {
    F(target) F(targetLayer) F(targetRegion) F(boundary) F(factor) F(pivot) F(pivotFallback) F(falloff)
}
template<typename Ar> void mapOp(Ar& ar, MatrixTransformOp& op) {
    F(target) F(targetLayer) F(targetRegion) F(matrix) F(coordinateSpace) F(rounding) F(sampling)
}
template<typename Ar> void mapOp(Ar& ar, BendOp& op) {
    F(targetRegion) F(targetLayer) F(boundary) F(strength) F(angle) F(falloff) F(coordinateSpace)
}
template<typename Ar> void mapOp(Ar& ar, WarpOp& op) {
    F(targetRegion) F(targetLayer) F(handlePoints) F(displacements) F(strength) F(radius)
    F(falloff) F(influenceRegion)
}
template<typename Ar> void mapOp(Ar& ar, LatticeDeformOp& op) {
    F(targetRegion) F(targetLayer) F(gridW) F(gridH) F(controlPoints) F(influenceRegion)
}
template<typename Ar> void mapOp(Ar& ar, EnvelopeDeformOp& op) {
    F(targetRegion) F(targetLayer) F(envelopeCurve) F(strength) F(falloff)
}
template<typename Ar> void mapOp(Ar& ar, PinDeformOp& op) {
    F(targetRegion) F(targetLayer) F(pins) F(pinTargets) F(stiffness) F(influenceRegion)
}
template<typename Ar> void mapOp(Ar& ar, WeightedDeformOp& op) {
    F(targetRegion) F(targetLayer) F(handles) F(weights) F(handleTargets) F(radius) F(falloff)
}
template<typename Ar> void mapOp(Ar& ar, BoundaryDeformOp& op) {
    F(targetRegion) F(targetLayer) F(boundary) F(targetShape) F(strength) F(falloff)
}
template<typename Ar> void mapOp(Ar& ar, PathDeformOp& op) {
    F(targetRegion) F(targetLayer) F(path) F(offset) F(followTangent) F(falloff)
}
template<typename Ar> void mapOp(Ar& ar, PluginOp& op) {
    F(typeId) F(params)
}

// Geometry shapes
template<typename Ar> void mapShape(Ar& ar, PointDesc& op)    { F(position) }
template<typename Ar> void mapShape(Ar& ar, LineDesc& op)     { F(start) F(end) }
template<typename Ar> void mapShape(Ar& ar, PolylineDesc& op) { F(points) F(closed) }
template<typename Ar> void mapShape(Ar& ar, RectDesc& op)     { F(origin) F(width) F(height) F(cornerRadius) }
template<typename Ar> void mapShape(Ar& ar, EllipseDesc& op)  { F(center) F(radiusX) F(radiusY) }
template<typename Ar> void mapShape(Ar& ar, CircleDesc& op)   { F(center) F(radius) }
template<typename Ar> void mapShape(Ar& ar, PolygonDesc& op)  { F(vertices) F(includeEdges) }
template<typename Ar> void mapShape(Ar& ar, CurveDesc& op)    { F(segments) F(closed) }
template<typename Ar> void mapShape(Ar& ar, StrokesDesc& op)  { F(strokes) }
template<typename Ar> void mapShape(Ar& ar, AreaDesc& op)     { F(contours) }
template<typename Ar> void mapShape(Ar& ar, FaceDesc& op)     { F(seed) F(area) F(tolerance) F(diagonal) }


#undef F

// Walk whichever operation the variant holds.
template<typename Ar>
void visitOperation(Ar& ar, Operation& operation) {
    std::visit([&ar](auto& concrete) { mapOp(ar, concrete); }, operation);
}

} // namespace reflect
} // namespace ls
