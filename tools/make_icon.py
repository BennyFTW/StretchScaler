"""Generates src/app.ico (16-256 px), docs/logo.png and docs/icon_preview.png. Requires Pillow.

Design: dark rounded tile, a 4:3 "screen" with a crosshair, and neon chevrons pushing outward (= stretch).
Small sizes (<= 32 px) use a simplified, thicker variant so they stay crisp in the tray.
"""
import os
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CYAN = (34, 228, 255)
MAGENTA = (255, 61, 203)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def h_gradient(size, c1, c2):
    g = Image.new("RGB", (size, 1))
    for x in range(size):
        g.putpixel((x, 0), lerp(c1, c2, x / (size - 1)))
    return g.resize((size, size))


def v_gradient(size, c1, c2):
    g = Image.new("RGB", (1, size))
    for y in range(size):
        g.putpixel((0, y), lerp(c1, c2, y / (size - 1)))
    return g.resize((size, size))


def render(px, simple):
    S = px * 8 if px < 64 else 1024  # supersample
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # Background tile
    tile = Image.new("L", (S, S), 0)
    m = 0.02 * S if simple else 0.04 * S
    ImageDraw.Draw(tile).rounded_rectangle([m, m, S - m, S - m], radius=0.22 * S, fill=255)
    img.paste(v_gradient(S, (30, 34, 68), (10, 11, 26)), (0, 0), tile)

    # Neon foreground (screen outline + chevrons), horizontal cyan -> magenta
    fg = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(fg)
    stroke = int((0.085 if simple else 0.05) * S)
    cx, cy = S / 2, S / 2
    sw, sh = (0.38 if simple else 0.40) * S, (0.285 if simple else 0.30) * S  # 4:3 screen
    d.rounded_rectangle([cx - sw / 2, cy - sh / 2, cx + sw / 2, cy + sh / 2],
                        radius=0.05 * S, outline=255, width=stroke)
    ch = (0.17 if simple else 0.15) * S   # chevron half-height
    cw = (0.10 if simple else 0.09) * S   # chevron depth
    for side in (-1, 1):
        tip_x = cx + side * (sw / 2 + (0.175 if simple else 0.17) * S)
        base_x = tip_x - side * cw
        pts = [(base_x, cy - ch), (tip_x, cy), (base_x, cy + ch)]
        d.line(pts, fill=255, width=stroke, joint="curve")
        for p in (pts[0], pts[2]):  # round caps
            r = stroke / 2
            d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=255)

    grad = h_gradient(S, CYAN, MAGENTA)
    # glow under the neon strokes
    glow = fg.filter(ImageFilter.GaussianBlur(S * (0.03 if simple else 0.045)))
    glow = glow.point(lambda v: int(v * 0.85))
    glow_layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    glow_layer.paste(grad, (0, 0), glow)
    img = Image.alpha_composite(img, Image.composite(glow_layer, Image.new("RGBA", (S, S)), tile))
    neon = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    neon.paste(grad, (0, 0), fg)
    img = Image.alpha_composite(img, neon)

    # White crosshair in the screen
    cr = Image.new("L", (S, S), 0)
    c = ImageDraw.Draw(cr)
    if simple:
        r = 0.05 * S
        c.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    else:
        r = 0.075 * S
        w = int(0.022 * S)
        c.ellipse([cx - r, cy - r, cx + r, cy + r], outline=255, width=w)
        t0, t1 = 0.035 * S, 0.115 * S
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            c.line([(cx + dx * t0, cy + dy * t0), (cx + dx * t1, cy + dy * t1)], fill=255, width=w)
        dot = 0.016 * S
        c.ellipse([cx - dot, cy - dot, cx + dot, cy + dot], fill=255)
    white = Image.new("RGBA", (S, S), (255, 255, 255, 255))
    img = Image.alpha_composite(img, Image.composite(white, Image.new("RGBA", (S, S)), cr))

    return img.resize((px, px), Image.LANCZOS)


def main():
    sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    images = [render(s, simple=s <= 32) for s in sizes]
    big = images[-1]
    ico = os.path.join(ROOT, "src", "app.ico")
    big.save(ico, format="ICO", sizes=[(s, s) for s in sizes], append_images=images[:-1])

    # preview sheet
    pad = 24
    w = sum(s for s in (16, 24, 32, 48, 256)) + pad * 6
    sheet = Image.new("RGBA", (w, 256 + pad * 2), (240, 240, 240, 255))
    dark = Image.new("RGBA", (w, 256 + pad * 2), (32, 32, 32, 255))
    x = pad
    for s in (256, 48, 32, 24, 16):
        im = images[sizes.index(s)]
        y = pad + (256 - s) // 2
        sheet.alpha_composite(im, (x, y))
        dark.alpha_composite(im, (x, y))
        x += s + pad
    out = Image.new("RGBA", (w, (256 + pad * 2) * 2))
    out.paste(sheet, (0, 0))
    out.paste(dark, (0, 256 + pad * 2))
    docs = os.path.join(ROOT, "docs")
    os.makedirs(docs, exist_ok=True)
    out.save(os.path.join(docs, "icon_preview.png"))
    big.save(os.path.join(docs, "logo.png"))
    print("wrote", ico)


if __name__ == "__main__":
    main()
