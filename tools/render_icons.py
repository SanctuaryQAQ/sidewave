from pathlib import Path
from io import BytesIO
import math
import struct
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
sizes = (16, 20, 24, 32, 40, 48, 64, 128, 256)
# SideWave mark on a 256-unit grid: one orange wave on a dark tile.
TILE = ((42, 42, 46), (16, 16, 18))
WAVE = ((255, 170, 96), (236, 84, 42))
EDGE = (62, 62, 68)

def wave():
    return [(52 + i, 128 - 32 * math.sin(i / 152 * 2 * math.pi)) for i in range(0, 153, 2)]

def gradient(colors, px):
    ramp = Image.linear_gradient("L").resize((px, px))
    ramp = Image.blend(ramp, ramp.transpose(Image.Transpose.TRANSPOSE), 0.5)
    return Image.composite(Image.new("RGBA", (px, px), colors[1] + (255,)), Image.new("RGBA", (px, px), colors[0] + (255,)), ramp)

def draw(size):
    # Render each Windows icon size independently; small icons get a heavier wave.
    scale = 4
    px = size * scale
    k = px / 256
    mask = Image.new("L", (px, px))
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, px - 1, px - 1), radius=round(56 * k), fill=255)
    im = Image.new("RGBA", (px, px))
    im.paste(gradient(TILE, px), mask=mask)
    edge = max(scale, round(3 * k))
    ImageDraw.Draw(im).rounded_rectangle((0, 0, px - 1, px - 1), radius=round(56 * k), outline=EDGE + (255,), width=edge)
    # Stamp round dabs along the curve so the thick stroke stays smooth with round caps.
    line = Image.new("L", (px, px))
    d = ImageDraw.Draw(line)
    r = (40 if size <= 24 else 34 if size <= 48 else 32) * k / 2
    points = [(x * k, y * k) for x, y in wave()]
    for (x0, y0), (x1, y1) in zip(points, points[1:]):
        steps = max(1, int(math.hypot(x1 - x0, y1 - y0) / 2))
        for i in range(steps + 1):
            x, y = x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps
            d.ellipse((x - r, y - r, x + r, y + r), fill=255)
    top = Image.new("RGBA", (px, px))
    top.paste(gradient(WAVE, px), mask=line)
    im.alpha_composite(top)
    return im.resize((size, size), Image.Resampling.LANCZOS)

def path(points):
    return "M" + " L".join("%g,%g" % (round(x, 1), round(y, 1)) for x, y in points)

hexa = lambda c: "#%02x%02x%02x" % c
svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256"><defs>'
       '<linearGradient id="t" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient>'
       '<linearGradient id="w" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient></defs>'
       '<rect x="1.5" y="1.5" width="253" height="253" rx="55" fill="url(#t)" stroke="%s" stroke-width="3"/>'
       '<path d="%s" fill="none" stroke="url(#w)" stroke-width="32" stroke-linecap="round" stroke-linejoin="round"/></svg>'
       % (hexa(TILE[0]), hexa(TILE[1]), hexa(WAVE[0]), hexa(WAVE[1]), hexa(EDGE), path(wave())))
# host/client keep their file names for existing references; the mark is the same for every role.
for role in ("app", "host", "client"):
    draw(512).save(root / "assets" / (role + ".png"))
    frames = []
    for size in sizes:
        out = BytesIO(); draw(size).save(out, format="PNG"); frames.append(out.getvalue())
    offset = 6 + 16 * len(frames)
    directory = []
    for size, frame in zip(sizes, frames):
        directory.append(struct.pack("<BBBBHHII", size if size < 256 else 0, size if size < 256 else 0,
                                     0, 0, 1, 32, len(frame), offset))
        offset += len(frame)
    (root / "assets" / (role + ".ico")).write_bytes(struct.pack("<HHH", 0, 1, len(frames)) + b"".join(directory) + b"".join(frames))
    (root / "assets" / (role + ".svg")).write_text(svg, encoding="utf-8")
print(path(wave()))
