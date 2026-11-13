#!/usr/bin/env python3
"""Convert a GIF into raw SSD1306 frames for `oled_anim play`.

usage: gif2oled.py input.gif output.bin [--invert] [--threshold N]

Each frame is scaled to fit 128x64 (aspect kept, centred on black),
converted to 1-bit (dithered by default, or a hard threshold), and packed in
the SSD1306 page layout: 8 pages x 128 columns, bit0 = top pixel of the page.
"""
import argparse

from PIL import Image, ImageOps, ImageSequence

W, H = 128, 64


def to_frame(img, invert, threshold):
    img = img.convert("RGB")
    img.thumbnail((W, H))
    canvas = Image.new("RGB", (W, H), "black")
    canvas.paste(img, ((W - img.width) // 2, (H - img.height) // 2))
    gray = canvas.convert("L")
    if invert:
        gray = ImageOps.invert(gray)
    if threshold is None:
        mono = gray.convert("1")                        # Floyd-Steinberg dither
    else:
        mono = gray.point(lambda p: 255 if p >= threshold else 0).convert("1")
    px = mono.load()
    out = bytearray()
    for page in range(H // 8):
        for x in range(W):
            b = 0
            for bit in range(8):
                if px[x, page * 8 + bit]:
                    b |= 1 << bit
            out.append(b)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gif")
    ap.add_argument("out")
    ap.add_argument("--invert", action="store_true", help="swap black/white")
    ap.add_argument("--threshold", type=int, help="0-255 hard cut instead of dithering")
    a = ap.parse_args()

    src = Image.open(a.gif)
    frames, durations = 0, []
    with open(a.out, "wb") as f:
        for fr in ImageSequence.Iterator(src):
            f.write(to_frame(fr, a.invert, a.threshold))
            durations.append(fr.info.get("duration", 100) or 100)
            frames += 1

    avg_ms = sum(durations) / len(durations)
    fps = max(1, round(1000 / avg_ms))
    print(f"{frames} frames -> {a.out} ({frames * 1024} bytes)")
    print(f"GIF speed ~{fps} fps. Play with: ./build/tools/oled_anim play {a.out} {fps}")


if __name__ == "__main__":
    main()
