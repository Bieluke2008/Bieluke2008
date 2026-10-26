#!/usr/bin/env python3
"""Builds assets/ui-emoji.png: the emoji the web HUD shows in its text that
OpenFront's emoji atlas lacks, drawn with Noto Color Emoji (what Chromium on
Linux uses). Run once; the output is committed.

usage: python3 scripts/make-ui-emoji.py /usr/share/fonts/truetype/noto/NotoColorEmoji.ttf
"""
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

EMOJI = ["⏳", "⚔️", "❌", "🏛️", "👤", "🔀", "✅", "🔒", "🔊", "🔇", "⚙️", "🗺️", "🎵", "🌙", "☀️", "🖱️", "⌨️", "📊"]
CELL = 72

font = ImageFont.truetype(sys.argv[1], 109)
cols = 6
rows = (len(EMOJI) + cols - 1) // cols
atlas = Image.new("RGBA", (cols * CELL, rows * CELL), (0, 0, 0, 0))
for i, e in enumerate(EMOJI):
    tile = Image.new("RGBA", (136, 128), (0, 0, 0, 0))
    ImageDraw.Draw(tile).text((0, 0), e, font=font, embedded_color=True)
    tile = tile.crop((0, 0, 136, 128)).resize((int(136 * CELL / 128), CELL), Image.LANCZOS)
    atlas.paste(tile.crop((0, 0, CELL, CELL)), ((i % cols) * CELL, (i // cols) * CELL))
out = Path(__file__).resolve().parent.parent / "assets"
atlas.save(out / "ui-emoji.png")
(out / "ui-emoji.json").write_text(
    json.dumps({"cellSize": CELL, "cols": cols, "emojis": {e: i for i, e in enumerate(EMOJI)}}, ensure_ascii=False, indent=1)
)
print("wrote", len(EMOJI))
