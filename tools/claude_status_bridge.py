#!/usr/bin/env python3
"""
ESP32 Claude Status BLE Bridge

Connects to the ESP32S3-TFT-BS device over BLE (Nordic UART Service) and
forwards Claude Code hook events to it. The device must be running in
CLAUDE mode (cycle BOOT button until LCD shows "MODE: CLAUDE").

Quick start:
    pip install bleak
    python3 claude_status_bridge.py --listen-port 8765

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
import json
import logging
import os
import signal
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

log = logging.getLogger("claude-ble-bridge")


# -----------------------------------------------------------------------------
# BLE link
# -----------------------------------------------------------------------------
class EspBleLink:
    """Persistent BLE NUS link to the ESP32. Reconnects automatically."""

    def __init__(self, device_name: Optional[str] = None, scan_timeout: float = 8.0):
        self.device_name = device_name
        self.scan_timeout = scan_timeout
        self._client: Optional[BleakClient] = None
        self._lock = asyncio.Lock()

    async def _scan(self) -> str:
        log.info("Scanning for BLE device (prefix=%s)...", DEVICE_NAME_PREFIX)
        devices = await BleakScanner.discover(timeout=self.scan_timeout)
        for d in devices:
            name = d.name or ""
            if self.device_name and name == self.device_name:
                return d.address
            if not self.device_name and name.startswith(DEVICE_NAME_PREFIX):
                log.info("Found device '%s' @ %s", name, d.address)
                return d.address
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
        line = (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
        async with self._lock:
            # Up to 3 attempts with reconnect on failure
            for attempt in range(3):
                try:
                    client = await self._ensure_connected()
                    # Write in 180-byte chunks (under typical MTU minus header)
                    chunk = 180
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
            self.state = str(patch["state"])[:24]
        if "tool" in patch:
            self.tool = str(patch["tool"])[:23]
        if "model" in patch:
            self.model = str(patch["model"])[:23]
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
            self.msg = str(patch["msg"])[:63]
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
async def run_tcp_server(host: str, port: int, link: EspBleLink, agg: StatusAggregator) -> None:
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

    server = await asyncio.start_server(handle_client, host=host, port=port)
    sockets = ", ".join(str(s.getsockname()) for s in server.sockets or [])
    log.info("listening on %s", sockets)
    async with server:
        await server.serve_forever()


# -----------------------------------------------------------------------------
# CLI
# -----------------------------------------------------------------------------
def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--device-name", help="exact BLE device name (default: any ESP32_Claude_*)")
    p.add_argument("--listen-host", default="127.0.0.1", help="TCP bind host (default 127.0.0.1)")
    p.add_argument("--listen-port", type=int, default=8765, help="TCP bind port (default 8765)")
    p.add_argument("--scan-timeout", type=float, default=8.0, help="BLE scan timeout seconds")
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

    link = EspBleLink(device_name=args.device_name, scan_timeout=args.scan_timeout)

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

    server_task = asyncio.create_task(
        run_tcp_server(args.listen_host, args.listen_port, link, agg)
    )

    # Initial connect attempt (will retry on demand)
    try:
        await link._ensure_connected()
    except Exception as exc:  # noqa: BLE001
        log.warning("initial BLE connect failed (%s); will retry on first hook", exc)

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
