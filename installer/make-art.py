# -*- coding: utf-8 -*-
# Иконка и картинки мастера установки.
#
#   python installer\make-art.py
#
# Рисуются кодом, а не лежат в репозитории неизвестно откуда: их видно, можно
# поправить и пересобрать. Готовые файлы при этом лежат рядом — собрать
# установщик должно быть можно и без Python.
#
# Нужен Pillow: pip install pillow
import os
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.dirname(os.path.abspath(__file__))
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


def play_badge(size, pad_ratio=0.14):
    """Квадратный значок: скруглённый корпус и треугольник воспроизведения.
    Текст на 16 пикселях не читается, а треугольник узнаётся всегда."""
    s = size
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    pad = max(1, int(s * pad_ratio))
    r = max(2, int(s * 0.22))
    d.rounded_rectangle([pad, pad, s - pad - 1, s - pad - 1], radius=r,
                        fill=(20, 24, 33, 255), outline=(58, 68, 86, 255),
                        width=max(1, s // 64))

    # треугольник по центру, слегка сдвинут вправо для оптического баланса
    cx, cy = s * 0.52, s * 0.5
    hh = s * 0.20
    ww = s * 0.34
    d.polygon([(cx - ww * 0.45, cy - hh), (cx - ww * 0.45, cy + hh), (cx + ww * 0.55, cy)],
              fill=ACCENT + (255,))
    return img


def fit_font(path, text, target_w, start):
    size = start
    while size > 8:
        f = ImageFont.truetype(path, size)
        if f.getbbox(text)[2] <= target_w:
            return f
        size -= 1
    return ImageFont.truetype(path, 8)


# ---------------------------------------------------------------- иконка
icon = play_badge(256)
icon.save(os.path.join(OUT, 'av1importer.ico'),
          sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)])
print('ok av1importer.ico')

# ------------------------------------------------- большая картинка мастера
# 164x314 - размер, который Inno ждёт при 100% масштабе
W, H = 164, 314
big = vgrad((W, H), BG_TOP, BG_BOT)
d = ImageDraw.Draw(big)

badge = play_badge(64).resize((64, 64), Image.LANCZOS)
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
b = play_badge(44).resize((44, 44), Image.LANCZOS)
small.paste(b, (int((SW - 44) / 2), int((SH - 44) / 2)), b)
small.save(os.path.join(OUT, 'wizard-small.bmp'), 'BMP')
print('ok wizard-small.bmp')
