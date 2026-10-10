#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The UI kit's font, port/ui/fonts/lexend.sdf (tools/render-sdf-font.cpp says what is in it), from
# tools/fonts/Lexend[wght].ttf. The file is committed: the build needs no font tools. Run it again
# after changing the font or the code points baked. Needs a C++ compiler and FreeType's and zlib's
# headers (libfreetype-dev, zlib1g-dev).

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
c++ -std=c++20 -O2 "$root/tools/render-sdf-font.cpp" $(pkg-config --cflags --libs freetype2 zlib) -o "$work/render-sdf-font"
mkdir -p "$root/port/ui/fonts"
"$work/render-sdf-font" "$root/tools/fonts/Lexend[wght].ttf" "$root/port/ui/fonts/lexend.sdf"
