#!/usr/bin/env python3
"""Restore a multi-pane shared-conversation layout and screenshot the result."""

from __future__ import annotations

import fcntl
import json
import os
import pty
import select
import struct
import sys
import tempfile
import termios
import time
import uuid
from pathlib import Path

import pyte
from PIL import Image, ImageDraw, ImageFont

CELL_W, CELL_H = 9, 16
BG = (12, 12, 12)
FG = (220, 220, 220)


def font(size: int = 12):
    for path in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    ):
        if Path(path).exists():
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def set_winsize(fd: int, rows: int, cols: int) -> None:
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def render_screen(screen: pyte.Screen, out: Path, title: str) -> None:
    cols, rows = screen.columns, screen.lines
    img = Image.new("RGB", (cols * CELL_W + 40, rows * CELL_H + 60), (8, 8, 8))
    draw = ImageDraw.Draw(img)
    f = font(12)
    draw.text((20, 12), title, fill=FG, font=f)
    ox, oy = 20, 36
    draw.rectangle(
        [ox, oy, ox + cols * CELL_W - 1, oy + rows * CELL_H - 1],
        fill=BG,
        outline=(60, 60, 60),
    )
    for y in range(rows):
        line = "".join((screen.buffer[y][x].data or " ") for x in range(cols))
        draw.text((ox + 2, oy + y * CELL_H), line, fill=FG, font=f)
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)
    print(f"wrote {out}")


def drain(master: int, stream: pyte.Stream, until: float) -> None:
    while time.time() < until:
        r, _, _ = select.select([master], [], [], 0.05)
        if not r:
            continue
        try:
            data = os.read(master, 65536)
        except OSError:
            break
        if not data:
            break
        stream.feed(data.decode("utf-8", errors="ignore"))


def screen_text(screen: pyte.Screen) -> str:
    cols, rows = screen.columns, screen.lines
    return "\n".join(
        "".join((screen.buffer[y][x].data or " ") for x in range(cols)).rstrip()
        for y in range(rows)
    )


def main() -> int:
    binary = sys.argv[1] if len(sys.argv) > 1 else "build/arbiter"
    out_dir = Path(sys.argv[2] if len(sys.argv) > 2 else "/opt/cursor/artifacts/screenshots")
    rows, cols = 40, 120
    marker = "COPPER-AGE-IBERIA-MARKER-UNIQUE"

    home = tempfile.mkdtemp(prefix="arbiter-restore-")
    base = Path(home) / ".arbiter"
    (base / "agents").mkdir(parents=True)
    conv = base / "conversations"
    conv.mkdir(parents=True)
    (base / "openrouter_api_key").write_text("dummy-key-no-network\n")
    (base / "agents" / "index.json").write_text(
        '{\n  "id": "index",\n  "name": "index",\n'
        '  "model": "openai/gpt-4o-mini",\n  "system": "test"\n}\n'
    )

    conv_id = str(uuid.uuid4())
    now = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    (conv / "active").write_text(conv_id + "\n")
    (conv / "manifest.json").write_text(json.dumps([
        {"id": conv_id, "title": "copper dossier", "cwd": home,
         "created_at": now, "updated_at": now}
    ], indent=2) + "\n")

    # Conversation with enough assistant text that pollution would be obvious.
    body = {
        "agents": {
            "index": [
                {"role": "user", "content": "summarize the dossier"},
                {"role": "assistant", "content":
                 f"Artifact: copper-age-iberia-dossier.md\n"
                 f"Artifact ID: #75\n"
                 f"{marker}\n"
                 f"## Summary\n"
                 f"Los Millares and Vila Nova de Sao Pedro show long-distance exchange.\n"
                 f"Site-specific amber and ostrich-eggshell evidence continues below.\n"
                 * 8},
            ]
        }
    }
    (conv / f"{conv_id}.json").write_text(json.dumps(body, indent=2) + "\n")

    # Same conversation on all four leaves — live splits inherit; restore must
    # replay only once (first leaf), leaving siblings empty.
    layout = {
        "version": 1,
        "focused_leaf": 3,
        "root": {
            "type": "split",
            "orient": "vertical",
            "weight": 1.0,
            "children": [
                {"type": "leaf", "conversation_id": conv_id, "agent": "index", "weight": 1.0},
                {
                    "type": "split",
                    "orient": "horizontal",
                    "weight": 1.0,
                    "children": [
                        {"type": "leaf", "conversation_id": conv_id, "agent": "index", "weight": 1.0},
                        {"type": "leaf", "conversation_id": conv_id, "agent": "index", "weight": 1.0},
                        {"type": "leaf", "conversation_id": conv_id, "agent": "index", "weight": 1.0},
                    ],
                },
            ],
        },
    }
    (conv / "layout.json").write_text(json.dumps(layout, indent=2) + "\n")

    env = os.environ.copy()
    env["HOME"] = home
    env["TERM"] = "xterm-256color"
    env["OPENROUTER_API_KEY"] = "dummy-key-no-network"
    env["ARBITER_AUTOSAVE_INTERVAL_SEC"] = "0"

    pid, master = pty.fork()
    if pid == 0:
        set_winsize(sys.stdout.fileno(), rows, cols)
        os.chdir(home)
        os.execve(os.path.abspath(binary), [binary], env)

    set_winsize(master, rows, cols)
    flags = fcntl.fcntl(master, fcntl.F_GETFL)
    fcntl.fcntl(master, fcntl.F_SETFL, flags | os.O_NONBLOCK)

    screen = pyte.Screen(cols, rows)
    stream = pyte.Stream(screen)

    deadline = time.time() + 10
    while time.time() < deadline:
        drain(master, stream, time.time() + 0.2)
        text = screen_text(screen)
        if marker in text or "╭" in text:
            # Give the layout a moment to settle after first paint.
            drain(master, stream, time.time() + 0.8)
            break

    text = screen_text(screen)
    (out_dir / "pty_restore_stacked.txt").write_text(text + "\n")
    render_screen(screen, out_dir / "pty_restore_stacked.png",
                  "PTY restore — shared conversation, stacked panes")

    corners = text.count("╭") + text.count("╰")
    marker_count = text.count(marker)
    print(f"rounded corners ╭+╰ = {corners}")
    print(f"marker occurrences on screen = {marker_count}")

    # Save counts for the walkthrough
    (out_dir / "pty_restore_stats.txt").write_text(
        f"corners={corners}\nmarker_count={marker_count}\n"
    )

    try:
        os.write(master, b"\x04")
        time.sleep(0.2)
        os.write(master, b"/quit\r")
        drain(master, stream, time.time() + 0.5)
    except OSError:
        pass
    try:
        os.close(master)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass

    # Inactive panes must still show input chrome (many corners).
    if corners < 8:
        print(f"FAIL: missing readline chrome (corners={corners})")
        return 1
    # Marker should appear in the replay owner pane, not duplicated across
    # every sibling viewport. pyte may still see it once; >3 would mean
    # obvious pollution across the 3 empty right panes + left.
    if marker_count > 2:
        print(f"FAIL: marker polluted across panes (count={marker_count})")
        return 1
    print("PASS: restore chrome + no multi-pane pollution")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
