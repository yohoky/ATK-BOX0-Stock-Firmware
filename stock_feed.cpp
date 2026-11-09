#include "stock_feed.h"
#include "app_config.h"
#include "ui_display.h"
#include <WiFi.h>
#include <HTTPClient.h>

static volatile bool s_refreshNow = false;

void stockRequestRefresh() { s_refreshNow = true; }
void stockRebuildTicker()  { uiRedrawTicker(); }

// v7.3: 支持 A股 / 港股 / 美股 / 国际指数 / 国内商品主连 / 外盘期货现货
enum Mkt { MKT_A, MKT_HK, MKT_US, MKT_IDX, MKT_FUT_CN, MKT_FUT_GL };

static Mkt marketOf(const char* code) {
  if (strncmp(code, "rt_hk", 5) == 0 || strncmp(code, "r_hk", 4) == 0) return MKT_HK;
  if (strncmp(code, "gb_", 3)  == 0) return MKT_US;
  if (strncmp(code, "int_", 4) == 0) return MKT_IDX;
  if (strncmp(code, "nf_", 3)  == 0) return MKT_FUT_CN;
  if (strncmp(code, "hf_", 3)  == 0) return MKT_FUT_GL;
  return MKT_A;
}

// 新浪返回的期货/外盘/指数名称是 GBK, 且各品种字段位置不统一; 直接内置短中文名
// (<=4 字, 适配 240x240 点阵), 既避免编码转换, 也保证名称在屏幕上显示得下。
// 只列实测 2026-11-09 能返回数据的代码; nf_IF0/int_dax 等格式不同或已下线, 不纳入。
struct CodeName { const char* code; const char* name; };
static const CodeName kCodeNames[] = {
  // 国内商品期货主连 (上期所等)
  { "nf_AU0", "沪金主连" }, { "nf_AG0", "沪银主连" }, { "nf_CU0", "沪铜主连" },
  { "nf_RB0", "螺纹主连" }, { "nf_SC0", "原油主连" },
  // 外盘期货 / 现货
  { "hf_GC",  "纽约金" },   { "hf_SI",  "纽约银" },   { "hf_CL",  "纽约油" },
  { "hf_OIL", "布伦特油" }, { "hf_XAU", "伦敦金" },   { "hf_XAG", "伦敦银" },
  { "hf_HG",  "纽约铜" },   { "hf_NG",  "天然气" },
  // 国际指数
  { "int_dji", "道琼斯" },     { "int_nasdaq", "纳斯达克" },
  { "int_sp500", "标普500" },  { "int_ftse", "富时100" },
  { "int_nikkei", "日经225" }, { "int_hangseng", "恒生" },
  // 美股指数 / 常见个股 (优先不带 $ 的别名, gb_spx 无效)
  { "gb_dji", "道琼斯" },   { "gb_ixic", "纳斯达克" },
  { "gb_inx", "标普500" },  { "gb_ndx", "纳指100" },
  { "gb_$dji", "道琼斯" },  { "gb_$ixic", "纳斯达克" },
  { "gb_$inx", "标普500" }, { "gb_$ndx", "纳指100" },
  { "gb_aapl", "苹果" },    { "gb_nvda", "英伟达" },  { "gb_tsla", "特斯拉" },
  { "gb_msft", "微软" },    { "gb_amzn", "亚马逊" },  { "gb_googl", "谷歌" },
  { "gb_meta", "Meta" },    { "gb_baba", "阿里巴巴" },
};
static const char* builtinName(const char* code) {
  for (unsigned i = 0; i < sizeof(kCodeNames) / sizeof(kCodeNames[0]); i++)
    if (strcmp(kCodeNames[i].code, code) == 0) return kCodeNames[i].name;
  return nullptr;
}

// 解析一条行情, 字段布局见项目 README (实测于 2026-11)
static void parseQuote(const char* code, const char* payload, Quote& q) {
  char* buf = strndup(payload, 600);
  char* fields[40];
  int nf = 0;
  char* save = nullptr;
  for (char* tok = strtok_r(buf, ",", &save); tok && nf < 40; tok = strtok_r(nullptr, ",", &save))
    fields[nf++] = tok;

  Mkt mkt = marketOf(code);
  q.valid = false;
  strncpy(q.code, code, STK_CODE_LEN - 1); q.code[STK_CODE_LEN - 1] = 0;
  q.ts = millis();
  q.name[0] = 0;
  const char* bn = builtinName(code);
  if (bn) { strncpy(q.name, bn, 47); q.name[47] = 0; }

  if (mkt == MKT_A && nf > 32) {
    // [0]名称 [1]开 [2]昨收 [3]现价 [4]高 [5]低 [8]成交量(手) [9]成交额
    if (!bn) { String nm = GBKtoUTF8((uint8_t*)fields[0], strlen(fields[0]));
               strncpy(q.name, nm.c_str(), 47); q.name[47] = 0; }
    q.open = atof(fields[1]);  q.prevClose = atof(fields[2]);
    q.price = atof(fields[3]); q.high = atof(fields[4]); q.low = atof(fields[5]);
    q.volume = atof(fields[8]); q.amount = atof(fields[9]);
    if (q.price > 0 && q.prevClose > 0) {
      q.chg = q.price - q.prevClose;
      q.chgpct = q.chg / q.prevClose * 100.0f;
      q.valid = true;
    }
  } else if (mkt == MKT_HK && nf > 8) {
    // [0]英文名 [1]中文名 [2]开 [3]昨收 [4]高 [5]低 [6]现价 [7]涨跌 [8]涨跌%
    if (!bn) { String nm = GBKtoUTF8((uint8_t*)fields[1], strlen(fields[1]));
               strncpy(q.name, nm.c_str(), 47); q.name[47] = 0; }
    q.open = atof(fields[2]);  q.prevClose = atof(fields[3]);
    q.high = atof(fields[4]);  q.low = atof(fields[5]);
    q.price = atof(fields[6]);
    q.chg = atof(fields[7]);   q.chgpct = atof(fields[8]);
    if (q.price > 0) q.valid = true;
  } else if (mkt == MKT_US && nf > 4) {
    // [0]名称(GBK/UTF8 混用) [1]现价 [2]涨跌% [3]时间 [4]涨跌额 [5]开盘
    if (!bn) {
      const char* nmRaw = fields[0];
      int nlen = strlen(nmRaw);
      if (utf8Valid((const uint8_t*)nmRaw, nlen)) {
        strncpy(q.name, nmRaw, 47); q.name[47] = 0;
      } else {
        String nm = GBKtoUTF8((uint8_t*)nmRaw, nlen);
        strncpy(q.name, nm.c_str(), 47); q.name[47] = 0;
      }
    }
    q.price = atof(fields[1]);
    q.chgpct = atof(fields[2]);
    q.chg = atof(fields[4]);
    // v7.3: fields[5] 是开盘价不是昨收; 昨收 = 现价 - 涨跌额(实测与 [22] 一致)
    q.prevClose = q.price - q.chg;
    q.open = nf > 5 ? atof(fields[5]) : q.prevClose;
    q.high = nf > 6 ? atof(fields[6]) : q.price;
    q.low  = nf > 7 ? atof(fields[7]) : q.price;
    if (q.price > 0) q.valid = true;
  } else if (mkt == MKT_FUT_CN && nf > 14) {
    // 国内商品主连 nf_XXX0: [2]开 [3]高 [4]低 [8]现价 [10]昨结算 [13]成交额 [14]成交量
    if (!bn) { String nm = GBKtoUTF8((uint8_t*)fields[0], strlen(fields[0]));
               strncpy(q.name, nm.c_str(), 47); q.name[47] = 0; }
    q.open = atof(fields[2]); q.high = atof(fields[3]); q.low = atof(fields[4]);
    q.price = atof(fields[8]); q.prevClose = atof(fields[10]);
    q.amount = atof(fields[13]); q.volume = atof(fields[14]);
    if (q.price > 0 && q.prevClose > 0) {
      q.chg = q.price - q.prevClose;
      q.chgpct = q.chg / q.prevClose * 100.0f;
      q.valid = true;
    }
  } else if (mkt == MKT_FUT_GL && nf > 8) {
    // 外盘 hf_XXX: [0]现价 [4]高 [5]低 [7]昨收/昨结算 [8]开盘
    q.price = atof(fields[0]);
    q.high = atof(fields[4]); q.low = atof(fields[5]);
    q.prevClose = atof(fields[7]);
    q.open = atof(fields[8]);
    if (q.price > 0 && q.prevClose > 0) {
      q.chg = q.price - q.prevClose;
      q.chgpct = q.chg / q.prevClose * 100.0f;
      q.valid = true;
    }
  } else if (mkt == MKT_IDX && nf >= 4) {
    // 国际指数 int_XXX: [0]名称 [1]现价 [2]涨跌额 [3]涨跌%
    if (!bn) { String nm = GBKtoUTF8((uint8_t*)fields[0], strlen(fields[0]));
               strncpy(q.name, nm.c_str(), 47); q.name[47] = 0; }
    q.price = atof(fields[1]);
    q.chg = atof(fields[2]);
    q.chgpct = atof(fields[3]);
    q.prevClose = q.price - q.chg;
    q.open = q.prevClose; q.high = q.price; q.low = q.price;
    if (q.price > 0) q.valid = true;
  }
  if (!q.name[0]) { strncpy(q.name, code, 47); q.name[47] = 0; }
  free(buf);
}

// 返回本次成功解析出的有效行情条数(0 = 网络未就绪 / HTTP 失败 / 解析失败)
static int fetchOnce() {
  if (WiFi.status() != WL_CONNECTED || g_nStock == 0) return 0;
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
  if (!http.begin(url)) return 0;
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
    http.end();
    int n = 0;
    if (quotesLock(50)) {
      for (int i = 0; i < g_nStock; i++) if (g_quotes[i].valid) n++;
      quotesUnlock();
    }
    return n;
  } else {
    Serial.printf("[Stock] HTTP %d\n", code);
  }
  http.end();
  return 0;
}

// ---------------- 同时段放量对比 ----------------
// 用户需求: 「放」= 今日此刻累计成交额 - 前一交易日同一时刻的累计成交额(单位亿元),
//           + 表示放量 / - 表示缩量。注意是「前一交易日」而非「自然日昨天」:
//           周一、节假日的上一自然日没有交易, 必须自动回退到最近一个有完整行情的交易日。
// v7.3 修复(单只大屏「放」一直不显示的两个根因):
//   ① 旧版按「自然日昨天」过滤, 周一/节假日前一个自然日无交易 -> 基准恒为 0;
//      现改为从 120 根 5 分钟线里自动挑「上一个完整交易日」作基准。
//   ② 新浪 amount 是带引号字符串 "amount":"64225321.06", 旧版 atof 指到引号上,
//      返回值恒为 0; 现跳过引号再取数。
// 新浪 5 分钟线: day 为该根结束时刻(09:35..11:30 / 13:05..15:00), 完整日 48 根。
#define PM_POINTS 48

// 累计成交额表: [股票][根数], 第 k 点 = 基准日开盘到第 k 根末的累计额(元)
static double s_prevAmtCum[MAX_STOCKS][PM_POINTS + 1];
// 按「本地日期 YYYYMMDD」而不是 wday 去重: 设备可能连续运行数周,
// 只比 wday 会让下周一沿用上周一的基准表(基准永远停在旧交易日)。
static uint32_t s_amtBuildDay = 0;  // 已成功建表的本地日期, 0=尚未建表
static char    s_amtBase[11] = "";  // 基准交易日 yyyy-mm-dd (供 /api/diag 诊断)
static int     s_amtOk = 0;         // 成功建表的股票数
static void applyPrevAmount();      // 前向声明: 建表成功后立刻刷新一次当前行情

// 把 HH:MM 映射到「已完成的 5 分钟 K 线根数」(0..48), 与基准表下标严格一致。
static int timeToPoint(int hh, int mm) {
  int mins = hh * 60 + mm;
  if (mins < 9 * 60 + 30) return 0;                          // 9:30 前
  if (mins <= 11 * 60 + 30) return (mins - (9 * 60 + 30)) / 5;    // 9:35→1 ... 11:30→24
  if (mins < 13 * 60) return 24;                             // 午休: 上午 24 根
  if (mins <= 15 * 60) return 24 + (mins - 13 * 60) / 5;    // 13:05→25 ... 15:00→48
  return PM_POINTS;                                          // 收盘后: 全天 48 根
}

// 从 K 线里挑「对比基准日」:
//   数据源最新日期 == 今天 -> 取次新的完整交易日; 否则取最新的完整交易日。
//   "完整" = 该日 >= 40 根(容忍个别缺根), 避免拿半天的日期当基准。
static bool pickBaseDay(const String& body, const char* today, char* out, size_t outN) {
  String days[8]; int cnt[8]; int n = 0;
  int search = 0;
  for (;;) {
    int dpos = body.indexOf("\"day\":\"", search);
    if (dpos < 0) break;
    const char* p = body.c_str() + dpos + 7;
    if (strlen(p) < 10) break;
    char d[11]; memcpy(d, p, 10); d[10] = 0;
    int k;
    for (k = 0; k < n; k++) if (days[k] == d) break;
    if (k == n) { if (n < 8) { days[n] = d; cnt[n] = 1; n++; } }
    else cnt[k]++;
    search = dpos + 7;
  }
  if (n == 0) return false;
  int start = (n >= 2 && days[n - 1] == today) ? n - 2 : n - 1;
  for (int k = start; k >= 0; k--) {
    if (cnt[k] >= 40) { strncpy(out, days[k].c_str(), outN - 1); out[outN - 1] = 0; return true; }
  }
  return false;
}

// 每日首次: 抓上一交易日 5 分钟线, 建立「同时刻累计成交额」表
static void fetchPrevAmountOnce() {
  struct tm t;
  if (!getLocalTime(&t, 20)) return;
  if (t.tm_wday == 0 || t.tm_wday == 6) return;             // 周末不取
  uint32_t ymd = (uint32_t)(t.tm_year + 1900) * 10000u +
                 (uint32_t)(t.tm_mon + 1) * 100u + (uint32_t)t.tm_mday;
  if (s_amtBuildDay == ymd && s_amtOk > 0) return;           // 今天已成功建表, 失败则允许重试
  if (WiFi.status() != WL_CONNECTED) return;
  char today[11];
  snprintf(today, sizeof(today), "%04d-%02d-%02d",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);

  int ok = 0;
  for (int i = 0; i < g_nStock; i++) {
    const char* c = g_stocks[i].code;
    // 只有沪深 A 股有该 5 分钟线接口; 期货/外盘/美股/港股基准置 0 -> UI 显示「放 --」
    if (strncmp(c, "sh", 2) != 0 && strncmp(c, "sz", 2) != 0) {
      for (int k = 0; k <= PM_POINTS; k++) s_prevAmtCum[i][k] = 0;
      continue;
    }
    // datalen=120 覆盖约 2.5 个交易日, 确保能找到上一个「完整」交易日
    String url = "https://quotes.sina.cn/cn/api/jsonp_v2.php/var%20_=/"
                 "CN_MarketDataService.getKLineData?symbol=" + String(c) +
                 "&scale=5&ma=no&datalen=120";
    HTTPClient h;
    h.setConnectTimeout(3000);
    h.setTimeout(5000);
    h.setReuse(false);
    if (!h.begin(url)) continue;
    h.addHeader("Referer", "https://finance.sina.com.cn");
    if (h.GET() == 200) {
      String body = h.getString();
      char base[11] = "";
      if (pickBaseDay(body, today, base, sizeof(base))) {
        double cum[PM_POINTS + 1];
        for (int k = 0; k <= PM_POINTS; k++) cum[k] = 0;
        // 第二遍: 按时间正序把基准日每根 amount 放进 cum[k]
        int k = 0, search = 0;
        for (;;) {
          int dpos = body.indexOf("\"day\":\"", search);
          if (dpos < 0) break;
          const char* dp = body.c_str() + dpos + 7;
          if (strlen(dp) < 10) break;
          int apos = body.indexOf("\"amount\":", dpos + 7);
          search = dpos + 7;
          if (strncmp(dp, base, 10) != 0) continue;
          if (apos < 0) break;
          // ★ amount 可能是带引号字符串, 必须跳过引号再 atof
          const char* vp = body.c_str() + apos + 9;
          while (*vp == '"' || *vp == ' ' || *vp == '\t') vp++;
          if (++k <= PM_POINTS) cum[k] = atof(vp);
        }
        // 前缀和: cum[k] = 基准日开盘到第 k 根末的累计成交额
        double acc = 0;
        for (int q = 0; q <= PM_POINTS; q++) { acc += cum[q]; s_prevAmtCum[i][q] = acc; }
        if (acc > 0) { ok++; strncpy(s_amtBase, base, sizeof(s_amtBase) - 1); }
      }
    }
    h.end();
    vTaskDelay(pdMS_TO_TICKS(80));   // 限速
  }
  s_amtOk = ok;
  if (ok > 0) {
    s_amtBuildDay = ymd;
    applyPrevAmount();   // 基准一建好就写入 g_quotes, 盘后也能立刻看到「放」
  }
  Serial.printf("[Stock] 同时段基准 %s 建表 %d 只\n", s_amtBase, ok);
}

// 每次实时抓取后: 按当前时刻把「上一交易日同时刻累计额」写入 q.prevAmount
static void applyPrevAmount() {
  struct tm t;
  if (!getLocalTime(&t, 50)) return;
  int pt = timeToPoint(t.tm_hour, t.tm_min);
  if (pt < 0) pt = 0;
  if (pt > PM_POINTS) pt = PM_POINTS;
  for (int i = 0; i < g_nStock; i++) {
    double v = s_prevAmtCum[i][pt];
    if (quotesLock()) { g_quotes[i].prevAmount = v; quotesUnlock(); }
  }
}

// /api/diag 诊断: 基准交易日 + 成功建表只数
void stockPrevAmtInfo(char* day, size_t dayN, int* ok, double* baseAmt) {
  if (day && dayN) { strncpy(day, s_amtBase, dayN - 1); day[dayN - 1] = 0; }
  if (ok) *ok = s_amtOk;
  if (baseAmt) *baseAmt = s_prevAmtCum[0][PM_POINTS];
}

static void stockTask(void* arg) {
  // 开机后无论是否处在「信息更新」时段, 都要先成功抓到一次, 避免新设备空屏。
  // v7.1.1 修复: 旧版开机时 WiFi/DNS 尚未就绪, fetchOnce() 静默失败却把 first 置 false,
  //   于是盘外(例如凌晨开机)会一直空屏到第二天 9:00。现在按结果判定 + 退避重试。
  bool first = true;
  int  bootFail = 0;
  for (;;) {
    if (s_refreshNow) {
      s_refreshNow = false;
      if (fetchOnce() > 0) first = false;
      fetchPrevAmountOnce();
      applyPrevAmount();
      continue;
    }
    // v7.1: 是否抓行情由「信息更新」时间范围决定, 不再写死 9:15-15:05
    bool inWin = isTradingTime();
    if (inWin || first || g_fetchOutside) {
      int got = fetchOnce();
      if (got > 0) {
        first = false;
        bootFail = 0;
        // 每日第一次: 建昨日分时累计表(同时段放量对比用)
        fetchPrevAmountOnce();
        // 每次抓取后按当前时刻刷新"昨日同时刻累计额"
        applyPrevAmount();
      } else if (first) {
        bootFail++;
        if (bootFail > 120) first = false;   // 长期离线(约 1 小时): 等进窗口再抓
        int wait = (bootFail < 10) ? 50 : 300;   // 前 10 次每 5 秒, 之后每 30 秒
        for (int i = 0; i < wait && !s_refreshNow; i++) vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
    }
    // 时段内按 g_refreshSec 刷新; 时段外: 允许低频就 300s, 否则 10s 空转等进窗口
    int delaySec = inWin ? g_refreshSec : (g_fetchOutside ? 300 : 10);
    for (int i = 0; i < delaySec * 10 && !s_refreshNow; i++)
      vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void stockTaskStart() {
  xTaskCreatePinnedToCore(stockTask, "stock", 8192, nullptr, 1, nullptr, 0);
}
