#!/usr/bin/env python3
"""Drive arbiter under a PTY, build stacked panes, capture a pyte screen PNG."""

from __future__ import annotations

import os
import select
import struct
import sys
import tempfile
import time
from pathlib import Path

import pyte
from PIL import Image, ImageDraw, ImageFont

# forkpty via pty module
import pty
import fcntl
import termios


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
    winsize = struct.pack("HHHH", rows, cols, 0, 0)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, winsize)


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
        line = screen.buffer[y]
        chars = []
        for x in range(cols):
            cell = line[x]
            chars.append(cell.data if cell.data else " ")
        draw.text((ox + 2, oy + y * CELL_H), "".join(chars), fill=FG, font=f)
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)
    print(f"wrote {out}")


def drain(master: int, stream: pyte.Stream, deadline: float) -> None:
    while time.time() < deadline:
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


def main() -> int:
    binary = sys.argv[1] if len(sys.argv) > 1 else "build/arbiter"
    out_dir = Path(sys.argv[2] if len(sys.argv) > 2 else "/opt/cursor/artifacts/screenshots")
    rows, cols = 40, 120

    home = tempfile.mkdtemp(prefix="arbiter-placement-")
    arbiter_dir = Path(home) / ".arbiter"
    (arbiter_dir / "agents").mkdir(parents=True)
    (arbiter_dir / "conversations").mkdir(parents=True)
    (arbiter_dir / "openrouter_api_key").write_text("dummy-key-no-network\n")

    # Minimal agent so startup doesn't fail hard
    (arbiter_dir / "agents" / "index.json").write_text(
        '{\n  "id": "index",\n  "name": "index",\n'
        '  "model": "openai/gpt-4o-mini",\n'
        '  "system": "test"\n}\n'
    )

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

    # Wait for alt-screen / first paint
    deadline = time.time() + 8
    while time.time() < deadline:
        drain(master, stream, time.time() + 0.2)
        # Look for any non-empty content
        text = "\n".join("".join(screen.buffer[y][x].data or " " for x in range(cols))
                         for y in range(rows))
        if "interrupt" in text or "help" in text or "╭" in text or "─" in text:
            break

    drain(master, stream, time.time() + 0.5)

    def send(data: str) -> None:
        os.write(master, data.encode())

    # Vertical split then two horizontal splits on the right column.
    send("\x17v")  # Ctrl-W v
    time.sleep(0.3)
    drain(master, stream, time.time() + 0.5)
    send("\x17h")  # Ctrl-W h
    time.sleep(0.3)
    drain(master, stream, time.time() + 0.5)
    send("\x17h")
    time.sleep(0.3)
    drain(master, stream, time.time() + 1.0)

    render_screen(screen, out_dir / "pty_stacked_panes_live.png",
                  "PTY live stacked panes (after ^W v / ^W h / ^W h)")

    # Dump plain screen text for assertions
    lines = []
    for y in range(rows):
        line = "".join((screen.buffer[y][x].data or " ") for x in range(cols))
        lines.append(line.rstrip())
    (out_dir / "pty_stacked_panes_live.txt").write_text("\n".join(lines) + "\n")

    # Count rounded-corner glyphs — each pane input+output box contributes corners.
    flat = "\n".join(lines)
    corners = flat.count("╭") + flat.count("╰")
    print(f"rounded corners seen: ╭+╰ = {corners}")
    (out_dir / "pty_corner_count.txt").write_text(f"{corners}\n")

    # Quit
    try:
        send("\x04")  # Ctrl-D
        time.sleep(0.3)
        send("/quit\r")
        drain(master, stream, time.time() + 1.0)
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

    # Focused pane: output + input boxes; inactive: output only. Expect
    # several rounded corners without idle readline chrome on every sibling.
    if corners < 6:
        print(f"FAIL: expected >= 6 rounded top/bottom corners, got {corners}")
        return 1
    print("PASS: stacked pane chrome present (content-only inactive)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
