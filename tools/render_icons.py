from pathlib import Path
from io import BytesIO
import struct
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
sizes = (16, 20, 24, 32, 40, 48, 64, 128, 256)

def draw(role, size):
    # Render each Windows icon size independently; tiny icons omit fine detail.
    scale = 4
    im = Image.new("RGBA", (size * scale, size * scale))
    d = ImageDraw.Draw(im)
    def rect(coords, radius, fill, outline=None, width=0):
        c = tuple(round(v * size / 256) * scale for v in coords)
        d.rounded_rectangle(c, radius=max(scale, round(radius * size / 256) * scale), fill=fill,
                           outline=outline, width=max(scale, round(width * size / 256) * scale) if width else 0)
    def line(points, color, width):
        pts = [(round(x * size / 256) * scale, round(y * size / 256) * scale) for x, y in points]
        w = max(scale, round(width * size / 256) * scale)
        d.line(pts, fill=color, width=w, joint="curve")
    stroke = 20 if size <= 24 else 12
    rect((0, 0, 255, 255), 48, "#141414")
    rect((30, 44, 181, 156), 12, "#222222", "#A8A8A8", stroke)
    line(((88, 162), (88, 198)), "#A8A8A8", stroke)
    line(((57, 198), (111, 198)), "#A8A8A8", stroke)
    rect((101, 108, 227, 207), 12, "#191919", "#EEEEEE", stroke)
    if size >= 32:
        if role == "client":
            line(((189, 157), (139, 157), (154, 142)), "#EEEEEE", 12)
            line(((139, 157), (154, 172)), "#EEEEEE", 12)
        else:
            line(((139, 157), (189, 157), (174, 142)), "#EEEEEE", 12)
            line(((189, 157), (174, 172)), "#EEEEEE", 12)
    return im.resize((size, size), Image.Resampling.LANCZOS)

for role in ("app", "host", "client"):
    draw(role, 512).save(root / "assets" / (role + ".png"))
    frames = []
    for size in sizes:
        out = BytesIO(); draw(role, size).save(out, format="PNG"); frames.append(out.getvalue())
    offset = 6 + 16 * len(frames)
    directory = []
    for size, frame in zip(sizes, frames):
        directory.append(struct.pack("<BBBBHHII", size if size < 256 else 0, size if size < 256 else 0,
                                     0, 0, 1, 32, len(frame), offset))
        offset += len(frame)
    (root / "assets" / (role + ".ico")).write_bytes(struct.pack("<HHH", 0, 1, len(frames)) + b"".join(directory) + b"".join(frames))
    arrow = "M189 157H139L154 142M139 157L154 172" if role == "client" else "M139 157H189L174 142M189 157L174 172"
    svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256"><rect width="256" height="256" rx="48" fill="#141414"/><rect x="30" y="44" width="151" height="112" rx="12" fill="#222222" stroke="#a8a8a8" stroke-width="12"/><path d="M88 162v36m-31 0h54" fill="none" stroke="#a8a8a8" stroke-width="12"/><rect x="101" y="108" width="126" height="99" rx="12" fill="#191919" stroke="#eeeeee" stroke-width="12"/><path d="' + arrow + '" fill="none" stroke="#eeeeee" stroke-width="12" stroke-linejoin="round"/></svg>'
    (root / "assets" / (role + ".svg")).write_text(svg, encoding="utf-8")
