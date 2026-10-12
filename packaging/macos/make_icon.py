#!/usr/bin/env python3
"""Generate imgview's macOS app icon (imgview.icns) with Pillow.

    python3 make_icon.py [output.icns]

The design: a small landscape drawn as large, visible pixels inside a frame,
on a dark rounded-square tile, a nod to imgview's true-pixel zoom. Pillow
writes every icon size macOS needs (16-1024 px, incl. @2x) from the 1024 px
master.
"""
import sys

from PIL import Image, ImageDraw

SIZE = 1024
# macOS icon grid: the tile is 824 px inside the 1024 px canvas.
TILE_MARGIN = 100
TILE_RADIUS = 185

# 16 x 12 "pixel art" landscape: sky, sun, two mountains with snow, ground.
SKY_TOP, SKY_BOTTOM = (60, 110, 190), (150, 195, 235)
SUN, SNOW = (255, 210, 80), (245, 248, 252)
MOUNTAIN_FAR, MOUNTAIN_NEAR = (90, 105, 140), (55, 70, 100)
GROUND_A, GROUND_B = (70, 140, 80), (55, 120, 65)
COLS, ROWS = 16, 12


def landscape():
    px = [[None] * COLS for _ in range(ROWS)]
    for y in range(ROWS):
        t = y / (ROWS - 1)
        sky = tuple(round(a + (b - a) * t) for a, b in zip(SKY_TOP, SKY_BOTTOM))
        for x in range(COLS):
            px[y][x] = sky
    for y, x in [(1, 11), (1, 12), (2, 10), (2, 11), (2, 12), (2, 13),
                 (3, 11), (3, 12)]:
        px[y][x] = SUN
    # Far mountain peaking at (row 4, column 10), near one at (row 3, col 4).
    # Snow covers each mountain's top two rows.
    for peak_row, peak_col, slope, colour in [(4, 10, 0.8, MOUNTAIN_FAR),
                                              (3, 4, 1.0, MOUNTAIN_NEAR)]:
        for x in range(COLS):
            top = peak_row + abs(x - peak_col) * slope
            for y in range(ROWS):
                if y >= top:
                    px[y][x] = SNOW if y < peak_row + 2 else colour
    for y in range(ROWS - 3, ROWS):
        for x in range(COLS):
            px[y][x] = GROUND_A if (x + y) % 2 else GROUND_B
    return px


def draw_icon():
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Dark tile with a subtle vertical gradient.
    tile = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    td = ImageDraw.Draw(tile)
    for y in range(TILE_MARGIN, SIZE - TILE_MARGIN):
        t = (y - TILE_MARGIN) / (SIZE - 2 * TILE_MARGIN)
        c = tuple(round(a + (b - a) * t) for a, b in zip((44, 50, 64), (24, 27, 36)))
        td.line([(TILE_MARGIN, y), (SIZE - TILE_MARGIN, y)], fill=c + (255,))
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [TILE_MARGIN, TILE_MARGIN, SIZE - TILE_MARGIN, SIZE - TILE_MARGIN],
        radius=TILE_RADIUS, fill=255)
    img.paste(tile, (0, 0), mask)

    # Picture frame and the pixel landscape, with a thin grid between pixels.
    cell = 36
    pw, ph = COLS * cell, ROWS * cell
    x0, y0 = (SIZE - pw) // 2, (SIZE - ph) // 2 + 10
    border = 22
    d.rounded_rectangle([x0 - border, y0 - border, x0 + pw + border, y0 + ph + border],
                        radius=18, fill=(235, 238, 244))
    for y, row in enumerate(landscape()):
        for x, c in enumerate(row):
            d.rectangle([x0 + x * cell, y0 + y * cell,
                         x0 + (x + 1) * cell - 1, y0 + (y + 1) * cell - 1], fill=c)
    grid = (0, 0, 0, 40)
    overlay = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    od = ImageDraw.Draw(overlay)
    for x in range(1, COLS):
        od.line([(x0 + x * cell, y0), (x0 + x * cell, y0 + ph - 1)], fill=grid, width=2)
    for y in range(1, ROWS):
        od.line([(x0, y0 + y * cell), (x0 + pw - 1, y0 + y * cell)], fill=grid, width=2)
    img.alpha_composite(overlay)
    return img


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "imgview.icns"
    icon = draw_icon()
    icon.save(out)
    print(f"wrote {out}")
