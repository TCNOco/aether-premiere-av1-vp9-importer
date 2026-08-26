# -*- coding: utf-8 -*-
# Иконка и картинки мастера установки.
#
#   python installer\make-art.py
#
# Значок — готовый рисунок `docs/assets/aether.png`. Скрипт только
# пересобирает из него .ico, BMP мастера и иконки панели, чтобы установщик
# можно было собрать и без правок руками. Исходник править — править PNG.
#
# Нужен Pillow: pip install pillow
import os
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(OUT)
SRC = os.path.join(ROOT, 'docs', 'assets', 'aether.png')
FONT = r'C:\Windows\Fonts\segoeuib.ttf'   # Segoe UI Bold
FONT_R = r'C:\Windows\Fonts\segoeui.ttf'

INK      = (232, 236, 242)
DIM      = (140, 152, 168)
ACCENT   = (94, 160, 255)
BG_TOP   = (24, 28, 38)
BG_BOT   = (14, 17, 24)


def vgrad(size, top, bot):
    w, h = size
    img = Image.new('RGB', size)
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)],
               fill=tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)))
    return img


def fit_font(path, text, target_w, start):
    size = start
    while size > 8:
        f = ImageFont.truetype(path, size)
        if f.getbbox(text)[2] <= target_w:
            return f
        size -= 1
    return ImageFont.truetype(path, 8)


def scaled(src, size):
    return src.resize((size, size), Image.LANCZOS)


master = Image.open(SRC).convert('RGBA')

# ---------------------------------------------------------------- иконка
icon = scaled(master, 256)
icon.save(os.path.join(OUT, 'aether.ico'),
          sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)])
print('ok aether.ico')

# ------------------------------------------------- большая картинка мастера
# 164x314 - размер, который Inno ждёт при 100% масштабе
W, H = 164, 314
big = vgrad((W, H), BG_TOP, BG_BOT)
d = ImageDraw.Draw(big)

badge = scaled(master, 64)
big.paste(badge, (int((W - 64) / 2), 40), badge)

f1 = fit_font(FONT, 'AV1', 110, 48)
f2 = fit_font(FONT, 'VP9', 110, 48)
fs = fit_font(FONT_R, 'I M P O R T E R', 130, 14)

def centre(text, font, y, fill):
    bb = font.getbbox(text)
    d.text(((W - (bb[2] - bb[0])) / 2 - bb[0], y), text, font=font, fill=fill)

centre('AV1', f1, 132, INK)
centre('VP9', f2, 180, ACCENT)

# тонкая линия-акцент, и под ней одно слово: картинка общая для обоих
# языков установщика, поэтому подпись без русских слов
d.line([(34, 240), (W - 34, 240)], fill=(52, 62, 80), width=1)
centre('I M P O R T E R', fs, 252, DIM)

big.save(os.path.join(OUT, 'wizard-large.bmp'), 'BMP')
print('ok wizard-large.bmp')

# ------------------------------------------------- маленькая картинка мастера
SW, SH = 55, 58
small = vgrad((SW, SH), BG_TOP, BG_BOT)
b = scaled(master, 40)
small.paste(b, (int((SW - 40) / 2), int((SH - 40) / 2)), b)
small.save(os.path.join(OUT, 'wizard-small.bmp'), 'BMP')
print('ok wizard-small.bmp')

# ------------------------------------------------- панель Premiere
cep = os.path.join(ROOT, 'cep', 'client')
os.makedirs(os.path.join(cep, 'icons'), exist_ok=True)
scaled(master, 64).save(os.path.join(cep, 'icon.png'), 'PNG', optimize=True)
print('ok cep/client/icon.png')
scaled(master, 23).save(os.path.join(cep, 'icons', 'icon.png'), 'PNG', optimize=True)
print('ok cep/client/icons/icon.png')
