# 编译与刷机

本文档记录本仓库（ChameleonUltra-Max / Max-Bravo 固件）在本机上的编译、打包、刷机与验证方法。

## 环境

| 项目 | 路径 / 版本 |
|---|---|
| 源码仓库 | `/Users/lynn/ChameleonUltra-Max` |
| 应用源码 | `firmware/application/src` |
| Arm GNU Toolchain | 15.3.1，`/Users/lynn/armgnu/expanded/Payload` |
| Python 虚拟环境 | `/Users/lynn/cu-venv`（已装 pyserial） |
| nrfutil | 系统 PATH 中，需支持 `nrfutil device program` |
| DFU 私钥 | `resource/dfu_key/chameleon.pem` |
| USB 串口 | macOS 下形如 `/dev/cu.usbmodem*`（脚本会自动查找） |

工具链不在仓库内。如果换机器，需要重新安装 Arm GNU Toolchain 15.3.1，或修改命令中的 `GNU_INSTALL_ROOT`。

## 一条命令编译 + 打包 + 刷机

```bash
/Users/lynn/ChameleonUltra-Max/tools/build_flash.sh
```

脚本会：

1. 在 `firmware/application` 下执行 `make`；
2. 把 `firmware/objects/application.bin` 打成 DFU 包 `build/v2.3.0-bravo.zip`；
3. 通过 USB 发送 `1010` 让设备进入 DFU；
4. 用 `nrfutil device program` 刷入并等待设备重启。

设备刷完后应自动重启，`nrfutil device list` 能看到 `fw_v515`。

## 手动编译

```bash
cd /Users/lynn/ChameleonUltra-Max/firmware/application
make clean
make -j8 GNU_INSTALL_ROOT=/Users/lynn/armgnu/expanded/Payload/bin/ GNU_VERSION=15.3.1
```

产物：

- `firmware/objects/application.hex`
- `firmware/objects/application.bin`

当前代码的正常大小约为：

```text
text 330844, data 2768, bss 123996   # 含 FindMy beacon
```

如果大小突然变化很多，先检查是否有多余改动或编译选项变化。

## 手动打包 DFU

```bash
cd /Users/lynn/ChameleonUltra-Max
mkdir -p build
nrfutil pkg generate \
  --hw-version 0 \
  --sd-req 0x100 \
  --application firmware/objects/application.bin \
  --application-version 1 \
  --key-file resource/dfu_key/chameleon.pem \
  build/v2.3.0-bravo.zip
```

可用 `nrfutil pkg display build/v2.3.0-bravo.zip` 检查包内容，正常应看到：

```text
Type: application
hw_version 0x00000000 (0)
sd_req: 0x100
```

## 进入 DFU

方式一（脚本，推荐）：

```bash
/Users/lynn/cu-venv/bin/python /Users/lynn/ChameleonUltra-Max/tools/enter_dfu.py
```

脚本会向当前固件发送命令 `1010`（ENTER_BOOTLOADER）。

方式二（手动）：

1. 拔掉设备 USB；
2. 按住 B 键再插入 USB；
3. 保持按住，直到 LED 4、5 闪烁后松开。

进入 DFU 后：

```bash
nrfutil device list
```

应看到 `Traits ... nordicDfu`，且 `fw_v256`。

## 刷机

```bash
nrfutil device program \
  --firmware /Users/lynn/ChameleonUltra-Max/build/v2.3.0-bravo.zip \
  --traits nordicDfu
```

刷完后等待设备重启（约 5 秒），再确认：

```bash
nrfutil device list
```

正常应显示 `ChameleonUltra: hw_v1, fw_v515`。

## 刷后验证

仓库自带一个简单的 USB 协议探测脚本：

```bash
/Users/lynn/cu-venv/bin/python /Users/lynn/ChameleonUltra-Max/tools/probe.py 1017
/Users/lynn/cu-venv/bin/python /Users/lynn/ChameleonUltra-Max/tools/probe.py 1034
```

- `1017` 应返回 `Max-Bravo`
- `1034` 第一个字节应为 `0x0D`（设置版本 13）
- `1055` 返回 FindMy beacon 状态（3 字节）
- `1053` 返回 FindMy beacon 配置

`probe.py` 用法：

```bash
python tools/probe.py <命令号> [十六进制数据]
# 例如读取 A 键超长按配置：
python tools/probe.py 1049 41
```

## 注意事项

- DFU 只更新 application，不会清空 FDS；卡槽、设置、BLE 绑定会保留。
- 如果设备 bootloader 的公钥不是 `resource/dfu_key/chameleon.pem` 对应的密钥，DFU 会拒绝签名包，需要用 SWD/J-Link 刷完整镜像。
- 刷机前不要同时运行小程序连接设备；BLE 和 USB 同时操作会干扰响应。
- 设备空闲休眠后 USB 会消失，按 A 或 B 键唤醒。
