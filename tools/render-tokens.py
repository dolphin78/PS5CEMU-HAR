#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The UI kit's tokens (docs/UI-REDESIGN.md, section 7): port/ui/tokens.h from port/ui/tokens.json,
which is the one place a colour, a size or a duration is set. Also checks that the mockups'
tokens.css (docs/ui-redesign/mockups) names the same colours, and says where it does not.

    tools/render-tokens.py
"""

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / "port/ui/tokens.json"
OUT = ROOT / "port/ui/tokens.h"
CSS = ROOT / "docs/ui-redesign/mockups/tokens.css"

WEIGHTS = {400: "Regular", 500: "Medium", 600: "SemiBold", 700: "Bold"}


def camel(name):
    return "k" + "".join(part.capitalize() for part in re.split(r"[-_]", name))


def rgba(value):
    """'#rrggbb' or '#rrggbbaa' as the kit's colour: RGBA, R in the lowest byte"""
    digits = value.lstrip("#")
    if len(digits) == 6:
        digits += "ff"
    r, g, b, a = (int(digits[i:i + 2], 16) for i in range(0, 8, 2))
    return r | g << 8 | b << 16 | a << 24


def css_colours(text):
    colours = {}
    for name, value in re.findall(r"--([a-z0-9-]+):\s*([^;]+);", text):
        value = value.strip()
        if value.startswith("#"):
            colours[name] = rgba(value)
        else:
            m = re.match(r"rgba\(\s*(\d+),\s*(\d+),\s*(\d+),\s*([0-9.]+)\s*\)", value)
            if m:
                r, g, b = (int(m.group(i)) for i in range(1, 4))
                colours[name] = r | g << 8 | b << 16 | round(float(m.group(4)) * 255) << 24
    return colours


def main():
    tokens = json.loads(SOURCE.read_text())
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// Written by tools/render-tokens.py from port/ui/tokens.json: the UI's colours, sizes, type and",
        "// motion (docs/UI-REDESIGN.md, section 7). Colours are RGBA, R in the lowest byte.",
        "",
        "#pragma once",
        "",
        '#include "text.h"',
        "",
        "#include <cstdint>",
        "",
        "namespace ui::tokens",
        "{",
    ]
    for name, value in tokens["colour"].items():
        lines.append(f"\tinline constexpr uint32_t {camel(name)} = 0x{rgba(value):08x}; // {value}")
    lines.append("")
    for group in ("space", "radius"):
        for name, value in tokens[group].items():
            prefix = "" if group == "space" else "Radius"
            lines.append(f"\tinline constexpr float k{prefix}{camel(name)[1:]} = {float(value)}f;")
        lines.append("")
    for name, style in tokens["type"].items():
        weight = WEIGHTS[style["weight"]]
        tracking = float(style.get("tracking", 0))
        upper = "true" if style.get("upper") else "false"
        lines.append(f"\tinline constexpr TextStyle k{camel(name)[1:]}Style{{{float(style['size'])}f, Weight::{weight}, {float(style['line'])}f, "
                     f"{tracking}f, {upper}, false}};")
    lines.append("")
    for name, value in tokens["motion"].items():
        if name.endswith("omega"):
            lines.append(f"\tinline constexpr float k{camel(name)[1:]} = {float(value)}f;")
        else:
            lines.append(f"\tinline constexpr float k{camel(name)[1:]}Seconds = {value / 1000:.3f}f;")
    lines.append("}")
    OUT.write_text("\n".join(lines) + "\n")
    print(f"{OUT.relative_to(ROOT)}: {len(tokens['colour'])} colours, {len(tokens['type'])} type styles")

    if CSS.exists():
        css = css_colours(CSS.read_text())
        differ = []
        for name, value in tokens["colour"].items():
            if name in css and abs((css[name] >> 24) - (rgba(value) >> 24)) > 1 or name in css and (css[name] & 0xffffff) != (rgba(value) & 0xffffff):
                differ.append(name)
        if differ:
            print(f"tokens.css differs from tokens.json in: {', '.join(differ)}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
