#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# ReShade as the app runs it, on a PC: builds tools/reshade-check (build/reshade-check) from the
# app's own ReShade code (port/cemu/ReShadeEffects.cpp and ReShadeRuntime.cpp) and ReShade's
# compiler (.deps/reshade), then runs it on a folder laid out as /data/ps5cemu/reshade is:
#
#   tools/reshade-check.sh FOLDER                          compile what the preset turns on
#   tools/reshade-check.sh FOLDER --run IN.png OUT.png     ... and draw it on IN.png (- for a test
#                                                          pattern) with the PC's Vulkan driver
#   options: --title 00050000101C9500 (a game's preset), --size 3840x2160, --frames N
#
# Needs `make deps` (for .deps/reshade), clang-18, and for --run a Vulkan driver and its headers
# (on Ubuntu: libvulkan-dev and mesa-vulkan-drivers, whose lavapipe needs no GPU). With
# spirv-tools and vulkan-validationlayers installed, the SPIR-V and the drawing are checked too.

set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
reshade=$PS5CEMU_ROOT/.deps/reshade
out=$PS5CEMU_BUILD/reshade-check
[[ -f $reshade/source/effect_codegen_spirv.cpp ]] || { echo "ReShade's compiler is missing: run make deps" >&2; exit 2; }
bash "$PS5CEMU_ROOT/tools/reshade-patches.sh" apply
mkdir -p "$out"

includes=(-I "$PS5CEMU_ROOT/port/cemu" -I "$reshade/source" -I "$reshade/deps/spirv/include/spirv/unified1"
    -I "$reshade/deps/stb" -I "$reshade/deps/stb_image")
sources=("$PS5CEMU_ROOT/port/cemu/ReShadeEffects.cpp" "$PS5CEMU_ROOT/port/cemu/ReShadeRuntime.cpp"
    "$PS5CEMU_ROOT/tools/reshade-check/reshade-check.cpp")
for file in effect_codegen_spirv effect_expression effect_lexer effect_parser_exp effect_parser_stmt effect_preprocessor effect_symbol_table; do
    sources+=("$reshade/source/$file.cpp")
done

# rebuilt when any input is newer than the tool
binary=$out/reshade-check
if [[ ! -x $binary ]] || [[ -n $(find "${sources[@]}" "$PS5CEMU_ROOT/port/cemu/ReShadeEffects.h" "$PS5CEMU_ROOT/port/cemu/ReShadeRuntime.h" -newer "$binary" 2>/dev/null) ]]; then
    objects=()
    jobs=()
    for source in "${sources[@]}"; do
        object=$out/$(basename "${source%.cpp}").o
        objects+=("$object")
        flags=(-std=c++20 -O2 -g "${includes[@]}")
        [[ $source == "$reshade"/* ]] && flags+=(-w) # third-party code, compiled as its authors do
        [[ $source == "$reshade"/* ]] || flags+=(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
        clang++-18 "${flags[@]}" -c "$source" -o "$object" &
        jobs+=($!)
    done
    for job in "${jobs[@]}"; do
        wait "$job"
    done
    clang++-18 -o "$binary" "${objects[@]}" -lvulkan -lpthread
fi
exec "$binary" "$@"
