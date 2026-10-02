#!/usr/bin/env python3
"""Touch monitor: python3 tools/touch_log.py (Ctrl-C to quit)."""
import glob, serial
port = glob.glob('/dev/cu.usbmodem*')[0]
ser = serial.Serial(port, 115200, timeout=1)
print(f"listening on {port}... touch the screen (Ctrl-C to quit)")
try:
    while True:
        line = ser.readline().decode(errors='replace').strip()
        if line:
            print(line)
except KeyboardInterrupt:
    pass
