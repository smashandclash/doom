"""Bakes the Smash&Clash deck into a DOOM PWAD: every character's art, the effect cards, the card back and the logo
become DOOM patches in the IWAD's own 256-colour palette, so the engine draws them like any other graphic.

  python tools/bake.py <iwad> build/deck.json web/smashandclash.wad

Needs Pillow (with WebP). Downloads are cached in build/cache/.
"""
import hashlib
import io
import json
import os
import struct
import sys
import urllib.request

from PIL import Image, ImageDraw, ImageFont

iwad_path, deck_path, out_path = sys.argv[1:4]
here = os.path.dirname(os.path.abspath(__file__))
cache = os.path.join(here, '..', 'build', 'cache')
os.makedirs(cache, exist_ok=True)


def read_wad(path):
    data = open(path, 'rb').read()
    count, offset = struct.unpack('<ii', data[4:12])
    lumps = {}
    for i in range(count):
        pos, size = struct.unpack('<ii', data[offset + i * 16:offset + i * 16 + 8])
        name = data[offset + i * 16 + 8:offset + i * 16 + 16].rstrip(b'\0').decode().upper()
        lumps.setdefault(name, data[pos:pos + size])
    return lumps


def fetch(url):
    path = os.path.join(cache, hashlib.sha1(url.encode()).hexdigest()[:16] + os.path.splitext(url)[1])
    if not os.path.exists(path):
        req = urllib.request.Request(url, headers={'User-Agent': 'smashandclash-doom-build/0.1.0'})
        with urllib.request.urlopen(req, timeout=60) as r:
            open(path, 'wb').write(r.read())
    return path


playpal = read_wad(iwad_path)['PLAYPAL'][:768]
PAL = [tuple(playpal[i * 3:i * 3 + 3]) for i in range(256)]


def nearest(rgb, memo={}):
    if rgb in memo:
        return memo[rgb]
    r, g, b = rgb
    best, bd = 0, 1 << 30
    for i, (pr, pg, pb) in enumerate(PAL):
        rm = (r + pr) / 2  # "redmean": a cheap perceptual distance
        d = (2 + rm / 256) * (r - pr) ** 2 + 4 * (g - pg) ** 2 + (2 + (255 - rm) / 256) * (b - pb) ** 2
        if d < bd:
            best, bd = i, d
    memo[rgb] = best
    return best


def to_patch(img, left=0, top=0):
    """An RGBA image as a DOOM patch: columns of posts, transparent where alpha < 128."""
    img = img.convert('RGBA')
    w, h = img.size
    px = img.load()
    cols = []
    for x in range(w):
        col = bytearray()
        y = 0
        while y < h:
            if px[x, y][3] < 128:
                y += 1
                continue
            start = y
            run = bytearray()
            while y < h and px[x, y][3] >= 128 and len(run) < 128:
                run.append(nearest(px[x, y][:3]))
                y += 1
            col += bytes([start, len(run), 0]) + run + b'\0'
        col.append(255)
        cols.append(bytes(col))
    head = struct.pack('<hhhh', w, h, left, top)
    offs, body, pos = [], bytearray(), 8 + 4 * w
    for c in cols:
        offs.append(pos)
        body += c
        pos += len(c)
    return head + b''.join(struct.pack('<i', o) for o in offs) + bytes(body)


def fit(img, w, h):
    """Scale an image to fit w x h, keeping its aspect, centred on a transparent canvas."""
    img = img.convert('RGBA')
    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)
    s = min(w / img.width, h / img.height)
    img = img.resize((max(1, round(img.width * s)), max(1, round(img.height * s))), Image.LANCZOS)
    canvas = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    canvas.paste(img, ((w - img.width) // 2, (h - img.height) // 2), img)
    return canvas


def card_face(img, w, h):
    """A full card face (effect cards, the back) at w x h, opaque."""
    return img.convert('RGBA').resize((w, h), Image.LANCZOS)


deck = json.load(open(deck_path, encoding='utf-8'))
cards, kit = deck['cards'], deck['kit']
lumps = []

# Characters: the frameless art at four sizes (wall monitors, board, hand, preview).
SIZES = {'SCT': 14, 'SCS': 24, 'SCM': 28, 'SCL': 56}
for card in cards:
    if card['kind'] == 'character':
        art = Image.open(fetch(card['art']))
        for prefix, size in SIZES.items():
            lumps.append((f"{prefix}{card['id']:02d}", to_patch(fit(art, size, size))))
    else:
        face = Image.open(fetch(card['image']))
        lumps.append((f"SCEM{card['id']}", to_patch(card_face(face, 34, 46))))
        lumps.append((f"SCEL{card['id']}", to_patch(card_face(face, 62, 85))))
        lumps.append((f"SCES{card['id']}", to_patch(card_face(face, 22, 30))))

back = Image.open(fetch(kit['card']['back']))
lumps.append(('SCBKM', to_patch(card_face(back, 34, 46))))
lumps.append(('SCBKS', to_patch(card_face(back, 11, 15))))
lumps.append(('SCBKL', to_patch(card_face(back, 62, 85))))

logo = Image.open(fetch(kit['brand']['logoPng']))
for name, width in (('SCLOGO', 200), ('SCLOGOM', 120), ('SCLOGOS', 72)):
    img = logo.convert('RGBA')
    img = img.crop(img.getbbox())
    h = round(img.height * width / img.width)
    lumps.append((name, to_patch(img.resize((width, h), Image.LANCZOS))))

# The main menu's new line, in the game's display face (Luckiest Guy) and DOOM's menu reds.
font = ImageFont.truetype(fetch(kit['fonts']['display']['url']), 17)
text = 'SMASH&CLASH'
x0, y0, x1, y1 = font.getbbox(text)
w, h = x1 - x0 + 4, y1 - y0 + 4
mask = Image.new('L', (w, h), 0)
ImageDraw.Draw(mask).text((2 - x0, 2 - y0), text, font=font, fill=255)
menu = Image.new('RGBA', (w, h), (0, 0, 0, 0))
outline = mask.copy()
for dx in (-1, 0, 1):
    for dy in (-1, 0, 1, 2):
        outline.paste(255, (0, 0), mask.transform(mask.size, Image.AFFINE, (1, 0, -dx, 0, 1, -dy)))
menu.paste((40, 0, 0, 255), (0, 0), outline)
grad = Image.new('RGBA', (w, h))
for y in range(h):  # bright red at the top to dark red at the bottom, like M_NGAME
    t = y / max(1, h - 1)
    grad.paste((int(255 - 120 * t), int(60 - 50 * t), int(40 - 30 * t), 255), (0, y, w, y + 1))
menu.paste(grad, (0, 0), mask)
lumps.append(('M_SNC', to_patch(menu)))

# A PWAD: header, lumps, directory.
body, directory = bytearray(), bytearray()
pos = 12
for name, data in lumps:
    directory += struct.pack('<ii', pos, len(data)) + name.encode().ljust(8, b'\0')
    body += data
    pos += len(data)
with open(out_path, 'wb') as f:
    f.write(b'PWAD' + struct.pack('<ii', len(lumps), pos) + body + directory)
print(f'{out_path}: {len(lumps)} lumps, {pos + len(directory)} bytes')
