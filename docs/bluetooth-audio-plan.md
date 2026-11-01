# 蓝牙耳机音频实现计划 / Bluetooth Audio Implementation Plan

> 2026-10-28 结论修订：片内经典蓝牙 A2DP 实测失败（`BTA_AV_OPEN_EVT` status 3、GETCAP 后 71 ms
> 断开、内部堆仅剩约 6 KB），已改为板载蓝牙音频模组路线：UART AT 选模式/配对，I2S 从机送
> 16 kHz PCM，片内控制器只保留 BLE 翻页器。下述 A2DP 方案仅作历史记录。
> 2026-10-28 outcome: the on-chip classic A2DP path failed on hardware (status 3, a 71 ms
> disconnect after GETCAP, ~6 KB of internal heap left), so audio now runs on the onboard
> Bluetooth audio module (AT over UART, I2S slave 16 kHz PCM) and the on-chip controller keeps
> BLE only. The A2DP plan below is kept as history.

本文只记录建议的实施路径与验收口径，不替代代码内的注释与冻结清单。

## 一、结论摘要 / Summary

用 `esp_bt_audio`（A2DP Source，把音频发到耳机）+ `esp_player`（解封装与解码）实现，
两者同属 ESP-GMF 生态，官方已给出 A2DP Source 的 pipeline 组合，**不需要改动板级硬件**
（蓝牙耳机自带 DAC，音频走无线）。

两个前提已经确认：

- S31 具备经典蓝牙硬件：`SOC_BT_CLASSIC_SUPPORTED=1`（注意与 `esp32s3` 区分，S3 没有）。
- `esp_player` 在 ESP32-S31 上有官方实测：MP3 解码 Internal 7.2 KB / PSRAM 91.8 KB / CPU 4%，
  解码本身压力很小。

真正的代价不在解码，而在**协议栈**：经典蓝牙（A2DP）只能用 Bluedroid，而 Bluedroid 会把
蓝牙 host 的内存压到内部 RAM。

## 二、当前状态基线 / Current baseline

| 项目 | 现状 |
|---|---|
| 蓝牙栈 | NimBLE，`CONFIG_BT_BLUEDROID_ENABLED=n` |
| 角色 | `ROLE_CENTRAL` + `ROLE_OBSERVER`，`MAX_CONNECTIONS=1` |
| host 内存 | `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y`（走 PSRAM，内部 RAM 留给面板 DMA） |
| 既有蓝牙功能 | `components/ble_page_turner`：BLE HID 主机，连蓝牙翻页器/键盘 |
| 代码引用面 | 约 100 处，集中在 `app_device_settings.c`(62)、`app_loop.c`(16)、`app_book.c`(15) |
| 静态内部 DRAM | .bss 73736 + .data 41900，约占总量的 23% |
| 翻页耗时基线 | 日间 `display` 中位 834936 us，夜间 878962 us（见 `turn_trace.csv`） |

## 三、最大风险 / Top risk

**Bluedroid 没有 NimBLE 的 PSRAM 分配开关。** 切换后，蓝牙 host 的分配从 PSRAM 回落到内部 RAM，
而 sdkconfig 明确写着内部 RAM 要留给面板 DMA 与显示：

```
# host 的全部动态分配走 PSRAM，内部 RAM 留给面板 DMA 与显示。
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y
```

可能的后果是**正在优化的翻页耗时被拖慢**（当前 837ms，目标 300ms）。
因此本计划把"翻页是否退化"放在第一阶段验收，而不是等功能做完再测。

## 四、分阶段实施 / Phases

### 阶段 0：建立基线（不动功能）

1. 编译当前固件，记录：`idf.py size` 的内部 DRAM 占用、运行时剩余内部 heap。
2. 用 `turn_trace` 采集一次干净的翻页数据（日间，连续 30 页），存档 CSV。
3. 记录 PSRAM 剩余（已稳定在约 9787 KB）。

验收：三个数字入档，后续每阶段都能比对。

### 阶段 1：协议栈切换验证（最高风险，先做）

任务：

1. `boards/metalio47/sdkconfig.defaults` 改为：
   - `CONFIG_BT_NIMBLE_ENABLED=n`
   - `CONFIG_BT_BLUEDROID_ENABLED=y`
   - `CONFIG_BT_CLASSIC_ENABLED=y`
   - `CONFIG_BT_A2DP_ENABLE=y`
   - `CONFIG_BT_AVRCP_ENABLED=y`
   - 视需要 `CONFIG_BT_A2DP_USE_EXTERNAL_CODEC`（编解码由应用层提供时打开）
2. `ble_page_turner` **先禁用而非删除**：从 `main/CMakeLists.txt` 源表移除，
   UI 入口暂时隐藏（保留代码便于回滚）。用户已确认该功能可放弃。
3. 编译并**实测**三项：
   - 编译是否通过（Bluedroid 与既有代码的符号冲突）
   - 内部 RAM 变化（重点：剩余内部 heap 是否仍够面板 DMA）
   - 翻页耗时是否退化（重跑阶段 0 的翻页采集，比对 `display_us`）
4. 用 `esp_bt_audio` 的 discovery/connect 辅助接口扫描并配对一只普通蓝牙耳机，
   只要 A2DP 连接建立即可，**此阶段不要求出声**。

验收：A2DP 连接成功，且翻页 `display_us` 退化不超过 5%。

回滚点：若内部 RAM 不足或翻页退化超过 5%，停止推进——此时仍可退回 NimBLE，
最多损失一次编译往返。

### 阶段 2：打通音频链路

任务：

1. `idf.py add-dependency "espressif/esp_player^1.1.1"` 与 `"espressif/esp_bt_audio^1.2.0"`。
2. 按官方组合搭 pipeline：

   ```
   io_file ──► aud_dec ──► aud_rate_cvt ──► aud_ch_cvt ──► aud_enc ──► io_bt ──► 耳机
    (SD卡)     (解mp3)       (重采样)          (声道)        (编SBC)
   ```

3. 在 `esp_bt_audio` 事件回调里按 stream 状态（ALLOCATED/STARTED/STOPPED/RELEASED）
   绑定 `io_bt` 与 pipeline。
4. 先固定播放 SD 卡上一个指定 mp3 做验证，不做 UI。

验收：耳机出声，连续播放 5 分钟无断流；同时翻页仍可用，`turn_trace` 无异常。

### 阶段 3：产品化

1. 设备设置里新增"蓝牙音频"入口：扫描、连接、断开（复用 `esp_bt_audio_classic.h` 的
   discovery/connect 辅助接口）。
2. 播放控制：`esp_bt_audio_playback.h` / AVRCP 映射播放、暂停、上下曲。
3. 决定 `ble_page_turner` 的去留：移植到 Bluedroid 的 BLE GATT API，或彻底移除并清理
   约 100 处引用与相关 UI。
4. 功耗与发热评估。

### 阶段 4：回归

1. 复测翻页耗时，与阶段 0 基线比对。
2. 长时间播放 + 翻页混合压力测试，观察内部 heap 是否单调下降。
3. 确认 `turn_trace` 仍能正常落盘（不要被音频缓冲挤掉 SD 卡带宽）。

## 五、待实测的未知数 / Open questions

1. Bluedroid 在 S31 上的实际内部 RAM 开销（无官方数据，必须实测）。
2. 音频缓冲与面板 DMA 是否争抢 PSRAM 带宽——阶段 2 需同时跑翻页与播放来暴露。
3. `esp_bt_audio` 要求 IDF ≥ 5.5，本项目用 6.1，需确认组件在该版本下无告警。
4. 若将来要保留 BLE 翻页器，需评估 Bluedroid 同时跑 BLE GATT 与 BR/EDR 的开销；
   另一条路是 LE Audio（NimBLE + `BT_AUDIO` + `BT_ISO`），不动现有栈，
   但要求耳机支持 LC3/BAP，兼容性风险大，且 S31 controller 是否支持 ISO 需先验证。

## 六、为什么不建议一次性做完 / Why staged

阶段 1 的失败成本最低而信息量最大：它同时回答"能不能连上"和"翻页会不会被拖慢"，
而后者是这个项目当前的主线（837ms → 300ms）。若把播放器、蓝牙、UI 一次性塞进去，
一旦出现性能退化，将无法定位是协议栈、解码还是 UI 造成的。
