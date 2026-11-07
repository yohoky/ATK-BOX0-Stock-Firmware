#pragma once
#include <Arduino.h>
#include "app_config.h"

// ============================================================
// 财经 / 科技 要闻
//   数据源: 新浪滚动新闻 JSON 接口 (http 明文, UTF-8, 无鉴权)
//     财经  pageid=153 lid=2516
//     科技  pageid=372 lid=2431
//   抓取放在独立任务, 不阻塞 UI; 结果进环形缓冲, 屏幕每 g_newsSec 秒切一条
// ============================================================

struct NewsItem {
  char    title[NEWS_TITLE];   // utf8
  uint8_t src;                 // 0=财经 1=科技 2=体育 3=社会
};

extern NewsItem         g_news[MAX_NEWS];
extern volatile uint8_t g_nNews;
extern volatile int8_t  g_curNews;

void  newsInit();            // 建任务(不联网)
void  newsTask(void* arg);   // FreeRTOS 任务体
void  newsKick();            // 立刻抓一次(网页保存设置/按键刷新)
void  newsNext();
void  newsPrev();
void  newsSelect(int i);
bool  newsReady();           // 至少有一条
uint32_t newsUpdatedAt();    // 上次成功抓取的时间戳(millis)
uint16_t newsRefreshCount(); // 累计成功抓取次数(诊断用)

// ---------------- 诊断(网页 /api/diag 用) ----------------
struct NewsDiag {
  int  finHttp;        // 财经源最近一次 HTTP 码(0=未发起/建连失败)
  int  finJsonErr;     // 财经源最近一次 JSON 错误码(0=成功)
  int  finCount;       // 财经源最近一次解析条数
  int  techHttp;       // 科技源最近一次 HTTP 码
  int  techJsonErr;    // 科技源最近一次 JSON 错误码
  int  techCount;      // 科技源最近一次解析条数
  uint32_t lastTryMs;  // 最近一次尝试抓取时间(millis)
};
extern volatile NewsDiag g_newsDiag;
