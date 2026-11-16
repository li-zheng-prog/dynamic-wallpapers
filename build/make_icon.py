"""Generate app.ico from logo.png for the executable, tray, windows, and installer.

The crop uses the Samsung One UI squircle with continuous curved edges.
Coordinates come from the icon background vector in the One UI Design Guidelines,
normalized to 0..1 without reshaping, matching the default One UI 7.0 shape.
Sharpen sizes up to 32px to preserve arrow and clock detail after downsampling.
Windows uses the 32px tray icon at 200% scaling, so small sizes matter.
Replace the root logo.png (square, preferably 512px or larger) and rerun build/build.sh.
Avoid large blank margins, which make the icon look too small.

Usage: python make_icon.py <output.ico> [path/to/logo.png]
"""
import io
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFilter

SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)

# Normalized One UI squircle segments: x1,y1 c1x,c1y c2x,c2y x2,y2.
# Zero-length control handles are retained from the original cubic Bezier path.
_SQUIRCLE_SEGS = (
    (0.988307, 0.351697, 0.976083, 0.259311, 0.943369, 0.175610, 0.885730, 0.114288),
    (0.885730, 0.114288, 0.824390, 0.056631, 0.740690, 0.023935, 0.648284, 0.011693),
    (0.648284, 0.011693, 0.560076, 0.000000, 0.499982, 0.002878, 0.499982, 0.002878),
    (0.499982, 0.002878, 0.439905, 0.000000, 0.351679, 0.011693, 0.351679, 0.011693),
    (0.351679, 0.011693, 0.259310, 0.023935, 0.175610, 0.056631, 0.114288, 0.114288),
    (0.114288, 0.114288, 0.056612, 0.175610, 0.023935, 0.259311, 0.011693, 0.351697),
    (0.011693, 0.351697, 0.000000, 0.439905, 0.002895, 0.500000, 0.002895, 0.500000),
    (0.002895, 0.500000, 0.002895, 0.500000, 0.000000, 0.560076, 0.011693, 0.648303),
    (0.011693, 0.648303, 0.023935, 0.740672, 0.056612, 0.824390, 0.114288, 0.885712),
    (0.114288, 0.885712, 0.175610, 0.943369, 0.259310, 0.976065, 0.351679, 0.988307),
    (0.351679, 0.988307, 0.439905, 1.000000, 0.499982, 0.997123, 0.499982, 0.997123),
    (0.499982, 0.997123, 0.560076, 1.000000, 0.648284, 0.988307, 0.648284, 0.988307),
    (0.648284, 0.988307, 0.740690, 0.976065, 0.824390, 0.943369, 0.885694, 0.885731),
    (0.885694, 0.885731, 0.943369, 0.824390, 0.976083, 0.740672, 0.988307, 0.648303),
    (0.988307, 0.648303, 1.000000, 0.560076, 0.997122, 0.500000, 0.997122, 0.500000),
    (0.997122, 0.500000, 0.997122, 0.500000, 1.000000, 0.439905, 0.988307, 0.351697),
)

_SQUIRCLE_BASE_PX = 1024      # Render once at high resolution, then downsample.
_squircleBase = None
_maskCache = {}


def squircle_mask(size: int) -> Image.Image:
    """Create an L-mode squircle mask with 255 inside the shape.
    Rasterize at 1024px and downsample with LANCZOS for antialiased edges."""
    global _squircleBase
    if size not in _maskCache:
        if _squircleBase is None:
            pts = []
            s = _SQUIRCLE_BASE_PX
            x1, y1 = _SQUIRCLE_SEGS[0][0], _SQUIRCLE_SEGS[0][1]
            pts.append((x1 * s, y1 * s))
            for (ax, ay, cx1, cy1, cx2, cy2, bx, by) in _SQUIRCLE_SEGS:
                for i in range(1, 33):
                    t = i / 32.0
                    u = 1.0 - t
                    x = u * u * u * ax + 3 * u * u * t * cx1 + 3 * u * t * t * cx2 + t * t * t * bx
                    y = u * u * u * ay + 3 * u * u * t * cy1 + 3 * u * t * t * cy2 + t * t * t * by
                    pts.append((x * s, y * s))
            big = Image.new("L", (s, s), 0)
            ImageDraw.Draw(big).polygon(pts, fill=255)
            _squircleBase = big
        _maskCache[size] = _squircleBase.resize((size, size), Image.LANCZOS)
    return _maskCache[size]


def make(size: int, src: Image.Image) -> Image.Image:
    img = src.resize((size, size), Image.LANCZOS)
    if size <= 32:                        # Light sharpening offsets downsampling blur.
        img = img.filter(ImageFilter.UnsharpMask(radius=1.2, percent=90, threshold=2))
    out = img.convert("RGBA")
    out.putalpha(squircle_mask(size))
    return out


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
