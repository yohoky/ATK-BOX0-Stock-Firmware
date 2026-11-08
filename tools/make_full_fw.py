#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从 Arduino CLI 的构建目录生成两个发布镜像：

  firmware/box0_stock_vX.Y.bin            应用镜像(app-only), 用于网页 OTA / 0x10000 烧录
  firmware/box0_stock_vX.Y_full_0x0.bin   完整镜像(merged, 从 0x0 开始), 用于首次 USB 刷机

完整镜像直接裁取 Arduino CLI 生成的 *.merged.bin 前 0x310000 字节：
  0x000000  bootloader  (e9)
  0x008000  partition table (aa50) —— 本项目 app0@0x10000 / app1@0x310000 / ffat
  0x00E000  otadata / boot_app0 —— 指向 app0
  0x010000  应用固件 (e9)
  0x310000  结束(app1 起点), 其余留给 OTA 与 FatFS, 保持 0xFF

为什么必须给完整镜像：正点原子出厂八合一固件 / 小智固件的分区表与本工程不同
(官方 ota_0@0x20000、assets@0x800000)。只刷 0x10000 的 app 镜像会因为分区表
不匹配而反复重启。完整镜像自带 bootloader 与分区表，从任意固件刷过来都能跑。

用法:
  python tools/make_full_fw.py build_v71 v7.1
"""
import hashlib
import os
import shutil
import sys

APP_OFF = 0x10000
APP1_OFF = 0x310000          # 与 partitions.csv 中 app1 的 Offset 一致
PT_OFF = 0x8000
OTA_OFF = 0xE000

# 发布前必须为零：任何真实 WiFi 凭据都不得进入公开仓库
FORBIDDEN = (b"Cisco", b"18583606600", b"ghp_", b"github_pat_")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    build = sys.argv[1]
    ver = sys.argv[2]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    merged = os.path.join(root, build, "box0_radio_stock.ino.merged.bin")
    app = os.path.join(root, build, "box0_radio_stock.ino.bin")
    outdir = os.path.join(root, "firmware")
    for f in (merged, app):
        if not os.path.isfile(f):
            print("[ERR] 缺少构建产物:", f)
            return 1
    os.makedirs(outdir, exist_ok=True)

    blob = open(merged, "rb").read()
    appbin = open(app, "rb").read()

    # 头部自检
    if blob[0] != 0xE9:
        print("[ERR] 0x0 不是 ESP 引导镜像")
        return 1
    if blob[PT_OFF] != 0xAA or blob[PT_OFF + 1] != 0x50:
        print("[ERR] 0x8000 不是分区表")
        return 1
    if blob[APP_OFF] != 0xE9:
        print("[ERR] 0x10000 不是应用镜像")
        return 1
    if blob[APP_OFF:APP_OFF + len(appbin)] != appbin:
        print("[ERR] merged 中的应用与 app-only 镜像不一致")
        return 1

    full = blob[:APP1_OFF]
    if len(full) != APP1_OFF:
        print("[ERR] merged 镜像不足", APP1_OFF)
        return 1

    for bad in FORBIDDEN:
        if bad in full or bad in appbin:
            print("[ERR] 镜像含禁止发布的字符串:", bad.decode(errors="replace"))
            return 1

    app_out = os.path.join(outdir, "box0_stock_%s.bin" % ver)
    full_out = os.path.join(outdir, "box0_stock_%s_full_0x0.bin" % ver)
    shutil.copyfile(app, app_out)
    open(full_out, "wb").write(full)

    for p in (app_out, full_out):
        d = open(p, "rb").read()
        print("%-58s %9d B  sha256=%s" %
              (os.path.basename(p), len(d), sha256(d)))
    print("烧录地址: app-only -> 0x10000 | full -> 0x0 (先 erase_flash)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
