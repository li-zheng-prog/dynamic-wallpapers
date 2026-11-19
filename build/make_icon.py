"""Generate app.ico from logo.png for the executable, tray, windows, and installer.

Use an opaque square master. Apply continuous corners when exporting it for Windows.

Usage: python make_icon.py <output.ico> [path/to/logo.png]
"""
import io
import os
import struct
import sys

from PIL import Image, ImageDraw

SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)


def windows_mask(side: int) -> Image.Image:
    # Supersample a fifth-order superellipse to approximate continuous corners.
    n = 4 * side
    mask = Image.new("L", (n, n))
    draw = ImageDraw.Draw(mask)
    for y in range(n):
        dy = abs((2 * y + 1) / n - 1)
        inset = round((1 - (1 - dy**5)**0.2) * n / 2)
        draw.line((inset, y, n - 1 - inset, y), fill=255)
    return mask.resize((side, side), Image.Resampling.BOX)


def make(size: int, src: Image.Image) -> Image.Image:
    target = (size, size)
    img = src.resize(target, Image.LANCZOS)
    # Average alpha coverage so Lanczos ringing cannot appear outside the shape.
    img.putalpha(src.getchannel("A").resize(target, Image.Resampling.BOX))
    return img


def write_ico(entries, path):
    """Write an ICO container with PNG data for each size."""
    pngs = []
    for size, img in entries:
        buf = io.BytesIO()
        img.convert("RGBA").save(buf, format="PNG")
        pngs.append((size, buf.getvalue()))
    header = struct.pack("<HHH", 0, 1, len(pngs))
    offset = 6 + 16 * len(pngs)
    dir_bytes, blob = b"", b""
    for size, data in pngs:
        wh = 0 if size >= 256 else size
        dir_bytes += struct.pack("<BBBBHHII", wh, wh, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        blob += data
    with open(path, "wb") as f:
        f.write(header + dir_bytes + blob)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "app.ico"
    here = os.path.dirname(os.path.abspath(__file__))
    logo = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "..", "logo.png")
    logo = os.path.normpath(logo)
    if not os.path.exists(logo):
        print("!! Cannot find logo.png:", logo)
        sys.exit(1)
    src = Image.open(logo).convert("RGBA")
    if src.width != src.height:                       # Center-crop nonsquare input.
        side = min(src.width, src.height)
        src = src.crop(((src.width - side) // 2, (src.height - side) // 2,
                        (src.width - side) // 2 + side, (src.height - side) // 2 + side))
    src.putalpha(windows_mask(src.width))
    entries = [(s, make(s, src)) for s in SIZES]
    write_ico(entries, out)
    # Embed this PNG as IDR_LOGO_PNG through app.rc and setup.rc.
    # The main app and installer share it as their title-bar logo.
    ui_png = os.path.join(os.path.dirname(os.path.abspath(out)), "logo_ui.png")
    make(128, src).save(ui_png)
    print(f"icon written: {out} sizes: {list(SIZES)} (from {logo}, {src.width}px)")
    print(f"ui mark written: {ui_png}")


if __name__ == "__main__":
    main()
