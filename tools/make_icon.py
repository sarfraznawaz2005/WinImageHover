"""Draw the ImageHover icon and save it as res/imagehover.ico (+ a PNG preview).

Design: blue-violet rounded tile, a white photo card (sky, mountains, sun),
and a mouse pointer resting on the card ("hover"). Small sizes drop the pointer
so the picture stays readable.

Run:  python -I tools/make_icon.py
"""
import os
import sys
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "res")
S = 1024  # master canvas, drawn large then downscaled for smooth edges


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def gradient(size, c1, c2, diagonal=True):
    w, h = size
    g = Image.new("RGBA", size)
    px = g.load()
    for y in range(h):
        for x in range(w):
            t = ((x + y) / (w + h - 2)) if diagonal else (y / (h - 1))
            px[x, y] = lerp(c1, c2, t)
    return g


def rounded_mask(size, box, radius):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle(box, radius=radius, fill=255)
    return m


def draw(with_pointer):
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # Background tile
    bg_box = (40, 40, S - 40, S - 40)
    bg = gradient((S, S), (64, 132, 255, 255), (124, 77, 255, 255))
    img.paste(bg, (0, 0), rounded_mask((S, S), bg_box, 210))

    # Photo card (bigger when there is no pointer)
    if with_pointer:
        card = (170, 220, 790, 720)
    else:
        card = (150, 210, 874, 814)
    d = ImageDraw.Draw(img)
    # soft shadow
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sx = ImageDraw.Draw(shadow)
    sx.rounded_rectangle((card[0] + 10, card[1] + 22, card[2] + 10, card[3] + 22), 70, fill=(20, 10, 70, 90))
    img = Image.alpha_composite(img, shadow)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle(card, 70, fill=(255, 255, 255, 255))

    # Picture area inside the card
    inset = 46 if with_pointer else 52
    pic = (card[0] + inset, card[1] + inset, card[2] - inset, card[3] - inset)
    pw, ph = pic[2] - pic[0], pic[3] - pic[1]
    sky = gradient((pw, ph), (186, 220, 255, 255), (232, 243, 255, 255), diagonal=False)
    picture = Image.new("RGBA", (pw, ph), (0, 0, 0, 0))
    picture.paste(sky, (0, 0))
    pd = ImageDraw.Draw(picture)
    # sun
    r = int(ph * 0.13)
    cx, cy = int(pw * 0.74), int(ph * 0.28)
    pd.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(255, 196, 46, 255))
    # back mountain
    pd.polygon([(int(pw * 0.30), ph), (int(pw * 0.68), int(ph * 0.38)), (int(pw * 1.05), ph)],
               fill=(84, 140, 235, 255))
    # front mountain
    pd.polygon([(-int(pw * 0.05), ph), (int(pw * 0.30), int(ph * 0.30)), (int(pw * 0.70), ph)],
               fill=(42, 92, 210, 255))
    mask = rounded_mask((pw, ph), (0, 0, pw - 1, ph - 1), 34)
    img.paste(picture, (pic[0], pic[1]), mask)

    if with_pointer:
        # Classic arrow pointer, tip resting on the card
        k = 21
        shape = [(0, 0), (0, 17), (4, 13.2), (7, 19.6), (9.6, 18.5), (6.7, 12.2), (12, 12)]
        tip = (560, 470)
        pts = [(tip[0] + x * k, tip[1] + y * k) for x, y in shape]
        # shadow
        sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        ImageDraw.Draw(sh).polygon([(x + 14, y + 18) for x, y in pts], fill=(20, 10, 60, 110))
        img = Image.alpha_composite(img, sh)
        d = ImageDraw.Draw(img)
        d.polygon(pts, fill=(255, 255, 255, 255), outline=(24, 22, 48, 255), width=26)
    return img


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    big = draw(with_pointer=True)
    small = draw(with_pointer=False)
    sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    frames = []
    for s in sizes:
        src = small if s <= 32 else big
        frames.append(src.resize((s, s), Image.LANCZOS))
    ico = os.path.join(OUT_DIR, "imagehover.ico")
    frames[-1].save(ico, format="ICO", sizes=[(s, s) for s in sizes], append_images=frames[:-1])
    big.resize((256, 256), Image.LANCZOS).save(os.path.join(OUT_DIR, "icon_preview.png"))
    print("wrote", ico)


if __name__ == "__main__":
    sys.exit(main())
