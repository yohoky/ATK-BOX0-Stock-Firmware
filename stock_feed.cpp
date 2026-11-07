#include "stock_feed.h"
#include "app_config.h"
#include "ui_display.h"
#include <WiFi.h>
#include <HTTPClient.h>

static volatile bool s_refreshNow = false;

void stockRequestRefresh() { s_refreshNow = true; }
void stockRebuildTicker()  { uiRedrawTicker(); }

enum Mkt { MKT_A, MKT_HK, MKT_US };

static Mkt marketOf(const char* code) {
  if (strncmp(code, "rt_hk", 5) == 0 || strncmp(code, "r_hk", 4) == 0) return MKT_HK;
  if (strncmp(code, "gb_", 3) == 0) return MKT_US;
  return MKT_A;
}

// 解析一条行情, 字段布局见项目 README (实测于 2026-11)
static void parseQuote(const char* code, const char* payload, Quote& q) {
  // payload: 逗号分隔字段串 (GBK), 按逗号切分; 限长拷贝防止越界
  char* buf = strndup(payload, 600);
  char* fields[40];
  int nf = 0;
  char* save = nullptr;
  for (char* tok = strtok_r(buf, ",", &save); tok && nf < 40; tok = strtok_r(nullptr, ",", &save))
    fields[nf++] = tok;

  Mkt mkt = marketOf(code);
  q.valid = false;
  strncpy(q.code, code, STK_CODE_LEN - 1);
  q.ts = millis();

  if (mkt == MKT_A && nf > 32) {
    // [0]名称 [1]开 [2]昨收 [3]现价 [4]高 [5]低 [8]成交量(手) [9]成交额
    String nm = GBKtoUTF8((uint8_t*)fields[0], strlen(fields[0]));
    strncpy(q.name, nm.c_str(), 47); q.name[47] = 0;
    q.open = atof(fields[1]);  q.prevClose = atof(fields[2]);
    q.price = atof(fields[3]); q.high = atof(fields[4]); q.low = atof(fields[5]);
    q.volume = atof(fields[8]);   // 成交量(手)
    q.amount = atof(fields[9]);
    if (q.price > 0 && q.prevClose > 0) {
      q.chg = q.price - q.prevClose;
      q.chgpct = q.chg / q.prevClose * 100.0f;
      q.valid = true;
    }
  } else if (mkt == MKT_HK && nf > 8) {
    // [0]英文名 [1]中文名 [2]开 [3]昨收 [4]高 [5]低 [6]现价 [7]涨跌 [8]涨跌%
    String nm = GBKtoUTF8((uint8_t*)fields[1], strlen(fields[1]));
    strncpy(q.name, nm.c_str(), 47); q.name[47] = 0;
    q.open = atof(fields[2]);  q.prevClose = atof(fields[3]);
    q.high = atof(fields[4]);  q.low = atof(fields[5]);
    q.price = atof(fields[6]);
    q.chg = atof(fields[7]);   q.chgpct = atof(fields[8]);
    if (q.price > 0) q.valid = true;
  } else if (mkt == MKT_US && nf > 4) {
    // [0]名称(通常UTF-8) [1]现价 [2]涨跌% [3]时间 [4]涨跌额 [5]昨收
    const char* nmRaw = fields[0];
    int nlen = strlen(nmRaw);
    if (utf8Valid((const uint8_t*)nmRaw, nlen)) {
      strncpy(q.name, nmRaw, 47); q.name[47] = 0;
    } else {
      String nm = GBKtoUTF8((uint8_t*)nmRaw, nlen);
      strncpy(q.name, nm.c_str(), 47); q.name[47] = 0;
    }
    q.price = atof(fields[1]);
    q.chgpct = atof(fields[2]);
    q.chg = atof(fields[4]);
    q.prevClose = nf > 5 ? atof(fields[5]) : q.price - q.chg;
    if (q.price > 0) q.valid = true;
  }
  free(buf);
}

static void fetchOnce() {
  if (WiFi.status() != WL_CONNECTED || g_nStock == 0) return;
  String url = "http://hq.sinajs.cn/list=";
  for (int i = 0; i < g_nStock; i++) {
    if (i) url += ',';
    url += g_stocks[i].code;
  }
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(6000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(url)) return;
  // 新浪接口现在必须带 Referer, 否则返回 403; UA 也补上, 免得被当爬虫
  http.addHeader("Referer", "https://finance.sina.com.cn");
  http.addHeader("User-Agent",
                 "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                 "(KHTML, like Gecko) Chrome/120.0 Safari/537.36");
  int code = http.GET();
  if (code == 200) {
    // 响应为 GBK, 按 latin1 读取保持字节
    String body = http.getString();
    int len = body.length();
    const uint8_t* p = (const uint8_t*)body.c_str();
    // 逐行解析 var hq_str_xxx="...";
    int pos = 0;
    while (pos < len) {
      const char* varTag = strstr((const char*)p + pos, "hq_str_");
      if (!varTag) break;
      const char* eq = strchr(varTag, '=');
      const char* q1 = eq ? strchr(eq, '"') : nullptr;
      if (!eq || !q1) break;
      const char* q2 = strchr(q1 + 1, '"');
      if (!q2) break;
      char varName[24] = "";
      int vn = eq - varTag - 7; // "hq_str_" 长7
      if (vn > 0 && vn < 24) { memcpy(varName, varTag + 7, vn); varName[vn] = 0; }
      int plen2 = q2 - q1 - 1;
      if (plen2 > 0) {
        if (quotesLock()) {
          for (int i = 0; i < g_nStock; i++) {
            if (strcmp(g_stocks[i].code, varName) == 0) {
              parseQuote(varName, q1 + 1, g_quotes[i]);
              break;
            }
          }
          quotesUnlock();
        }
      }
      pos = (q2 - (const char*)p) + 1;
    }
    uiRedrawTicker();
  } else {
    Serial.printf("[Stock] HTTP %d\n", code);
  }
  http.end();
}

// ---------------- 同时段放量对比 ----------------
// 用户需求: "今天10点和昨天10点对比成交额" —— 盘中实时放量, 不是整日对比。
// 实现: 用新浪 5 分钟线接口(quotes.sina.cn, 含 amount 字段)取昨日全部分时,
//       累加成"昨日累计成交额表"(每 5 分钟一个点, 共 48 点), 运行时按当前
//       时刻(HH:MM)查表, 得到"昨日同时刻累计额"写入 q.prevAmount。
// A股交易时段 9:30-11:30 / 13:00-15:00 = 240 分钟 = 48 个 5 分钟点。
#define PM_POINTS 48

// 昨日累计成交额表: [股票][5分钟点], 第 i 点 = 昨日开盘到第 i 个 5 分钟末的累计额(元)
static double s_prevAmtCum[MAX_STOCKS][PM_POINTS + 1];
static uint8_t s_amtDay = 255;      // 已抓取昨量的"日"(wday), 防止重复抓

// 把 HH:MM 映射到 5 分钟点索引(0..48)。开盘前返回 0, 收盘后返回 48。
static int timeToPoint(int hh, int mm) {
  int mins = hh * 60 + mm;
  if (mins < 9 * 60 + 30) return 0;                 // 9:30 前
  if (mins <= 11 * 60 + 30) return (mins - (9 * 60 + 30)) / 5 + 1;
  if (mins < 13 * 60) return 24;                    // 午休, 回到上午收盘累计
  if (mins <= 15 * 60) return 24 + (mins - 13 * 60) / 5 + 1;
  return PM_POINTS;                                 // 收盘后
}

// 每日首次: 抓昨日 5 分钟线, 建立累计表
static void fetchPrevAmountOnce() {
  struct tm t;
  if (!getLocalTime(&t, 20)) return;
  if (t.tm_wday == 0 || t.tm_wday == 6) return;      // 周末不取
  if (t.tm_wday == s_amtDay) return;                 // 今天已取过
  if (WiFi.status() != WL_CONNECTED) return;
  s_amtDay = t.tm_wday;                              // 先标记, 失败下周一再试

  for (int i = 0; i < g_nStock; i++) {
    const char* c = g_stocks[i].code;
    if (strncmp(c, "sh", 2) != 0 && strncmp(c, "sz", 2) != 0) continue;  // 只 A 股
    // datalen=60 覆盖昨日(约12根)+今日(48根), 只取昨日部分
    String url = "https://quotes.sina.cn/cn/api/jsonp_v2.php/var%20_=/"
                 "CN_MarketDataService.getKLineData?symbol=" + String(c) +
                 "&scale=5&ma=no&datalen=60";
    HTTPClient h;
    h.setConnectTimeout(3000);
    h.setTimeout(5000);
    h.setReuse(false);
    if (!h.begin(url)) continue;
    h.addHeader("Referer", "https://finance.sina.com.cn");
    if (h.GET() == 200) {
      String body = h.getString();
      // 提取昨天日期字符串, 用于过滤
      char yday[11] = "";
      {
        time_t ty = time(nullptr) - 86400;
        struct tm* tmY = localtime(&ty);
        snprintf(yday, sizeof(yday), "%04d-%02d-%02d",
                 tmY->tm_year + 1900, tmY->tm_mon + 1, tmY->tm_mday);
      }
      // 遍历所有 \"day\":\"yyyy-mm-dd HH:MM:SS\",\"amount\":xxx, 累加昨日
      double cum[PM_POINTS + 1];
      for (int k = 0; k <= PM_POINTS; k++) cum[k] = 0;
      int pt = 0;   // 5 分钟点索引
      int search = 0;
      while (true) {
        int dpos = body.indexOf("\"day\":", search);
        if (dpos < 0) break;
        int dq1 = body.indexOf('"', dpos + 6);
        int dq2 = body.indexOf('"', dq1 + 1);
        if (dq1 < 0 || dq2 < 0) break;
        String day = body.substring(dq1 + 1, dq2);
        int apos = body.indexOf("\"amount\":", dq2);
        if (apos < 0) break;
        double amt = atof(body.c_str() + apos + 9);
        if (day.startsWith(yday)) {
          if (pt <= PM_POINTS) cum[pt] = amt;
          pt++;
        }
        search = apos + 10;
      }
      // 转累计
      double acc = 0;
      for (int k = 0; k <= PM_POINTS; k++) { acc += cum[k]; s_prevAmtCum[i][k] = acc; }
    }
    h.end();
    vTaskDelay(pdMS_TO_TICKS(80));   // 限速
  }
  Serial.printf("[Stock] 昨分时累计表已建 %d 只\n", (int)g_nStock);
}

// 每次实时抓取后: 按当前时刻把"昨日同时刻累计额"写入 q.prevAmount
static void applyPrevAmount() {
  struct tm t;
  if (!getLocalTime(&t, 50)) return;
  int pt = timeToPoint(t.tm_hour, t.tm_min);
  for (int i = 0; i < g_nStock; i++) {
    double v = s_prevAmtCum[i][pt];
    if (quotesLock()) { g_quotes[i].prevAmount = v; quotesUnlock(); }
  }
}

static void stockTask(void* arg) {
  for (;;) {
    if (s_refreshNow) {
      s_refreshNow = false;
      fetchOnce();
      continue;
    }
    bool trading = isTradingTime();
    fetchOnce();
    // 每日开盘后第一次: 建昨日分时累计表(同时段放量对比用)
    fetchPrevAmountOnce();
    // 每次抓取后按当前时刻刷新"昨日同时刻累计额"
    applyPrevAmount();
    // 交易时段按配置刷新, 盘外 60s 一次, 失败退避
    int delaySec = trading ? g_refreshSec : 60;
    for (int i = 0; i < delaySec * 10 && !s_refreshNow; i++)
      vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void stockTaskStart() {
  xTaskCreatePinnedToCore(stockTask, "stock", 8192, nullptr, 1, nullptr, 0);
}
