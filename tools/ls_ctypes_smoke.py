#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 the LiveSprite authors
"""Loads the LiveSprite C ABI from Python and compiles a sprite with it.

The C header exists so that something which is not C++ can drive the engine.
This script is where that claim is tested: it uses nothing but ctypes -- no
compiler, no build step, no bindings package -- so if it works here it works
from C#, Rust, Node, or anything else with a foreign function interface.

    python3 tools/ls_ctypes_smoke.py build/liblivesprite_c.so

It also serves as the shortest worked example of the C API, which is why it
verifies a real property rather than just checking for a zero return code: a
12x12 square turned 45 degrees must come back with exactly 144 opaque pixels.
Nothing degrades, even across a language boundary.
"""

import ctypes
import sys


class Vec2f(ctypes.Structure):
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float)]


class Vec2i(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int32), ("y", ctypes.c_int32)]


class Color(ctypes.Structure):
    _fields_ = [("r", ctypes.c_uint8), ("g", ctypes.c_uint8),
                ("b", ctypes.c_uint8), ("a", ctypes.c_uint8)]


class Profile(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int32),
                ("width", ctypes.c_uint32),
                ("height", ctypes.c_uint32),
                ("palette_policy", ctypes.c_int32),
                ("export_origin", Vec2i)]


LS_OK = 0
LS_PROFILE_EXPORT = 1
LS_PALETTE_UNCONSTRAINED = 0


def load(path):
    dll = ctypes.CDLL(path)

    # ctypes guesses int for anything undeclared, which silently truncates
    # pointers on 64-bit. Everything returning a pointer, a size or a struct by
    # value has to be declared; the rest is int-in, int-out and works as is.
    dll.ls_context_create.restype = ctypes.c_void_p
    dll.ls_context_destroy.argtypes = [ctypes.c_void_p]
    dll.ls_error_string.restype = ctypes.c_char_p
    dll.ls_engine_version.restype = ctypes.c_uint32
    dll.ls_operation_type_count.restype = ctypes.c_size_t

    dll.ls_raster_data.restype = ctypes.POINTER(ctypes.c_uint8)
    dll.ls_raster_data.argtypes = [ctypes.c_void_p]
    dll.ls_raster_size.restype = ctypes.c_size_t
    dll.ls_raster_size.argtypes = [ctypes.c_void_p]
    dll.ls_raster_width.restype = ctypes.c_uint32
    dll.ls_raster_width.argtypes = [ctypes.c_void_p]
    dll.ls_raster_height.restype = ctypes.c_uint32
    dll.ls_raster_height.argtypes = [ctypes.c_void_p]
    dll.ls_raster_release.argtypes = [ctypes.c_void_p]

    dll.ls_geometry_rect.argtypes = [ctypes.c_void_p, ctypes.c_uint64, Vec2f,
                                     ctypes.c_float, ctypes.c_float, ctypes.c_float,
                                     ctypes.POINTER(ctypes.c_uint64)]
    dll.ls_operation_set_color.argtypes = [ctypes.c_void_p, ctypes.c_uint64,
                                           ctypes.c_char_p, Color]
    dll.ls_operation_set_float.argtypes = [ctypes.c_void_p, ctypes.c_uint64,
                                           ctypes.c_char_p, ctypes.c_float]
    dll.ls_operation_set_vec2.argtypes = [ctypes.c_void_p, ctypes.c_uint64,
                                          ctypes.c_char_p, Vec2f]
    dll.ls_compile_sprite.argtypes = [ctypes.c_void_p, ctypes.c_uint64,
                                      ctypes.POINTER(Profile),
                                      ctypes.POINTER(ctypes.c_void_p)]
    return dll


def main(argv):
    if len(argv) != 2:
        print("usage: ls_ctypes_smoke.py <path to livesprite_c library>")
        return 2

    dll = load(argv[1])

    def check(error, what):
        if error != LS_OK:
            raise SystemExit("%s failed: %s" % (what, dll.ls_error_string(error).decode()))

    version = dll.ls_engine_version()
    print("engine version   : %d.%d.%d" % ((version >> 16) & 0xFF,
                                           (version >> 8) & 0xFF,
                                           version & 0xFF))
    print("operation types  : %d" % dll.ls_operation_type_count())

    ctx = ctypes.c_void_p(dll.ls_context_create())
    if not ctx:
        raise SystemExit("could not create a context")

    doc, sprite, layer = ctypes.c_uint64(), ctypes.c_uint64(), ctypes.c_uint64()
    rect, region, fill, turn = (ctypes.c_uint64() for _ in range(4))

    check(dll.ls_document_create(ctx, b"from-python", 24, 24, ctypes.byref(doc)), "document")
    check(dll.ls_sprite_create(ctx, doc, ctypes.byref(sprite)), "sprite")
    check(dll.ls_layer_create(ctx, sprite, b"main", ctypes.byref(layer)), "layer")

    check(dll.ls_geometry_rect(ctx, doc, Vec2f(6.0, 6.0), 12.0, 12.0, 0.0,
                               ctypes.byref(rect)), "rect")
    check(dll.ls_region_from_geometry(ctx, rect, ctypes.byref(region)), "region")

    # An operation is named, then configured by parameter name -- the same names
    # the save file uses. There is no FillSolidOp struct in C at all.
    check(dll.ls_operation_add(ctx, layer, b"FillSolidOp", -1, ctypes.byref(fill)), "add fill")
    check(dll.ls_operation_set_id(ctx, fill, b"targetRegion", region), "targetRegion")
    check(dll.ls_operation_set_color(ctx, fill, b"fallbackColor", Color(255, 128, 0, 255)),
          "fallbackColor")

    check(dll.ls_operation_add(ctx, layer, b"RotateOp", -1, ctypes.byref(turn)), "add rotate")
    check(dll.ls_operation_set_id(ctx, turn, b"targetLayer", layer), "targetLayer")
    check(dll.ls_operation_set_float(ctx, turn, b"angleDegrees", 45.0), "angleDegrees")
    check(dll.ls_operation_set_vec2(ctx, turn, b"pivotFallback", Vec2f(12.0, 12.0)), "pivot")

    raster = ctypes.c_void_p()
    profile = Profile(LS_PROFILE_EXPORT, 24, 24, LS_PALETTE_UNCONSTRAINED, Vec2i(0, 0))
    check(dll.ls_compile_sprite(ctx, sprite, ctypes.byref(profile), ctypes.byref(raster)),
          "compile")

    width = dll.ls_raster_width(raster)
    height = dll.ls_raster_height(raster)
    size = dll.ls_raster_size(raster)
    data = dll.ls_raster_data(raster)
    pixels = bytes(data[i] for i in range(size))

    opaque = sum(1 for i in range(3, size, 4) if pixels[i])
    print("compiled         : %dx%d, %d opaque pixels\n" % (width, height, opaque))
    for y in range(height):
        print("  " + "".join("#" if pixels[(y * width + x) * 4 + 3] else "."
                             for x in range(width)))

    dll.ls_raster_release(raster)
    dll.ls_context_destroy(ctx)

    # The claim under test: a 12x12 square is 144 pixels, and a rotation resolves
    # from source truth rather than resampling, so the count survives the turn.
    if opaque != 144:
        print("\nFAIL: expected 144 opaque pixels after the rotation, got %d" % opaque)
        return 1

    print("\nOK: the engine was driven entirely from Python through the C ABI.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
