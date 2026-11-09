# -*- coding: utf-8 -*-
"""
BOX0 股票固件 · 界面渲染图生成脚本

用 Pillow 按固件真实坐标(240x240)重绘屏幕, 输出 README 用的 PNG。
这是「界面示意图」, 不是实拍照片 —— 数据为示例数据。

用法:  python tools/render_screens.py
输出:  docs/*.png
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs")
os.makedirs(OUT, exist_ok=True)

S = 3                     # 屏幕放大倍数
W = H = 240               # 逻辑分辨率
BG = "#0B0E14"
FG = "#E8EDF2"
UP = "#FF4D4F"            # 红涨 (A 股惯例)
DOWN = "#22C55E"          # 绿跌
ACC = "#22D3EE"           # 青(强调)
FLAT = "#7A8794"
WARN = "#F59E0B"
SEL = "#1D9E75"
DIM = "#1E2630"
LINE = "#20262E"
SHELL = "#232A33"
SHELL_EDGE = "#39424E"

FONT_R = "C:/Windows/Fonts/msyh.ttc"
FONT_B = "C:/Windows/Fonts/msyhbd.ttc"
if not os.path.exists(FONT_R):
    FONT_R = FONT_B = "C:/Windows/Fonts/simhei.ttf"
_fcache = {}


def font(px, bold=False):
    key = (px, bold)
    if key not in _fcache:
        _fcache[key] = ImageFont.truetype(FONT_B if bold else FONT_R, max(6, int(px * S)))
    return _fcache[key]


def text(d, x, y, s, px=10, color=FG, bold=False, anchor=None):
    d.text((x * S, y * S), str(s), font=font(px, bold), fill=color, anchor=anchor)


def rect(d, x, y, w, h, fill=None, outline=None, width=1, radius=0):
    box = [x * S, y * S, (x + w) * S, (y + h) * S]
    if radius:
        d.rounded_rectangle(box, radius=radius * S, fill=fill, outline=outline, width=width * S)
    else:
        d.rectangle(box, fill=fill, outline=outline, width=width * S)


def line(d, x1, y1, x2, y2, color=LINE, width=1):
    d.line([x1 * S, y1 * S, x2 * S, y2 * S], fill=color, width=width * S)


def new_screen():
    """返回 (画布, draw, 屏幕左上角偏移) —— 带设备外壳"""
    pad = 22
    im = Image.new("RGB", ((W + pad * 2) * S, (H + pad * 2) * S), "#11161D")
    d = ImageDraw.Draw(im)
    rect(d, 2, 2, W + pad * 2 - 4, H + pad * 2 - 4, fill=SHELL, outline=SHELL_EDGE,
         width=1, radius=18)
    rect(d, pad - 2, pad - 2, W + 4, H + 4, fill="#05070A", radius=6)
    # 把屏幕内容画在独立层, 再贴回去
    scr = Image.new("RGB", (W * S, H * S), BG)
    return im, scr, pad


def finish(im, scr, pad, name):
    im.paste(scr, (pad * S, pad * S))
    p = os.path.join(OUT, name)
    im.save(p, optimize=True)
    print("  ->", p)


STOCKS = [
    ("上证指数", "3686.17", "+2.16%", UP),
    ("深证成指", "11336.00", "+4.22%", UP),
    ("洛阳钼业", "10.18", "+1.29%", UP),
    ("长江证券", "7.08", "-0.84%", DOWN),
    ("中科曙光", "98.60", "+3.51%", UP),
    ("国盛金控", "12.34", "-1.20%", DOWN),
]


def statusbar(d, news_new=False):
    """状态栏 6 项: 星期 / 时间 / 页码 / 音量 / 电量 / WiFi(已连接只显示图标)
    v7.2 另有「新快讯」红点, 进新闻页即清."""
    rect(d, 0, 0, W, 19, fill="#05070A")
    if news_new:
        rect(d, 116, 4, 5, 10, fill=UP)
    text(d, 3, 2, "周日", 10, FG)
    text(d, 42, 2, "00:12", 10, FG)
    text(d, 88, 2, "1/3", 10, ACC)
    # 喇叭图标 + 数字
    rect(d, 126, 7, 2, 6, fill=ACC)
    rect(d, 128, 5, 2, 10, fill=ACC)
    rect(d, 130, 3, 2, 14, fill=ACC)
    text(d, 135, 2, "60", 10, ACC)
    # 电池: 数字在图标内部, 60-100 绿色
    bx, by, bw, bh = 166, 3, 28, 12
    rect(d, bx, by, bw, bh, fill=None, outline=DOWN, width=1)
    rect(d, bx + bw, by + 3, 2, bh - 6, fill=ACC)
    rect(d, bx + 2, by + 2, bw - 4, bh - 4, fill=DIM)
    rect(d, bx + 2, by + bh - 4, bw - 4, 2, fill=DOWN)
    text(d, bx + bw / 2, by + bh / 2 - 1, "100", 7, FG, bold=True, anchor="mm")
    # v7.1: 已连接只显示缩小的 3 格信号, 不再带「连」字; 中/热 才带一个汉字
    for i in range(3):
        h = 3 + i * 4
        rect(d, 210 + i * 4, 16 - h, 3, h, fill=ACC if i < 3 else LINE)
    line(d, 0, 19, W, 19, LINE)


def draw_list():
    im, scr, pad = new_screen()
    d = ImageDraw.Draw(scr)
    statusbar(d, True)          # v7.2: 有新快讯时状态栏出现红点
    for i, (name, price, chg, col) in enumerate(STOCKS):
        y = 21 + i * 36
        rect(d, 0, y, 3, 32, fill=col)
        text(d, 9, y + 4, name, 12, FG, bold=True)
        text(d, 9, y + 21, "sh000001" if i == 0 else "sz399001" if i == 1 else "60%d%03d" % (i, i * 137 % 1000),
             7, FLAT)
        text(d, 158, y + 6, price, 12, col, bold=True, anchor="ra")
        text(d, 232, y + 22, chg, 10, col, anchor="ra")
        if i < 5:
            line(d, 6, y + 34, 234, y + 34, LINE)
    finish(im, scr, pad, "ui_list.png")


def draw_single():
    """v7.3 单只大屏: 最下面一行新增彩色「放」(较前一交易日同时刻, 单位亿元)"""
    im, scr, pad = new_screen()
    d = ImageDraw.Draw(scr)
    statusbar(d)
    text(d, 8, 22, "上证指数", 16, FG, bold=True)
    text(d, 236, 24, "1/14", 9, FLAT, anchor="ra")
    text(d, 8, 42, "sh000001", 9, FLAT)
    line(d, 0, 58, 240, 58, LINE)
    text(d, 8, 76, "3686.17", 30, UP, bold=True)
    text(d, 8, 122, "+77.89   +2.16%", 13, UP, bold=True)
    text(d, 8, 154, "开 3621.40   高 3692.05", 11, FG)
    text(d, 8, 174, "低 3618.72   昨收 3608.28", 11, FG)
    text(d, 8, 196, "额 6825.33亿", 11, FG)
    text(d, 126, 196, "放 -901.02亿", 11, DOWN, bold=True)  # 缩量=绿, 放量用 UP=红
    text(d, 8, 222, "M 短按切回列表   左右键换股", 8, FLAT)
    finish(im, scr, pad, "ui_single.png")


MENU = ["自动熄屏", "测试提示音", "设备信息", "恢复出厂", "关机",
        "工作模式", "实时新闻", "提醒间隔", "底部快讯"]  # v7.3: 菜单共 16 项, 展示末页附近


def draw_menu():
    im, scr, pad = new_screen()
    d = ImageDraw.Draw(scr)
    rect(d, 0, 0, W, 24, fill="#141A22")
    text(d, 8, 4, "菜单", 13, FG, bold=True)
    text(d, 232, 6, "M长按退出", 8, FLAT, anchor="ra")
    vals = ["1 分钟", "试听", "版本说明", "恢复默认", "关机",
            "股票", "10条", "30分钟", "关"]
    for i, nm in enumerate(MENU):
        y = 26 + i * 21          # v7.3: 行高收紧, 给底部提示留出间距
        if i == 7:
            rect(d, 4, y - 3, 232, 20, fill=SEL, radius=3)
        c = "#0B0E14" if i == 7 else FG
        text(d, 10, y, nm, 11, c, bold=True)
        text(d, 230, y + 1, vals[i], 9, c if i == 7 else ACC, anchor="ra")
    rect(d, 237, 26, 2, 188, fill=DIM)
    rect(d, 237, 26, 2, 150, fill=ACC)
    text(d, 6, 222, "左右选 M确定 双键返回 · 共16项", 8, FLAT)
    finish(im, scr, pad, "ui_menu.png")


def draw_statusbar_zoom():
    """状态栏 6 项标注图"""
    scale = 5
    global S
    old = S
    S = scale
    w = W * scale
    h = 19 * scale + 118
    im = Image.new("RGB", (w, h), "#11161D")
    d = ImageDraw.Draw(im)
    scr = Image.new("RGB", (W * scale, 19 * scale), "#05070A")
    sd = ImageDraw.Draw(scr)
    statusbar(sd)
    im.paste(scr, (0, 0))
    items = [(3, 34, "① 星期"), (42, 76, "② 时间"), (88, 120, "③ 第几页"),
             (126, 160, "④ 音量\n喇叭+数字"), (166, 196, "⑤ 电量\n数字在电池内\n60-100 绿色"),
             (206, 220, "⑥ WiFi\n信号+模式")]
    col = [FG, FG, ACC, ACC, DOWN, ACC]
    for (x, label_x, label), c in zip(items, col):
        d.line([(x + 6) * scale, 19 * scale, label_x * scale / 5 * 5, 19 * scale + 16], fill=c, width=2)
    # 用绝对像素写标注
    labels = [("① 星期", 10), ("② 时间", 175), ("③ 页码", 340),
              ("④ 音量", 505), ("⑤ 电量 60-100 绿", 665), ("⑥ WiFi 仅图标", 855)]
    for (lb, px), c in zip(labels, col):
        d.text((px, 19 * scale + 24), lb, font=ImageFont.truetype(FONT_B, 17), fill=c)
    d.text((10, 19 * scale + 58),
           "六项固定显示：星期 · 时间 · 第几页 · 音量(喇叭图标+数字) · 电量(电池内数字) · WiFi 信号(已连接只显示图标)",
           font=ImageFont.truetype(FONT_R, 15), fill=FLAT)
    p = os.path.join(OUT, "ui_statusbar.png")
    im.save(p, optimize=True)
    print("  ->", p)
    S = old


def draw_web():
    """v7.3 网页管理界面示意: OTA / 工作模式 / 快讯 / 版本说明 / 中文开放 WiFi / 放量诊断"""
    S = 2
    globals()["S"] = S
    w, h = 520 * S, 700 * S
    im = Image.new("RGB", (w, h), "#0E0E0E")
    d = ImageDraw.Draw(im)
    rect(d, 10, 10, 500, 44, fill="#1B1B1B", outline="#2A2A2A", radius=10)
    text(d, 22, 20, "BOX0 股票固件 · 管理页", 13, FG, bold=True)
    text(d, 498, 22, "v7.3", 9, FLAT, anchor="ra")
    tabs = ["股票", "设置", "设备", "升级"]
    for i, t in enumerate(tabs):
        x = 10 + i * 126
        on = (i == 3)
        rect(d, x, 62, 120, 30, fill=SEL if on else "#1B1B1B", radius=8)
        text(d, x + 60, 69, t, 11, "#FFFFFF" if on else "#999999", bold=on, anchor="ma")

    # 1) OTA
    rect(d, 10, 102, 500, 70, fill="#1B1B1B", radius=10)
    text(d, 22, 110, "固件升级（网页 OTA）", 12, FG, bold=True)
    text(d, 22, 130, "当前 v7.3 · 约 12 秒写完自动重启 · 不用 USB / 下载模式 / RESET", 9, FLAT)
    rect(d, 22, 146, 300, 20, fill="#1A1A1A", outline="#333333", radius=6)
    text(d, 28, 150, "box0_stock_v7.3.bin", 8, FLAT)
    rect(d, 330, 146, 168, 20, fill=SEL, radius=6)
    text(d, 414, 150, "上传并升级", 9, "#FFFFFF", bold=True, anchor="ma")

    # 2) 工作模式
    rect(d, 10, 180, 500, 76, fill="#1B1B1B", radius=10)
    text(d, 22, 186, "工作模式", 12, FG, bold=True)
    text(d, 498, 189, "当前：股票模式", 9, ACC, anchor="ra")
    text(d, 22, 208, "开机一律股票模式 · 提示音与 MP3 自动交接 I2S", 9, FLAT)
    for i, t in enumerate(["切换收音机", "上一台", "播放/暂停", "停止"]):
        x = 22 + i * 122
        rect(d, x, 228, 112, 20, fill="#26303A", radius=5)
        text(d, x + 56, 232, t, 8, FG, anchor="ma")

    # 3) 实时快讯
    rect(d, 10, 262, 500, 76, fill="#1B1B1B", radius=10)
    text(d, 22, 268, "实时快讯 · v7.3 整页标题", 12, FG, bold=True)
    text(d, 498, 271, "10 条 · 60 秒", 9, ACC, anchor="ra")
    text(d, 22, 290, "东方财富 7×24 · 点开看全文 · 手动/自动切换 · 底部滚动条", 9, FLAT)
    for i, t in enumerate(["间隔 / 提示音", "60 秒", "立即抓取", "查看快讯"]):
        x = 22 + i * 122
        rect(d, x, 310, 112, 20, fill="#26303A", radius=5)
        text(d, x + 56, 314, t, 8, ACC if i >= 2 else FG, anchor="ma")

    # 4) 版本更新说明 (v7.3)
    rect(d, 10, 344, 500, 58, fill="#1B1B1B", radius=10)
    text(d, 22, 350, "版本更新说明（设备信息页同步显示）", 11, FG, bold=True)
    text(d, 22, 368, "v7.3 沪金银主连/美股指数/外盘商品一键添加、标普500 gb_inx、", 8, FLAT)
    text(d, 22, 384, "中文开放WiFi、提醒间隔、底部快讯条、修复单只大屏「放」…", 8, FLAT)

    # 5) v7.3 品种快捷添加
    rect(d, 10, 408, 500, 34, fill="#1B1B1B", radius=10)
    text(d, 22, 414, "股票页 · 一键添加", 10, FG, bold=True)
    text(d, 180, 415, "沪金主连  沪银主连  伦敦金  布伦特油  标普500 gb_inx", 8, ACC)

    # 6) 历史 WiFi (含中文开放网络)
    text(d, 10, 450, "已保存的 WiFi（最多 8 组 · 中文/GBK 原始字节 · 锁定 2.4G BSSID）", 10, FG, bold=True)
    for i, (nm, mark) in enumerate([("Home-2.4G", "★ 当前"), ("东安湖艺术中心", "开放"),
                                    ("Office-Guest", "开放"), ("Coffee-5G", "仅5G")]):
        y = 468 + i * 32
        rect(d, 10, y, 500, 28, fill="#1A1A1A", outline="#2D2D2D", radius=8)
        label = ("★ " if mark.startswith("★") else "") + nm
        if mark == "开放": label += "  (开放·无密码)"
        if mark == "仅5G": label += "  (本设备不支持)"
        text(d, 22, y + 6, label, 9, FG)
        text(d, 498, y + 6, "连接    删除", 8, FLAT, anchor="ra")

    # 7) 诊断 JSON (v7.3 放量字段)
    rect(d, 10, 602, 500, 90, fill="#111111", outline="#333333", radius=8)
    text(d, 22, 608, "GET /api/diag  ->  HTTP 200", 8, "#9CCC9C")
    for i, ln in enumerate(['  "fw": "v7.3",  "appMode": 0,',
                            '  "prevAmtDay": "2026-11-06",  "prevAmtOk": 14,',
                            '  "amtNow": 6825.33,  "amtPrev": 7726.35,',
                            '  "amtDiff": -901.02,  "newsCount": 10,',
                            '  "micOn": 0,  "resetReason": 3']):
        text(d, 34, 624 + i * 13, ln, 8, "#9CCC9C")
    p = os.path.join(OUT, "ui_web.png")
    im.save(p, optimize=True)
    print("  ->", p)
    globals()["S"] = 3


def page_title(d, left, right):
    """设备端 drawTitle(): 24px 标题栏"""
    rect(d, 0, 0, W, 24, fill="#141A22")
    text(d, 6, 3, left, 13, FG, bold=True)
    text(d, 234, 6, right, 8, WARN if right.startswith("NEW") else ACC, anchor="ra")
    line(d, 0, 24, W, 24, LINE)


NEWS = [
    ("英国工党据悉预计将下调英国汽车行业的电动汽车销售目标", "02:00"),
    ("埃及列车脱轨事故已致7死112伤", "01:30"),
    ("2026中国企业全球化报告 在第九届进博会首发", "01:05"),
    ("央行：10月末外币贷款余额同比降幅继续收窄", "00:48"),
    ("国内首个低空经济产业基金落地成都", "00:21"),
]
NEWS_SUM = "【埃及列车脱轨事故已致7死112伤】当地时间11月8日，根据埃及卫生部的最新通报，当天发生在吉萨省阿亚特地区的列车脱轨事故…"


def draw_news():
    """v7.3 实时快讯: 整页只显示标题(约 10 行), 底部不再挤摘要预览"""
    im, scr, pad = new_screen()
    d = ImageDraw.Draw(scr)
    page_title(d, "实时快讯", "10 条")
    rows = ["埃及列车脱轨事故已致7死112伤",
            "国内首个低空经济产业基金落地成都",
            "央行开展 5000 亿元 MLF 操作",
            "10 月 CPI 同比上涨 0.3%",
            "新能源车 10 月零售同比增 28%",
            "两市成交额突破 2.5 万亿元",
            "美联储议息会议维持利率不变",
            "黄金现货站上 4100 美元",
            "工信部发布算力基础设施新规",
            "沪深 300 指数成分股调整名单"]
    for k in range(10):
        y = 26 + k * 20
        on = (k == 1)
        if on:
            rect(d, 0, y - 2, 240, 20, fill=SEL)
        c = "#0B0E14" if on else FG
        label = rows[k] if len(rows[k]) <= 18 else rows[k][:17] + "…"
        text(d, 4, y + 2, label, 10, c, bold=on)
    text(d, 4, 222, "2/10", 9, FLAT)
    text(d, 60, 222, "M看全文  双击刷新", 9, FLAT)
    finish(im, scr, pad, "ui_news.png")


def draw_radio():
    """v7.2 网络收音机全屏页"""
    im, scr, pad = new_screen()
    d = ImageDraw.Draw(scr)
    page_title(d, "网络收音机", "四连M进入")
    text(d, 8, 32, "1. 中央人民广播电台 中国之声", 11, ACC)
    text(d, 8, 58, "正在播放", 11, DOWN)
    text(d, 8, 80, "码率 64 kbps  播放 128s", 10, FLAT)
    text(d, 8, 102, "（电台未提供曲名）", 10, DIM)
    rect(d, 8, 128, 224, 12, fill="#212028")
    rect(d, 8, 128, int(224 * 65 / 100), 12, fill=ACC)
    text(d, 8, 146, "音量 65", 10, FG)
    text(d, 8, 174, "左右短按 切台", 10, FLAT)
    text(d, 8, 194, "M播放/暂停  双击停止", 10, FLAT)
    text(d, 8, 216, "双键返回  M长按回股票", 10, FLAT)
    finish(im, scr, pad, "ui_radio.png")


def draw_info():
    """v7.3 设备信息: 版本更新说明(1-100字) + 运行摘要"""
    im, scr, pad = new_screen()
    d = ImageDraw.Draw(scr)
    page_title(d, "设备信息", "任意键返回")
    text(d, 6, 26, "固件 v7.3  股票", 11, ACC)
    notes = ["新增沪金银主连/美股指数/外盘商品与网页",
             "一键添加(标普500用gb_inx);修中文开放",
             "WiFi;提醒间隔可选、关机15分档、底部快",
             "讯条、新闻自动切换、敲击代M键;修复单只",
             "大屏「放」:较前一交易日同时刻放量,亿元。"]
    for i, ln in enumerate(notes):
        text(d, 6, 44 + i * 15, ln, 9, FG)
    rows = ["网络 已连接", "IP 10.0.0.135  -46dBm", "音量60 亮度30 列表",
            "内存107K PSRAM8M 运行9m", "快讯10条 12s前 电台0", "提醒开 3.0% 间隔30分"]
    for i, r in enumerate(rows):
        text(d, 6, 124 + i * 16, r, 10, FLAT)
    text(d, 6, 220, "左右同按=中键  长按=关机", 9, FLAT)
    finish(im, scr, pad, "ui_info.png")


def draw_overview():
    """README 头图: 标题 + 三张界面缩略"""
    im = Image.new("RGB", (1400, 640), "#0E131A")
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, 1399, 639], radius=0, outline="#1E2630", width=2)
    Fbig = ImageFont.truetype(FONT_B, 52)
    Fsub = ImageFont.truetype(FONT_R, 22)
    Ftag = ImageFont.truetype(FONT_R, 18)
    d.text((56, 60), "正点原子 BOX0 股票固件", font=Fbig, fill="#FFFFFF")
    d.text((58, 130), "v7.3 · ESP32-S3 · 240×240 桌面行情终端 · 网页管理 · OTA 升级", font=Fsub, fill=ACC)
    feats = ["• 多品种：A股 / 指数 / 港股 / 美股 / 沪金银主连 / 伦敦金 / 布伦特油",
             "• 单只大屏「放」：较前一交易日同时刻成交额（亿元，+放量 −缩量）",
             "• 实时快讯整页标题 + 点开看全文，可选底部滚动快讯条",
             "• 网络收音机 · 提醒间隔可选 · 中文开放 WiFi · 敲击代 M",
             "• 8 组 WiFi 漫游 · 完整固件 0x0 一键刷入 · 网页 OTA · 无需服务器"]
    for i, f in enumerate(feats):
        d.text((58, 200 + i * 42), f, font=Ftag, fill="#B9C4CF")
    d.text((58, 470), "M 三连看快讯  ·  M 四连听广播  ·  网页「升级」一键 OTA", font=Fsub, fill=SEL)
    # 右侧 2x2 缩略图
    thumbs = [("ui_list.png", 830, 60, 250), ("ui_single.png", 1100, 60, 250),
              ("ui_news.png", 830, 340, 250), ("ui_radio.png", 1100, 340, 250)]
    for fn, x, y, size in thumbs:
        p = os.path.join(OUT, fn)
        if not os.path.exists(p):
            continue
        t = Image.open(p).convert("RGB")
        t.thumbnail((size, size), Image.LANCZOS)
        im.paste(t, (x, y))
    for x, y, lb in [(830, 320, "列表模式"), (1100, 320, "单只大屏"),
                     (830, 600, "实时快讯"), (1100, 600, "网络收音机")]:
        d.text((x, y), lb, font=Ftag, fill=FLAT)
    p = os.path.join(OUT, "overview.png")
    im.save(p, optimize=True)
    print("  ->", p)


if __name__ == "__main__":
    print("Rendering BOX0 UI screenshots ...")
    draw_list()
    draw_single()
    draw_menu()
    draw_news()
    draw_radio()
    draw_info()
    draw_statusbar_zoom()
    draw_web()
    draw_overview()
    print("Done.")
