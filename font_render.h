#pragma once
#include <LovyanGFX.hpp>

// 16x16 中文点阵渲染 (宋体, GB2312, 见 font_cn.h)
// 字形索引: ASCII c-0x20 -> ascii_font16; GBK (hi-0xA1)*94+(lo-0xA1) -> gbk_font16
int  cnTextWidth(const char* utf8);
uint16_t uniToGbk(uint32_t uni);   // unicode -> GBK 双字节(0=查无)
uint16_t gbkToUni(uint16_t gbk);   // GBK 双字节 -> unicode(0=查无), 供 GBKtoUTF8
// 绘制目标用基类 LovyanGFX: 既能传精灵(LGFX_Sprite), 也能传屏幕(LGFX_Device)
void drawCN(LovyanGFX& s, int x, int y, const char* utf8,
            uint16_t color, uint16_t bg = 0x0000, bool bgFill = true);
// 直接绘制到 RGB565 缓冲 (用于滚动行情条)
void drawCNBuf(uint16_t* buf, int bufW, int x, int y, const char* utf8,
               uint16_t color, uint16_t bg);

// 同上, 但只在 [0, clipW) 列范围内落笔(越界丢弃), 供跑马灯逐帧绘制使用
// x 可以是负数, 用于把字符串"推"到屏幕左外侧
void drawCNBufClip(uint16_t* buf, int bufW, int y, int x, const char* utf8,
                   uint16_t color, uint16_t bg, int clipW);

// 自动换行绘制: 超过 maxW 像素折行, 返回占用高度(行数*lineH)
// 遇 '\n' 强制换行; 仅用于 16px 点阵(中文16 / ASCII 8)
int drawCNWrap(LovyanGFX& s, int x, int y, int maxW, const char* utf8,
               uint16_t color, uint16_t bg, int lineH = 18);

// 按宽度截断(末尾加 ..), 用于单行显示
void cnEllipsis(char* out, int outSize, const char* utf8, int maxW);
