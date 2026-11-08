#pragma once
#include <Arduino.h>
#include "app_config.h"

// ============================================================
// v7.2 实时财经快讯
//   数据源: 东方财富 7x24 快讯 (http 明文, UTF-8, 无鉴权, 单条约 600B)
//     /comm/web/getFastNewsList?client=web&biz=web_724&fastColumn=102&pageSize=20
//   每条含 title(标题) + summary(约 110 字摘要) + showTime + 唯一 code。
//   ★ 不再用 ArduinoJson: 旧版新浪滚动接口单条 40+ 字段、16KB 文档,
//     filter 数组写法踩坑 + 大文档吃内存, 在 ESP32 上反复解析出 0 条。
//     这里用「带边界的字符串扫描」直接抠字段, 内存固定、不会解析失败。
//   独立任务按 g_newsRefSec(默认 60 秒)轮询, 按 code 去重并累计新条数。
// ============================================================

struct NewsItem {
  char    title[NEWS_TITLE];     // utf8
  char    summary[NEWS_SUMMARY]; // utf8, 摘要(为空则退化为纯标题)
  char    code[NEWS_CODE];       // 唯一 ID, 增量去重
  char    time[NEWS_TIME];       // "YYYY-MM-DD HH:MM:SS"
  uint8_t src;                   // 0=财经快讯
};

extern NewsItem         g_news[MAX_NEWS];
extern volatile uint8_t g_nNews;
extern volatile int8_t  g_curNews;
extern volatile uint8_t g_newsNew;    // 未读新条数(状态栏 NEW 角标/toast)

void  newsInit();             // 建锁(不联网)
void  newsTask(void* arg);    // FreeRTOS 任务体
void  newsKick();             // 立刻抓一次
void  newsNext();
void  newsPrev();
void  newsSelect(int i);
bool  newsReady();
uint32_t newsUpdatedAt();     // 上次抓取时刻(millis)
uint16_t newsRefreshCount();  // 累计成功抓取次数
uint8_t  newsTakeNew();       // 取走并清零未读计数
bool  newsLock(uint32_t ms = 200);
void  newsUnlock();

// ---------------- 诊断(/api/diag 用) ----------------
struct NewsDiag {
  int      httpCode;     // 最近一次 HTTP 码(0=没发起)
  int      parsed;       // 最近一次解析出的条数
  uint16_t fetchCount;   // 累计成功次数
  uint32_t lastTryMs;
  uint32_t lastOkMs;
  uint32_t lastErrMs;
};
extern volatile NewsDiag g_newsDiag;
