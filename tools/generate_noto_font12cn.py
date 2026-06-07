#!/usr/bin/env python3
"""
Generate Font12CN C bitmap font table using Noto Sans CJK SC.
Output is compatible with cFONT in lib/Fonts/fonts.h.

Default glyph metrics: 16x21 to match existing Font12CN layout.
"""

import argparse
import os
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont


def load_charset(charset_file: Path, books_dir: Path, extra_chars: str) -> list[str]:
    chars = set()

    if charset_file and charset_file.exists():
        chars.update(list(charset_file.read_text(encoding="utf-8")))

    if books_dir and books_dir.exists():
        for txt in books_dir.glob("*.txt"):
            data = txt.read_bytes()
            for enc in ("utf-8", "gb2312", "gbk"):
                try:
                    text = data.decode(enc)
                    for ch in text:
                        if ord(ch) >= 32:
                            chars.add(ch)
                    break
                except Exception:
                    continue

    chars.update(list(extra_chars))

    # Keep printable ASCII as fallback for mixed rendering in CN font path.
    for code in range(32, 127):
        chars.add(chr(code))

    # Remove control / separator-like chars that cannot be represented as 2-byte index safely.
    chars = {c for c in chars if c not in {"\n", "\r", "\t"}}

    return sorted(chars)


def to_gb2312_index(ch: str) -> tuple[int, int]:
    # Existing renderer expects either single-byte ASCII in index[0], or 2-byte GBK/GB2312 in index[0..1].
    if ord(ch) < 128:
        return ord(ch), 0

    # Try GBK first (superset of GB2312, covers — " " … etc)
    b = ch.encode("gbk", errors="ignore")
    if len(b) == 2:
        return b[0], b[1]

    # Fallback to GB2312
    b = ch.encode("gb2312", errors="ignore")
    if len(b) == 2:
        return b[0], b[1]

    # Character not in GBK/GB2312: skip by signaling invalid marker.
    return -1, -1


def _dilate_1px(img: Image.Image) -> Image.Image:
    """Apply minimal 4-connected dilation on a grayscale image to thicken strokes slightly."""
    pix = img.load()
    w, h = img.size
    # Work on temp copy
    tmp = [[pix[x, y] for y in range(h)] for x in range(w)]
    for x in range(w):
        for y in range(h):
            if tmp[x][y] > 0:
                for dx, dy in ((-1,0),(1,0),(0,-1),(0,1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < w and 0 <= ny < h and pix[nx, ny] == 0:
                        pix[nx, ny] = tmp[x][y] // 2  # half-bright neighbor
    return img


def rasterize_glyph(font: ImageFont.FreeTypeFont, ch: str, width: int, height: int,
                    ascent: int, descent: int) -> bytes:
    """Render a glyph into a bitmap, with supersampling for thicker strokes at small sizes."""
    # Render at 3x internal resolution for anti-aliased scaling.
    scale = 3
    sw = width * scale
    sh = height * scale

    img = Image.new("L", (sw, sh), 0)
    draw = ImageDraw.Draw(img)

    font2 = ImageFont.truetype(font.path, font.size * scale)

    bbox = draw.textbbox((0, 0), ch, font=font2)
    gw = bbox[2] - bbox[0]
    x = (sw - gw) // 2 - bbox[0]
    y = (sh - (ascent + descent) * scale) // 2

    draw.text((x, y), ch, font=font2, fill=255)

    # Light dilation on the high-res image to thicken strokes before scaling down.
    _dilate_1px(img)

    # Scale down with LANCZOS resampling, then threshold to binary.
    small = img.resize((width, height), Image.LANCZOS)
    # Adaptive threshold: any pixel > 1/3 max becomes black.
    small = small.point(lambda v: 1 if v > 80 else 0, mode="1")

    row_bytes = (width + 7) // 8
    out = bytearray()
    pix = small.load()

    for yy in range(height):
        for rb in range(row_bytes):
            val = 0
            for bit in range(8):
                xx = rb * 8 + bit
                if xx < width and pix[xx, yy]:
                    val |= 0x80 >> bit
            out.append(val)

    return bytes(out)


# Map GBK codepoint pair -> new bitmap bytes (currently empty; quotes are rendered from font).
_PUNCT_FIX = {
}

def _fix_punctuation_bitmaps(chars, rows, W, H):
    """Replace bitmap data for punctuation with wider, well-separated variants."""
    mat_bytes = ((W + 7) // 8) * H
    for idx, ch in enumerate(chars):
        i0, i1 = to_gb2312_index(ch)
        key = (i0, i1)
        if key in _PUNCT_FIX:
            new_bmp = _PUNCT_FIX[key]
            if len(new_bmp) == mat_bytes:
                line = rows[idx]
                brace = line.index("{", line.index(",{"))
                hex_str = ",".join(f"0x{b:02X}" for b in new_bmp)
                rows[idx] = line[:brace+1] + hex_str + "}},"
    return rows


def generate_c_file(chars: list[str], font_path: str, pixel_size: int, width: int, height: int, ascii_width: int, out_file: Path):
    font = ImageFont.truetype(font_path, pixel_size)
    ascent, descent = font.getmetrics()
    matrix_bytes = ((width + 7) // 8) * height

    rows = []
    skipped = []

    for ch in chars:
        i0, i1 = to_gb2312_index(ch)
        if i0 < 0:
            skipped.append(ch)
            continue

        bitmap = rasterize_glyph(font, ch, width, height, ascent, descent)
        if len(bitmap) != matrix_bytes:
            skipped.append(ch)
            continue

        comment_char = ch if ch not in {'\\', '"'} else "?"
        bytes_hex = ",".join(f"0x{b:02X}" for b in bitmap)
        rows.append(
            f"/*-- char: {comment_char} --*/\n"
            f"{{{{\"\\x{i0:02X}\\x{i1:02X}\"}},{{{bytes_hex}}}}},"
        )

    # Override punctuation glyphs with wider manually-designed bitmaps
    rows = _fix_punctuation_bitmaps(chars, rows, width, height)

    content = f'''/**
 * Auto-generated by tools/generate_noto_font12cn.py
 * Source font: {font_path}
 * Pixel size: {pixel_size}, Glyph box: {width}x{height}, ASCII width: {ascii_width}
 */
#include "fonts.h"

const CH_CN Font12CN_Table[] = {{
{os.linesep.join(rows)}
}};

cFONT Font12CN = {{
    Font12CN_Table,
    sizeof(Font12CN_Table) / sizeof(CH_CN),
    {ascii_width},
    {width},
    {height},
}};
'''

    out_file.write_text(content, encoding="utf-8")

    print(f"Generated: {out_file}")
    print(f"Glyphs: {len(rows)}, Skipped: {len(skipped)}")
    if skipped:
        print("Skipped sample:", "".join(skipped[:20]))


def main():
    parser = argparse.ArgumentParser(description="Generate Font12CN from Noto Sans CJK SC")
    parser.add_argument("--font", default="/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc")
    parser.add_argument("--pixel-size", type=int, default=18)
    parser.add_argument("--width", type=int, default=18)
    parser.add_argument("--height", type=int, default=21)
    parser.add_argument("--ascii-width", type=int, default=8)
    parser.add_argument("--charset-file", default="assets/font_charset_sc.txt")
    parser.add_argument("--books-dir", default="books")
    parser.add_argument("--extra-chars", default="，。！？：；、“”‘’（）《》【】—…· 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")
    parser.add_argument("--output", default="components/e-Paper/Quectel-Pi-H1/c/lib/Fonts/font12CN.c")
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    charset_file = root / args.charset_file
    books_dir = root / args.books_dir
    out_file = root / args.output

    chars = load_charset(charset_file, books_dir, args.extra_chars)
    generate_c_file(
        chars=chars,
        font_path=args.font,
        pixel_size=args.pixel_size,
        width=args.width,
        height=args.height,
        ascii_width=args.ascii_width,
        out_file=out_file,
    )


if __name__ == "__main__":
    main()
