# Chameleon Ultra — Max 固件 / Max Firmware

基于 **RfidResearchGroup/ChameleonUltra 官方最新正式版 v2.3.0** 的增强固件，
参考 Neo-403 改版固件的功能思路，用完全开源的方式重新实现，并新增离线整卡克隆。

Based on the official **RfidResearchGroup/ChameleonUltra v2.3.0** release, with the
Neo-403 mod features re-implemented in open source plus a new offline full-card
clone feature.

> ⚠️ 本固件为独立修改版，与官方及 Neo-403 均无隶属关系。刷机前请阅读下方的「能力边界」。
> This is an independent fork. Not affiliated with RfidResearchGroup or Neo-403.

---

## 新增功能 / New features

### 1. 长按 A 键离线整卡克隆 / Offline full-card clone (assignable to any button)

* 不连接电脑/手机：把真实 Mifare Classic 卡（Mini / 1K / 2K / 4K）**整张**读入当前卡槽
* 密钥恢复：固件内置 78 个常用密钥字典（含传输默认密钥、NXP/MAD 密钥、常见系统密钥、UID 派生密钥），
  用固件自带的 `mf1_toolbox_check_keys_of_sectors` 扫全部扇区；成功后还会自动从 trailer 收获可读的 key B
* 卡槽类型自动匹配：读 4K 卡时卡槽自动切成 4K（失败自动还原）
* 成功后立即仿真 + 写入 flash，拔掉原卡后设备就是原卡的完整副本
* 全程纯读卡，不向真实卡写任何数据

**LED 指示 / LED patterns**（当前卡槽 LED，闪烁 3 轮）：

| 模式 | 含义 |
|---|---|
| 蓝色常亮 | 正在克隆 |
| 绿色 2 闪 | 成功 |
| 红色 1 闪 | 没找到卡 |
| 红色 2 闪 | 不是 Mifare Classic |
| 红色 3 闪 | 读块通信失败（卡槽已还原） |
| 红色 4~8 闪 | 有 N 个扇区密钥不在字典里（卡槽已还原） |

在官方上位机的按键设置里选择 `Full clone card`（整卡克隆）即可分配给
短按/长按 A 或 B。默认不占用长按 A（官方默认长按 A 是克隆 UID，可自行改）。

按键现在有三档：短按、长按（默认 >1s）、超长按（默认 >3s）。超长按可以单独分配一个
功能（`1049`/`1050` 读写 A/B 超长按动作，`1051`/`1052` 设置超长阈值）；松手时只触发
对应的那一档，不会同时触发长按和超长按。短按/长按都设为“无动作”时，超长按仍可工作。

按住 A 或 B 时，LED 会从 1 秒开始每秒多亮一个（最多 8 个），松手后恢复正常卡槽指示；
可以用它判断当前按到了长按还是超长按档位。

### 2. 卡槽自动轮询 / Slot auto-polling

**场驱动**：只有读卡器在场（`g_is_tag_emulating`，HF 或 LF）时才轮询；没有场就停表，
不会在桌面上自己切槽。场存在时：

* 读卡器 HALT/REQA/SELECT 事件会触发切槽（智能模式按 UID/MRU 优先）；
* 若场存在但一段时间没有读卡器帧，用配置的间隔做 sweep 兜底；
* 切槽走 `tag_emulation_change_slot_live()`，不停止/重启 NFC sense，场会话不中断。

设置项：

* 开关（默认关）
* 间隔：10ms ~ 1s（默认 500ms），到场会话超时兜底
* 轮询卡槽掩码：最多 255 个卡槽任意组合
* 智能轮询（UID/MRU 优先，默认开）；IC/HF、ID/LF 可分别开关
* 可分配按键功能 `Toggle slot polling`（一键开关轮询）

### 3. 卡组 / Card groups

* 卡组大小可设为 2 或 4（0 = 关闭，默认）
* 普通切换卡槽（A/B 键、GUI）只在当前卡组内循环
* 新按键功能 `Switch card group`（切换卡组）跳到下一组

### 4. 软重启 / Soft reboot

保存卡槽数据 + 设置后立即重启。新按键功能 `Soft reboot`，
以及新命令 `DATA_CMD_RESET_DEVICE (1046)`。

---

## 上位机 / Companion app

配套修改版 GUI（Flutter，Android APK + Windows）：见 GUI 仓库同名文档。
* 设置页四个按键配置改为下拉框，含全部 10 种功能
* 新增「卡槽自动轮询」设置区（开关/间隔/卡槽/卡组）
* 新增「离线整卡克隆」按钮：触发后轮询进度，结果以通知显示（也支持不看手机直接看 LED）
* 新增「软重启」按钮
* 全部新功能通过 `GET_DEVICE_CAPABILITIES` 能力协商，连旧固件不会显示新选项，官方固件兼容

---

## 协议新增（供开发者）/ New protocol commands

| 命令 | 值 | 说明 |
|---|---|---|
| `GET_SLOT_POLLING_CONFIG` | 1044 | 读 `slot_count` 字节，每槽 1 字节（0/1），下标即槽位 0..slot_count-1 |
| `SET_SLOT_POLLING_CONFIG` | 1045 | 写 `slot_count` 字节每槽列表（1..255 字节；兼容旧 4 字节 `{enable, interval, mask, group}`；记得再发 1013 保存） |
| `SLOT_ADD` | 1047 | 追加一个卡槽；可带 2 字节大端卡类型（0 或空表示稍后设置）。先检查 FDS 空间，失败返回 0x70；成功返回 1 字节新槽数 |
| `SLOT_REMOVE` | 1048 | 删除最后一个卡槽（含卡片数据和昵称），最少保留 8 个；成功返回 1 字节新槽数 |
| `GET_DEVICE_AUTO_POLLING` | 6038 | 读 1 字节：轮询总开关（与 1044/1045 同一份设置，兼容小程序） |
| `SET_DEVICE_AUTO_POLLING` | 6039 | 写 1 字节开关 |
| `GET_EXTRA_LONG_BUTTON_PRESS_CONFIG` | 1049 | 读超长按动作：1 字节按键（A/B）→ 1 字节动作 |
| `SET_EXTRA_LONG_BUTTON_PRESS_CONFIG` | 1050 | 写超长按动作：按键 + 动作 |
| `GET_EXTRA_LONG_PRESS_THRESHOLD` | 1051 | 读超长按阈值（2 字节大端毫秒） |
| `SET_EXTRA_LONG_PRESS_THRESHOLD` | 1052 | 写超长按阈值（2 字节大端毫秒） |
| `GET_DEVICE_AUTO_POLLING_DELAY` | 6040 | 读 2 字节大端：轮询间隔（毫秒） |
| `SET_DEVICE_AUTO_POLLING_DELAY` | 6041 | 写 2 字节大端毫秒（内部按 10ms 量化，10..1000ms） |
| `GET_IC_POLLING` / `SET_IC_POLLING` | 6046 / 6047 | HF/IC 是否参与轮询 |
| `GET_ID_POLLING` / `SET_ID_POLLING` | 6048 / 6049 | LF/ID 是否参与轮询 |
| `GET_SMART_POLLING` / `SET_SMART_POLLING` | 6062 / 6063 | UID/MRU 智能轮询开关 |
| `RESET_DEVICE` | 1046 | 保存并软重启 |
| `MF1_OFFLINE_CLONE` | 2023 | 触发离线整卡克隆（立即返回，作业在主循环同步执行，期间设备不响应其他命令） |
| `MF1_OFFLINE_CLONE_STATUS` | 2024 | 轮询状态，返回 `{state, missing_sectors}`；state: 0=idle 1=running 2=ok 3=no card 4=not MF1 5=keys missing 6=read fail 7=apply fail。作业执行期间响应会被延迟到作业结束，上位机需容忍超时并重试 |

`GET_DEVICE_SETTINGS (1034)` 现返回 18 字节：官方 13 字节布局不变，`[13]` = sleep timeout（与 GUI 既有约定一致），
`[14..17]` = 轮询配置（`[16]` 仅为 256 位 bitmap 第 0 字节，完整 bitmap 用 1044 读）。设置版本号 v8/v9/v10/v11/v12 → v13，旧数据自动迁移；v13 在结构末尾新增 A/B 超长按动作（各 4 bit）和超长按阈值（默认 3000ms）。

> 本分支是 **自适应槽位 + 场驱动智能轮询** 版本：对外槽数 `slot_count` 默认 **8**，可逐槽加到 **255**；
> 真正的上限是 FDS 空间——M1 1K 约 180 个、M1 4K 约 50 个、ID 卡受 255 槽限制。
> `GET_SLOT_INFO (1019)` 返回 `slot_count * 4` 字节，`GET_ENABLED_SLOTS (1023)` 返回 `slot_count * 2` 字节。
> `GET_ALL_SLOT_NICKS (1038)` 支持分页：请求 2 字节 `{start, count}`，每页最多 32 槽；不带参数时返回前 32 槽。
> 槽位配置结构版本 v8/v9 → v10（68/260 → 2044 字节），升级保留原有槽位：v9 固定 32 槽版本迁移为 32 槽，旧 8 槽版本迁移为 8 槽。
> FDS 保留页 28（224KB）。注意：1044 的返回格式已改为每槽 1 字节，旧版 Flutter GUI 的轮询页需要同步修改。

---

## 能力边界（务必阅读）/ Capability boundaries (read this!)

**离线（无电脑/手机）能做的：**
* 任意卡的 UID 克隆（官方原有功能）
* **字典密钥可恢复的卡**的整卡克隆 —— 现实中绝大多数门禁/电梯/水卡使用默认或常见密钥，覆盖面很大
* key A 已知时从 trailer 顺带获得 key B，并自动把已恢复密钥传播到同卡其他扇区

**离线做不到的（这是物理/内存限制，不是没写）：**
* darkside / nested / hardnested 等 crypto1 密钥**破解**需要 8MB 级内存做 lfsr_recovery32
  运算，nRF52840 只有 256KB RAM，设备端无法完成（官方固件同样只做采集、破解在电脑端完成）。
  完全自定义密钥 + 硬 PRNG 的卡请用配套 GUI 连机攻击（darkside/nested/hardnested 均已支持），
  拿到完整 dump 后再导入卡槽。

固件不会装作破解成功：字典未命中的扇区会导致克隆中止并还原卡槽，LED 报数。

---

## 编译 / Build

1. 把本仓库推到你的 GitHub（如 `yourname/ChameleonUltra`）
2. Actions 自动构建：push 到 `main` → `dev` 预发布（`ultra-dfu-app.zip` / `ultra-dfu-full.zip` / `.hex`）；
   打 `v*` tag → 正式 release
3. 建议先打个 tag 让版本号正确（不打 tag 也能编译，版本号回落为 2.3.0）：
   ```bash
   git tag v2.3.0-max.1 && git push origin v2.3.0-max.1
   ```

## 烧录 / Flash (DFU)

* **只更新应用（推荐，安全可回滚）**：解压 release 里的 `ultra-dfu-app.zip`
  * 安卓：官方 ChameleonUltra APP → Firmware management → Flash .zip FW via DFU
  * 电脑：https://hackady.github.io/chameleon-ultra-dfu-web/ 或 nRF Connect → DFU
* **全镜像（含 bootloader，慎用）**：`ultra-dfu-full.zip`，仅在 bootloader 也需要更新时使用
* DFU 步骤：设备关机 → 按住 B 键插 USB（或 APP 里 Enter DFU）→ 出现 DFU 盘符/设备 → 刷入 zip

## 与官方仓库的差异 / Diff summary vs official v2.3.0

| 文件 | 改动 |
|---|---|
| `firmware/application/src/settings.h/.c` | 设置 v13：轮询/卡组/智能轮询字段 + 10ms 间隔单位 + A/B 超长按动作与阈值 + 迁移 |
| `firmware/application/src/data_cmd.h` | 5 个新命令号 |
| `firmware/application/src/app_cmd.c` | 新命令处理器 + settings blob 扩展 |
| `firmware/application/src/app_main.c` | 新按键功能、轮询状态机、卡组切换、克隆入口与 LED |
| `firmware/application/src/rfid/reader/hf/mf1_offline_clone.c/.h` | 全新：离线整卡克隆模块 + 内置字典 |
| `firmware/application/Makefile` | 新增源文件 |
| `firmware/Makefile.defs` | 无 tag 仓库的版本号兜底 |
