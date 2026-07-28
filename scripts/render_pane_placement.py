#!/usr/bin/env python3
"""Render pane-placement screenshots from layout geometry JSON + optional PTY captures."""

from __future__ import annotations

import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


CELL_W = 9
CELL_H = 16
BG = (12, 12, 12)
SCROLL = (12, 12, 12)
BORDER = (120, 120, 120)
FOCUS = (245, 165, 36)
INPUT_BG = (31, 31, 31)
PAD_BG = (20, 20, 24)
SEP_BG = (28, 28, 32)
TEXT = (220, 220, 220)
MUTED = (140, 140, 140)
POLLUTED = (180, 80, 80)
EMPTY = (70, 140, 90)


def font(size: int = 12):
    for path in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    ):
        if Path(path).exists():
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def draw_layout(data: dict, out: Path, title: str) -> None:
    cols = int(data["cols"])
    rows = int(data["rows"])
    img = Image.new("RGB", (cols * CELL_W + 40, rows * CELL_H + 80), (8, 8, 8))
    draw = ImageDraw.Draw(img)
    f = font(11)
    f_sm = font(10)
    ox, oy = 20, 40
    draw.text((ox, 12), title, fill=TEXT, font=f)

    # Terminal backdrop
    draw.rectangle(
        [ox, oy, ox + cols * CELL_W - 1, oy + rows * CELL_H - 1],
        fill=BG,
        outline=(60, 60, 60),
    )

    for pane in data["panes"]:
        r = pane["rect"]
        x0 = ox + r["x"] * CELL_W
        y0 = oy + r["y"] * CELL_H
        x1 = x0 + r["w"] * CELL_W - 1
        y1 = y0 + r["h"] * CELL_H - 1
        focused = pane.get("focused", False)
        polluted = pane.get("polluted", False)
        has_content = pane.get("has_content", False)
        border = FOCUS if focused else BORDER

        # Pane fill
        draw.rectangle([x0, y0, x1, y1], fill=SCROLL, outline=border)

        input_rows = int(pane["input_rows"])
        bottom_pad = int(pane["bottom_pad_rows"])
        outer_top = bool(pane.get("outer_top", pane["rect"]["y"] == 0))
        # Chrome bands (bottom → top): pad, optional input, output
        pad_top = y1 - bottom_pad * CELL_H + 1 if bottom_pad > 0 else y1 + 1
        input_top = pad_top - input_rows * CELL_H if input_rows >= 2 else pad_top
        # top float only on outer-top panes
        out_top = y0 + (CELL_H if outer_top else 0)

        # Trailing pad
        if bottom_pad > 0:
            draw.rectangle([x0 + 1, y1 - bottom_pad * CELL_H + 1, x1 - 1, y1 - 1], fill=PAD_BG)
        # Input box — focused panes only
        if input_rows >= 2:
            draw.rectangle(
                [x0 + 4, input_top, x1 - 4, pad_top - 1],
                fill=INPUT_BG,
                outline=border,
            )
        # Output box fills to pad (inactive) or to input (focused)
        out_bottom = (input_top - 1) if input_rows >= 2 else (y1 - bottom_pad * CELL_H)
        if out_bottom > out_top:
            draw.rectangle(
                [x0 + 4, out_top, x1 - 4, out_bottom],
                outline=border,
            )

        label = pane.get("label", f"pane {pane.get('index', '?')}")
        status = []
        status.append(f"input_rows={input_rows}")
        status.append(f"pad={bottom_pad}")
        status.append("outer-bottom" if pane.get("outer_bottom") else "stacked")
        if focused:
            status.append("FOCUSED")
        if has_content:
            status.append("content")
        if polluted:
            status.append("POLLUTED")
        color = POLLUTED if polluted else (EMPTY if not has_content else TEXT)
        draw.text((x0 + 8, out_top + 4), label, fill=color, font=f_sm)
        draw.text((x0 + 8, out_top + 18), " · ".join(status), fill=MUTED, font=f_sm)
        if has_content and not polluted:
            draw.text(
                (x0 + 8, out_top + 34),
                "transcript replay owner",
                fill=TEXT,
                font=f_sm,
            )
        elif not has_content:
            draw.text(
                (x0 + 8, out_top + 34),
                "empty scrollback (ok)",
                fill=EMPTY,
                font=f_sm,
            )

        # Cursor block in focused input
        if focused and input_rows >= 2:
            cx = x0 + 12
            cy = input_top + CELL_H
            draw.rectangle([cx, cy, cx + CELL_W - 2, cy + CELL_H - 4], fill=FOCUS)

    # Separators annotation
    for sep in data.get("separators", []):
        if sep["orient"] == "h":
            y = oy + sep["y"] * CELL_H
            x0 = ox + sep["x"] * CELL_W
            x1 = x0 + sep["w"] * CELL_W - 1
            draw.rectangle([x0, y, x1, y + CELL_H - 1], fill=SEP_BG)
        else:
            x = ox + sep["x"] * CELL_W
            y0 = oy + sep["y"] * CELL_H
            y1 = y0 + sep["h"] * CELL_H - 1
            draw.rectangle([x, y0, x + CELL_W - 1, y1], fill=SEP_BG)

    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)
    print(f"wrote {out}")


def render_pyte_screen(lines: list[str], out: Path, title: str) -> None:
    rows = len(lines)
    cols = max((len(l) for l in lines), default=80)
    img = Image.new("RGB", (cols * CELL_W + 40, rows * CELL_H + 60), (8, 8, 8))
    draw = ImageDraw.Draw(img)
    f = font(12)
    draw.text((20, 12), title, fill=TEXT, font=f)
    ox, oy = 20, 36
    draw.rectangle(
        [ox, oy, ox + cols * CELL_W - 1, oy + rows * CELL_H - 1],
        fill=BG,
        outline=(60, 60, 60),
    )
    for y, line in enumerate(lines):
        draw.text((ox + 2, oy + y * CELL_H), line, fill=TEXT, font=f)
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)
    print(f"wrote {out}")


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: render_pane_placement.py <geometry.json> <out.png> [title]")
        print("   or: render_pane_placement.py --screen <screen.txt> <out.png> [title]")
        return 2
    if sys.argv[1] == "--screen":
        screen = Path(sys.argv[2]).read_text(encoding="utf-8").splitlines()
        out = Path(sys.argv[3])
        title = sys.argv[4] if len(sys.argv) > 4 else "PTY screen capture"
        render_pyte_screen(screen, out, title)
        return 0
    data = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    out = Path(sys.argv[2])
    title = sys.argv[3] if len(sys.argv) > 3 else "Pane placement"
    draw_layout(data, out, title)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
