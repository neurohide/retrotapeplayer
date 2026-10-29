#!/usr/bin/env python3
"""Converts a photo into an indexed image for main.c: the cassette screen's photos
(with pre-rotated hubs) and the splash.

    sips -s format bmp assets/tdk-d46.jpg --out /tmp/tdk-d46.bmp
    python3 tools/cassette_image.py tdk-d46 /tmp/tdk-d46.bmp main/cassette-tdk-d46.bin
    sips -Z 1920 -s format bmp assets/sony-hf60.jpg --out /tmp/sony-hf60.bmp
    python3 tools/cassette_image.py sony-hf60 /tmp/sony-hf60.bmp main/cassette-sony-hf60.bin
    sips -s format bmp assets/splash.jpeg --out /tmp/splash.bmp
    python3 tools/cassette_image.py splash /tmp/splash.bmp main/splash.bin

The input is the photo as a 24-bit BMP at the size given in PHOTOS; the crop and
hub positions there are measured on it. Only the standard library is needed.

Output, all integers little-endian uint16:
    first_index, colors, width, height, hub_size, frames,
    left_x, left_y, right_x, right_y     top-left of each hub square in the image
    colors x RGB565                      palette entries first_index onwards
    width x height bytes                 the image as palette indices; 255 = transparent
    frames x (left, right) x hub_size^2  each hub turned anticlockwise by
                                         360 / frames per frame (none without hubs)
"""
import math
import struct
import sys

CASSETTE_SIZE = (256, 164)
PHOTOS = {
    # 1920 x 1329. The hub radius is just outside the white sprocket rim.
    'tdk-d46': {'crop': (82, 94, 1754, 1122), 'size': CASSETTE_SIZE,
                'hubs': ((575.8, 606.8), (1338.8, 600.6)), 'radius': 194},
    # 1920 x 1230 (scaled from 3131 x 2006), already cropped to the shell. Centres are
    # on the chrome spline ring; the radius covers the grey hub inside the window.
    'sony-hf60': {'crop': (0, 0, 1920, 1230), 'size': CASSETTE_SIZE,
                  'hubs': ((557.0, 563.0), (1364.5, 561.0)), 'radius': 184},
    # 1408 x 768: the rounded icon. Its white surround becomes transparent so the
    # screen background shows through.
    'splash': {'crop': (396, 80, 614, 614), 'size': (200, 200), 'white_transparent': 235},
}
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


def scale_photo(rows, crop, width, height, scale_x, scale_y):
    """Area-averages the cropped photo down to width x height."""
    image = []
    for y in range(height):
        top, bottom = crop[1] + round(y * scale_y), crop[1] + round((y + 1) * scale_y)
        line = []
        for x in range(width):
            left, right = crop[0] + round(x * scale_x), crop[0] + round((x + 1) * scale_x)
            total = [0, 0, 0]
            for source in rows[top:bottom]:
                for pixel in source[left:right]:
                    for i in range(3):
                        total[i] += pixel[i]
            count = (bottom - top) * (right - left)
            line.append(tuple(value / count for value in total))
        image.append(line)
    return image


def render_hub(rows, crop, centre, hub_radius, scale_x, scale_y, angle, size, corner):
    """One hub square, rotated anticlockwise on screen; None outside the reel."""
    # Hub centre and radius in output pixels.
    centre_x = (centre[0] - crop[0]) / scale_x
    centre_y = (centre[1] - crop[1]) / scale_y
    radius = hub_radius / scale_x
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
    if len(sys.argv) != 4 or sys.argv[1] not in PHOTOS:
        sys.exit(f'usage: {sys.argv[0]} {{{",".join(PHOTOS)}}} photo.bmp output.bin')
    photo, source, target = PHOTOS[sys.argv[1]], sys.argv[2], sys.argv[3]
    crop, (width, height) = photo['crop'], photo['size']
    _, _, rows = read_bmp(source)
    scale_x, scale_y = crop[2] / width, crop[3] / height
    image = scale_photo(rows, crop, width, height, scale_x, scale_y)
    if 'white_transparent' in photo:
        image = [[None if min(pixel) > photo['white_transparent'] else pixel for pixel in line]
                 for line in image]

    size, corners, frames = 0, [(0, 0), (0, 0)], []
    if 'hubs' in photo:
        hubs, hub_radius = photo['hubs'], photo['radius']
        radius = math.ceil(hub_radius / scale_x)
        size = 2 * radius + 1
        corners = [(round((x - crop[0]) / scale_x) - radius, round((y - crop[1]) / scale_y) - radius)
                   for x, y in hubs]
        for frame in range(FRAMES):
            angle = 2 * math.pi * frame / FRAMES
            frames.append([render_hub(rows, crop, centre, hub_radius, scale_x, scale_y, angle, size,
                                      corner)
                           for centre, corner in zip(hubs, corners)])

    pixels = [pixel for line in image for pixel in line if pixel]
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

    output = struct.pack('<10H', FIRST_INDEX, len(palette), width, height, size, len(frames),
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
