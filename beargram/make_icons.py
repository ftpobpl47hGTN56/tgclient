"""Regenerate all BearGram app icons from beargram/icon_master.png.

Usage (from the repository root):
    python beargram/make_icons.py

The master is a square RGBA PNG (1024px or larger) with a transparent
background outside the rounded icon shape and no extra margin. Requires Pillow.
"""
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
ART = ROOT / 'Telegram' / 'Resources' / 'art'
MASTER = Image.open(Path(__file__).with_name('icon_master.png')).convert('RGBA')

# The original Telegram assets keep a ~4.5% transparent margin around the shape.
side = MASTER.width
pad = round(side * 0.045)
WITH_MARGIN = Image.new('RGBA', (side + 2 * pad, side + 2 * pad), (0, 0, 0, 0))
WITH_MARGIN.paste(MASTER, (pad, pad))


def save(image, size, name):
    image.resize((size, size), Image.LANCZOS).save(ART / name, optimize=True)


for size in (16, 32, 48, 64, 128, 256, 512):
    save(WITH_MARGIN, size, f'icon{size}.png')
    save(WITH_MARGIN, size * 2, f'icon{size}@2x.png')

save(WITH_MARGIN, 256, 'logo_256.png')
save(MASTER, 256, 'logo_256_no_margin.png')
save(WITH_MARGIN, 1024, 'icon_round512@2x.png')

# Windows executable icon, used by Telegram/Resources/winrc/Telegram.rc.
WITH_MARGIN.resize((256, 256), Image.LANCZOS).save(
    ART / 'icon256.ico',
    sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (256, 256)])

print('BearGram icons written to', ART)
