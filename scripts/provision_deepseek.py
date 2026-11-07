#!/usr/bin/env python3
"""Store the local DeepSeek credential in WT32-SC01 NVS over USB."""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path


PROVISION_PREFIX = b"__zclaw_deepseek_key__:"
RESPONSE_PREFIX = b"__zclaw_deepseek_key_resp__:"
DEFAULT_AUTH_FILE = Path.home() / ".letta/lc-local-backend/providers/auth.json"
MIN_KEY_LENGTH = 16
MAX_KEY_LENGTH = 255


class ProvisionError(RuntimeError):
    """A safe, user-facing provisioning failure that never includes credentials."""


def validate_api_key(api_key: str) -> None:
    if not isinstance(api_key, str) or not MIN_KEY_LENGTH <= len(api_key) <= MAX_KEY_LENGTH:
        raise ProvisionError("DeepSeek credential has an unsupported length")
    try:
        encoded = api_key.encode("ascii")
    except UnicodeEncodeError as exc:
        raise ProvisionError("DeepSeek credential must contain printable ASCII only") from exc
    if any(byte < 0x21 or byte > 0x7E for byte in encoded):
        raise ProvisionError("DeepSeek credential must contain printable ASCII only")


def load_deepseek_api_key(auth_file: Path) -> str:
    try:
        document = json.loads(auth_file.read_text(encoding="utf-8"))
        api_key = document["providers"]["deepseek"]["auth"]["key"]
    except (OSError, UnicodeError, json.JSONDecodeError, KeyError, TypeError) as exc:
        raise ProvisionError("DeepSeek credential not found in the local provider store") from exc
    validate_api_key(api_key)
    return api_key


def build_frame(api_key: str) -> bytes:
    validate_api_key(api_key)
    return PROVISION_PREFIX + api_key.encode("ascii") + b"\n"


def _wait_for_status(connection, timeout_seconds: float) -> str | None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        line = connection.readline()
        marker = line.find(RESPONSE_PREFIX)
        if marker >= 0:
            status = line[marker + len(RESPONSE_PREFIX) :].splitlines()[0].strip()
            return status.decode("ascii", errors="replace")
    return None


def provision_device(api_key: str, port: str, baudrate: int = 115200) -> None:
    validate_api_key(api_key)
    try:
        import serial
    except ImportError as exc:
        raise ProvisionError("pyserial is required; use the ESP-IDF Python environment") from exc

    try:
        with serial.Serial(port, baudrate=baudrate, timeout=0.25, write_timeout=2) as connection:
            connection.dtr = False
            connection.rts = False
            # Opening a USB-UART adapter may reset the ESP32. Let its channel task start first.
            time.sleep(2.0)
            connection.reset_input_buffer()

            # A rejected short probe proves the new firmware handles this frame before any key
            # is sent. Older firmware will not acknowledge it, so the credential stays local.
            connection.write(PROVISION_PREFIX + b"probe\n")
            connection.flush()
            if _wait_for_status(connection, 5.0) != "ERR":
                raise ProvisionError("WT32 provisioning protocol not detected; no credential sent")

            connection.write(build_frame(api_key))
            connection.flush()
            if _wait_for_status(connection, 10.0) != "OK":
                raise ProvisionError("WT32 did not confirm the NVS update")
    except ProvisionError:
        raise
    except Exception as exc:
        # Avoid echoing serial buffers or any exception payload that might contain input bytes.
        raise ProvisionError("USB provisioning failed") from exc


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyUSB0", help="ESP32 serial port")
    parser.add_argument("--baud", type=int, default=115200, help="serial baud rate")
    parser.add_argument("--auth-file", type=Path, default=DEFAULT_AUTH_FILE)
    args = parser.parse_args()

    try:
        api_key = load_deepseek_api_key(args.auth_file)
        provision_device(api_key, args.port, args.baud)
    except ProvisionError as exc:
        print(f"Provisioning failed: {exc}", file=sys.stderr)
        return 1

    print("DeepSeek settings saved in ESP32 NVS; the board is restarting.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
