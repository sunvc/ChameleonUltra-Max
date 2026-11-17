#!/usr/bin/env python3
"""Send one ChameleonUltra USB command and print the response.

Usage:
    python tools/probe.py <cmd> [hex-data] [port]
    python tools/probe.py 1017
    python tools/probe.py 1049 41
"""

import glob
import sys
import time

import serial

BAUD = 115200
STATUS = {
    0x00: "SUCCESS",
    0x60: "PAR_ERR",
    0x66: "DEVICE_MODE_ERROR",
    0x67: "INVALID_CMD",
    0x68: "SUCCESS",
    0x69: "NOT_IMPLEMENTED",
    0x70: "FLASH_WRITE_FAIL",
    0x71: "FLASH_READ_FAIL",
    0x72: "INVALID_SLOT_TYPE",
    0x73: "MEM_ERR",
    0x74: "CREATE_RESPONSE_ERR",
    0x75: "CMD_ERR",
}


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


def read_frame(ser, timeout):
    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(1)
        if not chunk:
            continue
        buf += chunk
        if len(buf) >= 9:
            length = int.from_bytes(buf[6:8], "big")
            if len(buf) >= 9 + length + 1:
                break
    if len(buf) < 10:
        return None
    length = int.from_bytes(buf[6:8], "big")
    return (
        int.from_bytes(buf[2:4], "big"),
        int.from_bytes(buf[4:6], "big"),
        bytes(buf[9:9 + length]),
    )


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    cmd = int(sys.argv[1], 0)
    data = bytes.fromhex(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2] else b""
    port = sys.argv[3] if len(sys.argv) > 3 else find_port()
    if not port:
        print("未找到 USB 串口（/dev/cu.usbmodem*）")
        return 1

    try:
        with serial.Serial(port, BAUD, timeout=0.05) as ser:
            time.sleep(0.3)
            ser.reset_input_buffer()
            ser.write(make_frame(cmd, data))
            ser.flush()
            response = read_frame(ser, 2.0)
    except Exception as exc:
        print(f"通信失败：{exc}")
        return 1

    if response is None:
        print("无响应（设备可能休眠、正在复位，或端口不对）")
        return 1

    resp_cmd, status, payload = response
    print(f"cmd={resp_cmd} status=0x{status:02X} ({STATUS.get(status, 'UNKNOWN')})")
    print(f"len={len(payload)} data={payload.hex()}")
    printable = "".join(chr(b) if 32 <= b < 127 else "." for b in payload)
    if printable and all(32 <= b < 127 for b in payload):
        print(f"ascii={printable!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
