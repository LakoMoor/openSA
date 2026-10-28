"""Generates the openSA logo: sunset badge with a palm silhouette. Original artwork, no Rockstar assets.
usage: python make_logo.py   (writes logo.png, logo_512.png, icon.ico, favicon.png next to this script)
"""
import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

OUT = Path(__file__).parent
S = 2048  # supersampled canvas


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def gradient(top_stops):
    img = Image.new("RGB", (S, S))
    px = ImageDraw.Draw(img)
    for y in range(S):
        t = y / (S - 1)
        for (t0, c0), (t1, c1) in zip(top_stops, top_stops[1:]):
            if t0 <= t <= t1:
                px.line([(0, y), (S, y)], fill=lerp(c0, c1, (t - t0) / (t1 - t0)))
                break
    return img


def bez(p0, p1, p2, n=40):
    return [((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0],
             (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1]) for t in [i / n for i in range(n + 1)]]


def frond(base, tip, bend, width):
    """Pointed curved leaf from base to tip, `bend` pushes the control point sideways."""
    mx, my = (base[0] + tip[0]) / 2, (base[1] + tip[1]) / 2
    dx, dy = tip[0] - base[0], tip[1] - base[1]
    L = math.hypot(dx, dy)
    nx, ny = -dy / L, dx / L
    ctrl = (mx + nx * bend, my + ny * bend)
    spine = bez(base, ctrl, tip)
    left, right = [], []
    for i, (x, y) in enumerate(spine):
        t = i / (len(spine) - 1)
        w = width * math.sin(math.pi * min(1.0, t * 1.05)) ** 0.8
        # normal of the curve at this point
        if i < len(spine) - 1:
            tx, ty = spine[i + 1][0] - x, spine[i + 1][1] - y
        else:
            tx, ty = x - spine[i - 1][0], y - spine[i - 1][1]
        tl = math.hypot(tx, ty) or 1
        px_, py_ = -ty / tl, tx / tl
        left.append((x + px_ * w, y + py_ * w))
        right.append((x - px_ * w, y - py_ * w))
    return left + right[::-1]


def build():
    sky = gradient([(0.0, (46, 16, 84)), (0.38, (150, 36, 110)), (0.62, (244, 98, 54)), (0.85, (255, 178, 52)), (1.0, (255, 214, 96))])
    img = sky.convert("RGBA")
    d = ImageDraw.Draw(img)

    # sun
    cx, cy, r = S * 0.5, S * 0.60, S * 0.25
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([cx - r * 1.5, cy - r * 1.5, cx + r * 1.5, cy + r * 1.5], fill=(255, 220, 120, 120))
    img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(S * 0.05)))
    d = ImageDraw.Draw(img)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(255, 238, 150, 255))
    # sun stripes
    for i in range(5):
        y0 = cy + r * (0.05 + 0.18 * i)
        d.rectangle([cx - r, y0, cx + r, y0 + r * (0.03 + 0.025 * i)], fill=lerp((244, 98, 54), (150, 36, 110), 0.2 + 0.15 * i) + (255,))

    # skyline silhouette
    sil = (22, 8, 40, 255)
    x = 0
    heights = [0.12, 0.2, 0.16, 0.26, 0.18, 0.1, 0.22, 0.3, 0.2, 0.14, 0.24, 0.16, 0.12]
    bw = S / len(heights)
    for i, h in enumerate(heights):
        d.rectangle([i * bw, S * (0.86 - h * 0.55), (i + 1) * bw + 2, S], fill=sil)
    d.rectangle([0, S * 0.90, S, S], fill=sil)

    # palm
    base = (S * 0.34, S * 0.93)
    top = (S * 0.46, S * 0.34)
    trunk = bez(base, (S * 0.30, S * 0.62), top, 60)
    for i in range(len(trunk) - 1):
        t = i / (len(trunk) - 1)
        w = S * (0.028 - 0.012 * t)
        d.line([trunk[i], trunk[i + 1]], fill=sil, width=int(w * 2))
    crown = top
    leaves = [(-340, -120, 120, 60), (-300, 70, -110, 50), (-170, 190, 90, 46), (0, -210, 0, 46), (180, 210, -90, 46),
              (330, -80, -120, 56), (310, 110, 110, 50), (-60, -280, 110, 40)]
    for dx, dy, bend, w in leaves:
        tip = (crown[0] + dx * S / 1024, crown[1] + dy * S / 1024 + (abs(dx) * 0.28 * S / 1024))
        d.polygon(frond(crown, tip, bend * S / 1024, w * S / 1024), fill=sil)

    # badge mask: rounded square
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.2), fill=255)
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(img, (0, 0), mask)
    # inner border
    ring = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    rd = ImageDraw.Draw(ring)
    rd.rounded_rectangle([S * 0.02, S * 0.02, S * 0.98, S * 0.98], radius=int(S * 0.18), outline=(255, 236, 170, 255), width=int(S * 0.012))
    return Image.alpha_composite(out, ring)


def main():
    big = build()
    logo = big.resize((1024, 1024), Image.LANCZOS)
    logo.save(OUT / "logo.png")
    logo.resize((512, 512), Image.LANCZOS).save(OUT / "logo_512.png")
    logo.resize((64, 64), Image.LANCZOS).save(OUT / "favicon.png")
    sizes = [16, 24, 32, 48, 64, 128, 256]
    big.resize((256, 256), Image.LANCZOS).save(OUT / "icon.ico", sizes=[(s, s) for s in sizes])
    print("written:", [p.name for p in sorted(OUT.glob("*.png"))] + ["icon.ico"])


if __name__ == "__main__":
    main()
