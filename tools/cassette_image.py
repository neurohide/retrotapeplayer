#!/usr/bin/env python3
"""Converts the cassette photo into main/cassette.bin for the cassette screen.

    sips -s format bmp assets/tdk-d46.jpg --out /tmp/tdk-d46.bmp
    python3 tools/cassette_image.py /tmp/tdk-d46.bmp main/cassette.bin

The input is the 1920 x 1329 photo as a 24-bit BMP; the crop and hub positions
below are measured on it. Only the standard library is needed.

Output, all integers little-endian uint16:
    first_index, colors, width, height, hub_size, frames,
    left_x, left_y, right_x, right_y     top-left of each hub square in the image
    colors x RGB565                      palette entries first_index onwards
    width x height bytes                 the cassette as palette indices
    frames x (left, right) x hub_size^2  each hub turned anticlockwise by
                                         360 / frames per frame; 255 = transparent
"""
import math
import struct
import sys

CROP = (82, 94, 1754, 1122)  # x, y, width, height of the cassette shell
HUBS = ((575.8, 606.8), (1338.8, 600.6))  # reel centres
HUB_RADIUS = 194  # just outside the white sprocket rim
WIDTH, HEIGHT = 256, 164
FRAMES = 24
FIRST_INDEX = 7  # main.c's UI colours come first
TRANSPARENT = 255
SUBSAMPLES = 5


def read_bmp(path):
    data = open(path, 'rb').read()
    offset = struct.unpack_from('<I', data, 10)[0]
    width, height = struct.unpack_from('<ii', data, 18)
    if struct.unpack_from('<H', data, 28)[0] != 24:
        sys.exit('expected a 24-bit BMP')
    top_down = height < 0
    height = abs(height)
    stride = (width * 3 + 3) & ~3
    rows = []
    for row in range(height):
        base = offset + (row if top_down else height - 1 - row) * stride
        line = data[base:base + width * 3]
        rows.append([(line[i + 2], line[i + 1], line[i]) for i in range(0, width * 3, 3)])
    return width, height, rows


def bilinear(rows, x, y):
    x0, y0 = int(x), int(y)
    fx, fy = x - x0, y - y0
    a, b = rows[y0][x0], rows[y0][x0 + 1]
    c, d = rows[y0 + 1][x0], rows[y0 + 1][x0 + 1]
    return tuple((a[i] * (1 - fx) + b[i] * fx) * (1 - fy) + (c[i] * (1 - fx) + d[i] * fx) * fy
                 for i in range(3))


def scale_cassette(rows, scale_x, scale_y):
    """Area-averages the cropped shell down to WIDTH x HEIGHT."""
    image = []
    for y in range(HEIGHT):
        top, bottom = CROP[1] + round(y * scale_y), CROP[1] + round((y + 1) * scale_y)
        line = []
        for x in range(WIDTH):
            left, right = CROP[0] + round(x * scale_x), CROP[0] + round((x + 1) * scale_x)
            total = [0, 0, 0]
            for source in rows[top:bottom]:
                for pixel in source[left:right]:
                    for i in range(3):
                        total[i] += pixel[i]
            count = (bottom - top) * (right - left)
            line.append(tuple(value / count for value in total))
        image.append(line)
    return image


def render_hub(rows, centre, scale_x, scale_y, angle, size, corner):
    """One hub square, rotated anticlockwise on screen; None outside the reel."""
    # Hub centre and radius in output pixels.
    centre_x = (centre[0] - CROP[0]) / scale_x
    centre_y = (centre[1] - CROP[1]) / scale_y
    radius = HUB_RADIUS / scale_x
    cos, sin = math.cos(angle), math.sin(angle)
    square = []
    for j in range(size):
        for i in range(size):
            x, y = corner[0] + i + 0.5, corner[1] + j + 0.5
            if math.hypot(x - centre_x, y - centre_y) > radius:
                square.append(None)
                continue
            total = [0, 0, 0]
            for sub_y in range(SUBSAMPLES):
                for sub_x in range(SUBSAMPLES):
                    dx = x - 0.5 + (sub_x + 0.5) / SUBSAMPLES - centre_x
                    dy = y - 0.5 + (sub_y + 0.5) / SUBSAMPLES - centre_y
                    # Screen y points down, so this turns the picture anticlockwise.
                    source_x = centre[0] + (dx * cos - dy * sin) * scale_x
                    source_y = centre[1] + (dx * sin + dy * cos) * scale_y
                    pixel = bilinear(rows, source_x, source_y)
                    for k in range(3):
                        total[k] += pixel[k]
            square.append(tuple(value / SUBSAMPLES ** 2 for value in total))
    return square


def rgb565(pixel):
    r, g, b = (min(255, max(0, round(value))) for value in pixel)
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def expand(color):
    return ((color >> 11) * 255 // 31, (color >> 5 & 63) * 255 // 63, (color & 31) * 255 // 31)


def median_cut(counts, colors):
    """Weighted median cut over RGB565 colour counts; returns RGB565 palette entries."""
    boxes = [[(expand(color), count) for color, count in counts.items()]]
    while len(boxes) < colors:
        def spread(box):
            ranges = [max(c[0][i] for c in box) - min(c[0][i] for c in box) for i in range(3)]
            return max(ranges) * sum(c[1] for c in box) ** 0.5, ranges.index(max(ranges))
        splittable = [box for box in boxes if len(box) > 1]
        if not splittable:
            break
        box = max(splittable, key=lambda b: spread(b)[0])
        channel = spread(box)[1]
        box.sort(key=lambda c: c[0][channel])
        half = sum(c[1] for c in box) / 2
        running = 0
        for split, entry in enumerate(box):
            running += entry[1]
            if running >= half:
                break
        split = min(max(split, 1), len(box) - 1)
        boxes.remove(box)
        boxes += [box[:split], box[split:]]
    palette = []
    for box in boxes:
        weight = sum(c[1] for c in box)
        palette.append(rgb565([sum(c[0][i] * c[1] for c in box) / weight for i in range(3)]))
    return palette


def main():
    source, target = sys.argv[1], sys.argv[2]
    _, _, rows = read_bmp(source)
    scale_x, scale_y = CROP[2] / WIDTH, CROP[3] / HEIGHT
    image = scale_cassette(rows, scale_x, scale_y)

    radius = math.ceil(HUB_RADIUS / scale_x)
    size = 2 * radius + 1
    corners = [(round((x - CROP[0]) / scale_x) - radius, round((y - CROP[1]) / scale_y) - radius)
               for x, y in HUBS]
    frames = []
    for frame in range(FRAMES):
        angle = 2 * math.pi * frame / FRAMES
        frames.append([render_hub(rows, centre, scale_x, scale_y, angle, size, corner)
                       for centre, corner in zip(HUBS, corners)])

    pixels = [pixel for line in image for pixel in line]
    pixels += [pixel for frame in frames for hub in frame for pixel in hub if pixel]
    counts = {}
    for pixel in pixels:
        color = rgb565(pixel)
        counts[color] = counts.get(color, 0) + 1
    palette = median_cut(counts, TRANSPARENT - FIRST_INDEX)
    expanded = [expand(color) for color in palette]
    nearest = {}

    def index(pixel):
        if pixel is None:
            return TRANSPARENT
        color = rgb565(pixel)
        if color not in nearest:
            rgb = expand(color)
            nearest[color] = FIRST_INDEX + min(
                range(len(expanded)),
                key=lambda i: sum((a - b) ** 2 for a, b in zip(rgb, expanded[i])))
        return nearest[color]

    output = struct.pack('<10H', FIRST_INDEX, len(palette), WIDTH, HEIGHT, size, FRAMES,
                         *corners[0], *corners[1])
    output += struct.pack(f'<{len(palette)}H', *palette)
    output += bytes(index(pixel) for line in image for pixel in line)
    for frame in frames:
        for hub in frame:
            output += bytes(index(pixel) for pixel in hub)
    open(target, 'wb').write(output)
    print(f'{target}: {len(output)} bytes, {len(palette)} colours, hubs {size} px at {corners}')


if __name__ == '__main__':
    main()
