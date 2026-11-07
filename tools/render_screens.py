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


def statusbar(d):
    """v7.0 状态栏 6 项: 星期 / 时间 / 页码 / 音量 / 电量 / WiFi(模式)"""
    rect(d, 0, 0, W, 19, fill="#05070A")
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
    # WiFi 小信号 + 模式字
    for i in range(3):
        h = 3 + i * 4
        rect(d, 206 + i * 4, 16 - h, 3, h, fill=ACC if i < 3 else LINE)
    text(d, 220, 2, "连", 10, ACC)
    line(d, 0, 19, W, 19, LINE)


def draw_list():
    im, scr, pad = new_screen()
    d = ImageDraw.Draw(scr)
    statusbar(d)
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
    im, scr, pad = new_screen()
    d = ImageDraw.Draw(scr)
    statusbar(d)
    text(d, 12, 28, "上证指数", 18, FG, bold=True)
    text(d, 12, 52, "sh000001", 9, FLAT)
    text(d, 12, 76, "3686.17", 30, UP, bold=True)
    text(d, 12, 118, "+77.89   +2.16%", 13, UP, bold=True)
    line(d, 10, 145, 230, 145, LINE)
    for i, (k, v) in enumerate([("开", "3621.40"), ("高", "3692.05"),
                                ("低", "3618.72"), ("额", "5821亿")]):
        x = 12 + (i % 2) * 112
        y = 154 + (i // 2) * 32
        text(d, x, y, k, 10, FLAT)
        text(d, x + 22, y - 2, v, 11, FG, bold=True)
    text(d, 12, 218, "M 短按切回列表   左右键换股", 8, FLAT)
    finish(im, scr, pad, "ui_single.png")


MENU = ["显示模式", "轮播间隔", "刷新间隔", "涨跌提醒", "自动关机", "亮度", "音量"]


def draw_menu():
    im, scr, pad = new_screen()
    d = ImageDraw.Draw(scr)
    rect(d, 0, 0, W, 24, fill="#141A22")
    text(d, 8, 4, "菜单", 13, FG, bold=True)
    text(d, 232, 6, "M长按退出", 8, FLAT, anchor="ra")
    vals = ["列表 6只/屏", "5 秒", "6 秒", "开 4.0%", "息屏后30分", "30%", "60"]
    for i, nm in enumerate(MENU):
        y = 28 + i * 27
        if i == 0:
            rect(d, 4, y - 3, 232, 25, fill=SEL, radius=3)
        c = "#0B0E14" if i == 0 else FG
        text(d, 10, y, nm, 12, c, bold=True)
        text(d, 230, y + 1, vals[i], 9, c if i == 0 else ACC, anchor="ra")
    rect(d, 237, 26, 2, 175, fill=DIM)
    rect(d, 237, 26, 2, 75, fill=ACC)
    text(d, 6, 220, "左右选 M确定 双键返回", 8, FLAT)
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
              ("④ 音量", 505), ("⑤ 电量 60-100 绿", 665), ("⑥ WiFi 信号/模式", 855)]
    for (lb, px), c in zip(labels, col):
        d.text((px, 19 * scale + 24), lb, font=ImageFont.truetype(FONT_B, 17), fill=c)
    d.text((10, 19 * scale + 58),
           "六项固定显示：星期 · 时间 · 第几页 · 音量(喇叭图标+数字) · 电量(电池内数字) · WiFi 信号(连/中/热)",
           font=ImageFont.truetype(FONT_R, 15), fill=FLAT)
    p = os.path.join(OUT, "ui_statusbar.png")
    im.save(p, optimize=True)
    print("  ->", p)
    S = old


def draw_web():
    """网页管理界面示意(升级 tab)"""
    S = 2
    globals()["S"] = S
    w, h = 520 * S, 660 * S
    im = Image.new("RGB", (w, h), "#0E0E0E")
    d = ImageDraw.Draw(im)
    rect(d, 10, 10, 500, 44, fill="#1B1B1B", outline="#2A2A2A", radius=10)
    text(d, 22, 20, "BOX0 股票固件 · 管理页", 13, FG, bold=True)
    text(d, 498, 22, "v7.0", 9, FLAT, anchor="ra")
    tabs = ["股票", "设置", "设备", "升级"]
    for i, t in enumerate(tabs):
        x = 10 + i * 126
        on = i == 3
        rect(d, x, 62, 120, 30, fill=SEL if on else "#1B1B1B", radius=8)
        text(d, x + 60, 69, t, 11, "#FFFFFF" if on else "#999999", bold=on, anchor="ma")
    rect(d, 10, 102, 500, 74, fill="#1B1B1B", radius=10)
    text(d, 22, 112, "固件升级（网页 OTA）", 12, FG, bold=True)
    text(d, 22, 134, "当前固件 v7.0  ·  选择 .bin 上传，约 8 秒自动重启", 9, FLAT)
    text(d, 22, 152, "不用插 USB / 不用下载模式 / 不用按 RESET", 9, ACC)
    rect(d, 10, 186, 500, 34, fill="#1A1A1A", outline="#333333", radius=8)
    text(d, 22, 194, "选择文件   box0_stock_v7.0.bin", 9, FLAT)
    rect(d, 10, 228, 500, 34, fill=SEL, radius=8)
    text(d, 260, 236, "上传并升级", 11, "#FFFFFF", bold=True, anchor="ma")
    rect(d, 10, 276, 500, 40, fill="#1B1B1B", radius=10)
    text(d, 22, 286, "全部接口（点击即调用）", 11, FG, bold=True)
    text(d, 22, 303, "/api/cfg   /api/diag   /api/scan   /api/beep   ...", 8, ACC)
    text(d, 10, 326, "已保存的 WiFi（最多 8 组 · 自动漫游 · 支持无密码）", 11, FG, bold=True)
    for i, (nm, mark) in enumerate([("Home-2.4G", "★ 当前"), ("Office-Guest", "开放"), ("Coffee-5G", "")]):
        y = 348 + i * 34
        rect(d, 10, y, 500, 30, fill="#1A1A1A", outline="#2D2D2D", radius=8)
        label = ("★ " if mark.startswith("★") else "") + nm + ("  (开放)" if mark == "开放" else "")
        text(d, 22, y + 7, label, 9, FG)
        text(d, 498, y + 7, "连接    删除", 8, FLAT, anchor="ra")
    rect(d, 10, 460, 500, 120, fill="#111111", outline="#333333", radius=8)
    text(d, 22, 470, "GET /api/diag  ->  HTTP 200", 8, "#9CCC9C")
    text(d, 22, 488, "{", 8, "#9CCC9C")
    for i, ln in enumerate(['  "fw": "v7.0",', '  "volume": 60,',
                            '  "wifiProfileN": 3,', '  "batPct": 100']):
        text(d, 34, 502 + i * 15, ln, 8, "#9CCC9C")
    p = os.path.join(OUT, "ui_web.png")
    im.save(p, optimize=True)
    print("  ->", p)
    globals()["S"] = 3


def draw_overview():
    """README 头图: 标题 + 三张界面缩略"""
    im = Image.new("RGB", (1400, 640), "#0E131A")
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, 1399, 639], radius=0, outline="#1E2630", width=2)
    Fbig = ImageFont.truetype(FONT_B, 52)
    Fsub = ImageFont.truetype(FONT_R, 22)
    Ftag = ImageFont.truetype(FONT_R, 18)
    d.text((56, 60), "正点原子 BOX0 股票固件", font=Fbig, fill="#FFFFFF")
    d.text((58, 130), "ESP32-S3 · 240×240 桌面行情终端 · 网页管理 · OTA 升级", font=Fsub, fill=ACC)
    feats = ["• 13+ 自选股，红涨绿跌，单只 / 列表一键切换",
             "• 8 组历史 WiFi 自动漫游，支持无密码开放网络",
             "• 状态栏 6 项：星期 / 时间 / 页码 / 音量 / 电量 / WiFi",
             "• 涨跌提醒弹屏 + 温和提示音，自动熄屏与关机",
             "• 全部设置可在本机菜单或网页完成，网页一键 OTA"]
    for i, f in enumerate(feats):
        d.text((58, 200 + i * 42), f, font=Ftag, fill="#B9C4CF")
    d.text((58, 470), "M 短按切显示  ·  M 长按进菜单  ·  网页「升级」一键 OTA", font=Fsub, fill=SEL)
    # 右侧三张缩略图
    thumbs = [("ui_list.png", 830, 60, 250), ("ui_single.png", 1100, 60, 250),
              ("ui_menu.png", 965, 340, 250)]
    for fn, x, y, size in thumbs:
        p = os.path.join(OUT, fn)
        if not os.path.exists(p):
            continue
        t = Image.open(p).convert("RGB")
        t.thumbnail((size, size), Image.LANCZOS)
        im.paste(t, (x, y))
    d.text((830, 320), "列表模式", font=Ftag, fill=FLAT)
    d.text((1100, 320), "单只大屏", font=Ftag, fill=FLAT)
    d.text((965, 600), "设置菜单", font=Ftag, fill=FLAT)
    p = os.path.join(OUT, "overview.png")
    im.save(p, optimize=True)
    print("  ->", p)


if __name__ == "__main__":
    print("Rendering BOX0 UI screenshots ...")
    draw_list()
    draw_single()
    draw_menu()
    draw_statusbar_zoom()
    draw_web()
    draw_overview()
    print("Done.")
