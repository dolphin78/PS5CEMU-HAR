#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the launcher's layouts (RmlUi documents) and its stylesheets, with nothing but the standard
library.

    render-layout.py OUTPUT_UI

  start.rml          the start screen: PS5CEMU-HAR's name, and a card for each emulator
  main.rml           Cemu's launcher (the Wii U), over the Wii U Homebrew Launcher's bubbles
  azahar.rml         Azahar's (the 3DS), over the 3DS one's waves, in gold
  styles/har.rcss, styles/har-3ds.rcss
                     port/frontend/ui/har.rcss with each side's colours (THEMES)

Each side has three screens behind tabs (Home, Library, Settings), a game's page (Details), and the
pages they open: a game's graphic packs, a player's controls and buttons, the file browser for
games and installs, Artic Base, a dropdown's choices and a setting's longer help. The pictures are
the app's own: the glyphs (tools/render-glyphs.py, white, tinted by the stylesheet), the icons
(tools/render-icons.py) and the fonts (tools/render-fonts.py). launcher.cpp fills them in by the
element ids written here.
"""

import os
import sys

THEMES = {
    # name: (suffix, colours)
    "wiiu": ("", {
        "ground": "rgba(4, 10, 22, 150)", "text": "#f3f7ff", "muted": "#a9bcd6", "dim": "#7f93ae",
        "accent": "#5aa9ff", "accent-strong": "#2f7fe8", "on-accent": "#04101f", "panel": "rgba(7, 16, 31, 225)",
        "card": "rgba(255, 255, 255, 16)", "card-edge": "rgba(255, 255, 255, 30)", "focus-fill": "rgba(90, 169, 255, 40)",
        "glow": "rgba(90, 169, 255, 60)", "shade": "rgba(2, 6, 14, 200)", "line": "rgba(255, 255, 255, 30)",
        "tab-text": "#c9d4e3", "hero-glow": "rgba(90, 169, 255, 26)", "placeholder": "#16304f",
        # the tiles: the recent games' and the library's (box art 5:7)
        "shelf-w": "160px", "shelf-h": "160px", "cell-w": "212px", "cell-h": "290px",
    }),
    "3ds": ("-3ds", {
        "ground": "rgba(16, 10, 2, 150)", "text": "#fff7e8", "muted": "#d8c6a3", "dim": "#a8977a",
        "accent": "#f4b63f", "accent-strong": "#d9961b", "on-accent": "#1a1204", "panel": "rgba(26, 18, 6, 225)",
        "card": "rgba(255, 255, 255, 16)", "card-edge": "rgba(255, 255, 255, 30)", "focus-fill": "rgba(244, 182, 63, 40)",
        "glow": "rgba(244, 182, 63, 60)", "shade": "rgba(14, 9, 2, 200)", "line": "rgba(255, 255, 255, 30)",
        "tab-text": "#e6d8bc", "hero-glow": "rgba(244, 182, 63, 26)", "placeholder": "#3a2a0c",
        # 3DS box art is wider than tall
        "shelf-w": "160px", "shelf-h": "160px", "cell-w": "212px", "cell-h": "196px",
    }),
}

WIIU = {
    "theme": "wiiu", "name": "Wii U", "cover": "icons/ps5cemu.tga", "packs": True, "artic": False,
    # the library's grid: columns, rows, and a tile's size (box art is 5:7 on the Wii U)
    "grid": (7, 2, 212, 290),
    "settings": [
        ("video", "Video", "video"), ("audio", "Audio", "audio"), ("controls", "Controls", "controls"), ("stream", "Remote touch screen", "video"),
        ("usb", "USB devices", "usb"), ("files", "Game files", "folder"), ("installs", "Updates and DLC", "install"),
        ("online", "Online", "online"),
        ("diagnostics", "Diagnostics", "diagnostics"), ("about", "About", "about"),
    ],
}
N3DS = {
    "theme": "3ds", "name": "Nintendo 3DS", "cover": "icons/azahar.tga", "packs": False, "artic": True,
    # 3DS box art is wider than tall
    "grid": (7, 3, 212, 196),
    "settings": [
        ("video", "Video", "video"), ("audio", "Audio", "audio"), ("controls", "Controls", "controls"), ("stream", "Remote touch screen", "video"),
        ("borders", "Borders", "borders"), ("system", "System", "system"), ("files", "Game files", "folder"),
        ("installs", "Install CIA files", "install"), ("online", "Online", "online"),
        ("diagnostics", "Diagnostics", "diagnostics"), ("about", "About", "about"),
    ],
}
SIDES = {"main.rml": WIIU, "azahar.rml": N3DS}

LIST_ROWS = 7     # a list page's rows
FILE_ROWS = 6
PRESET_ROWS = 4
PICKER_ROWS = 6
SETTING_ROWS = 7  # a settings category's rows
HINTS = 4


def head(title, theme):
    suffix = THEMES[theme][0]
    return f'''<rml>
  <!-- PS5CEMU-HAR's launcher (tools/render-layout.py): its own layout, stylesheet (port/frontend/ui/
       har.rcss) and pictures. The background is drawn under the page (bubbles.cpp, wave.cpp). -->
  <head>
    <title>{title}</title>
    <link type="text/rcss" href="styles/har{suffix}.rcss" />
  </head>
  <body>
    <div id="app">'''


TAIL = '''    </div>
  </body>
</rml>
'''


def glyph(name, size=30, cls="glyph"):
    return f'<img class="{cls}" src="glyphs/{name}-{size}.tga" width="{size}" height="{size}" alt=""/>'


def update_sheet():
    """The app's own update: a newer release, asked about; then how its install goes
    (launcher.cpp's UpdatePrompt)."""
    pills = "".join(f'<span id="update-{i}" class="pill{" primary" if i == 0 else ""}"><span id="update-label-{i}"></span></span>' for i in range(2))
    return f'''      <div id="update" class="overlay">
        <div class="sheet update-sheet">
          <span class="kicker">UPDATE</span>
          <span id="update-title" class="sheet-title"></span>
          <div id="update-text" class="sheet-text"></div>
          <div id="update-bar"><span id="update-fill"></span></div>
          <div id="update-buttons">{pills}</div>
        </div>
      </div>'''


def hints():
    """The hints along the bottom right: launcher.cpp sets each one's glyph and words."""
    slots = "".join(f'<span id="hint-{i}" class="hint"><img id="hint-img-{i}" class="glyph" src="glyphs/cross-30.tga" width="30" height="30" alt=""/>'
                    f'<span id="hint-text-{i}" class="hint-text"></span></span>' for i in range(HINTS))
    return f'      <div id="hints">{slots}</div>'


def rows(prefix, count, inner, cls="row"):
    return "\n".join(f'            <span id="{prefix}-row-{i}" class="{cls}">{inner(i)}</span>' for i in range(count))


def page(id_, title, title_id, copy_id, body):
    """A page over the screens: a title, then what it has (a list and a panel beside it)."""
    title_attr = f'id="{title_id}" ' if title_id else ''
    copy_attr = f'id="{copy_id}" ' if copy_id else ''
    return f'''      <div id="{id_}" class="page">
        <span {title_attr}class="page-title">{title}</span><span {copy_attr}class="page-copy"></span>
{body}
      </div>'''


def side(S):
    columns, grid_rows, w, h = S["grid"]
    out = [head(f"PS5CEMU-HAR: {S['name']}", S["theme"])]
    add = out.append

    # the bar along the top: the side, its tabs, the time
    add(f'''      <div id="topbar">
        <span id="side-mark">{glyph(S["theme"], 36, "mark-glyph")}</span><span id="side-name">{S["name"]}</span>
        <div id="tabs"><span id="tab-0" class="tab">Home</span><span id="tab-1" class="tab">Library</span><span id="tab-2" class="tab">Settings</span></div>
        <span id="menu-clock">--:--</span>
      </div>''')

    # Home: the last game large, with its buttons and box art, and the recent ones below
    buttons = [("Play", "play"), ("Details", None)] + ([("Artic Base", None)] if S["artic"] else [])
    pills = "".join(f'<span id="hero-{i}" class="pill{" primary" if i == 0 else ""}">'
                    f'{glyph(icon, 24, "pill-glyph") if icon else ""}<span id="hero-label-{i}">{label}</span></span>'
                    for i, (label, icon) in enumerate(buttons))
    shelf = "".join(f'<span id="shelf-{i}" class="tile shelf-tile"><img id="shelf-img-{i}" class="tile-img" src="{S["cover"]}" width="150" height="150" alt=""/>'
                    f'<span id="shelf-text-{i}" class="tile-text"></span><span class="ring"></span></span>' for i in range(5))
    add(f'''      <div id="home" class="screen">
        <span id="hero-glow"></span>
        <div id="hero-text">
          <span id="hero-kicker">CONTINUE</span>
          <span id="hero-title"></span>
          <div id="hero-meta"><span id="hero-facts"></span><span id="hero-status" class="chip"></span></div>
        </div>
        <div id="hero-buttons">{pills}</div>
        <span id="hero-card" class="cover-card"></span>
        <img id="hero-cover" src="{S["cover"]}" width="400" height="400" alt=""/>
        <span id="home-notice"></span>
        <span id="shelf-label">Recent</span>
        <div id="shelf">{shelf}<span id="shelf-all" class="tile shelf-tile all"><span class="all-text">All games</span><span class="ring"></span></span></div>
        <span id="shelf-name"></span><span id="shelf-meta"></span>
      </div>''')

    # Library: every game's box art in a grid, the focused one's name below
    cells = "".join(f'<span id="cell-{i}" class="tile cell"><img id="cell-img-{i}" class="tile-img" src="{S["cover"]}" width="{w}" height="{h}" alt=""/>'
                    f'<span id="cell-text-{i}" class="tile-text"></span><span class="ring"></span></span>' for i in range(columns * grid_rows))
    add(f'''      <div id="library" class="screen">
        <div id="library-head"><span id="library-title">Library</span><span id="library-count"></span></div>
        <div id="grid">{cells}</div>
        <span id="library-empty"></span>
        <span id="grid-scroll"><span id="grid-thumb"></span></span>
        <div id="lib-strip"><span id="lib-name"></span><span id="lib-meta"></span><span id="lib-status" class="chip"></span></div>
      </div>''')

    # Details: a game's page, from GameTDB (app/gameinfo.h)
    facts = "".join(f'<div id="fact-{i}" class="fact"><span id="fact-label-{i}" class="fact-label"></span><span id="fact-value-{i}" class="fact-value"></span></div>'
                    for i in range(6))
    chips = "".join(f'<span id="chip-{i}" class="chip"></span>' for i in range(4))
    actions = "".join(f'<span id="action-{i}" class="pill{" primary" if i == 0 else ""}">{glyph("play", 24, "pill-glyph") if i == 0 else ""}'
                      f'<span id="action-label-{i}"></span></span>' for i in range(4))
    add(f'''      <div id="details" class="screen">
        <span id="details-glow"></span>
        <span id="details-card" class="cover-card"></span>
        <img id="details-cover" src="{S["cover"]}" width="520" height="520" alt=""/><span id="details-cover-text"></span>
        <div id="details-info">
          <span id="details-kicker"></span>
          <span id="details-title"></span>
          <div id="details-chips">{chips}</div>
          <div id="facts">{facts}</div>
        </div>
        <div id="synopsis-box"><div id="synopsis"></div></div>
        <div id="details-actions">{actions}</div>
      </div>''')

    # Settings: the categories down the left, the focused one's settings on the right
    rail = "".join(f'<span id="rail-{i}" class="rail-item">{glyph(icon, 36, "rail-glyph")}<span class="rail-label">{label}</span></span>'
                   for i, (_, label, icon) in enumerate(S["settings"]))
    srows = "".join(f'<span id="srow-{i}" class="srow"><span id="srow-label-{i}" class="srow-label"></span>'
                    f'<span id="srow-value-{i}" class="srow-value"></span><span id="srow-desc-{i}" class="srow-desc"></span>'
                    + ('<span id="stream-url" class="srow-url"></span><img id="stream-qr"/>' if i == 0 else "") + "</span>"
                    for i in range(SETTING_ROWS))
    add(f'''      <div id="settings" class="screen">
        <div id="rail">{rail}</div>
        <span id="panel-title"></span>
        <div id="srows">{srows}</div>
        <div id="panel-text"></div>
      </div>''')

    # the pages the screens open, over them
    if S["packs"]:
        add(page("packs-dialog", "Graphic packs", None, "packs-game", f'''        <div class="list-panel">
          <div class="list">
{rows("pack", LIST_ROWS, lambda i: f'<span id="pack-name-{i}" class="row-label"></span><span id="pack-state-{i}" class="row-value"></span>')}
          </div>
          <span id="packs-empty" class="empty">No graphic packs for this game.</span>
          <span id="packs-position" class="position"></span>
        </div>
        <div class="aside">
          <span id="pack-detail-kicker" class="kicker"></span>
          <span id="pack-detail-title" class="aside-title"></span>
          <span id="pack-detail-description" class="aside-text pack-text"></span>
          <span id="presets-kicker" class="kicker presets-kicker"></span>
          <span id="presets-empty" class="empty presets-empty">On or off only.</span>
          <div id="presets" class="list">
{rows("preset", PRESET_ROWS, lambda i: f'<span id="preset-label-{i}" class="row-label"></span><span id="preset-value-{i}" class="row-value"></span>')}
          </div>
          <span id="presets-position" class="position"></span>
        </div>'''))

    add(page("player-dialog", "Player 1", "player-title", "player-copy", f'''        <div class="list-panel">
          <div class="list">
{rows("player", LIST_ROWS, lambda i: f'<span id="player-name-{i}" class="row-label"></span><span id="player-value-{i}" class="row-value"></span>')}
          </div>
        </div>
        <div class="aside">
          <span id="player-detail-title" class="aside-title"></span>
          <span id="player-help" class="aside-text"></span>
        </div>'''))

    add(page("mapping-dialog", "Buttons", None, "mapping-copy", f'''        <div class="list-panel">
          <span id="mapping-kicker" class="kicker"></span>
          <div class="list">
{rows("map", LIST_ROWS, lambda i: f'<span id="map-name-{i}" class="row-label"></span><span id="map-value-{i}" class="row-value"></span>')}
          </div>
          <span id="mapping-position" class="position"></span>
        </div>
        <div class="aside">
          <span id="map-title" class="aside-title"></span>
          <span class="kicker">DUALSENSE</span><span id="map-input" class="aside-big"></span>
          <span id="map-message" class="aside-text"></span>
        </div>'''))

    add(page("files-dialog", "Game files", "files-title", "files-copy", f'''        <div class="list-panel">
          <span id="files-path" class="kicker"></span>
          <div class="list">
{rows("files", FILE_ROWS, lambda i: f'{glyph("folder", 24, "row-glyph")}<span id="files-name-{i}" class="row-label"></span><span id="files-meta-{i}" class="row-value"></span>')}
          </div>
          <span id="files-empty" class="empty">No folders here.</span>
          <span id="files-position" class="position"></span>
        </div>
        <div class="aside">
          <span id="files-kicker" class="kicker"></span>
          <span id="files-current" class="aside-title"></span>
          <div class="facts-small">
            <span id="files-label-0" class="fact-label"></span><span id="files-value-0" class="fact-value"></span>
            <span id="files-label-1" class="fact-label"></span><span id="files-value-1" class="fact-value"></span>
            <span id="files-label-2" class="fact-label"></span><span id="files-value-2" class="fact-value"></span>
          </div>
          <span id="files-message" class="aside-text"></span>
        </div>'''))

    if S["artic"]:
        add(page("artic-dialog", "Artic Base", None, None, f'''        <div class="list-panel">
          <div class="list">
{rows("artic", 4, lambda i: f'<span id="artic-name-{i}" class="row-label"></span><span id="artic-value-{i}" class="row-value"></span>')}
          </div>
        </div>
        <div class="aside">
          <span id="artic-detail-title" class="aside-title"></span>
          <span id="artic-help" class="aside-text"></span>
        </div>'''))

    # a dropdown's choices, a setting's longer help, and a game starting
    add(f'''      <div id="picker" class="overlay">
        <div class="sheet">
          <span id="picker-kicker" class="kicker"></span>
          <span id="picker-title" class="sheet-title"></span>
          <div class="list">
{rows("picker", PICKER_ROWS, lambda i: f'<span id="picker-name-{i}" class="row-label"></span><span id="picker-mark-{i}" class="row-value"></span>')}
          </div>
          <span id="picker-position" class="position"></span>
        </div>
      </div>
      <div id="info" class="overlay">
        <div class="sheet info-sheet">
          <span id="info-title" class="sheet-title"></span>
          <div id="info-text" class="sheet-text"></div>
        </div>
      </div>
{update_sheet()}
      <div id="loading-screen" class="overlay loading">
        <img id="loading-cover" src="{S["cover"]}" width="320" height="320" alt=""/>
        <span id="loading-title"></span>
        <span id="loading-caption">Starting</span>
      </div>''')
    add(hints())
    out.append(TAIL)
    return "\n".join(out)


def start():
    """The two emulators side by side, each in its own colours."""
    out = [head("PS5CEMU-HAR", "wiiu")]
    add = out.append
    add('''      <span id="start-left-ground"></span><span id="start-right-ground"></span>
      <span id="start-wordmark">PS5CEMU-HAR</span>''')
    for key, name, art in (("wiiu", "Wii U", "start-wiiu"), ("3ds", "Nintendo 3DS", "start-3ds")):
        add(f'''      <div id="start-{key}" class="start-card start-{key}">
        <img class="start-art" src="icons/{art}.tga" width="420" height="420" alt=""/>
        <span class="start-name">{name}</span>
        <span id="start-status-{key}" class="start-status"></span>
      </div>''')
    add('      <span id="start-version"></span>')
    add(update_sheet())
    add(hints())
    out.append(TAIL)
    return "\n".join(out)


def stylesheets(source, out):
    with open(source, encoding="utf-8") as file:
        template = file.read()
    os.makedirs(os.path.join(out, "styles"), exist_ok=True)
    for suffix, colours in THEMES.values():
        text = template
        # the longest names first: $accent-strong before $accent
        for name in sorted(colours, key=len, reverse=True):
            text = text.replace("$" + name, colours[name])
        if "$" in text:
            at = text.index("$")
            sys.exit(f"{source}: a colour without a value: {text[at:at + 24]}")
        with open(os.path.join(out, "styles", f"har{suffix}.rcss"), "w", newline="\n", encoding="utf-8") as file:
            file.write(text)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    documents = {"start.rml": start()}
    documents.update({name: side(S) for name, S in SIDES.items()})
    for name, text in documents.items():
        with open(os.path.join(out, name), "w", newline="\n", encoding="utf-8") as file:
            file.write(text)
    stylesheets(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "port", "frontend", "ui", "har.rcss"), out)


if __name__ == "__main__":
    main()
