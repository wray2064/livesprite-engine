// tests/test_basic.cpp — Engine smoke tests
// Verifies that the API surface compiles and core entity wiring is correct.
// Not a full test suite — expand as implementations are added.

#include <livesprite/livesprite.h>
#include <cassert>
#include <iostream>

using namespace ls;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
#define LS_ASSERT_OK(expr) \
    do { auto r = (expr); assert(r.ok() && "Expected ok: " #expr); } while(0)

#define LS_ASSERT_ERR(expr, expected_err) \
    do { auto r = (expr); assert(r.fail() && r.error == expected_err && "Expected error: " #expr); } while(0)

// ---------------------------------------------------------------------------
// Test: engine context creation
// ---------------------------------------------------------------------------
void test_context_create() {
    auto ctx = LSContext::create();
    assert(ctx != nullptr);
    assert(ctx->engineVersion() == LS_ENGINE_VERSION);
    std::cout << "[PASS] test_context_create\n";
}

// ---------------------------------------------------------------------------
// Test: document and sprite CRUD
// ---------------------------------------------------------------------------
void test_document_sprite_crud() {
    auto ctx = LSContext::create();

    // Create document
    auto docResult = ctx->createDocument();
    assert(docResult.ok());
    DocumentId doc = docResult.value;
    assert(doc.valid());

    // Create sprite
    auto spriteResult = ctx->createSprite(doc);
    assert(spriteResult.ok());
    SpriteId sprite = spriteResult.value;
    assert(sprite.valid());

    // Create layer
    LayerDesc desc;
    desc.name    = "bg";
    desc.opacity = 1.f;
    desc.blend   = BlendMode::Normal;
    desc.visible = true;
    auto layerResult = ctx->createLayer(sprite, desc);
    assert(layerResult.ok());
    LayerId layer = layerResult.value;
    assert(layer.valid());

    // Invalid document
    auto badSprite = ctx->createSprite(DocumentId{9999});
    assert(badSprite.fail());
    assert(badSprite.error == LSError::InvalidId);

    std::cout << "[PASS] test_document_sprite_crud\n";
}

// ---------------------------------------------------------------------------
// Test: add and retrieve an operation
// ---------------------------------------------------------------------------
void test_add_retrieve_operation() {
    auto ctx = LSContext::create();
    auto doc    = ctx->createDocument().value;
    auto sprite = ctx->createSprite(doc).value;
    auto layer  = ctx->createLayer(sprite, LayerDesc{}).value;

    FillSolidOp fill;
    fill.paletteRole   = 1;
    fill.fallbackColor = Color{255, 0, 0, 255};
    fill.blend         = BlendMode::Normal;
    fill.opacity       = 1.f;

    auto opResult = ctx->addOperation(layer, Operation{fill});
    assert(opResult.ok());
    OperationId opId = opResult.value;
    assert(opId.valid());

    auto getResult = ctx->getOperation(opId);
    assert(getResult.ok());

    auto* retrieved = std::get_if<FillSolidOp>(&getResult.value);
    assert(retrieved != nullptr);
    assert(retrieved->paletteRole == 1);
    assert(retrieved->fallbackColor.r == 255);

    std::cout << "[PASS] test_add_retrieve_operation\n";
}

// ---------------------------------------------------------------------------
// Test: pivot creation
// ---------------------------------------------------------------------------
void test_pivot() {
    auto ctx    = LSContext::create();
    auto doc    = ctx->createDocument().value;
    auto sprite = ctx->createSprite(doc).value;

    auto pivotResult = ctx->createPivot(sprite, {16.f, 16.f});
    assert(pivotResult.ok());
    PivotId pivot = pivotResult.value;

    auto getResult = ctx->getPivot(pivot);
    assert(getResult.ok());
    assert(getResult.value.x == 16.f);
    assert(getResult.value.y == 16.f);

    LS_ASSERT_OK(ctx->setPivot(pivot, {8.f, 8.f}));
    assert(ctx->getPivot(pivot).value.x == 8.f);

    std::cout << "[PASS] test_pivot\n";
}

// ---------------------------------------------------------------------------
// Test: layer visibility and dirty propagation
// ---------------------------------------------------------------------------
void test_layer_visibility() {
    auto ctx    = LSContext::create();
    auto doc    = ctx->createDocument().value;
    auto sprite = ctx->createSprite(doc).value;
    auto layer  = ctx->createLayer(sprite, LayerDesc{.visible = true}).value;

    LS_ASSERT_OK(ctx->setLayerVisibility(layer, false));
    LS_ASSERT_OK(ctx->setLayerOpacity(layer, 0.5f));
    LS_ASSERT_OK(ctx->setLayerBlendMode(layer, BlendMode::Multiply));

    // Invalid layer ID
    LS_ASSERT_ERR(ctx->setLayerVisibility(LayerId{9999}, true), LSError::InvalidId);

    std::cout << "[PASS] test_layer_visibility\n";
}

// ---------------------------------------------------------------------------
// Test: plugin registration
// ---------------------------------------------------------------------------
void test_plugin_registration() {
    auto ctx = LSContext::create();

    assert(!ctx->isOperationTypeRegistered("com.test.myop"));

    PluginOperationDesc desc;
    desc.typeId      = "com.test.myop";
    desc.displayName = "My Test Op";
    desc.isDeterministic = true;

    LS_ASSERT_OK(ctx->registerOperationType(std::move(desc)));
    assert(ctx->isOperationTypeRegistered("com.test.myop"));

    auto types = ctx->registeredOperationTypes();
    assert(types.size() == 1);
    assert(types[0] == "com.test.myop");

    // Duplicate registration should succeed (overwrite)
    PluginOperationDesc desc2;
    desc2.typeId = "com.test.myop";
    LS_ASSERT_OK(ctx->registerOperationType(std::move(desc2)));

    // Empty typeId should fail
    PluginOperationDesc bad;
    bad.typeId = "";
    LS_ASSERT_ERR(ctx->registerOperationType(std::move(bad)), LSError::InvalidParameter);

    std::cout << "[PASS] test_plugin_registration\n";
}

// ---------------------------------------------------------------------------
// Test: Mat3f operations
// ---------------------------------------------------------------------------
void test_mat3f() {
    Mat3f identity = Mat3f::identity();
    Vec2f p = {3.f, 4.f};
    Vec2f tp = identity.transformPoint(p);
    assert(tp.x == 3.f && tp.y == 4.f);

    // Translation matrix
    Mat3f translate;
    translate.m[2] = 10.f;   // tx
    translate.m[5] = 20.f;   // ty
    Vec2f shifted = translate.transformPoint(p);
    assert(shifted.x == 13.f && shifted.y == 24.f);

    std::cout << "[PASS] test_mat3f\n";
}

// ---------------------------------------------------------------------------
// Test: IntervalSet region creation
// ---------------------------------------------------------------------------
void test_region_from_intervals() {
    auto ctx = LSContext::create();
    auto doc = ctx->createDocument().value;

    IntervalSet s;
    s.intervals = { {0, 0, 4}, {0, 2, 6}, {1, 1, 5} };  // overlapping — should normalize

    auto r = ctx->createRegionFromIntervals(s);
    assert(r.ok());

    auto retrieved = ctx->getRegionIntervals(r.value);
    assert(retrieved.ok());
    // After normalization, row 0 should have one merged interval [0, 6)
    bool foundMerged = false;
    for (auto& iv : retrieved.value.intervals) {
        if (iv.y == 0 && iv.x0 == 0 && iv.x1 == 6) { foundMerged = true; break; }
    }
    assert(foundMerged);

    std::cout << "[PASS] test_region_from_intervals\n";
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "LiveSprite Engine — Smoke Tests\n";
    std::cout << "Engine version: " << LS_ENGINE_VERSION << "\n\n";

    test_context_create();
    test_document_sprite_crud();
    test_add_retrieve_operation();
    test_pivot();
    test_layer_visibility();
    test_plugin_registration();
    test_mat3f();
    test_region_from_intervals();

    std::cout << "\nAll tests passed.\n";
    return 0;
}
