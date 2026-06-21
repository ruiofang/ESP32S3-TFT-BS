#!/usr/bin/env python3
"""
ESP32 Claude Status BLE Bridge

Connects to the ESP32S3-TFT-BS device over BLE (Nordic UART Service) and
forwards Claude Code hook events to it. The device must be running in
CLAUDE mode (cycle BOOT button until LCD shows "MODE: CLAUDE").

Quick start:
    pip install bleak
    python3 claude_status_bridge.py --listen-port 8765
    # If you want BLE to connect immediately at startup:
    python3 claude_status_bridge.py --listen-port 8765 --connect-on-start

Then wire Claude Code hooks (`~/.claude/settings.json`) to call
`tools/claude_hook_post.py` which posts JSON to localhost:8765.

Send a single status update manually:
    python3 claude_status_bridge.py --once \
        --json '{"state":"thinking","tool":"Read","msg":"Reading main.c"}'

Protocol JSON shape (all fields optional except `state`):
    {
        "state": "idle|thinking|tool|writing|waiting|error|done",
        "tool":  "Read|Edit|Bash|...",
        "model": "Opus 4.7",
        "ti":    123,    // total input tokens
        "to":    45,     // total output tokens
        "msg":   "short status message"
    }
"""

from __future__ import annotations

import argparse
import asyncio
import errno
import json
import logging
import os
import signal
import subprocess
import sys
import time
from typing import Optional

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.stderr.write("bleak not installed. Run: pip install bleak\n")
    sys.exit(1)


DEVICE_NAME_PREFIX = "ESP32_Claude_"
NUS_SVC_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # PC -> ESP32 (write)
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # ESP32 -> PC (notify)
BLE_WRITE_CHUNK = 96
MSG_BYTE_LIMIT = 24

log = logging.getLogger("claude-ble-bridge")


def clip_utf8(value: object, byte_limit: int) -> str:
    data = str(value or "").encode("utf-8")[:byte_limit]
    return data.decode("utf-8", errors="ignore")


def compact_message(value: object, byte_limit: int = MSG_BYTE_LIMIT) -> str:
    text = str(value or "").replace("\r", "\n").strip()
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    text = lines[0] if lines else ""

    lower = text.lower()
    if text.startswith("command="):
        text = "cmd=" + text[len("command="):].strip()
    elif text.startswith("file_path="):
        text = "file=" + os.path.basename(text[len("file_path="):].strip())
    elif text.startswith("path="):
        text = "path=" + os.path.basename(text[len("path="):].strip())
    elif any(word in lower for word in ("未登录", "请先登录", "login", "auth", "unauthorized")):
        text = "未登录/请登录"
    elif any(word in lower for word in ("失败", "错误", "error", "failed", "exception", "traceback")):
        text = "失败/错误"
    elif any(word in lower for word in ("蓝牙", "ble", "bluetooth")):
        text = "蓝牙状态"

    return clip_utf8(text, byte_limit)


def compact_payload(obj: dict) -> dict:
    payload = dict(obj)
    if "state" in payload:
        payload["state"] = clip_utf8(payload["state"], 24)
    if "tool" in payload:
        payload["tool"] = clip_utf8(payload["tool"], 23)
    if "model" in payload:
        payload["model"] = clip_utf8(payload["model"], 23)
    if "msg" in payload:
        payload["msg"] = compact_message(payload["msg"])
    return payload


# -----------------------------------------------------------------------------
# BLE link
# -----------------------------------------------------------------------------
class EspBleLink:
    """Persistent BLE NUS link to the ESP32. Reconnects automatically."""

    def __init__(self, device_name: Optional[str] = None, device_address: Optional[str] = None,
                 scan_timeout: float = 8.0):
        self.device_name = device_name
        self.device_address = device_address
        self.scan_timeout = scan_timeout
        self._client: Optional[BleakClient] = None
        self._lock = asyncio.Lock()

    def _cached_bluez_address(self) -> Optional[str]:
        try:
            proc = subprocess.run(
                ["bluetoothctl", "devices"],
                check=False,
                capture_output=True,
                text=True,
                timeout=2.0,
            )
        except (OSError, subprocess.TimeoutExpired):
            return None
        for line in proc.stdout.splitlines():
            parts = line.strip().split(maxsplit=2)
            if len(parts) < 3 or parts[0] != "Device":
                continue
            addr, name = parts[1], parts[2]
            if self.device_name and name == self.device_name:
                log.info("Using cached BlueZ device '%s' @ %s", name, addr)
                return addr
            if not self.device_name and name.startswith(DEVICE_NAME_PREFIX):
                log.info("Using cached BlueZ device '%s' @ %s", name, addr)
                return addr
        return None

    def _bluez_dbus_write(self, data: bytes) -> bool:
        if sys.platform != "linux":
            return False
        try:
            import dbus  # type: ignore[import-not-found]
        except ImportError:
            return False

        bus = dbus.SystemBus()
        manager = dbus.Interface(
            bus.get_object("org.bluez", "/"),
            "org.freedesktop.DBus.ObjectManager",
        )
        objects = manager.GetManagedObjects()

        device_path: Optional[str] = None
        for path, interfaces in objects.items():
            props = interfaces.get("org.bluez.Device1")
            if not props:
                continue
            addr = str(props.get("Address", ""))
            name = str(props.get("Name", props.get("Alias", "")))
            if self.device_address and addr.lower() == self.device_address.lower():
                device_path = str(path)
                break
            if self.device_name and name == self.device_name:
                device_path = str(path)
                break
            if not self.device_name and not self.device_address and name.startswith(DEVICE_NAME_PREFIX):
                device_path = str(path)
                break
        if not device_path:
            return False

        dev_obj = bus.get_object("org.bluez", device_path)
        dev_props = dbus.Interface(dev_obj, "org.freedesktop.DBus.Properties")
        connected = bool(dev_props.Get("org.bluez.Device1", "Connected"))
        if not connected:
            dev = dbus.Interface(dev_obj, "org.bluez.Device1")
            try:
                dev.Connect()
            except dbus.exceptions.DBusException as exc:
                if "AlreadyConnected" not in (exc.get_dbus_name() or ""):
                    raise

            deadline = time.monotonic() + max(5.0, self.scan_timeout)
            while time.monotonic() < deadline:
                if bool(dev_props.Get("org.bluez.Device1", "ServicesResolved")):
                    break
                time.sleep(0.1)

        objects = manager.GetManagedObjects()
        rx_path: Optional[str] = None
        for path, interfaces in objects.items():
            props = interfaces.get("org.bluez.GattCharacteristic1")
            if not props:
                continue
            if str(props.get("UUID", "")).lower() != NUS_RX_UUID:
                continue
            service = str(props.get("Service", ""))
            if service.startswith(device_path + "/"):
                rx_path = str(path)
                break
        if not rx_path:
            return False

        log.info("Writing via BlueZ D-Bus GATT characteristic %s", rx_path)
        ch = dbus.Interface(
            bus.get_object("org.bluez", rx_path),
            "org.bluez.GattCharacteristic1",
        )
        opts = dbus.Dictionary({"type": dbus.String("command")}, signature="sv")
        for i in range(0, len(data), BLE_WRITE_CHUNK):
            value = dbus.Array([dbus.Byte(b) for b in data[i:i + BLE_WRITE_CHUNK]], signature="y")
            ch.WriteValue(value, opts)
        return True

    async def _try_bluez_dbus_write(self, data: bytes) -> bool:
        return await asyncio.to_thread(self._bluez_dbus_write, data)

    async def _scan(self) -> str:
        if self.device_address:
            return self.device_address

        log.info("Scanning for BLE device (prefix=%s)...", DEVICE_NAME_PREFIX)
        devices = await BleakScanner.discover(timeout=self.scan_timeout)
        for d in devices:
            name = d.name or ""
            if self.device_name and name == self.device_name:
                log.info("Found device '%s' @ %s", name, d.address)
                return d.address
            if not self.device_name and name.startswith(DEVICE_NAME_PREFIX):
                log.info("Found device '%s' @ %s", name, d.address)
                return d.address

        cached = self._cached_bluez_address()
        if cached:
            return cached
        raise RuntimeError("ESP32 Claude device not found in scan results")

    async def _ensure_connected(self) -> BleakClient:
        if self._client and self._client.is_connected:
            return self._client
        addr = await self._scan()
        client = BleakClient(addr)
        await client.connect()
        log.info("BLE connected to %s", addr)
        self._client = client
        return client

    async def send_json(self, obj: dict) -> None:
        line = (json.dumps(compact_payload(obj), separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")
        async with self._lock:
            try:
                if await self._try_bluez_dbus_write(line):
                    return
            except Exception as exc:  # noqa: BLE001
                log.warning("BlueZ D-Bus write failed: %s", exc)

            # Up to 3 attempts with reconnect on failure
            for attempt in range(3):
                try:
                    client = await self._ensure_connected()
                    # Write in 180-byte chunks (under typical MTU minus header)
                    chunk = BLE_WRITE_CHUNK
                    for i in range(0, len(line), chunk):
                        await client.write_gatt_char(NUS_RX_UUID, line[i:i + chunk], response=False)
                    return
                except Exception as exc:  # noqa: BLE001
                    log.warning("send_json attempt %d failed: %s", attempt + 1, exc)
                    if self._client:
                        try:
                            await self._client.disconnect()
                        except Exception:  # noqa: BLE001
                            pass
                        self._client = None
                    await asyncio.sleep(1.0)
            raise RuntimeError("send_json failed after retries")

    async def close(self) -> None:
        if self._client:
            try:
                await self._client.disconnect()
            except Exception:  # noqa: BLE001
                pass
            self._client = None


# -----------------------------------------------------------------------------
# Optional aggregator: keeps a rolling status that hooks update incrementally
# -----------------------------------------------------------------------------
class StatusAggregator:
    """Collapses high-frequency hook events into a single rolling status."""

    def __init__(self) -> None:
        self.state: str = "idle"
        self.tool: str = ""
        self.model: str = ""
        self.tokens_in: int = 0
        self.tokens_out: int = 0
        self.msg: str = ""

    def update(self, patch: dict) -> dict:
        if "state" in patch:
            self.state = clip_utf8(patch["state"], 24)
        if "tool" in patch:
            self.tool = clip_utf8(patch["tool"], 23)
        if "model" in patch:
            self.model = clip_utf8(patch["model"], 23)
        if "ti" in patch:
            try:
                self.tokens_in = int(patch["ti"])
            except (TypeError, ValueError):
                pass
        if "to" in patch:
            try:
                self.tokens_out = int(patch["to"])
            except (TypeError, ValueError):
                pass
        if "msg" in patch:
            self.msg = compact_message(patch["msg"])
        return {
            "state": self.state,
            "tool": self.tool,
            "model": self.model,
            "ti": self.tokens_in,
            "to": self.tokens_out,
            "msg": self.msg,
        }


# -----------------------------------------------------------------------------
# TCP server: accepts one line-delimited JSON per connection
# -----------------------------------------------------------------------------
async def run_tcp_server(host: str, port: int, link: EspBleLink, agg: StatusAggregator,
                         started: Optional[asyncio.Future] = None) -> None:
    async def handle_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        peer = writer.get_extra_info("peername")
        try:
            data = await asyncio.wait_for(reader.read(4096), timeout=2.0)
            if not data:
                return
            text = data.decode("utf-8", errors="replace").strip()
            for line in text.splitlines():
                line = line.strip()
                if not line:
                    continue
                try:
                    patch = json.loads(line)
                    if not isinstance(patch, dict):
                        raise ValueError("not an object")
                except (ValueError, json.JSONDecodeError) as exc:
                    log.warning("bad JSON from %s: %s (%s)", peer, exc, line[:120])
                    continue
                full = agg.update(patch)
                log.debug("forwarding: %s", full)
                try:
                    await link.send_json(full)
                except Exception as exc:  # noqa: BLE001
                    log.error("BLE forward failed: %s", exc)
            writer.write(b"OK\n")
            await writer.drain()
        except asyncio.TimeoutError:
            log.warning("client %s timed out", peer)
        except Exception as exc:  # noqa: BLE001
            log.error("client %s error: %s", peer, exc)
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:  # noqa: BLE001
                pass

    try:
        server = await asyncio.start_server(handle_client, host=host, port=port)
    except Exception as exc:
        if started and not started.done():
            started.set_exception(exc)
        raise
    sockets = ", ".join(str(s.getsockname()) for s in server.sockets or [])
    log.info("listening on %s", sockets)
    if started and not started.done():
        started.set_result(None)
    async with server:
        await server.serve_forever()


# -----------------------------------------------------------------------------
# CLI
# -----------------------------------------------------------------------------
def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--device-name", help="exact BLE device name (default: any ESP32_Claude_*)")
    p.add_argument("--address", help="BLE device address (skips scan; useful on Ubuntu/BlueZ cached devices)")
    p.add_argument("--listen-host", default="127.0.0.1", help="TCP bind host (default 127.0.0.1)")
    p.add_argument("--listen-port", type=int, default=8765, help="TCP bind port (default 8765)")
    p.add_argument("--scan-timeout", type=float, default=8.0, help="BLE scan timeout seconds")
    p.add_argument("--connect-on-start", action="store_true",
                   help="send an initial idle status to connect BLE immediately")
    p.add_argument("--once", action="store_true", help="send one status from --json and exit")
    p.add_argument("--json", help="JSON payload for --once mode")
    p.add_argument("-v", "--verbose", action="store_true", help="debug logging")
    return p.parse_args()


async def amain() -> int:
    args = parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        datefmt="%H:%M:%S",
    )

    link = EspBleLink(device_name=args.device_name, device_address=args.address,
                      scan_timeout=args.scan_timeout)

    if args.once:
        if not args.json:
            log.error("--once requires --json")
            return 2
        try:
            payload = json.loads(args.json)
        except json.JSONDecodeError as exc:
            log.error("invalid JSON: %s", exc)
            return 2
        try:
            await link.send_json(payload)
            log.info("sent.")
        finally:
            await link.close()
        return 0

    agg = StatusAggregator()

    stop = asyncio.Event()

    def _on_signal(*_: object) -> None:
        log.info("shutting down...")
        stop.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            asyncio.get_running_loop().add_signal_handler(sig, _on_signal)
        except (NotImplementedError, RuntimeError):
            signal.signal(sig, lambda *_: _on_signal())

    started = asyncio.get_running_loop().create_future()
    server_task = asyncio.create_task(
        run_tcp_server(args.listen_host, args.listen_port, link, agg, started)
    )
    try:
        await started
    except OSError as exc:
        if exc.errno == errno.EADDRINUSE:
            log.error("%s:%d is already in use; stop the existing bridge first", args.listen_host, args.listen_port)
            try:
                await server_task
            except OSError:
                pass
            return 1
        raise
    except Exception:
        server_task.cancel()
        try:
            await server_task
        except asyncio.CancelledError:
            pass
        raise

    cached = link._cached_bluez_address()
    if cached:
        log.info("BLE device cached by BlueZ @ %s; will connect/write on first hook", cached)
    else:
        log.info("BLE device is not cached yet; this is OK. Will scan on first hook (ESP32 must show MODE: CLAUDE)")

    if args.connect_on_start:
        try:
            await link.send_json({"state": "idle", "msg": "Bridge ready"})
            log.info("initial BLE status sent")
        except Exception as exc:  # noqa: BLE001
            log.warning("initial BLE connect/send failed (%s); will retry on first hook", exc)

    await stop.wait()
    server_task.cancel()
    try:
        await server_task
    except asyncio.CancelledError:
        pass
    await link.close()
    return 0


def main() -> None:
    try:
        sys.exit(asyncio.run(amain()))
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
