#include "news_feed.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>

// ---------------- 数据 ----------------
NewsItem         g_news[MAX_NEWS];
volatile uint8_t g_nNews   = 0;
volatile int8_t  g_curNews = 0;
volatile uint8_t g_newsNew = 0;
volatile NewsDiag g_newsDiag = {0, 0, 0, 0, 0, 0};

static SemaphoreHandle_t s_mux = nullptr;
static volatile bool     s_kick     = false;
static volatile bool     s_fetching = false;
static uint32_t          s_lastFetch = 0;
static uint32_t          s_retryAt   = 0;
static char              s_lastCode[NEWS_CODE] = "";  // 上次最新一条, 用于增量判定

// HTTP 明文: ESP32-S3 上省掉 TLS 握手与 WiFiClientSecure 的 ~40KB 堆,
// 让新闻任务与 MP3 解码可以并存。实测 2026-11 返回 200 / 6~12KB / UTF-8 中文不转义。
static const char* NEWS_URL =
  "http://np-listapi.eastmoney.com/comm/web/getFastNewsList?client=web&biz=web_724&fastColumn=102&sortEnd=&pageSize=20&req_trace=1";
static const char* NEWS_REF = "https://kuaixun.eastmoney.com/";

// 在 [from,end) 内找 JSON key, 返回 key 之后第一个非空白字符(通常是 '"')。
// 不用 strstr: 它会越过 end 找到下一条记录里去。
static const char* findKey(const char* from, const char* end, const char* key) {
  size_t kl = strlen(key);
  if (!kl) return nullptr;
  for (const char* p = from; p + kl <= end; ++p) {
    if (*p == key[0] && memcmp(p, key, kl) == 0) {
      const char* v = p + kl;
      while (v < end && (*v == ' ' || *v == '\t')) ++v;
      return v;
    }
  }
  return nullptr;
}

// 拷出 JSON 字符串值: 处理 \" \\ \/ \n \uXXXX, 按 UTF-8 字符边界截断,
// 保证不会出现半个汉字(旧版底部新闻条显示"锟斤拷"就是这个原因)。
static void copyVal(char* out, int outSz, const char* src, const char* end) {
  const uint8_t* p = (const uint8_t*)src;
  const uint8_t* e = (const uint8_t*)end;
  int o = 0;
  if (!src || src >= end) { out[0] = 0; return; }
  if (p < e && *p == '"') ++p;
  while (p < e && o < outSz - 1) {
    if (*p == '"') break;                       // 未转义的结束引号
    if (*p == '\\' && p + 1 < e) {
      uint8_t c = p[1];
      if (c == '"' || c == '\\' || c == '/') { out[o++] = (char)c; p += 2; continue; }
      if (c == 'n' || c == 'r' || c == 't')  { out[o++] = ' ';     p += 2; continue; }
      if (c == 'u' && p + 5 < e)             { out[o++] = '?';     p += 6; continue; }
      p += 2; continue;
    }
    uint8_t c = *p;
    int n = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
    if (p + n > e || o + n > outSz - 1) break;
    for (int k = 0; k < n; k++) out[o++] = (char)p[k];
    p += n;
  }
  out[o] = 0;
  // 尾部空白压掉
  while (o > 0 && out[o - 1] == ' ') out[--o] = 0;
}

// 以 "summary": 作为每条记录的锚点(东财固定字段顺序 summary->code->...->title),
// 两条 summary 之间即一条完整记录; 不建 JSON 树, 内存恒定。
static int parseFastNews(const String& body, NewsItem* out, int maxN) {
  const char* s   = body.c_str();
  const char* end = s + body.length();
  const char* p   = findKey(s, end, "\"fastNewsList\":");
  if (!p) return 0;
  int n = 0;
  while (n < maxN) {
    const char* rec = findKey(p, end, "\"summary\":");
    if (!rec) break;
    const char* next = findKey(rec + 10, end, "\"summary\":");
    const char* recEnd = next ? next : end;
    // rec 本身已经是 "summary": 之后的值起点, 不能再从 rec 往后找同名 key
    // (key 在 rec 之前), 否则 sv 恒为 null -> 摘要永远为空。
    const char* sv = rec;
    const char* cv = findKey(rec, recEnd, "\"code\":");
    const char* tv = findKey(rec, recEnd, "\"title\":");
    const char* dv = findKey(rec, recEnd, "\"showTime\":");
    if (cv && tv) {
      memset(&out[n], 0, sizeof(NewsItem));
      copyVal(out[n].title, sizeof(out[n].title), tv, recEnd);
      copyVal(out[n].code,  sizeof(out[n].code),  cv, recEnd);
      if (sv) copyVal(out[n].summary, sizeof(out[n].summary), sv, recEnd);
      else    out[n].summary[0] = 0;      // 摘要缺失 -> 自动降级为纯标题
      if (dv) copyVal(out[n].time, sizeof(out[n].time), dv, recEnd);
      else    out[n].time[0] = 0;
      out[n].src = 0;
      if (out[n].title[0] && out[n].code[0]) n++;
    }
    if (!next) break;
    p = next;
  }
  return n;
}

static void failAndBackoff() {
  s_lastFetch = millis();
  s_retryAt   = millis() + 60000UL;   // 失败后 60 秒再试, 别疯转
  g_newsDiag.lastErrMs = millis();
  s_fetching = false;
}

static void doFetch() {
  if (WiFi.status() != WL_CONNECTED) return;
  s_fetching = true;
  g_newsDiag.lastTryMs = millis();
  g_newsDiag.httpCode = 0;

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(9000);
  if (!http.begin(client, NEWS_URL)) { failAndBackoff(); return; }
  http.addHeader("Referer", NEWS_REF);
  http.addHeader("User-Agent", "Mozilla/5.0 (ESP32)");
  int code = http.GET();
  g_newsDiag.httpCode = code;
  if (code != 200) { http.end(); failAndBackoff(); return; }
  String body = http.getString();
  http.end();

  static NewsItem buf[MAX_NEWS];       // 约 7.6KB 静态, 不占栈
  int n = parseFastNews(body, buf, MAX_NEWS);
  g_newsDiag.parsed = n;
  if (n == 0) { failAndBackoff(); return; }

  // 按唯一 code 计算「本次新增几条」: 从最新一条往下数, 遇到上次的最新码即止。
  // 首次启动(s_lastCode 为空)不报 NEW, 否则一开机 20 条全标成未读。
  int fresh = 0;
  if (s_lastCode[0]) {
    for (int i = 0; i < n; i++) {
      if (!strcmp(buf[i].code, s_lastCode)) break;
      fresh++;
    }
    if (fresh > 0) {
      int v = (int)g_newsNew + fresh;
      g_newsNew = (uint8_t)(v > 99 ? 99 : v);
    }
  }
  strncpy(s_lastCode, buf[0].code, sizeof(s_lastCode) - 1);
  s_lastCode[sizeof(s_lastCode) - 1] = 0;

  if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
    memcpy(g_news, buf, (size_t)n * sizeof(NewsItem));
    g_nNews = (uint8_t)n;
    if (g_curNews >= n) g_curNews = 0;
    xSemaphoreGive(s_mux);
  }
  s_lastFetch = millis();
  s_retryAt   = 0;
  g_newsDiag.lastOkMs  = millis();
  g_newsDiag.fetchCount++;
  s_fetching = false;
}

// ---------------- 对外 ----------------
void newsInit() {
  if (!s_mux) s_mux = xSemaphoreCreateMutex();
  g_nNews = 0;
  g_curNews = 0;
  g_newsNew = 0;
  s_lastCode[0] = 0;
}

void newsKick() { s_kick = true; }
bool newsReady() { return g_nNews > 0; }
uint32_t newsUpdatedAt() { return s_lastFetch; }
uint16_t newsRefreshCount() { return g_newsDiag.fetchCount; }
uint8_t  newsTakeNew() { uint8_t v = g_newsNew; g_newsNew = 0; return v; }
bool newsLock(uint32_t ms) { return s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(ms)) == pdTRUE; }
void newsUnlock() { if (s_mux) xSemaphoreGive(s_mux); }

void newsNext() {
  if (g_nNews == 0) return;
  g_curNews = (int8_t)((g_curNews + 1) % g_nNews);
}
void newsPrev() {
  if (g_nNews == 0) return;
  g_curNews = (int8_t)((g_curNews + g_nNews - 1) % g_nNews);
}
void newsSelect(int i) {
  if (g_nNews == 0) return;
  if (i < 0) i = 0;
  if (i >= g_nNews) i = g_nNews - 1;
  g_curNews = (int8_t)i;
}

void newsTask(void* arg) {
  (void)arg;
  vTaskDelay(pdMS_TO_TICKS(5000));      // 等 WiFi 起来, 与行情任务错峰
  for (;;) {
    bool due = g_newsOn &&
               (s_kick || s_lastFetch == 0 ||
                (millis() - s_lastFetch >= (uint32_t)g_newsRefSec * 1000UL));
    if (due && !s_fetching && (!s_retryAt || (int32_t)(millis() - s_retryAt) >= 0)) {
      s_kick = false;
      doFetch();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
