#!/usr/bin/env python3
"""
ESP32 Claude Status WiFi Bridge

Discovers an ESP32S3-TFT-BS device on the local network via UDP broadcast and
forwards Claude Code hook events to it. The device must be running in
CLAUDE_WIFI mode (cycle BOOT button until LCD shows "MODE: CLAUDE_WIFI") and
must be connected to your WiFi (first time: connect to the AP
"ESP32_Claude_XXXX" / "claude123" and open http://192.168.4.1/ to enter your
home WiFi credentials).

Quick start:
    python3 claude_status_wifi_bridge.py

The script reads its settings from `tools/claude_wifi_bridge.json` (created
next to the script on first run with sensible defaults). Edit that file to
change device id / listen port / etc., then rerun. No CLI args needed.

Config keys:
    device_id        ESP32 last-4-hex MAC shown on LCD ("ID:XXXX"); empty = any
    static_ip        Skip broadcast, unicast straight to this IP
    broadcast        Broadcast address (255.255.255.255 + per-iface /24)
    listen_host      TCP bind host (default 127.0.0.1)
    listen_port      TCP bind port (default 8765, hooks point here)
    connect_on_start Send an initial idle status at startup
    verbose          Enable DEBUG logging

Then wire Claude Code hooks (`~/.claude/settings.json`) to call
`tools/claude_hook_post.py` which posts JSON to localhost:8765.

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

import asyncio
import errno
import json
import logging
import os
import signal
import socket
import sys
from types import SimpleNamespace
from typing import Optional, Tuple

ESP_UDP_PORT = 8266
DISCOVERY_INTERVAL = 2.0      # seconds between broadcasts when searching
DISCOVERY_TIMEOUT = 8.0       # seconds before warning
SEND_RETRY = 2
MSG_BYTE_LIMIT = 24

log = logging.getLogger("claude-wifi-bridge")


def _enumerate_broadcasts(default_broadcast: str = "255.255.255.255") -> list:
    """Return broadcast targets to fan discovery out to.

    `255.255.255.255` is included but on Linux it is often dropped by the
    kernel or sent on the wrong interface in multi-NIC setups. We also
    compute /24 subnet-directed broadcasts (e.g. 192.168.31.255) for every
    IPv4 interface we can detect and probe all of them.
    """
    bcasts: list = [default_broadcast]

    def _add(ip: str) -> None:
        if not ip or ip.startswith("127."):
            return
        parts = ip.split(".")
        if len(parts) != 4:
            return
        sb = ".".join(parts[:3] + ["255"])
        if sb not in bcasts:
            bcasts.append(sb)

    # POSIX path (Linux/macOS): enumerate every iface via SIOCGIFADDR.
    try:
        import fcntl
        import struct
        for _idx, name in socket.if_nameindex():
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                packed = fcntl.ioctl(
                    s.fileno(),
                    0x8915,  # SIOCGIFADDR
                    struct.pack("256s", name[:15].encode()),
                )[20:24]
                _add(socket.inet_ntoa(packed))
            except OSError:
                pass
            finally:
                s.close()
    except (ImportError, AttributeError, OSError):
        pass

    # Cross-platform fallback: ask the kernel which local IP it would use
    # for an outbound UDP to a public address. Catches the default-route IP
    # only, but better than nothing on Windows.
    if len(bcasts) == 1:
        for target in ("8.8.8.8", "1.1.1.1"):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                s.connect((target, 1))
                _add(s.getsockname()[0])
            except OSError:
                continue
            finally:
                s.close()

    return bcasts


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
    elif any(word in lower for word in ("蓝牙", "ble", "bluetooth", "wifi")):
        text = "网络状态"

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
# UDP link to the ESP32
# -----------------------------------------------------------------------------
class EspWifiLink:
    """Persistent UDP link: broadcast-discover, then unicast status to device."""

    def __init__(self, device_id: Optional[str], static_ip: Optional[str],
                 broadcast: str = "255.255.255.255") -> None:
        self.device_id = device_id.upper() if device_id else None
        self.static_ip = static_ip
        self.broadcast = broadcast
        # 把 255.255.255.255 + 所有本地接口的 /24 定向广播 (e.g. 192.168.31.255) 都列出
        self.broadcasts = _enumerate_broadcasts(broadcast)
        self.peer: Optional[Tuple[str, int]] = None
        self._lock = asyncio.Lock()
        self._sock: Optional[socket.socket] = None

    def _ensure_socket(self) -> socket.socket:
        if self._sock is not None:
            return self._sock
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # Bind ephemeral local port so device's reply has somewhere to land.
        s.bind(("", 0))
        s.setblocking(False)
        self._sock = s
        return s

    async def discover(self, timeout: float = DISCOVERY_TIMEOUT) -> Tuple[str, int]:
        """Broadcast 'discover' until the device with matching id answers."""
        if self.static_ip:
            log.info("Using static IP %s:%d (no broadcast discovery)", self.static_ip, ESP_UDP_PORT)
            self.peer = (self.static_ip, ESP_UDP_PORT)
            return self.peer

        sock = self._ensure_socket()
        loop = asyncio.get_running_loop()
        q = {"q": "discover"}
        if self.device_id:
            q["id"] = self.device_id
        payload = (json.dumps(q) + "\n").encode("utf-8")

        log.info("discover targets: %s", ", ".join(self.broadcasts))

        deadline = loop.time() + timeout
        next_send = 0.0
        while loop.time() < deadline:
            now = loop.time()
            if now >= next_send:
                for target in self.broadcasts:
                    try:
                        sock.sendto(payload, (target, ESP_UDP_PORT))
                        log.debug("discover -> %s (id=%s)", target, self.device_id)
                    except OSError as exc:
                        log.debug("discover -> %s failed: %s", target, exc)
                next_send = now + DISCOVERY_INTERVAL

            # Wait up to (next_send - now) but no more than the remaining budget
            wait = max(0.05, min(next_send - now, deadline - now))
            try:
                fut = loop.create_future()

                def _on_readable() -> None:
                    if not fut.done():
                        fut.set_result(None)

                loop.add_reader(sock.fileno(), _on_readable)
                try:
                    await asyncio.wait_for(fut, timeout=wait)
                finally:
                    loop.remove_reader(sock.fileno())
            except asyncio.TimeoutError:
                continue

            try:
                data, src = sock.recvfrom(2048)
            except BlockingIOError:
                continue
            try:
                obj = json.loads(data.decode("utf-8", errors="replace").strip())
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if not isinstance(obj, dict):
                continue
            if obj.get("r") != "discover":
                continue
            their_id = str(obj.get("id", "")).upper()
            if self.device_id and their_id != self.device_id:
                log.debug("ignoring discover reply from id %s (want %s)", their_id, self.device_id)
                continue
            ip = obj.get("ip") or src[0]
            port = int(obj.get("port") or ESP_UDP_PORT)
            self.peer = (ip, port)
            log.info("Paired with device id=%s name=%s @ %s:%d",
                     their_id, obj.get("name"), ip, port)
            return self.peer

        raise TimeoutError(f"No device id={self.device_id or 'any'} responded within {timeout:.0f}s. "
                           "Verify the ESP32 LCD shows MODE: CLAUDE_WIFI and 'WiFi: <ip>'.")

    async def send_json(self, obj: dict) -> None:
        line = (json.dumps(compact_payload(obj), separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")
        async with self._lock:
            for attempt in range(SEND_RETRY + 1):
                if self.peer is None:
                    try:
                        await self.discover()
                    except TimeoutError as exc:
                        log.error("discover failed: %s", exc)
                        if attempt == SEND_RETRY:
                            raise
                        await asyncio.sleep(0.5)
                        continue
                assert self.peer is not None
                sock = self._ensure_socket()
                try:
                    sock.sendto(line, self.peer)
                    log.debug("sent %d bytes to %s:%d", len(line), *self.peer)
                    return
                except OSError as exc:
                    log.warning("send to %s:%d failed: %s", *self.peer, exc)
                    if exc.errno in (errno.EHOSTUNREACH, errno.ENETUNREACH):
                        # device may have changed IP; force re-discover
                        self.peer = None
                    if attempt == SEND_RETRY:
                        raise

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None


# -----------------------------------------------------------------------------
# Aggregator: collapse hook events into one rolling status (same shape as BLE)
# -----------------------------------------------------------------------------
class StatusAggregator:
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
async def run_tcp_server(host: str, port: int, link: EspWifiLink, agg: StatusAggregator,
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
                try:
                    await link.send_json(full)
                except Exception as exc:  # noqa: BLE001
                    log.error("UDP forward failed: %s", exc)
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
# 配置: 同目录的 claude_wifi_bridge.json. 不存在则写一份默认模板.
# -----------------------------------------------------------------------------
CONFIG_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "claude_wifi_bridge.json",
)

DEFAULT_CONFIG = {
    "device_id": "",                # ESP32 设备号 = MAC 后 4 位 hex (LCD 上 ID:XXXX); 空 = 任意应答的设备
    "static_ip": None,              # 跳过广播发现, 直接单播这个 IP (例如 "192.168.1.50")
    "broadcast": "255.255.255.255", # 默认广播地址 (会和本地子网定向广播一起 fan-out)
    "listen_host": "127.0.0.1",     # TCP 监听地址 (Claude Code 钩子连这里)
    "listen_port": 8765,            # TCP 监听端口
    "connect_on_start": False,      # 启动时立即发一条 idle 给设备 (顺便完成发现)
    "verbose": False,               # 打开 DEBUG 日志
}


def load_config() -> SimpleNamespace:
    """读取同目录配置. 文件缺失则写默认模板."""
    if not os.path.exists(CONFIG_PATH):
        try:
            with open(CONFIG_PATH, "w", encoding="utf-8") as f:
                json.dump(DEFAULT_CONFIG, f, indent=2, ensure_ascii=False)
                f.write("\n")
            log.info("已创建默认配置: %s", CONFIG_PATH)
            log.info("请编辑该文件 (至少填 device_id), 然后重新运行.")
        except OSError as exc:
            log.warning("写默认配置失败 (%s), 仍用内置默认值", exc)

    cfg = dict(DEFAULT_CONFIG)
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            user = json.load(f)
        if isinstance(user, dict):
            cfg.update(user)
        else:
            log.warning("%s 顶层不是 object, 忽略", CONFIG_PATH)
    except FileNotFoundError:
        pass
    except (OSError, json.JSONDecodeError) as exc:
        log.warning("读配置失败 (%s), 用内置默认值", exc)

    return SimpleNamespace(**cfg)


async def amain() -> int:
    # 先开 INFO 日志, 这样 load_config 里的提示可见; 拿到 verbose 后再升级
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        datefmt="%H:%M:%S",
    )
    args = load_config()
    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    log.info("配置文件: %s", CONFIG_PATH)
    log.info("device_id=%s static_ip=%s listen=%s:%d",
             args.device_id or "(any)", args.static_ip or "(broadcast)",
             args.listen_host, args.listen_port)

    link = EspWifiLink(device_id=args.device_id or None,
                       static_ip=args.static_ip,
                       broadcast=args.broadcast)

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
            log.error("%s:%d is already in use; stop the existing bridge first",
                      args.listen_host, args.listen_port)
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

    if args.connect_on_start:
        try:
            await link.send_json({"state": "idle", "msg": "Bridge ready"})
            log.info("initial UDP status sent")
        except Exception as exc:  # noqa: BLE001
            log.warning("initial UDP send failed (%s); will retry on first hook", exc)

    await stop.wait()
    server_task.cancel()
    try:
        await server_task
    except asyncio.CancelledError:
        pass
    link.close()
    return 0


def main() -> None:
    try:
        sys.exit(asyncio.run(amain()))
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
