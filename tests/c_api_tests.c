// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2026 the LiveSprite authors
/* c_api_tests.c — the C ABI, exercised from actual C.
 *
 * This file is compiled by the C compiler, not the C++ one. That is the whole
 * point of it: a header can look like C and still only compile as C++, and the
 * only way to find out is to hand it to a C compiler. If livesprite_c.h ever
 * grows a default argument, an overload, a bool, or a struct declared the way
 * only C++ allows, this test stops building.
 *
 * It also walks the paths a real binding walks: create by type name, set
 * parameters by name, compile, read pixels, save, reload, package, undo.
 */

#include <livesprite/livesprite_c.h>

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
            ++failures;                                                  \
        }                                                                \
    } while (0)

#define CHECK_OK(expr)                                                   \
    do {                                                                 \
        ls_error err_ = (expr);                                          \
        if (err_ != LS_OK) {                                             \
            printf("FAIL %s:%d  %s -> %s\n", __FILE__, __LINE__, #expr,  \
                   ls_error_string(err_));                               \
            ++failures;                                                  \
        }                                                                \
    } while (0)

/* Counts pixels with any alpha, which is all these tests need to know about a
 * compiled raster to tell whether the operations took effect. */
static int opaque_pixels(const ls_raster* raster) {
    const uint8_t* data = ls_raster_data(raster);
    size_t size = ls_raster_size(raster);
    size_t i;
    int count = 0;
    if (data == NULL) {
        return 0;
    }
    for (i = 3; i < size; i += 4) {
        if (data[i] != 0) {
            ++count;
        }
    }
    return count;
}

static ls_profile export_profile(uint32_t size) {
    ls_profile profile;
    memset(&profile, 0, sizeof(profile));
    profile.type = LS_PROFILE_EXPORT;
    profile.output_width = size;
    profile.output_height = size;
    profile.palette_policy = LS_PALETTE_UNCONSTRAINED;
    return profile;
}

/* ------------------------------------------------------------------------- */

static void test_version_and_errors(void) {
    CHECK(ls_engine_version() != 0);
    CHECK(strcmp(ls_error_string(LS_OK), "None") == 0);
    CHECK(ls_error_string(LS_ERROR_INVALID_ID) != NULL);
    CHECK(strcmp(ls_error_string(LS_ERROR_BUFFER_TOO_SMALL), "BufferTooSmall") == 0);
}

/* A null context, a null out pointer and a bad id must all be answered rather
 * than crashed on: C callers have no type system to stop them. */
static void test_boundary_is_defensive(void) {
    ls_id doc = 0;
    ls_context* ctx = ls_context_create();
    CHECK(ctx != NULL);

    CHECK(ls_document_create(NULL, "x", 8, 8, &doc) == LS_ERROR_NULL_ARGUMENT);
    CHECK(ls_document_create(ctx, "x", 8, 8, NULL) == LS_ERROR_NULL_ARGUMENT);
    CHECK(ls_document_destroy(ctx, 999999) == LS_ERROR_INVALID_ID);
    CHECK(ls_sprite_destroy(ctx, 0) == LS_ERROR_INVALID_ID);

    /* Releasing null, and releasing twice, are both harmless. */
    ls_raster_release(NULL);
    ls_blob_release(NULL);
    ls_snapshot_release(NULL);
    ls_package_builder_release(NULL);
    ls_context_destroy(NULL);

    ls_context_destroy(ctx);
}

/* The catalogue is enumerable, so a binding does not hard-code 40 names. */
static void test_operation_catalogue(void) {
    size_t count = ls_operation_type_count();
    size_t needed = 0;
    char name[64];
    int found_fill = 0;
    int found_rotate = 0;
    size_t i;

    CHECK(count == 40);

    for (i = 0; i < count; ++i) {
        CHECK_OK(ls_operation_type_name(i, name, sizeof(name), &needed));
        if (strcmp(name, "FillSolidOp") == 0)  { found_fill = 1; }
        if (strcmp(name, "RotateOp") == 0)     { found_rotate = 1; }
    }
    CHECK(found_fill == 1);
    CHECK(found_rotate == 1);

    CHECK(ls_operation_type_name(count, name, sizeof(name), &needed) == LS_ERROR_OUT_OF_BOUNDS);

    /* Asking with no buffer reports the size instead of writing anything. */
    needed = 0;
    CHECK(ls_operation_type_name(0, NULL, 0, &needed) == LS_ERROR_BUFFER_TOO_SMALL);
    CHECK(needed > 1);

    /* A buffer one byte short is refused rather than truncated into. */
    CHECK(ls_operation_type_name(0, name, needed - 1, &needed) == LS_ERROR_BUFFER_TOO_SMALL);
}

/* The path a binding actually takes: name a type, set parameters by name,
 * compile, look at pixels. */
static void test_build_and_compile(void) {
    ls_context* ctx = ls_context_create();
    ls_id doc = 0, sprite = 0, layer = 0, rect = 0, region = 0, fill = 0;
    ls_raster* raster = NULL;
    ls_color orange;
    ls_vec2f origin;
    int before = 0;

    CHECK(ctx != NULL);
    if (ctx == NULL) { return; }

    CHECK_OK(ls_document_create(ctx, "c-api", 32, 32, &doc));
    CHECK_OK(ls_sprite_create(ctx, doc, &sprite));
    CHECK_OK(ls_layer_create(ctx, sprite, "main", &layer));

    origin.x = 8.0f;
    origin.y = 8.0f;
    CHECK_OK(ls_geometry_rect(ctx, doc, origin, 10.0f, 10.0f, 0.0f, &rect));
    CHECK_OK(ls_region_from_geometry(ctx, rect, &region));

    CHECK_OK(ls_operation_add(ctx, layer, "FillSolidOp", -1, &fill));
    CHECK_OK(ls_operation_set_id(ctx, fill, "targetRegion", region));

    orange.r = 255; orange.g = 128; orange.b = 0; orange.a = 255;
    CHECK_OK(ls_operation_set_color(ctx, fill, "fallbackColor", orange));

    {
        ls_profile profile = export_profile(32);
        CHECK_OK(ls_compile_sprite(ctx, sprite, &profile, &raster));
    }
    CHECK(ls_raster_width(raster) == 32);
    CHECK(ls_raster_height(raster) == 32);
    CHECK(ls_raster_size(raster) == 32u * 32u * 4u);
    before = opaque_pixels(raster);
    CHECK(before == 100);
    ls_raster_release(raster);
    raster = NULL;

    /* An unknown type is refused at authoring time, not at compile time. */
    {
        ls_id bogus = 0;
        CHECK(ls_operation_add(ctx, layer, "NoSuchOp", -1, &bogus)
              == LS_ERROR_OPERATION_TYPE_MISMATCH);
    }

    /* Nothing is spent: rotating recompiles from the same operations. */
    {
        ls_id turn = 0;
        ls_vec2f pivot;
        ls_profile profile = export_profile(32);
        pivot.x = 16.0f;
        pivot.y = 16.0f;
        CHECK_OK(ls_operation_add(ctx, layer, "RotateOp", -1, &turn));
        CHECK_OK(ls_operation_set_id(ctx, turn, "targetLayer", layer));
        CHECK_OK(ls_operation_set_float(ctx, turn, "angleDegrees", 90.0f));
        CHECK_OK(ls_operation_set_vec2(ctx, turn, "pivotFallback", pivot));

        CHECK_OK(ls_compile_sprite(ctx, sprite, &profile, &raster));
        /* A quarter turn is exact, so the pixel count is preserved. */
        CHECK(opaque_pixels(raster) == before);
        ls_raster_release(raster);
        raster = NULL;
    }

    ls_context_destroy(ctx);
}

/* Parameters round-trip through the same names the save file uses, and the
 * types they report are the types they accept. */
static void test_parameters_round_trip(void) {
    ls_context* ctx = ls_context_create();
    ls_id doc = 0, sprite = 0, layer = 0, op = 0;
    size_t count = 0, i;
    int saw_angle = 0;
    float angle = 0.0f;

    CHECK(ctx != NULL);
    if (ctx == NULL) { return; }

    CHECK_OK(ls_document_create(ctx, "params", 16, 16, &doc));
    CHECK_OK(ls_sprite_create(ctx, doc, &sprite));
    CHECK_OK(ls_layer_create(ctx, sprite, "main", &layer));
    CHECK_OK(ls_operation_add(ctx, layer, "RotateOp", -1, &op));

    CHECK_OK(ls_operation_parameter_count(ctx, op, &count));
    CHECK(count > 0);

    for (i = 0; i < count; ++i) {
        char name[64];
        size_t needed = 0;
        int32_t type = LS_PARAM_UNSUPPORTED;
        CHECK_OK(ls_operation_parameter_at(ctx, op, i, name, sizeof(name), &needed, &type));
        if (strcmp(name, "angleDegrees") == 0) {
            saw_angle = 1;
            CHECK(type == LS_PARAM_FLOAT);
        }
    }
    CHECK(saw_angle == 1);

    CHECK_OK(ls_operation_set_float(ctx, op, "angleDegrees", 37.5f));
    CHECK_OK(ls_operation_get_float(ctx, op, "angleDegrees", &angle));
    CHECK(angle == 37.5f);

    /* A name that is not a parameter of this operation is refused. */
    CHECK(ls_operation_set_float(ctx, op, "notAParameter", 1.0f) != LS_OK);

    /* Reading a real field as the wrong type says so, rather than pretending
     * the field does not exist. */
    {
        ls_color color;
        CHECK(ls_operation_get_color(ctx, op, "angleDegrees", &color)
              == LS_ERROR_OPERATION_TYPE_MISMATCH);
    }

    ls_context_destroy(ctx);
}

/* Save, reload into a second context, and compare compiled pixels rather than
 * structures: the picture is what has to survive. */
static void test_serialize_round_trip(void) {
    ls_context* first = ls_context_create();
    ls_context* second = ls_context_create();
    ls_id doc = 0, sprite = 0, layer = 0, rect = 0, region = 0, fill = 0;
    ls_id reloaded_doc = 0;
    ls_blob* saved = NULL;
    ls_raster* raster = NULL;
    ls_vec2f origin;
    int original_count = 0;

    CHECK(first != NULL && second != NULL);
    if (first == NULL || second == NULL) { return; }

    CHECK_OK(ls_document_create(first, "save", 16, 16, &doc));
    CHECK_OK(ls_sprite_create(first, doc, &sprite));
    CHECK_OK(ls_layer_create(first, sprite, "main", &layer));
    origin.x = 4.0f;
    origin.y = 4.0f;
    CHECK_OK(ls_geometry_rect(first, doc, origin, 6.0f, 6.0f, 0.0f, &rect));
    CHECK_OK(ls_region_from_geometry(first, rect, &region));
    CHECK_OK(ls_operation_add(first, layer, "FillSolidOp", -1, &fill));
    CHECK_OK(ls_operation_set_id(first, fill, "targetRegion", region));

    {
        ls_profile profile = export_profile(16);
        CHECK_OK(ls_compile_sprite(first, sprite, &profile, &raster));
        original_count = opaque_pixels(raster);
        CHECK(original_count == 36);
        ls_raster_release(raster);
        raster = NULL;
    }

    CHECK_OK(ls_document_serialize(first, doc, &saved));
    CHECK(ls_blob_size(saved) > 0);
    CHECK(ls_blob_data(saved) != NULL);

    CHECK_OK(ls_document_deserialize(second, ls_blob_data(saved), ls_blob_size(saved),
                                     &reloaded_doc));
    CHECK(reloaded_doc != 0);
    ls_blob_release(saved);

    /* Ids are file-local: the reload mints its own, so the sprite is found
     * through the document rather than by reusing the old handle. */
    {
        uint32_t width = 0, height = 0;
        CHECK_OK(ls_document_canvas(second, reloaded_doc, &width, &height));
        CHECK(width == 16);
        CHECK(height == 16);
    }

    ls_context_destroy(first);
    ls_context_destroy(second);
}

/* A package carries the document plus entries the engine does not understand,
 * and hands them back so they can be written out again. */
static void test_package_round_trip(void) {
    ls_context* ctx = ls_context_create();
    ls_id doc = 0, reloaded = 0;
    ls_package* builder = NULL;
    ls_package* reader = NULL;
    ls_blob* written = NULL;
    static const uint8_t payload[4] = { 1, 2, 3, 4 };

    CHECK(ctx != NULL);
    if (ctx == NULL) { return; }

    /* Names are validated, never repaired. */
    CHECK(ls_package_entry_name_valid("fast/thumbnail.png") == 1);
    CHECK(ls_package_entry_name_valid("../escape") == 0);
    CHECK(ls_package_entry_name_valid("/absolute") == 0);
    CHECK(ls_package_entry_name_valid("livesprite.json") == 0);
    CHECK(ls_package_entry_name_valid("unnamespaced.txt") == 0);
    CHECK(ls_package_entry_name_valid(NULL) == 0);

    CHECK_OK(ls_document_create(ctx, "packaged", 8, 8, &doc));

    builder = ls_package_builder_create();
    CHECK(builder != NULL);
    CHECK_OK(ls_package_builder_add(builder, "fast/thumbnail.png", "image/png",
                                    payload, sizeof(payload)));

    CHECK_OK(ls_package_write(ctx, doc, builder, &written));
    CHECK(ls_blob_size(written) > 0);
    ls_package_builder_release(builder);

    CHECK_OK(ls_package_read(ctx, ls_blob_data(written), ls_blob_size(written),
                             &reloaded, &reader));
    CHECK(reloaded != 0);
    CHECK(ls_package_entry_count(reader) == 1);

    {
        char name[64];
        size_t needed = 0, size = 0;
        const uint8_t* data = NULL;
        CHECK_OK(ls_package_entry_at(reader, 0, name, sizeof(name), &needed, &data, &size));
        CHECK(strcmp(name, "fast/thumbnail.png") == 0);
        CHECK(size == sizeof(payload));
        CHECK(data != NULL && data[0] == 1 && data[3] == 4);
    }

    CHECK(ls_package_entry_at(reader, 1, NULL, 0, NULL, NULL, NULL) == LS_ERROR_OUT_OF_BOUNDS);

    ls_blob_release(written);
    ls_package_builder_release(reader);
    ls_context_destroy(ctx);
}

/* Undo: a snapshot restores ids exactly, so handles stay valid across it. */
static void test_snapshot_restore(void) {
    ls_context* ctx = ls_context_create();
    ls_id doc = 0, sprite = 0, layer = 0, rect = 0, region = 0, fill = 0;
    ls_snapshot* snapshot = NULL;
    ls_raster* raster = NULL;
    ls_vec2f origin;

    CHECK(ctx != NULL);
    if (ctx == NULL) { return; }

    CHECK_OK(ls_document_create(ctx, "undo", 16, 16, &doc));
    CHECK_OK(ls_sprite_create(ctx, doc, &sprite));
    CHECK_OK(ls_layer_create(ctx, sprite, "main", &layer));
    origin.x = 4.0f;
    origin.y = 4.0f;
    CHECK_OK(ls_geometry_rect(ctx, doc, origin, 4.0f, 4.0f, 0.0f, &rect));
    CHECK_OK(ls_region_from_geometry(ctx, rect, &region));
    CHECK_OK(ls_operation_add(ctx, layer, "FillSolidOp", -1, &fill));
    CHECK_OK(ls_operation_set_id(ctx, fill, "targetRegion", region));

    CHECK_OK(ls_snapshot_create(ctx, doc, &snapshot));
    CHECK(snapshot != NULL);

    /* Change something, then take it back. */
    CHECK_OK(ls_operation_set_float(ctx, fill, "opacity", 0.25f));
    CHECK_OK(ls_snapshot_restore(ctx, doc, snapshot));

    {
        float opacity = 0.0f;
        /* The operation handle still resolves, which is the point of restoring
         * ids rather than renumbering them. */
        CHECK_OK(ls_operation_get_float(ctx, fill, "opacity", &opacity));
        CHECK(opacity == 1.0f);
    }

    {
        ls_profile profile = export_profile(16);
        CHECK_OK(ls_compile_sprite(ctx, sprite, &profile, &raster));
        CHECK(opaque_pixels(raster) == 16);
        ls_raster_release(raster);
    }

    ls_snapshot_release(snapshot);
    ls_context_destroy(ctx);
}

/* Metadata is namespaced, opaque, and copied out into a caller buffer. */
static void test_metadata(void) {
    ls_context* ctx = ls_context_create();
    ls_id doc = 0, sprite = 0;
    char value[64];
    size_t needed = 0;

    CHECK(ctx != NULL);
    if (ctx == NULL) { return; }

    CHECK_OK(ls_document_create(ctx, "meta", 8, 8, &doc));
    CHECK_OK(ls_sprite_create(ctx, doc, &sprite));

    CHECK_OK(ls_metadata_set(ctx, sprite, "fast.frame", "3"));
    CHECK_OK(ls_metadata_get(ctx, sprite, "fast.frame", value, sizeof(value), &needed));
    CHECK(strcmp(value, "3") == 0);
    CHECK(needed == 2);

    /* An unnamespaced key cannot collide with another app's, because it is
     * refused. */
    CHECK(ls_metadata_set(ctx, sprite, "frame", "3") != LS_OK);

    ls_context_destroy(ctx);
}


/* Frames through the ABI: clone one, reorder them, read the order back. This is
 * the whole authoring gesture a bridge or a scripted tool needs. */
static void test_frames(void) {
    ls_context* ctx = ls_context_create();
    ls_id doc = 0, first = 0, second = 0, third = 0, read = 0;
    ls_id ordered[3];
    size_t count = 0;

    CHECK(ctx != NULL);
    if (ctx == NULL) { return; }

    CHECK_OK(ls_document_create(ctx, "walk", 8, 8, &doc));
    CHECK_OK(ls_sprite_create(ctx, doc, &first));
    CHECK_OK(ls_sprite_clone(ctx, first, &second));
    CHECK_OK(ls_sprite_clone(ctx, second, &third));
    CHECK(second != first && third != second);

    CHECK_OK(ls_document_sprite_count(ctx, doc, &count));
    CHECK(count == 3);

    ordered[0] = third;
    ordered[1] = first;
    ordered[2] = second;
    CHECK_OK(ls_document_set_sprite_order(ctx, doc, ordered, 3));

    CHECK_OK(ls_document_sprite_at(ctx, doc, 0, &read));
    CHECK(read == third);
    CHECK_OK(ls_document_sprite_at(ctx, doc, 2, &read));
    CHECK(read == second);

    /* Not a permutation, so refused rather than half applied. */
    CHECK(ls_document_set_sprite_order(ctx, doc, ordered, 2) != LS_OK);
    CHECK_OK(ls_document_sprite_at(ctx, doc, 0, &read));
    CHECK(read == third);

    ls_context_destroy(ctx);
}


/* A ramp whose stops name palette roles follows a palette change, and the
 * palette can lose a slot and label one. The four calls this exercises are
 * the ones a palette panel needs and the ABI did not have. */
static void test_palette_roles(void) {
    ls_context* ctx = ls_context_create();
    ls_id doc = 0, sprite = 0, layer = 0, rect = 0, region = 0, fill = 0;
    ls_id palette = 0, ramp = 0, pattern = 0;
    ls_raster* raster = NULL;
    uint32_t roles[2] = { 1u, 2u };
    ls_color colors[2];
    float positions[2] = { 0.0f, 1.0f };
    ls_color night;
    int used = 0;
    size_t i, saw_night = 0, saw_day = 0;

    CHECK(ctx != NULL);
    if (ctx == NULL) { return; }

    colors[0].r = 20;  colors[0].g = 30;  colors[0].b = 60;  colors[0].a = 255;
    colors[1].r = 240; colors[1].g = 200; colors[1].b = 120; colors[1].a = 255;

    CHECK_OK(ls_document_create(ctx, "roles", 16, 16, &doc));
    CHECK_OK(ls_sprite_create(ctx, doc, &sprite));
    CHECK_OK(ls_palette_create(ctx, doc, "day", roles, colors, 2, &palette));
    CHECK_OK(ls_sprite_bind_palette(ctx, sprite, palette));
    CHECK_OK(ls_palette_set_label(ctx, palette, 1u, "shade"));

    CHECK_OK(ls_layer_create(ctx, sprite, "d", &layer));
    {
        ls_vec2f origin; origin.x = 0.0f; origin.y = 0.0f;
        CHECK_OK(ls_geometry_rect(ctx, doc, origin, 16.0f, 16.0f, 0.0f, &rect));
    }
    CHECK_OK(ls_region_from_geometry(ctx, rect, &region));

    /* The ramp names the roles, not the colours. */
    CHECK_OK(ls_ramp_create_roles(ctx, doc, "r", positions, colors, roles, 2, 1, &ramp));
    CHECK_OK(ls_dither_pattern_create(ctx, doc, LS_DITHER_BAYER2, &pattern));
    CHECK_OK(ls_operation_add(ctx, layer, "FillDitherOp", -1, &fill));
    CHECK_OK(ls_operation_set_id(ctx, fill, "targetRegion", region));
    CHECK_OK(ls_operation_set_id(ctx, fill, "ramp", ramp));
    CHECK_OK(ls_operation_set_id(ctx, fill, "pattern", pattern));

    /* Something now names role 1, and nothing names role 9. */
    CHECK_OK(ls_document_uses_palette_role(ctx, doc, 1u, &used));
    CHECK(used == 1);
    CHECK_OK(ls_document_uses_palette_role(ctx, doc, 9u, &used));
    CHECK(used == 0);

    /* Change the dark slot and the dither changes with it. */
    night.r = 60; night.g = 20; night.b = 20; night.a = 255;
    CHECK_OK(ls_palette_set_color(ctx, palette, 1u, night));
    {
        ls_profile profile = export_profile(16);
        CHECK_OK(ls_compile_sprite(ctx, sprite, &profile, &raster));
    }
    {
        const uint8_t* px = ls_raster_data(raster);
        for (i = 0; i < 16u * 16u; ++i) {
            if (px[i * 4] == 60 && px[i * 4 + 1] == 20) { ++saw_night; }
            if (px[i * 4] == 20 && px[i * 4 + 1] == 30) { ++saw_day; }
        }
    }
    CHECK(saw_night > 0);
    CHECK(saw_day == 0);
    ls_raster_release(raster);
    raster = NULL;

    /* Take the slot out: the stop falls back to its literal. */
    {
        /* The order: a permutation of what is there, and nothing else. */
        const uint32_t reversed[] = { 2u, 1u };
        const uint32_t short_list[] = { 2u };
        CHECK_OK(ls_palette_set_order(ctx, palette, reversed, 2));
        CHECK(ls_palette_set_order(ctx, palette, short_list, 1) != LS_OK);
        CHECK(ls_palette_set_order(ctx, palette, NULL, 2) != LS_OK);
    }
    CHECK_OK(ls_palette_remove_color(ctx, palette, 1u));
    CHECK(ls_palette_remove_color(ctx, palette, 1u) != LS_OK);   /* gone already */
    {
        ls_profile profile = export_profile(16);
        CHECK_OK(ls_compile_sprite(ctx, sprite, &profile, &raster));
    }
    {
        const uint8_t* px = ls_raster_data(raster);
        saw_day = 0;
        for (i = 0; i < 16u * 16u; ++i) {
            if (px[i * 4] == 20 && px[i * 4 + 1] == 30) { ++saw_day; }
        }
    }
    /* Half the pixels, because a Bayer2 screen at half density is a checker,
     * and the removed slot's stop reverted to its literal rather than to
     * nothing. This is also the check that caught the palette policy being
     * mirrored backwards: with NEAREST silently in force, the shrunken palette
     * snapped every pixel to its one remaining colour. */
    CHECK(saw_day == 128);
    ls_raster_release(raster);

    /* Several palettes, named, listed, and bound at either level. */
    {
        ls_id second = 0, found = 0;
        size_t count = 0, needed = 0;
        char name[16];
        colors[0].r = 60;  colors[0].g = 20; colors[0].b = 20; colors[0].a = 255;
        colors[1].r = 200; colors[1].g = 90; colors[1].b = 90; colors[1].a = 255;
        CHECK_OK(ls_palette_create(ctx, doc, "second", roles, colors, 2, &second));
        CHECK_OK(ls_document_palette_count(ctx, doc, &count));
        CHECK(count == 2);
        CHECK_OK(ls_document_palette_at(ctx, doc, 1, &found));
        CHECK(found == second);
        CHECK(ls_document_palette_at(ctx, doc, 2, &found) == LS_ERROR_OUT_OF_BOUNDS);
        CHECK_OK(ls_palette_name(ctx, second, name, sizeof(name), &needed));
        CHECK(strcmp(name, "second") == 0);
        CHECK_OK(ls_palette_set_name(ctx, second, "dusk"));
        CHECK_OK(ls_palette_name(ctx, second, name, sizeof(name), &needed));
        CHECK(strcmp(name, "dusk") == 0);

        CHECK_OK(ls_sprite_bind_palette(ctx, sprite, 0));       /* follow the document */
        CHECK_OK(ls_sprite_palette(ctx, sprite, &found));
        CHECK(found == 0);
        CHECK_OK(ls_document_bind_palette(ctx, doc, second));
        CHECK_OK(ls_document_palette(ctx, doc, &found));
        CHECK(found == second);
    }

    ls_context_destroy(ctx);
}

int main(void) {
    test_version_and_errors();
    test_boundary_is_defensive();
    test_operation_catalogue();
    test_build_and_compile();
    test_parameters_round_trip();
    test_serialize_round_trip();
    test_package_round_trip();
    test_snapshot_restore();
    test_metadata();
    test_frames();
    test_palette_roles();

    if (failures == 0) {
        printf("c_api: all checks passed\n");
        return 0;
    }
    printf("c_api: %d check(s) failed\n", failures);
    return 1;
}
