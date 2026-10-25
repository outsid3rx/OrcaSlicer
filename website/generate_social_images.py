"""Regenerate the checked-in 1200×630 social cards (optional tooling: Pillow)."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ASSETS = Path(__file__).resolve().parent / 'dist/assets'
YELLOW, INK, WHITE = '#e6e51e', '#000000', '#ffffff'


def font(name, size):
    return ImageFont.truetype(str(ASSETS / name), size)


def draw_logo(image, x, y, size):
    """Use the shared PNG export of favicon.svg for both social cards."""
    logo = Image.open(ASSETS / 'logo.png').convert('RGBA')
    logo = logo.resize((size, size), Image.Resampling.LANCZOS)
    image.paste(logo, (x, y), logo)


def draw_card(language, lines, subtitle, label):
    image = Image.new('RGB', (1200, 630), '#f2f2f2')
    draw = ImageDraw.Draw(image)
    for x in range(14, 1200, 20):
        for y in range(14, 630, 20):
            draw.ellipse((x, y, x + 1, y + 1), fill='#d7d7d7')
    draw.rounded_rectangle((35, 35, 1165, 595), radius=38, fill=WHITE)
    draw_logo(image, 68, 68, 56)
    draw.text((139, 70), 'orca / agent', font=font('inter-semibold.ttf', 35), fill=INK)
    small = font('inter-semibold.ttf', 16)
    label_width = draw.textbbox((0, 0), label, font=small)[2]
    draw.rounded_rectangle((1128 - label_width - 34, 76, 1130, 115), radius=20, outline='#e6e6e6', width=2)
    draw.text((1111 - label_width, 86), label, font=small, fill=INK)
    size = 79
    while max(draw.textbbox((0, 0), line, font=font('nunito-extra-bold.ttf', size))[2] for line in lines) > 825:
        size -= 1
    headline = font('nunito-extra-bold.ttf', size)
    draw.text((70, 180), lines[0], font=headline, fill=INK)
    draw.text((70, 270), lines[1], font=headline, fill=INK)
    draw.text((73, 403), subtitle, font=font('inter-regular.ttf', 26), fill=INK)
    address = 'outsid3rx.github.io/OrcaSlicer-Agent-control/'
    address_font = font('inter-semibold.ttf', 22)
    address_width = draw.textbbox((0, 0), address, font=address_font)[2]
    draw.rounded_rectangle((73, 495, 123 + address_width, 547), radius=27, fill=YELLOW)
    draw.text((98, 506), address, font=address_font, fill=INK)
    draw.text((158 + address_width, 512), 'macOS Apple Silicon', font=font('inter-regular.ttf', 18), fill=INK)
    draw_logo(image, 963, 219, 164)
    draw.text((988, 421), 'MCP', font=font('inter-semibold.ttf', 32), fill=INK)
    image.save(ASSETS / f'og-{language}.png', optimize=True)


if __name__ == '__main__':
    draw_card('en', ('Your slicer.', 'Now with AI agents.'),
              'Prepare models. Apply recipes. Review slices.', 'OPEN SOURCE · LOCAL MCP')
    draw_card('ru', ('Ваш слайсер.', 'Теперь с ИИ-агентом.'),
              'Модели. Настройки. Проверка нарезки.', 'ОТКРЫТЫЙ КОД · ЛОКАЛЬНЫЙ MCP')
