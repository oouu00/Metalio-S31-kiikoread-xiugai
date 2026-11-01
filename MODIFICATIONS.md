# 修改说明 / Modifications

> 本仓库是 **kiikoread（Metalio S31）的第三方修改版**，并非 mindreset 的原版，也不是官方 Metalio 发布。
> This repository is a **third-party modified version of kiikoread (Metalio S31)** — neither mindreset's original nor an official Metalio release.

## 原始项目 / Upstream

- 原作者（版权人 / Original rights holder）：**mindreset**
- 原始仓库 / Original repository：[wegooo-cell/read-pico-reader](https://github.com/wegooo-cell/read-pico-reader)
- 本项目基线 / Based on：**v0.3.3-rc107**（Metalio S31）
- 官网与安装器 / Site & installer：<https://kiikoread.com/>

## 修改人 / Modified by

- **zhuyue**（2026）

## 许可范围 / License scope

- **既有代码与历史版本**：继续适用原有 **Apache-2.0** 或各自组件许可，原版权与许可文本保持不变（见 `LICENSE`、`licenses/`、`THIRD_PARTY_NOTICES.md`）。本修改版未移除任何原作者的版权声明或 SPDX 标识。
  *Existing code and historical versions* remain under their original **Apache-2.0** or respective component licenses; original copyright and license texts are preserved (`LICENSE`, `licenses/`, `THIRD_PARTY_NOTICES.md`). No upstream copyright or SPDX notice was removed.
- **本修改版新增的原创内容**（构建/线刷脚本等）：按原项目政策显式标注 `SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0`。仅允许个人学习、研究与测试，以及个人设备上的非商业阅读；商业使用需相关权利人事先书面许可。
  *New original material added in this fork* (build/flash scripts, etc.) is explicitly marked `SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0`, permitting personal study, research and testing, and personal noncommercial reading only; commercial use requires prior written permission from the relevant rights holder.
- **对既有文件的修改**：在对应源文件头以「用户修订 / User revision」行标注，遵循 Apache-2.0 第 4(b) 条"标注修改"的要求。
  *Modifications to existing files* are noted in each file header with "用户修订 / User revision" lines, per Apache-2.0 §4(b).

## 主要修改 / Key changes (rc107 → rc107.1)

- **书架 / Bookshelf**：修复"扫到多少存多少"——支持的书扫到即写入 SD 卡快照（`/sdcard/.readpico/shelf.bin`）；修正不支持文件引起的"部分目录或文件不可读"误报；扫描延后至 SD 就绪。
- **目录页 / TOC page**：去除页码 pill 灰底与整页灰背景（消除反复闪烁）；接入速读黑白模式，选中态不再显示灰选中，改用「当前：」文本标记当前章节。
- **字体 / Fonts**：自定义 TTF/OTF 切换时加固 PSRAM 连续块分配（缓存清理 + 同字体重试 + 日志），避免无连续块导致失败。
- **速读黑白 / Fast monochrome**：新增整页二值化 + DU 差分刷新路径，提升刷新速度。
- **蓝牙 / 音频 / Bluetooth & audio**：保留原作者蓝牙扫描路径（`bt_audio_scan_start` + `AT+BT:` 解析）；确认耳机 AVRCP 音量键由模组固件吞掉、不转发 host，改由机身侧键 / 屏幕键控制音量（设置项「播放音频时侧键仅调音量」）。
- **构建与工具 / Build & tooling（新增原创内容）**：`build_metalio.py`、各类双击编译 / 线刷脚本、一键线刷 `flash_partitions.py`（刷全部分区，偏移从 `partitions.csv` 动态读取）。

## 构建产物补迁 / Build artifacts restored

本源码树交付时缺失 `boards/metalio47/partitions.csv` 与部分 `managed_components` 的预编译 `lib/`（esp_audio_codec 2.6.2、esp_new_jpeg 1.0.2、esp-dl 3.3.14、esp-sr 2.6.0）。这些已按**版本逐一核对一致**地从原工程树补齐——仅补缺失文件，**未改动任何源码或头文件**。

## 免责声明 / Disclaimer

本修改版按"原样"提供，仅供个人学习与非商业用途，与官方 Metalio / kiikoread 无关；作者不对任何使用后果负责。

This modified version is provided "as is" for personal study and noncommercial use only. It is not affiliated with official Metalio or kiikoread; the authors accept no liability for any consequences of use.
