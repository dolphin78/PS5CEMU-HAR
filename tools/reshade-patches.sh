#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The port's changes to ReShade's effect compiler, kept as patches/reshade/*.patch against the
# pinned commit (the same way as tools/cemu-patches.sh keeps Cemu's).
#
#   tools/reshade-patches.sh apply    put them on a branch named ps5 in .deps/reshade (once)
#   tools/reshade-patches.sh export   write the ps5 branch's commits back to patches/reshade
#
# To change ReShade: edit .deps/reshade on the ps5 branch, commit, then export.

set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
reshade=$PS5CEMU_ROOT/.deps/reshade
patches=$PS5CEMU_ROOT/patches/reshade
pin=$(python3 -c 'import json,sys; print(next(i["commit"] for i in json.load(open(sys.argv[1]))["items"] if i["name"] == "reshade"))' \
    "$PS5CEMU_ROOT/tools/deps.json")
identity=(-c user.name=ps5cemu -c user.email=ps5cemu@localhost)

case ${1:-} in
apply)
    if git -C "$reshade" rev-parse -q --verify refs/heads/ps5 >/dev/null; then
        exit 0 # already there (and possibly being worked on)
    fi
    git -C "$reshade" checkout -q -b ps5 "$pin"
    shopt -s nullglob
    files=("$patches"/*.patch)
    if ((${#files[@]})); then
        git "${identity[@]}" -C "$reshade" am -q --keep-cr "${files[@]}"
    fi
    echo "==> [reshade] applied ${#files[@]} patches on branch ps5"
    ;;
export)
    [[ -z $(git -C "$reshade" status --porcelain --untracked-files=no) ]] ||
        { echo "commit the changes in .deps/reshade first" >&2; exit 1; }
    rm -f "$patches"/*.patch
    mkdir -p "$patches"
    git -C "$reshade" format-patch -q --no-signature --zero-commit --no-numbered -o "$patches" "$pin..ps5"
    ls "$patches"
    ;;
*)
    echo "usage: tools/reshade-patches.sh apply|export" >&2
    exit 2
    ;;
esac
