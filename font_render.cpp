#include "font_render.h"
#include "font_cn.h"

// 解码一个 UTF-8 字符 -> unicode, 返回字节数
static int utf8Next(const uint8_t* p, uint32_t& uni) {
  uint8_t c = p[0];
  if (c < 0x80) { uni = c; return 1; }
  int n = 0; uint32_t u = 0;
  if ((c & 0xE0) == 0xC0) { n = 2; u = c & 0x1F; }
  else if ((c & 0xF0) == 0xE0) { n = 3; u = c & 0x0F; }
  else if ((c & 0xF8) == 0xF0) { n = 4; u = c & 0x07; }
  else { uni = '?'; return 1; }
  for (int k = 1; k < n; k++) {
    if ((p[k] & 0xC0) != 0x80) { uni = '?'; return 1; }
    u = (u << 6) | (p[k] & 0x3F);
  }
  uni = u;
  return n;
}

// unicode -> gbk 双字节 (查 uni2gbk 排序表, 二分)
uint16_t uniToGbk(uint32_t uni) {
  uint32_t key = uni << 16;
  int lo = 0, hi = UNI2GBK_N - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    uint32_t v = uni2gbk[mid];
    if ((v & 0xFFFF0000) == key) return (uint16_t)(v & 0xFFFF);
    if (v < key) lo = mid + 1; else hi = mid - 1;
  }
  return 0;
}

// GBK 双字节 -> unicode
// ★ 这里原来是二分查找, 但 uni2gbk 表是按 **unicode 高16位** 排序的,
//   拿 gbk 去比低16位永远命中不了 -> 新浪返回的 GBK 股票名全变成 '?'.
//   (v4.0 之前屏幕上 "????" 就是这个原因, 不是字库缺字)
//   表只有 7445 项, 线性扫一遍约 30~40us; 名称只在首次解析时转换, 开销可忽略.
//   顺序读 flash 有 cache, 连续查同一个名字基本全命中.
uint16_t gbkToUni(uint16_t gbk) {
  static uint16_t s_lastG = 0;
  static uint16_t s_lastU = 0;
  if (s_lastG == gbk) return s_lastU;          // 简单的单条缓存: 连续同字直接返回
  for (int i = 0; i < UNI2GBK_N; i++) {
    uint32_t v = uni2gbk[i];
    if ((uint16_t)(v & 0xFFFF) == gbk) {
      s_lastG = gbk;
      s_lastU = (uint16_t)(v >> 16);
      return s_lastU;
    }
  }
  return 0;
}

static const uint8_t* glyphOfGB(uint8_t hi, uint8_t lo) {
  if (hi < 0xA1 || lo < 0xA1) return nullptr;
  uint32_t idx = (uint32_t)(hi - 0xA1) * 94 + (lo - 0xA1);
  if (idx >= GB_SLOTS) return nullptr;
  return gbk_font16[idx];
}

// 取一个字符的字形(32B)与宽度; utf8 输入, 内部转 GBK
static int nextGlyph(const uint8_t* p, const uint8_t** g, int& w) {
  uint32_t uni;
  int n = utf8Next(p, uni);
  if (uni < 0x80) {
    uint8_t c = (uint8_t)uni;
    if (c < 0x20 || c > 0x7E) { *g = nullptr; w = 8; return n; }
    *g = ascii_font16[c - 0x20];
    w = 8;
    return n;
  }
  uint16_t gbk = uniToGbk(uni);
  if (gbk == 0) { *g = nullptr; w = 16; return n; }  // 字库外的字显示空白
  *g = glyphOfGB(gbk >> 8, gbk & 0xFF);
  w = 16;
  return n;
}

int cnTextWidth(const char* utf8) {
  int x = 0;
  const uint8_t* p = (const uint8_t*)utf8;
  while (*p) {
    const uint8_t* g; int w;
    int n = nextGlyph(p, &g, w);
    x += w;
    p += n;
  }
  return x;
}

void drawCN(LovyanGFX& s, int x, int y, const char* utf8,
            uint16_t color, uint16_t bg, bool bgFill) {
  const uint8_t* p = (const uint8_t*)utf8;
  static uint16_t buf[256];
  while (*p) {
    const uint8_t* g; int w;
    int n = nextGlyph(p, &g, w);
    if (g) {
      int byw = w / 8;  // 每行字节数
      for (int r = 0; r < 16; r++) {
        for (int b = 0; b < byw; b++) {
          uint8_t bits = g[r * byw + b];
          for (int k = 0; k < 8; k++) {
            bool on = bits & (0x80 >> k);
            buf[r * 16 + b * 8 + k] = on ? color : (bgFill ? bg : 0);
          }
        }
      }
      for (int r = 0; r < 16; r++)
        for (int c = w; c < 16; c++) buf[r * 16 + c] = bgFill ? bg : 0;
      s.pushImage(x, y, 16, 16, buf);
    } else if (bgFill) {
      s.fillRect(x, y, w, 16, bg);
    }
    x += w;
    p += n;
  }
}

int drawCNWrap(LovyanGFX& s, int x, int y, int maxW, const char* utf8,
               uint16_t color, uint16_t bg, int lineH) {
  const uint8_t* p = (const uint8_t*)utf8;
  int cx = 0, cy = 0, lines = 0;
  char one[8];
  while (*p) {
    if (*p == '\n') { p++; cx = 0; cy += lineH; lines++; continue; }
    uint32_t uni;
    int n = utf8Next(p, uni);
    int w = (uni < 0x80) ? 8 : 16;
    if (cx + w > maxW && cx > 0) { cx = 0; cy += lineH; lines++; }
    memcpy(one, p, n); one[n] = 0;
    drawCN(s, x + cx, y + cy, one, color, bg, true);
    cx += w;
    p += n;
  }
  return (lines + 1) * lineH;
}

void cnEllipsis(char* out, int outSize, const char* utf8, int maxW) {
  const uint8_t* p = (const uint8_t*)utf8;
  int w = 0, o = 0;
  const int dotW = 16;  // ".." 两个 ASCII
  while (*p && o < outSize - 4) {
    uint32_t uni;
    int n = utf8Next(p, uni);
    int cw = (uni < 0x80) ? 8 : 16;
    if (w + cw + dotW > maxW) {
      out[o++] = '.'; out[o++] = '.';
      break;
    }
    memcpy(out + o, p, n);
    o += n; w += cw; p += n;
  }
  out[o] = 0;
}

void drawCNBufClip(uint16_t* buf, int bufW, int y, int x, const char* utf8,
                   uint16_t color, uint16_t bg, int clipW) {
  const uint8_t* p = (const uint8_t*)utf8;
  while (*p) {
    const uint8_t* g; int w;
    int n = nextGlyph(p, &g, w);
    // 整个字都在可视区外: 直接跳过(负 x 时避免无谓写入)
    if (x >= clipW) break;
    if (x + w > 0) {
      for (int r = 0; r < 16; r++) {
        uint16_t* row = buf + (size_t)(y + r) * bufW;
        int byw = w / 8;
        for (int b = 0; b < byw; b++) {
          uint8_t bits = g ? g[r * byw + b] : 0;
          for (int k = 0; k < 8; k++) {
            int cx = x + b * 8 + k;
            if (cx < 0 || cx >= clipW) continue;
            row[cx] = (bits & (0x80 >> k)) ? color : bg;
          }
        }
        // ASCII 只占 8 列, 补满 16 列的背景, 防止旧像素残留
        for (int c = w; c < 16; c++) {
          int cx = x + c;
          if (cx < 0 || cx >= clipW) continue;
          row[cx] = bg;
        }
      }
    }
    x += w;
    p += n;
  }
}

void drawCNBuf(uint16_t* buf, int bufW, int x, int y, const char* utf8,
               uint16_t color, uint16_t bg) {
  const uint8_t* p = (const uint8_t*)utf8;
  while (*p) {
    const uint8_t* g; int w;
    int n = nextGlyph(p, &g, w);
    for (int r = 0; r < 16; r++) {
      uint16_t* row = buf + (y + r) * bufW + x;
      if (g) {
        int byw = w / 8;
        for (int b = 0; b < byw; b++) {
          uint8_t bits = g[r * byw + b];
          for (int k = 0; k < 8; k++)
            row[b * 8 + k] = (bits & (0x80 >> k)) ? color : bg;
        }
      } else {
        for (int c = 0; c < w; c++) row[c] = bg;
      }
      for (int c = w; c < 16; c++) row[c] = bg;
    }
    x += w;
    p += n;
  }
}
