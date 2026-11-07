# -*- coding: utf-8 -*-
# 从 Windows 宋体生成 16x16 点阵中文字库 (GB2312) + Unicode->GBK 反查表
# 输出: font_cn.h (嵌入固件)
import struct
from PIL import Image, ImageFont, ImageDraw

W, H = 16, 16
FONT_PATH = r"C:\Windows\Fonts\simsun.ttc"

# ---- 1. 收集 GB2312 字符 (区码 0xA1-0xF7, 位码 0xA1-0xFE) ----
chars = {}   # (hi,lo) -> unicode char
for hi in range(0xA1, 0xF8):
    for lo in range(0xA1, 0xFF):
        try:
            ch = bytes([hi, lo]).decode("gb2312")
            chars[(hi, lo)] = ch
        except Exception:
            pass
print("GB2312 chars:", len(chars))

font = ImageFont.truetype(FONT_PATH, H, index=0)

def render(ch, width):
    """渲染单个字符为 1bpp 位图, 返回 bytes (每行 width/8 字节, 共 H 行)"""
    img = Image.new("1", (width, H), 0)
    d = ImageDraw.Draw(img)
    d.text((0, 0), ch, fill=1, font=font)
    data = bytearray()
    px = img.load()
    for y in range(H):
        b = 0
        bit = 0x80
        out = 0
        for x in range(width):
            if px[x, y]:
                out |= bit
            bit >>= 1
            if bit == 0:
                bit = 0x80
        # 逐行按 width 打包
        row = 0
        for x in range(width):
            row = (row << 1) | (1 if px[x, y] else 0)
        nbytes = width // 8
        rowbytes = bytearray(nbytes)
        for i in range(nbytes):
            rowbytes[i] = (row >> (8 * (nbytes - 1 - i))) & 0xFF
        data.extend(rowbytes)
    return bytes(data)

# ---- 2. ASCII 0x20-0x7E (半角, 8x16, 存于 32B 槽前 16B) ----
ascii_glyphs = {}
for c in range(0x20, 0x7F):
    ascii_glyphs[c] = render(chr(c), 8)

# ---- 3. 汉字 16x16, 按 (hi-0xA1)*94+(lo-0xA1) 索引 ----
ROWS = 0xF7 - 0xA1 + 1  # 87
SLOTS = ROWS * 94       # 8178
glyphs = bytearray(SLOTS * 32)
defined = 0
for (hi, lo), ch in chars.items():
    g = render(ch, 16)
    idx = ((hi - 0xA1) * 94 + (lo - 0xA1)) * 32
    glyphs[idx:idx + 32] = g
    defined += 1
print("rendered hanzi:", defined)

# ---- 4. unicode -> gbk 反查表 (按 unicode 排序, uint32: uni<<16|gbk) ----
uni2gbk = []
for (hi, lo), ch in chars.items():
    uni = ord(ch)
    uni2gbk.append((uni << 16) | (hi << 8) | lo)
uni2gbk.sort()

# ---- 5. 输出 C 头文件 ----
out = []
out.append("// 自动生成: 宋体 16x16 点阵字库 (GB2312) + Unicode->GBK 反查表")
out.append("#pragma once")
out.append("#include <stdint.h>")
out.append("#define GB_ROWS %d" % ROWS)
out.append("#define GB_SLOTS %d" % SLOTS)
out.append("")
out.append("// ASCII 0x20-0x7E, 8x16 (占32B槽, 有效数据在前16B)")
out.append("static const uint8_t ascii_font16[%d][32] PROGMEM = {" % 95)
for c in range(0x20, 0x7F):
    g = ascii_glyphs[c] + b"\x00" * 16
    out.append("{" + ",".join("0x%02X" % b for b in g) + "},")
out.append("};")
out.append("")
out.append("// GB2312 字形, 索引 (hi-0xA1)*94+(lo-0xA1), 16x16=32B")
out.append("static const uint8_t gbk_font16[GB_SLOTS][32] PROGMEM = {")
for i in range(SLOTS):
    g = glyphs[i * 32:(i + 1) * 32]
    if i % 16 == 0:
        out.append("/*%04d*/" % i + "{" + ",".join("0x%02X" % b for b in g) + "},")
    else:
        out.append("{" + ",".join("0x%02X" % b for b in g) + "},")
out.append("};")
out.append("")
out.append("// Unicode->GBK 反查 (排序, 二分), 值 = uni<<16 | gb_hi<<8 | gb_lo")
out.append("#define UNI2GBK_N %d" % len(uni2gbk))
out.append("static const uint32_t uni2gbk[UNI2GBK_N] PROGMEM = {")
for i in range(0, len(uni2gbk), 8):
    out.append(",".join("0x%08X" % v for v in uni2gbk[i:i + 8]) + ",")
out.append("};")

with open("font_cn.h", "w", encoding="utf-8") as f:
    f.write("\n".join(out))
print("font_cn.h written, size:", sum(len(l) + 1 for l in out))
