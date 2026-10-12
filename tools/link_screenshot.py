"""Capture the screen of a Link device as PNG, or as an animated GIF.

Usage:
    python tools/link_screenshot.py COM3 journal
    python tools/link_screenshot.py COM3 journal --scale 3 --outdir shots
    python tools/link_screenshot.py COM3 journal --gif 20 --interval 1

Writes <name>_x1.png (exact pixels) and <name>_x<scale>.png (enlarged
without smoothing). With --gif N, captures N frames back to back and
writes <name>_x<scale>.gif instead, each frame shown for --interval
seconds: a capture takes longer than that, so the GIF is a time-lapse.

Exit code: 0 on success, 1 if a capture failed, 2 if the port could not
be used.
"""

import argparse
import base64
import json
import sys
import time
import zlib
from pathlib import Path

from PIL import Image

from send_cmd import open_port

COMMAND = b'{"cmd":"screenshot"}\n'


class CaptureError(Exception):
    pass


def read_messages(port, timeout):
    buffer = b""
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        buffer += port.read(65536)

        while b"\n" in buffer:
            raw, buffer = buffer.split(b"\n", 1)
            text = raw.decode("utf-8", errors="replace").strip()
            if not text.startswith("{"):
                continue
            try:
                msg = json.loads(text)
            except json.JSONDecodeError:
                continue
            if isinstance(msg, dict) and msg.get("cmd", msg.get("type")) == "screenshot":
                yield msg
                deadline = time.monotonic() + timeout

    raise CaptureError(f"no reply within {timeout} s")


def capture(port, timeout):
    port.reset_input_buffer()
    port.write(COMMAND)
    port.flush()

    header = None
    chunks = {}

    for msg in read_messages(port, timeout):
        if msg["type"] == "err":
            raise CaptureError(f"device refused: {msg.get('msg')}")

        if msg["type"] == "screenshot":
            chunks[msg["seq"]] = base64.b64decode(msg["data"])
            continue

        if msg.get("phase") == "begin":
            header = msg
            chunks.clear()
        elif msg.get("phase") == "end":
            break

    if header is None:
        raise CaptureError("end received without begin")
    if header.get("format") != "rgb565be":
        raise CaptureError(f"unsupported format {header.get('format')}")
    if sorted(chunks) != list(range(msg["count"])):
        raise CaptureError(f"{msg['count'] - len(chunks)} chunk(s) lost")

    data = b"".join(chunks[i] for i in range(msg["count"]))
    if len(data) != header["size"]:
        raise CaptureError(f"got {len(data)} bytes, expected {header['size']}")
    if zlib.crc32(data) != msg["crc32"]:
        raise CaptureError("CRC mismatch")

    return to_image(data, header["width"], header["height"])


def to_image(data, width, height):
    rgb = bytearray(width * height * 3)

    for i in range(width * height):
        px = (data[2 * i] << 8) | data[2 * i + 1]
        r, g, b = px >> 11, (px >> 5) & 0x3F, px & 0x1F
        rgb[3 * i] = (r << 3) | (r >> 2)
        rgb[3 * i + 1] = (g << 2) | (g >> 4)
        rgb[3 * i + 2] = (b << 3) | (b >> 2)

    return Image.frombytes("RGB", (width, height), bytes(rgb))


def enlarge(image, scale):
    return image.resize((image.width * scale, image.height * scale),
                        Image.Resampling.NEAREST)


def save_png(image, outdir, name, scale):
    paths = [outdir / f"{name}_x1.png"]
    image.save(paths[0])

    if scale > 1:
        paths.append(outdir / f"{name}_x{scale}.png")
        enlarge(image, scale).save(paths[1])

    return paths


def save_gif(port, args, outdir):
    frames = []

    for n in range(args.gif):
        start = time.monotonic()
        frames.append(enlarge(capture(port, args.timeout), args.scale))
        print(f"frame {n + 1}/{args.gif}: {time.monotonic() - start:.2f} s",
              file=sys.stderr)

    path = outdir / f"{args.name}_x{args.scale}.gif"
    frames[0].save(path, save_all=True, append_images=frames[1:],
                   duration=round(args.interval * 1000), loop=0,
                   optimize=False)
    return [path]


def main():
    parser = argparse.ArgumentParser(
        description="Capture the screen of a Link device."
    )
    parser.add_argument("port", help="serial port, e.g. COM3")
    parser.add_argument("name", help="base name of the output files, e.g. journal")
    parser.add_argument("--outdir", type=Path, default=Path("."),
                        help="output directory (default: current)")
    parser.add_argument("--scale", type=int, default=4,
                        help="enlargement factor, nearest neighbour (default: 4)")
    parser.add_argument("--gif", type=int, metavar="N",
                        help="capture N frames into an animated GIF")
    parser.add_argument("--interval", type=float, default=1.0,
                        help="seconds each GIF frame is shown (default: 1)")
    parser.add_argument("--timeout", type=float, default=5.0,
                        help="seconds to wait for the device (default: 5)")
    args = parser.parse_args()

    if args.scale < 1 or (args.gif is not None and args.gif < 1):
        parser.error("--scale and --gif must be at least 1")

    args.outdir.mkdir(parents=True, exist_ok=True)

    with open_port(args.port) as port:
        try:
            if args.gif:
                paths = save_gif(port, args, args.outdir)
            else:
                paths = save_png(capture(port, args.timeout), args.outdir,
                                 args.name, args.scale)
        except CaptureError as e:
            print(f"capture failed: {e}", file=sys.stderr)
            sys.exit(1)

    for path in paths:
        print(path)


if __name__ == "__main__":
    main()
