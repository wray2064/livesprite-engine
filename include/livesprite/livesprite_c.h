// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 the LiveSprite authors
#ifndef LIVESPRITE_C_H
#define LIVESPRITE_C_H

/* livesprite_c.h — the ABI-stable door into the engine.
 *
 * The C++ header is the pleasant way to use LiveSprite, and it is the right
 * choice for anything compiled alongside the engine. It is also unusable across
 * a binary boundary: it passes std::vector, std::string and std::function
 * through the interface, so a caller must share the engine's compiler, standard
 * library and flags exactly. Ship a shared library built with MSVC and a Clang
 * program cannot safely call it, and no other language can call it at all.
 *
 * This header is the same engine with those obstacles removed. It is the door
 * that Python, C#, Rust, Node and a WebAssembly build come through, and the one
 * that survives the engine being rebuilt with a different toolchain.
 *
 * The rules it holds to:
 *
 *   - Every entry point returns ls_error. Results come back through out
 *     parameters. Nothing throws, and nothing returns a value that has to be
 *     checked against a sentinel.
 *   - Handles are opaque integers. C cannot enforce that a layer id is not
 *     passed where a sprite id belongs the way the C++ TypedId does, so the
 *     engine validates every id it is given and answers LS_ERROR_INVALID_ID.
 *   - Memory the engine allocates is released by the engine. Anything with a
 *     _create or _compile in its name has a matching _release, and calling it
 *     twice, or on NULL, is harmless.
 *   - Strings are UTF-8. Out-strings are copied into a caller buffer; pass NULL
 *     to ask how large the buffer needs to be.
 *   - Operations are not 39 structs here. An operation is created by type name
 *     and configured by parameter name, through the same reflection the save
 *     file and the live instruction system already use. One field table, so a
 *     parameter cannot exist for C++ and be missing for everyone else.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(LS_C_SHARED)
#  if defined(LS_C_BUILDING)
#    define LS_C_API __declspec(dllexport)
#  else
#    define LS_C_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(LS_C_SHARED)
#  define LS_C_API __attribute__((visibility("default")))
#else
#  define LS_C_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ types -- */

/* Every entity handle. Zero is the null handle and is never valid.
 * The C++ API distinguishes SpriteId from LayerId at compile time; C cannot,
 * so the engine checks at the boundary instead. */
typedef uint64_t ls_id;

typedef int32_t ls_error;

/* Mirrors ls::LSError. The numbering is part of the ABI: append only. */
#define LS_OK                                 0
#define LS_ERROR_INVALID_ID                   1
#define LS_ERROR_INVALID_PARAMETER            2
#define LS_ERROR_NULL_ARGUMENT                3
#define LS_ERROR_OUT_OF_BOUNDS                4
#define LS_ERROR_DEPENDENCY_CYCLE             5
#define LS_ERROR_COMPILE_FAILURE              6
#define LS_ERROR_SERIALIZATION_FAILURE        7
#define LS_ERROR_DESERIALIZATION_FAILURE      8
#define LS_ERROR_VERSION_MISMATCH             9
#define LS_ERROR_VERSION_MIGRATION_FAILED    10
#define LS_ERROR_PLUGIN_NOT_FOUND            11
#define LS_ERROR_PLUGIN_ERROR                12
#define LS_ERROR_PLUGIN_DETERMINISM_VIOLATION 13
#define LS_ERROR_OPERATION_TYPE_MISMATCH     14
#define LS_ERROR_REGION_EMPTY                15
#define LS_ERROR_PALETTE_NOT_BOUND           16
#define LS_ERROR_RASTER_ALLOCATION_FAILED    17
#define LS_ERROR_NOT_IMPLEMENTED             18
#define LS_ERROR_PACKAGE_MALFORMED           19
#define LS_ERROR_PACKAGE_LIMIT_EXCEEDED      20
#define LS_ERROR_PACKAGE_ENTRY_REJECTED      21

/* Raised only by this header: the buffer handed in was too small. The call
 * reports the size it needed, so the caller can retry rather than guess. */
#define LS_ERROR_BUFFER_TOO_SMALL           1000

typedef struct ls_context ls_context;
typedef struct ls_raster  ls_raster;
typedef struct ls_blob    ls_blob;
typedef struct ls_package  ls_package;
typedef struct ls_snapshot ls_snapshot;

typedef struct { float x, y; } ls_vec2f;
typedef struct { int32_t x, y; } ls_vec2i;
typedef struct { uint8_t r, g, b, a; } ls_color;

/* Compile profile types, mirroring ls::CompileProfileType. */
#define LS_PROFILE_PREVIEW    0
#define LS_PROFILE_EXPORT     1
#define LS_PROFILE_DEBUG      2
#define LS_PROFILE_MASK_ONLY  3
#define LS_PROFILE_BOUNDS_ONLY 4

/* Palette policies, mirroring ls::PalettePolicy. */
#define LS_PALETTE_UNCONSTRAINED 0
#define LS_PALETTE_STRICT        1
#define LS_PALETTE_NEAREST       2

typedef struct {
    int32_t  type;            /* LS_PROFILE_*                                */
    uint32_t output_width;
    uint32_t output_height;
    int32_t  palette_policy;  /* LS_PALETTE_*                                */
    /* Only meaningful for patterns anchored in Export space, such as a sheet
     * cell that must keep its dither aligned across frames. */
    ls_vec2i export_origin;
} ls_profile;

/* Parameter types reported by ls_operation_parameter_at, mirroring
 * ls::ParameterType. Enumerations report as LS_PARAM_INT: set them with
 * ls_operation_set_int, using the value of the corresponding C++ enumerator. */
#define LS_PARAM_UNSUPPORTED 0
#define LS_PARAM_BOOL        1
#define LS_PARAM_INT         2
#define LS_PARAM_FLOAT       3
#define LS_PARAM_VEC2        4
#define LS_PARAM_COLOR       5
#define LS_PARAM_MATRIX      6
#define LS_PARAM_TEXT        7
#define LS_PARAM_ID          8

/* ------------------------------------------------------------ engine info -- */

/* Packed major<<16 | minor<<8 | patch. A file written by a newer major is
 * refused, so a binding can compare before it opens anything. */
LS_C_API uint32_t    ls_engine_version(void);

/* A short name for an error code. Never NULL, never owned by the caller. */
LS_C_API const char* ls_error_string(ls_error error);

/* ---------------------------------------------------------------- context -- */

/* One context owns every entity given out through it. It is not thread-safe:
 * one thread at a time, or lock outside. */
LS_C_API ls_context* ls_context_create(void);
LS_C_API void        ls_context_destroy(ls_context* ctx);

/* --------------------------------------------------------------- document -- */

LS_C_API ls_error ls_document_create(ls_context* ctx, const char* name,
                                     uint32_t width, uint32_t height,
                                     ls_id* out_document);
LS_C_API ls_error ls_document_destroy(ls_context* ctx, ls_id document);
LS_C_API ls_error ls_document_canvas(ls_context* ctx, ls_id document,
                                     uint32_t* out_width, uint32_t* out_height);

/* How large a canvas this application is willing to work with. Applies to
 * documents created, resized and read from files, so raising it lets this
 * context open files a stricter one refuses. Refused if it exceeds what the
 * engine can represent -- everything below that is a cost decision. */
LS_C_API ls_error ls_set_canvas_limits(ls_context* ctx, uint32_t max_dimension,
                                       uint64_t max_pixels);
LS_C_API ls_error ls_canvas_limits(ls_context* ctx, uint32_t* out_max_dimension,
                                   uint64_t* out_max_pixels);

/* What a document contains. The way to find your way around a document you did
 * not build -- one just read from a file, where every id was minted fresh. */
LS_C_API ls_error ls_document_sprite_count(ls_context* ctx, ls_id document,
                                           size_t* out_count);
LS_C_API ls_error ls_document_sprite_at(ls_context* ctx, ls_id document, size_t index,
                                        ls_id* out_sprite);
LS_C_API ls_error ls_document_name(ls_context* ctx, ls_id document,
                                   char* buffer, size_t buffer_size, size_t* out_needed);

/* ------------------------------------------------------- sprites & layers -- */

LS_C_API ls_error ls_sprite_create(ls_context* ctx, ls_id document, ls_id* out_sprite);
LS_C_API ls_error ls_sprite_destroy(ls_context* ctx, ls_id sprite);

LS_C_API ls_error ls_layer_create(ls_context* ctx, ls_id sprite, const char* name,
                                  ls_id* out_layer);
LS_C_API ls_error ls_layer_destroy(ls_context* ctx, ls_id layer);
LS_C_API ls_error ls_layer_set_name(ls_context* ctx, ls_id layer, const char* name);
LS_C_API ls_error ls_layer_name(ls_context* ctx, ls_id layer,
                                char* buffer, size_t buffer_size, size_t* out_needed);
LS_C_API ls_error ls_layer_set_visible(ls_context* ctx, ls_id layer, int visible);
LS_C_API ls_error ls_layer_set_opacity(ls_context* ctx, ls_id layer, float opacity);

/* The sprite transform places the sprite itself, and is the frame that sockets,
 * pivots and attached children resolve through. It is not an operation. */
LS_C_API ls_error ls_sprite_translate(ls_context* ctx, ls_id sprite, ls_vec2f delta);
/* pivot may be 0, meaning the sprite's own default pivot. */
LS_C_API ls_error ls_sprite_rotate(ls_context* ctx, ls_id sprite, float degrees,
                                   ls_id pivot);
LS_C_API ls_error ls_sprite_scale(ls_context* ctx, ls_id sprite, ls_vec2f factor,
                                  ls_id pivot);
LS_C_API ls_error ls_sprite_reset_transform(ls_context* ctx, ls_id sprite);

/* --------------------------------------------------------------- geometry -- */

LS_C_API ls_error ls_geometry_rect(ls_context* ctx, ls_id document, ls_vec2f origin,
                                   float width, float height, float corner_radius,
                                   ls_id* out_geometry);
LS_C_API ls_error ls_geometry_circle(ls_context* ctx, ls_id document, ls_vec2f center,
                                     float radius, ls_id* out_geometry);
LS_C_API ls_error ls_geometry_ellipse(ls_context* ctx, ls_id document, ls_vec2f center,
                                      float radius_x, float radius_y,
                                      ls_id* out_geometry);
LS_C_API ls_error ls_geometry_polyline(ls_context* ctx, ls_id document,
                                       const ls_vec2f* points, size_t point_count,
                                       int closed, ls_id* out_geometry);
LS_C_API ls_error ls_geometry_destroy(ls_context* ctx, ls_id geometry);

/* ---------------------------------------------------------------- regions -- */

LS_C_API ls_error ls_region_from_geometry(ls_context* ctx, ls_id geometry,
                                          ls_id* out_region);

/* Authored pixels. A closed same-colour loop seals its interior; an open stroke
 * encloses nothing. Colours are parallel to the points, one per pixel. */
LS_C_API ls_error ls_region_from_pixels(ls_context* ctx, ls_id document,
                                        const ls_vec2i* pixels, const ls_color* colors,
                                        size_t pixel_count, ls_id* out_region);

LS_C_API ls_error ls_region_add_pixels(ls_context* ctx, ls_id region,
                                       const ls_vec2i* pixels, size_t pixel_count);
LS_C_API ls_error ls_region_erase_pixels(ls_context* ctx, ls_id region,
                                         const ls_vec2i* pixels, size_t pixel_count);

/* Boolean maths. op is one of LS_REGION_*. */
#define LS_REGION_UNION     0
#define LS_REGION_SUBTRACT  1
#define LS_REGION_INTERSECT 2
#define LS_REGION_XOR       3
LS_C_API ls_error ls_region_combine(ls_context* ctx, ls_id a, ls_id b, int32_t op,
                                    ls_id* out_region);

/* The geometry a region was built from, or 0 when it has none. An application
 * offering editable shapes needs this to recognise one in a loaded document. */
LS_C_API ls_error ls_region_source_geometry(ls_context* ctx, ls_id region,
                                            ls_id* out_geometry);
LS_C_API ls_error ls_region_pixel_count(ls_context* ctx, ls_id region, uint64_t* out_count);
LS_C_API ls_error ls_region_bind_palette_role(ls_context* ctx, ls_id region, uint32_t role);
LS_C_API ls_error ls_region_destroy(ls_context* ctx, ls_id region);

/* -------------------------------------------------- palettes, ramps, tiles -- */

/* Colours are roles, not literals: a palette swap repaints artwork without
 * touching an operation. Roles are parallel to colours. */
LS_C_API ls_error ls_palette_create(ls_context* ctx, ls_id document, const char* name,
                                    const uint32_t* roles, const ls_color* colors,
                                    size_t entry_count, ls_id* out_palette);
LS_C_API ls_error ls_palette_set_color(ls_context* ctx, ls_id palette, uint32_t role,
                                       ls_color color);
LS_C_API ls_error ls_sprite_bind_palette(ls_context* ctx, ls_id sprite, ls_id palette);

/* Ramp stops: positions in [0,1], parallel to colours. */
LS_C_API ls_error ls_ramp_create(ls_context* ctx, ls_id document, const char* name,
                                 const float* positions, const ls_color* colors,
                                 size_t stop_count, int interpolate,
                                 ls_id* out_ramp);

/* One of the built-in threshold matrices. kind mirrors ls::DitherPatternKind. */
LS_C_API ls_error ls_dither_pattern_create(ls_context* ctx, ls_id document, int32_t kind,
                                           ls_id* out_pattern);

/* A supplied tile: a threshold rank per cell, row-major, width*height of them.
 * This is how an external dither pattern or a tileable texture gets in. */
/* colors may be NULL for a plain threshold screen, or width*height entries for
 * a tile that paints its own colours -- which is how a tileable texture fill
 * works. */
LS_C_API ls_error ls_pattern_create(ls_context* ctx, ls_id document, const char* name,
                                    const uint8_t* mask, const ls_color* colors,
                                    uint32_t width, uint32_t height,
                                    uint32_t levels, ls_id* out_pattern);

/* ------------------------------------------------------------- operations -- */

/* The catalogue, without hard-coding it. Index from 0 to ls_operation_type_count. */
LS_C_API size_t   ls_operation_type_count(void);
LS_C_API ls_error ls_operation_type_name(size_t index, char* buffer, size_t buffer_size,
                                         size_t* out_needed);

/* Create an operation by type name -- "FillSolidOp", "RotateOp", and so on --
 * then set its parameters by name. at_index of -1 appends. */
LS_C_API ls_error ls_operation_add(ls_context* ctx, ls_id layer, const char* type_name,
                                   int32_t at_index, ls_id* out_operation);
LS_C_API ls_error ls_operation_remove(ls_context* ctx, ls_id layer, ls_id operation);

/* What this operation can be driven by. The names are the same ones the save
 * file uses, because both walk one field table. */
LS_C_API ls_error ls_operation_parameter_count(ls_context* ctx, ls_id operation,
                                               size_t* out_count);
LS_C_API ls_error ls_operation_parameter_at(ls_context* ctx, ls_id operation, size_t index,
                                            char* name_buffer, size_t name_buffer_size,
                                            size_t* out_needed, int32_t* out_type);

/* Setters. The engine converts where a conversion is lossless and answers
 * LS_ERROR_OPERATION_TYPE_MISMATCH where it is not. */
LS_C_API ls_error ls_operation_set_float(ls_context* ctx, ls_id op, const char* name, float v);
LS_C_API ls_error ls_operation_set_int(ls_context* ctx, ls_id op, const char* name, int64_t v);
LS_C_API ls_error ls_operation_set_bool(ls_context* ctx, ls_id op, const char* name, int v);
LS_C_API ls_error ls_operation_set_id(ls_context* ctx, ls_id op, const char* name, ls_id v);
LS_C_API ls_error ls_operation_set_vec2(ls_context* ctx, ls_id op, const char* name, ls_vec2f v);
LS_C_API ls_error ls_operation_set_color(ls_context* ctx, ls_id op, const char* name, ls_color v);

LS_C_API ls_error ls_operation_get_float(ls_context* ctx, ls_id op, const char* name, float* out);
LS_C_API ls_error ls_operation_get_int(ls_context* ctx, ls_id op, const char* name, int64_t* out);
LS_C_API ls_error ls_operation_get_id(ls_context* ctx, ls_id op, const char* name, ls_id* out);
LS_C_API ls_error ls_operation_get_vec2(ls_context* ctx, ls_id op, const char* name, ls_vec2f* out);
LS_C_API ls_error ls_operation_get_color(ls_context* ctx, ls_id op, const char* name, ls_color* out);

/* ---------------------------------------------------------------- compile -- */

/* The first moment pixels exist. The returned raster is owned by the caller
 * until ls_raster_release. */
LS_C_API ls_error ls_compile_sprite(ls_context* ctx, ls_id sprite, const ls_profile* profile,
                                    ls_raster** out_raster);
LS_C_API ls_error ls_compile_layer(ls_context* ctx, ls_id layer, const ls_profile* profile,
                                   ls_raster** out_raster);

/* A sprite with everything attached to it, placed by the attachment chain. */
LS_C_API ls_error ls_compile_assembly(ls_context* ctx, ls_id root, const ls_profile* profile,
                                      ls_raster** out_raster);

LS_C_API uint32_t       ls_raster_width(const ls_raster* raster);
LS_C_API uint32_t       ls_raster_height(const ls_raster* raster);
/* Tightly packed RGBA8, row-major, width*height*4 bytes. Valid until release. */
LS_C_API const uint8_t* ls_raster_data(const ls_raster* raster);
LS_C_API size_t         ls_raster_size(const ls_raster* raster);
LS_C_API void           ls_raster_release(ls_raster* raster);

/* ---------------------------------------------------- pivots and sockets -- */

LS_C_API ls_error ls_pivot_create(ls_context* ctx, ls_id sprite, const char* name,
                                  ls_vec2f position, ls_id* out_pivot);
LS_C_API ls_error ls_socket_create(ls_context* ctx, ls_id sprite, const char* name,
                                   ls_vec2f position, float angle_degrees,
                                   ls_id* out_socket);
/* Hangs child off a socket, presenting one of its own pivots. A cycle is
 * refused rather than built. */
/* child_pivot may be 0. */
LS_C_API ls_error ls_sprite_attach(ls_context* ctx, ls_id child, ls_id socket,
                                   ls_id child_pivot);
LS_C_API ls_error ls_sprite_detach(ls_context* ctx, ls_id child);

/* -------------------------------------------------------------- documents -- */

/* Deterministic UTF-8 JSON: the same state saves to byte-identical output. */
LS_C_API ls_error ls_document_serialize(ls_context* ctx, ls_id document, ls_blob** out_blob);
LS_C_API ls_error ls_document_deserialize(ls_context* ctx, const uint8_t* data, size_t size,
                                          ls_id* out_document);

LS_C_API const uint8_t* ls_blob_data(const ls_blob* blob);
LS_C_API size_t         ls_blob_size(const ls_blob* blob);
LS_C_API void           ls_blob_release(ls_blob* blob);

/* --------------------------------------------------------------- packages -- */

/* A package is the document plus whole files an app keeps with it. The engine
 * owns the container; entries belong to whoever namespaced them.
 *
 * Entry names are validated, never repaired: a sanitised name can collide with
 * a real one. A name must carry a namespace segment ("fast/thumbnail.png") and
 * must not contain "..", a leading slash, a backslash, a drive letter, an empty
 * segment or an embedded null. */
LS_C_API int ls_package_entry_name_valid(const char* name);

LS_C_API ls_package* ls_package_builder_create(void);
LS_C_API ls_error    ls_package_builder_add(ls_package* builder, const char* name,
                                            const char* content_type,
                                            const uint8_t* data, size_t size);
LS_C_API void        ls_package_builder_release(ls_package* builder);

LS_C_API ls_error ls_package_write(ls_context* ctx, ls_id document,
                                   const ls_package* builder, ls_blob** out_blob);

/* Reading gives back a document and, through the reader handle, the entries the
 * package carried -- including entries this app does not understand, which must
 * be written back out if the file is to be shared. */
LS_C_API ls_error ls_package_read(ls_context* ctx, const uint8_t* data, size_t size,
                                  ls_id* out_document, ls_package** out_reader);
LS_C_API size_t   ls_package_entry_count(const ls_package* reader);
LS_C_API ls_error ls_package_entry_at(const ls_package* reader, size_t index,
                                      char* name_buffer, size_t name_buffer_size,
                                      size_t* out_name_needed,
                                      const uint8_t** out_data, size_t* out_size);

/* ------------------------------------------------------ metadata and undo -- */

/* Namespaced, opaque to the engine, carried through save, load and undo. Keys
 * must be namespaced so two apps annotating one document cannot collide.
 * Values are bytes the engine never parses: treat what you read back from a
 * received file as untrusted. */
LS_C_API ls_error ls_metadata_set(ls_context* ctx, ls_id entity, const char* key,
                                  const char* value);
LS_C_API ls_error ls_metadata_get(ls_context* ctx, ls_id entity, const char* key,
                                  char* buffer, size_t buffer_size, size_t* out_needed);

/* Undo. The snapshot restores every id exactly, so handles the interface is
 * holding stay valid. It is an in-memory capture: cheap enough per action. */
LS_C_API ls_error ls_snapshot_create(ls_context* ctx, ls_id document,
                                     ls_snapshot** out_snapshot);
LS_C_API ls_error ls_snapshot_restore(ls_context* ctx, ls_id document,
                                      const ls_snapshot* snapshot);
LS_C_API void     ls_snapshot_release(ls_snapshot* snapshot);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIVESPRITE_C_H */
