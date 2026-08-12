#!/usr/bin/env python3
"""Capture a boot log from the Note4C over USB, leaving the chip running.

On ESP32-S3 the native USB-Serial-JTAG peripheral derives EN and GPIO0 from
DTR and RTS, so a session that closes the port while those lines are in the
wrong state can leave the chip in download mode: no application, no serial
output and no button response until the board is power cycled. This script
pulses reset to start the capture and always restores both lines before
closing.

Usage:
    python3 epd_serial_capture.py <port> <output> [seconds]
"""

import sys
import time

import serial


def capture(port: str, out_path: str, seconds: float) -> None:
    link = serial.Serial(port, 115200, timeout=0.2)
    try:
        # Reset into the application: GPIO0 must stay released the whole time,
        # otherwise the chip comes back up in download mode.
        link.dtr = False
        link.rts = True
        time.sleep(0.15)
        link.rts = False
        link.reset_input_buffer()

        deadline = time.time() + seconds
        with open(out_path, "w") as out:
            while time.time() < deadline:
                line = link.readline()
                if line:
                    out.write(line.decode("utf-8", "replace"))
                    out.flush()
    finally:
        # Leave both lines released so closing the port cannot latch reset or
        # download mode.
        link.dtr = False
        link.rts = False
        time.sleep(0.05)
        link.close()


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0
    capture(sys.argv[1], sys.argv[2], seconds)
    print(f"captured {seconds:.0f}s from {sys.argv[1]} to {sys.argv[2]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
