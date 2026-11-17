#!/usr/bin/env python3
"""Send the ChameleonUltra ENTER_BOOTLOADER command (1010) over USB CDC."""

import glob
import sys
import time

import serial

BAUD = 115200
CMD_ENTER_BOOTLOADER = 1010


def find_port():
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        ports = sorted(glob.glob(pattern))
        if ports:
            return ports[0]
    return None


def lrc(data):
    return (0x100 - (sum(data) & 0xFF)) & 0xFF


def make_frame(cmd, data=b""):
    frame = bytearray(9)
    frame[0] = 0x11
    frame[2:4] = cmd.to_bytes(2, "big")
    frame[6:8] = len(data).to_bytes(2, "big")
    frame[1] = lrc(frame[0:1])
    frame[8] = lrc(frame[0:8])
    return bytes(frame) + data + bytes([lrc(data)])


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    if not port:
        print("未找到 USB 串口（/dev/cu.usbmodem*）")
        return 1
    try:
        with serial.Serial(port, BAUD, timeout=0.1) as ser:
            time.sleep(0.3)
            ser.write(make_frame(CMD_ENTER_BOOTLOADER))
            ser.flush()
            time.sleep(0.5)
    except Exception as exc:
        print(f"打开或写入串口失败：{exc}")
        return 1
    print(f"已向 {port} 发送 1010，设备应进入 DFU")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
