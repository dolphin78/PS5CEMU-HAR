#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The UI kit's shaders (port/ui/shaders/*.vert, *.frag) as SPIR-V in port/ui/shaders.h, which is
# committed: the build needs no shader compiler. Run it again after changing a shader. Needs
# glslangValidator (glslang-tools).

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
out=$root/port/ui/shaders.h
{
    echo "// SPDX-License-Identifier: GPL-3.0-or-later"
    echo "// Written by tools/render-shaders.sh from port/ui/shaders: the UI kit's shaders as SPIR-V."
    echo
    echo "#pragma once"
    echo
    echo "#include <cstdint>"
    echo
    echo "namespace ui::shaders"
    echo "{"
} >"$out"
for shader in "$root"/port/ui/shaders/*.vert "$root"/port/ui/shaders/*.frag; do
    name=$(basename "$shader" | tr '.' '_')
    glslangValidator -V --target-env vulkan1.0 --vn "k_$name" -o "$work/$name.h" "$shader" >/dev/null
    # glslang's header, without its comment and with the array in the namespace
    grep -v '^\s*//' "$work/$name.h" | sed -e 's/^const uint32_t/\tinline constexpr uint32_t/' -e 's/^\([0-9x]\)/\t\t\1/' -e 's/^};/\t};/' -e '/^\s*$/d' -e '/#pragma once/d' >>"$out"
    echo >>"$out"
done
echo "}" >>"$out"
echo "port/ui/shaders.h: $(grep -c 'inline constexpr' "$out") shaders"
