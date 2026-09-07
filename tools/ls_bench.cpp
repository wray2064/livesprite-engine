// ls_bench.cpp — a baseline, so "fast enough for a live canvas" is a number
// rather than a hope.
//
// Reports milliseconds per operation for the paths an editor leans on: a full
// compile, a cached compile, a compile after one parameter changes, region
// maths, an assembly, an undo snapshot and restore.
//
//   livesprite_bench [--size N] [--layers N] [--repeats N]

#include <livesprite/livesprite.h>

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace ls;

namespace {

double millisecondsOf(const std::function<void()>& work, int repeats) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < repeats; ++i) {
        work();
    }
    const auto finish = std::chrono::steady_clock::now();
    const std::chrono::duration<double, std::milli> elapsed = finish - start;
    return elapsed.count() / static_cast<double>(repeats);
}

void report(const std::string& label, double milliseconds, const std::string& note = {}) {
    std::cout << "  " << std::left << std::setw(34) << label
              << std::right << std::setw(9) << std::fixed << std::setprecision(3)
              << milliseconds << " ms";
    if (!note.empty()) {
        std::cout << "   " << note;
    }
    std::cout << "\n";
}

std::string verdict(double milliseconds) {
    if (milliseconds <= 16.0) {
        return "fits a 60 fps frame";
    }
    if (milliseconds <= 33.0) {
        return "fits 30 fps";
    }
    return "too slow for live preview";
}

} // namespace

int main(int argc, char** argv) {
    uint32_t size = 128;
    int layerCount = 8;
    int repeats = 20;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--size" && hasValue)         { size = static_cast<uint32_t>(std::atoi(argv[++i])); }
        else if (arg == "--layers" && hasValue)  { layerCount = std::atoi(argv[++i]); }
        else if (arg == "--repeats" && hasValue) { repeats = std::max(1, std::atoi(argv[++i])); }
        else {
            std::cout << "usage: livesprite_bench [--size N] [--layers N] [--repeats N]\n";
            return arg == "--help" ? 0 : 1;
        }
    }

    auto ctx = LSContext::create();
    const DocumentId doc = ctx->createDocument({"bench", size, size}).value;
    const SpriteId sprite = ctx->createSprite(doc).value;

    const RampId ramp = ctx->createRamp(doc, {"tone", {
        {0.f, {30, 34, 52, 255}}, {0.5f, {120, 132, 170, 255}},
        {1.f, {228, 234, 250, 255}}}, true}).value;
    const PatternId pattern = ctx->createDitherPattern(doc, DitherPatternKind::Bayer8).value;

    // A sprite that looks like something an artist would actually build: a
    // dithered fill and an outline per layer, layers overlapping.
    OperationId drivenOperation;
    RegionId drivenRegion;
    for (int i = 0; i < layerCount; ++i) {
        const LayerId layer = ctx->createLayer(sprite, {"layer" + std::to_string(i)}).value;
        const float inset = static_cast<float>(i) * 2.f;
        const GeometryId rect = ctx->createRect(doc, {
            {4.f + inset, 4.f + inset},
            static_cast<float>(size) - 8.f - inset * 2.f,
            static_cast<float>(size) - 8.f - inset * 2.f, 2.f}).value;
        const RegionId region = ctx->createRegionFromGeometry(rect).value;

        FillDitherOp dither;
        dither.targetRegion = region;
        dither.ramp = ramp;
        dither.pattern = pattern;
        dither.modulation = DitherModulation::Linear;
        dither.gradientStart = {0.f, 0.f};
        dither.gradientEnd = {static_cast<float>(size), static_cast<float>(size)};
        const OperationId fill = ctx->addOperation(layer, dither).value;

        GenerateOuterOutlineOp outline;
        outline.targetRegion = region;
        outline.fallbackColor = {20, 22, 30, 255};
        ctx->addOperation(layer, outline);

        if (i == 0) {
            drivenOperation = fill;
            drivenRegion = region;
        }
    }

    RotateOp turn;
    turn.targetLayer = ctx->getSpriteInfo(sprite).value.layers.back();
    turn.angleDegrees = 20.f;
    turn.pivotFallback = { static_cast<float>(size) * 0.5f, static_cast<float>(size) * 0.5f };
    ctx->addOperation(turn.targetLayer, turn);

    CompileProfile profile;
    profile.type = CompileProfileType::Export;
    profile.outputWidth = size;
    profile.outputHeight = size;

    std::cout << "LiveSprite baseline: " << size << "x" << size << ", " << layerCount
              << " layers, " << repeats << " repeats per measurement\n\n";

    // A compile from cold, which is what a document open costs.
    const double cold = millisecondsOf([&]() {
        ctx->markDirty(sprite.value);
        ctx->compileSprite(sprite, profile);
    }, repeats);
    report("compile, everything dirty", cold, verdict(cold));

    // The same compile with nothing changed: what idling costs.
    ctx->compileSprite(sprite, profile);
    const double cached = millisecondsOf([&]() {
        ctx->compileSprite(sprite, profile);
    }, repeats);
    report("compile, cached", cached, verdict(cached));

    // One parameter changed, which is what dragging a slider costs.
    float angle = 0.f;
    const double driven = millisecondsOf([&]() {
        angle += 1.f;
        ctx->setOperationParameter(drivenOperation, "density", 0.3f + angle * 0.001f);
        ctx->compileSprite(sprite, profile);
    }, repeats);
    report("compile, one parameter driven", driven, verdict(driven));

    // A stroke landing in a region, which is what drawing costs.
    int32_t strokeY = 0;
    const double drawing = millisecondsOf([&]() {
        PixelRegionDesc stroke;
        strokeY = (strokeY + 3) % static_cast<int32_t>(size);
        for (int32_t x = 0; x < 12; ++x) {
            stroke.pixels.push_back({{x + 4, strokeY}, Color::black()});
        }
        ctx->addPixelsToRegion(drivenRegion, stroke);
        ctx->compileSprite(sprite, profile);
    }, repeats);
    report("draw a stroke, then compile", drawing, verdict(drawing));

    // Region maths on a shape the size of the canvas.
    const RegionId a = ctx->createRegionFromGeometry(
        ctx->createEllipse(doc, {{static_cast<float>(size) * 0.4f, static_cast<float>(size) * 0.5f},
                                 static_cast<float>(size) * 0.35f,
                                 static_cast<float>(size) * 0.35f}).value).value;
    const RegionId b = ctx->createRegionFromGeometry(
        ctx->createEllipse(doc, {{static_cast<float>(size) * 0.6f, static_cast<float>(size) * 0.5f},
                                 static_cast<float>(size) * 0.35f,
                                 static_cast<float>(size) * 0.35f}).value).value;
    report("region union", millisecondsOf([&]() { ctx->unionRegions(a, b); }, repeats));
    report("region outset by 2", millisecondsOf([&]() { ctx->expandRegion(a, 2.f); }, repeats));

    // An articulated assembly, which is what a posed character costs.
    const SocketId socket = ctx->addSocket(sprite, {"mount",
        {static_cast<float>(size) * 0.5f, static_cast<float>(size) * 0.5f}, 0.f}).value;
    for (int i = 0; i < 3; ++i) {
        const SpriteId part = ctx->createSprite(doc).value;
        const LayerId layer = ctx->createLayer(part, {"part"}).value;
        const GeometryId rect = ctx->createRect(doc, {{0.f, 0.f}, 12.f, 24.f, 1.f}).value;
        const RegionId region = ctx->createRegionFromGeometry(rect).value;
        FillSolidOp fill;
        fill.targetRegion = region;
        fill.fallbackColor = {200, 160, 120, 255};
        ctx->addOperation(layer, fill);
        ctx->createPivot(part, PivotDesc{"root", {6.f, 2.f}});
        ctx->attachSprite(part, socket);
    }
    const double assembly = millisecondsOf([&]() {
        ctx->markDirty(sprite.value);
        ctx->compileAssembly(sprite, profile);
    }, repeats);
    report("assembly of 4 sprites", assembly, verdict(assembly));

    // Undo, which an editor pays on every action.
    auto snapshot = ctx->snapshotDocumentState(doc);
    const double capture = millisecondsOf([&]() { ctx->snapshotDocumentState(doc); }, repeats);
    report("snapshot document state", capture, verdict(capture));
    const double restore = millisecondsOf([&]() {
        ctx->restoreDocumentState(doc, snapshot.value);
    }, repeats);
    report("restore document state", restore, verdict(restore));

    // What the same document costs through the save format, which is what a
    // durable save costs rather than an undo step.
    auto saved = ctx->serializeDocument(doc);
    const double save = millisecondsOf([&]() { ctx->serializeDocument(doc); }, repeats);
    report("serialize document (a file save)", save,
           std::to_string(saved.value.bytes.size() / 1024) + " KB");

    std::cout << "\n";
    return 0;
}
