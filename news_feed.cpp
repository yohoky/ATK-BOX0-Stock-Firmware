#include "news_feed.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ---------------- 数据 ----------------
NewsItem         g_news[MAX_NEWS];
volatile uint8_t g_nNews   = 0;
volatile int8_t  g_curNews = 0;
volatile NewsDiag g_newsDiag = {0, 0, 0, 0, 0, 0, 0};

static SemaphoreHandle_t s_mux       = nullptr;
static volatile bool     s_kick      = false;
static volatile bool     s_fetching  = false;
static uint32_t          s_lastFetch = 0;
static uint32_t          s_retryAt   = 0;   // 抓取失败后的重试时刻(不用减法, 免回绕)
static uint16_t          s_okCount   = 0;

#define SRC_FIN   0
#define SRC_TECH  1

// 新浪滚动新闻统一接口(pageid=153), 财经 2516 / 科技 2515
static const char* URL_FIN   = "http://feed.mix.sina.com.cn/api/roll/get?pageid=153&lid=2516&num=8&page=1";
static const char* URL_TECH  = "http://feed.mix.sina.com.cn/api/roll/get?pageid=153&lid=2515&num=8&page=1";
static const char* REF_FIN   = "https://finance.sina.com.cn";
static const char* REF_TECH  = "https://tech.sina.com.cn";

// 标题清理: 去首尾空白、换行/制表符压成空格、按 utf8 边界截断
//   顺手把最常见的几个 HTML 实体还原, 否则屏幕上会显示 "&quot;" 这种噪音
static void cleanTitle(char* out, int outSz, const char* in) {
  int o = 0;
  const uint8_t* p = (const uint8_t*)in;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  while (*p && o < outSz - 4) {
    uint8_t c = *p;
    if (c == '&') {                       // 实体替换
      const char* rep = nullptr;
      int skip = 0;
      if      (!strncmp((const char*)p, "&quot;", 6)) { rep = "\""; skip = 6; }
      else if (!strncmp((const char*)p, "&#39;", 5))  { rep = "'"; skip = 5; }
      else if (!strncmp((const char*)p, "&apos;", 6)) { rep = "'"; skip = 6; }
      else if (!strncmp((const char*)p, "&amp;", 5))  { rep = "&"; skip = 5; }
      else if (!strncmp((const char*)p, "&lt;", 4))   { rep = "<"; skip = 4; }
      else if (!strncmp((const char*)p, "&gt;", 4))   { rep = ">"; skip = 4; }
      else if (!strncmp((const char*)p, "&nbsp;", 6)) { rep = " "; skip = 6; }
      if (rep) { out[o++] = rep[0]; p += skip; continue; }
    }
    int n = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
    if (c == '\r' || c == '\n' || c == '\t') { out[o++] = ' '; p++; continue; }
    if ((c & 0xC0) == 0x80) { p++; continue; }        // 游离续字节, 丢掉
    for (int k = 0; k < n && p[k]; k++) out[o++] = (char)p[k];
    p += n;
  }
  // 去掉尾部空格
  while (o > 0 && out[o - 1] == ' ') o--;
  out[o] = 0;
}

// 抓一个源, 解析 title 列表写入 dst, 返回条数
static int fetchInto(NewsItem* dst, int maxN, const char* url,
                     const char* ref, uint8_t src) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(9000);
  if (!http.begin(client, url)) {
    Serial.printf("[News] begin fail: %s\n", url);
    return 0;
  }
  http.addHeader("Referer", ref);
  http.addHeader("User-Agent", "Mozilla/5.0 (ESP32)");
  int code = http.GET();
  if (src == SRC_FIN) g_newsDiag.finHttp = code;
  else                 g_newsDiag.techHttp = code;
  if (code != 200) {
    Serial.printf("[News] HTTP %d (src=%d)\n", code, src);
    http.end();
    return 0;
  }
  int len = http.getSize();
  Serial.printf("[News] GET 200 len=%d (src=%d)\n", len, src);

  // 用 filter 只保留 data[].title, 其余几十字段全丢弃, 省内存。
  //   ★ ArduinoJson 7 的 filter 数组必须用 .add() 显式创建, 不能用 [0]:
  //     filter["result"]["data"][0]["title"] 会让 "data" 被当成对象, 与真实数组类型
  //     不匹配 -> 整个 data 被排除 -> 解析出 0 条(ArduinoJson issue #2207 同款坑)。
  //   正确写法:
  //     JsonObject f = filter["result"]["data"].add();  // .add() 把 data 变数组并加模板元素
  //     f["title"] = true;
  JsonDocument filter;
  JsonObject fdata = filter["result"]["data"].add();
  fdata["title"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    if (src == SRC_FIN) g_newsDiag.finJsonErr = (int)err.code();
    else                 g_newsDiag.techJsonErr = (int)err.code();
    Serial.printf("[News] JSON %s (src=%d)\n", err.c_str(), src);
    return 0;
  }

  int n = 0;
  // ★ 先把数组取出来再遍历: 直接写 for (JsonObject it : doc["result"]["data"].as<JsonArray>())
  //   拿到的是临时 JsonVariant 的引用, range-for 期间可能已析构(-Wdangling-reference)。
  JsonArray arr = doc["result"]["data"].as<JsonArray>();
  Serial.printf("[News] data 数组 %d 条 (src=%d)\n", (int)arr.size(), src);
  for (JsonObject it : arr) {
    if (n >= maxN) break;
    const char* t = it["title"] | "";
    if (!t || !t[0]) continue;
    cleanTitle(dst[n].title, sizeof(dst[n].title), t);
    if (!dst[n].title[0]) continue;
    dst[n].src = src;
    n++;
  }
  if (src == SRC_FIN) g_newsDiag.finCount = n;
  else                 g_newsDiag.techCount = n;
  return n;
}

// 抓取 + 合并(财经/科技交替, 免得一屏全是一类)
static void doFetch() {
  if (WiFi.status() != WL_CONNECTED) return;
  s_fetching = true;
  g_newsDiag.lastTryMs = millis();

  static NewsItem bufA[8], bufB[8];
  int na = 0, nb = 0;

  // g_newsSrc: 0=财经+科技 1=只看财经 2=只看科技
  bool wantFin  = (g_newsSrc == 0 || g_newsSrc == 1);
  bool wantTech = (g_newsSrc == 0 || g_newsSrc == 2);

  if (wantFin)  na = fetchInto(bufA, 8, URL_FIN,  REF_FIN,  SRC_FIN);
  if (wantTech) nb = fetchInto(bufB, 8, URL_TECH, REF_TECH, SRC_TECH);

  int total = na + nb;
  if (total == 0) {
    // 没网/接口抽风: 保留旧内容, 90 秒后再试。
    // ★ 不能写成 s_lastFetch = millis() - refSec*1000 + 90000:
    //   refSec 最小 60 秒时 60000-90000 会无符号回绕, 变成"永远到点", 任务会疯狂重抓。
    s_lastFetch = millis();
    s_retryAt   = millis() + 90000UL;
    Serial.println("[News] 本次没抓到, 保留旧内容");
    s_fetching = false;
    return;
  }

  // 写进共享区(加锁, 别让 UI 读到半条): 财经/科技交替
  if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
    int o = 0;
    for (int i = 0; i < 8 && o < MAX_NEWS; i++) {
      if (i < na) g_news[o++] = bufA[i];
      if (i < nb && o < MAX_NEWS) g_news[o++] = bufB[i];
    }
    g_nNews = (uint8_t)o;
    if (g_curNews >= o) g_curNews = 0;
    xSemaphoreGive(s_mux);
  }

  s_lastFetch = millis();
  s_retryAt   = 0;
  s_okCount++;
  Serial.printf("[News] 抓到 %d 条 (财经%d 科技%d)\n", total, na, nb);
  s_fetching = false;
}

// ---------------- 对外 ----------------
void newsInit() {
  if (!s_mux) s_mux = xSemaphoreCreateMutex();
  g_nNews = 0;
  g_curNews = 0;
}

void newsKick() { s_kick = true; }

bool newsReady() { return g_nNews > 0; }
uint32_t newsUpdatedAt() { return s_lastFetch; }
uint16_t newsRefreshCount() { return s_okCount; }

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
  vTaskDelay(pdMS_TO_TICKS(4000));      // 等 WiFi 起来
  for (;;) {
    bool due = (s_lastFetch == 0) ||
               (millis() - s_lastFetch >= (uint32_t)g_newsRefSec * 1000UL);
    if (s_retryAt && (int32_t)(millis() - s_retryAt) < 0) due = false;  // 退避期内不重试
    if ((due || s_kick) && !s_fetching) {
      s_kick = false;
      doFetch();
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
