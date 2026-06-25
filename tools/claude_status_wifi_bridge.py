#!/usr/bin/env python3
"""
ESP32 Claude Status WiFi Bridge

Uses UDP broadcast only for discovery/pairing, then keeps one persistent TCP
connection per ESP32 target for status delivery. That gives the ESP32 a real
connection lifecycle: orderly close, reset, or heartbeat timeout can all be
treated as offline.

The device must be running in CLAUDE_WIFI mode and connected to your LAN.
The bridge reads config from tools/claude_wifi_bridge.json and accepts Claude
Code hook events over localhost TCP from tools/claude_hook_post.py.

Supported config styles:
1. Legacy single-target fields: device_id/static_ip
2. device_ids: ["AB12", "CD34"]
3. targets: [{"device_id":"AB12","label":"desk"}, ...]

Discovery auto-fills the target IP and TCP port from the ESP32 discovery
reply; you only need the device id for pairing unless you want static_ip.
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
import time
from dataclasses import dataclass
from types import SimpleNamespace
from typing import Optional

ESP_UDP_PORT = 8266
ESP_TCP_PORT = 8267
DISCOVERY_INTERVAL = 2.0      # seconds between broadcasts when searching
DISCOVERY_TIMEOUT = 8.0       # seconds before warning
SEND_RETRY = 2
CONNECT_TIMEOUT = 3.0
HEARTBEAT_INTERVAL = 4.0
IDLE_FALLBACK_CHECK_INTERVAL = 1.0
MSG_BYTE_LIMIT = 24
EXIT_RESEND_COUNT = 3
EXIT_RESEND_INTERVAL = 0.4

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


@dataclass
class TargetConfig:
    label: str
    device_id: Optional[str]
    static_ip: Optional[str]
    tcp_port: int = ESP_TCP_PORT


class EspTcpTarget:
    """One ESP32 target: UDP discover + persistent TCP connection."""

    def __init__(self, config: TargetConfig, broadcast: str, source_name: str) -> None:
        self.config = config
        self.device_id = config.device_id.upper() if config.device_id else None
        self.broadcasts = _enumerate_broadcasts(broadcast)
        self.source_name = clip_utf8(source_name, 23) or "host"
        self.peer_ip: Optional[str] = None
        self.peer_port: int = config.tcp_port or ESP_TCP_PORT
        self.peer_name: str = config.label or ""
        self.discovered_id: Optional[str] = self.device_id
        self._writer: Optional[asyncio.StreamWriter] = None
        self._reader_task: Optional[asyncio.Task] = None
        self._lock = asyncio.Lock()

    @property
    def display_name(self) -> str:
        if self.config.label:
            return self.config.label
        if self.device_id:
            return self.device_id
        if self.peer_name:
            return self.peer_name
        return self.peer_ip or "auto"

    async def discover(self, timeout: float = DISCOVERY_TIMEOUT) -> tuple[str, int]:
        if self.config.static_ip:
            self.peer_ip = self.config.static_ip
            self.peer_port = self.config.tcp_port or ESP_TCP_PORT
            log.info("%s using static TCP target %s:%d",
                     self.display_name, self.peer_ip, self.peer_port)
            return self.peer_ip, self.peer_port

        loop = asyncio.get_running_loop()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("", 0))
        sock.setblocking(False)

        try:
            q = {"q": "discover"}
            if self.device_id:
                q["id"] = self.device_id
            payload = (json.dumps(q, separators=(",", ":")) + "\n").encode("utf-8")

            deadline = loop.time() + timeout
            next_send = 0.0
            while loop.time() < deadline:
                now = loop.time()
                if now >= next_send:
                    for target in self.broadcasts:
                        try:
                            sock.sendto(payload, (target, ESP_UDP_PORT))
                            log.debug("discover %s -> %s (id=%s)",
                                      self.display_name, target, self.device_id or "*")
                        except OSError as exc:
                            log.debug("discover %s -> %s failed: %s",
                                      self.display_name, target, exc)
                    next_send = now + DISCOVERY_INTERVAL

                wait = max(0.05, min(next_send - now, deadline - now))
                fut = loop.create_future()

                def _on_readable() -> None:
                    if not fut.done():
                        fut.set_result(None)

                loop.add_reader(sock.fileno(), _on_readable)
                try:
                    await asyncio.wait_for(fut, timeout=wait)
                except asyncio.TimeoutError:
                    continue
                finally:
                    loop.remove_reader(sock.fileno())

                try:
                    data, src = sock.recvfrom(2048)
                except BlockingIOError:
                    continue

                try:
                    obj = json.loads(data.decode("utf-8", errors="replace").strip())
                except (UnicodeDecodeError, json.JSONDecodeError):
                    continue
                if not isinstance(obj, dict) or obj.get("r") != "discover":
                    continue

                their_id = str(obj.get("id", "") or "").upper()
                if self.device_id and their_id != self.device_id:
                    log.debug("ignoring discover reply from %s for target %s",
                              their_id, self.display_name)
                    continue

                self.discovered_id = their_id or self.device_id
                self.peer_ip = str(obj.get("ip") or src[0])
                self.peer_port = int(obj.get("tcp_port") or obj.get("port") or ESP_TCP_PORT)
                self.peer_name = str(obj.get("name") or self.display_name)
                log.info("paired %s -> id=%s name=%s @ %s:%d",
                         self.display_name,
                         self.discovered_id or "(any)",
                         self.peer_name,
                         self.peer_ip,
                         self.peer_port)
                return self.peer_ip, self.peer_port

        finally:
            sock.close()

        raise TimeoutError(
            f"No device for target {self.display_name} responded within {timeout:.0f}s. "
            "Verify the ESP32 LCD shows MODE: CLAUDE_WIFI and a WiFi IP."
        )

    async def _watch_reader(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            while True:
                data = await reader.read(64)
                if not data:
                    break
        except Exception as exc:  # noqa: BLE001
            log.debug("reader for %s ended: %s", self.display_name, exc)
        finally:
            if self._writer is writer:
                self._drop_connection(log_message=False)
                log.info("TCP closed by ESP32: %s", self.display_name)

    def _drop_connection(self, log_message: bool = True) -> None:
        writer = self._writer
        self._writer = None

        if self._reader_task is not None:
            self._reader_task.cancel()
            self._reader_task = None

        if writer is not None:
            try:
                writer.close()
            except Exception:  # noqa: BLE001
                pass

        if log_message:
            log.debug("connection reset for %s", self.display_name)

    async def _connect_locked(self) -> None:
        if self._writer is not None:
            return

        await self.discover()
        assert self.peer_ip is not None

        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(self.peer_ip, self.peer_port),
            timeout=CONNECT_TIMEOUT,
        )

        sock = writer.get_extra_info("socket")
        if sock is not None:
            try:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            except OSError:
                pass

        self._writer = writer
        self._reader_task = asyncio.create_task(self._watch_reader(reader, writer))
        bind_msg = {
            "q": "bind",
            "id": self.discovered_id or self.device_id or "",
            "source": self.source_name,
            "label": self.config.label or "",
        }
        writer.write((json.dumps(bind_msg, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8"))
        # Re-sync visible state immediately after (re)connect so ESP32 does not stay OFFLINE
        # when no fresh hook event has arrived yet.
        writer.write((json.dumps({"state": "idle", "tool": "", "msg": "Bridge linked"}, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8"))
        await writer.drain()
        log.info("TCP linked: %s -> %s:%d", self.display_name, self.peer_ip, self.peer_port)

    async def send_json(self, obj: dict) -> None:
        line = (json.dumps(compact_payload(obj), separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")
        async with self._lock:
            for attempt in range(SEND_RETRY + 1):
                try:
                    await self._connect_locked()
                    assert self._writer is not None
                    self._writer.write(line)
                    await self._writer.drain()
                    return
                except (OSError, asyncio.TimeoutError, ConnectionError) as exc:
                    log.warning("send to %s failed: %s", self.display_name, exc)
                    self._drop_connection()
                    if attempt == SEND_RETRY:
                        raise
                    await asyncio.sleep(0.3)

    async def send_ping(self) -> None:
        async with self._lock:
            try:
                # Keepalive should also recover broken links proactively.
                await self._connect_locked()
                if self._writer is None:
                    return
                msg = {
                    "q": "ping",
                    "id": self.discovered_id or self.device_id or "",
                    "source": self.source_name,
                }
                self._writer.write((json.dumps(msg, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8"))
                await self._writer.drain()
            except (OSError, asyncio.TimeoutError, ConnectionError) as exc:
                log.warning("ping/reconnect %s failed: %s", self.display_name, exc)
                self._drop_connection()

    async def close(self) -> None:
        async with self._lock:
            self._drop_connection(log_message=False)


class EspTargetFanout:
    def __init__(self, targets: list[EspTcpTarget]) -> None:
        self.targets = targets

    async def send_json(self, obj: dict) -> None:
        if not self.targets:
            raise RuntimeError("no ESP32 targets configured")

        results = await asyncio.gather(
            *(target.send_json(obj) for target in self.targets),
            return_exceptions=True,
        )
        failures = []
        for target, result in zip(self.targets, results):
            if isinstance(result, Exception):
                failures.append((target.display_name, result))
        for name, exc in failures:
            log.error("target %s failed: %s", name, exc)
        if len(failures) == len(self.targets):
            raise RuntimeError("all ESP32 targets failed")

    async def send_ping(self) -> None:
        if not self.targets:
            return
        await asyncio.gather(*(target.send_ping() for target in self.targets), return_exceptions=True)

    async def close(self) -> None:
        await asyncio.gather(*(target.close() for target in self.targets), return_exceptions=True)


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
        self._last_event_monotonic: float = time.monotonic()
        self._idle_fallback_sent: bool = False
        self.exited: bool = False

    def update(self, patch: dict) -> dict:
        self._last_event_monotonic = time.monotonic()
        self._idle_fallback_sent = False
        incoming_state = clip_utf8(patch["state"], 24) if "state" in patch else None
        # Latch the exit flag so idle fallback won't overwrite the exit notice.
        # Any non-"exited" state arriving later (e.g. a new session) clears it.
        if incoming_state is not None:
            self.exited = incoming_state.lower() == "exited"
        if "state" in patch:
            self.state = incoming_state or self.state
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

    def maybe_idle_fallback(self, idle_timeout_sec: float) -> Optional[dict]:
        """Return an idle payload once when no hook event arrives for too long."""
        if idle_timeout_sec <= 0:
            return None
        if self._idle_fallback_sent:
            return None
        if self.exited:
            # Don't erase the "Claude exited" notice with an idle fallback.
            return None
        if (time.monotonic() - self._last_event_monotonic) < idle_timeout_sec:
            return None

        if self.state.lower() == "idle":
            self._idle_fallback_sent = True
            return None

        self.state = "idle"
        self.tool = ""
        self.msg = "No recent activity"
        self._idle_fallback_sent = True
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
async def run_tcp_server(host: str, port: int, link: EspTargetFanout, agg: StatusAggregator,
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
                    log.error("ESP forward failed: %s", exc)
                if full.get("state", "").lower() == "exited":
                    # Re-emit the exit notice a few times — a single TCP packet
                    # can be dropped on a flaky link and we won't get another
                    # event after Claude has actually exited.
                    for _ in range(EXIT_RESEND_COUNT):
                        await asyncio.sleep(EXIT_RESEND_INTERVAL)
                        try:
                            await link.send_json(full)
                        except Exception as exc:  # noqa: BLE001
                            log.debug("exit resend failed: %s", exc)
                    log.info("Claude exited notice delivered: %s", full.get("msg", ""))
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
    "device_id": "",                # 旧单设备配置: ESP32 设备号 = MAC 后 4 位 hex (LCD 上 ID:XXXX)
    "device_ids": [],               # 多设备简写: ["AB12", "CD34"]
    "targets": [],                  # 推荐: [{"device_id":"AB12","label":"desk"}, ...]
    "static_ip": None,              # 旧单设备配置: 跳过广播发现, 直接连这个 IP
    "broadcast": "255.255.255.255", # 默认广播地址 (会和本地子网定向广播一起 fan-out)
    "listen_host": "127.0.0.1",     # TCP 监听地址 (Claude Code 钩子连这里)
    "listen_port": 8765,            # TCP 监听端口
    "source_name": "copilot",      # 发给 ESP32 的绑定名称
    "connect_on_start": False,      # 启动时立即发一条 idle 给设备 (顺便完成发现)
    "idle_timeout_sec": 45,         # 多久没收到 hook 事件就自动回落到 idle (<=0 关闭)
    "verbose": False,               # 打开 DEBUG 日志
}


def build_target_configs(args: SimpleNamespace) -> list[TargetConfig]:
    configs: list[TargetConfig] = []

    raw_targets = getattr(args, "targets", None)
    if isinstance(raw_targets, list):
        for index, item in enumerate(raw_targets, start=1):
            if not isinstance(item, dict):
                continue
            device_id = str(item.get("device_id") or "").strip().upper() or None
            static_ip = str(item.get("static_ip") or "").strip() or None
            label = str(item.get("label") or device_id or f"target-{index}")
            tcp_port = int(item.get("tcp_port") or ESP_TCP_PORT)
            configs.append(TargetConfig(label=label, device_id=device_id, static_ip=static_ip, tcp_port=tcp_port))

    raw_ids = getattr(args, "device_ids", None)
    if not configs and isinstance(raw_ids, list):
        for index, item in enumerate(raw_ids, start=1):
            device_id = str(item or "").strip().upper()
            if not device_id:
                continue
            configs.append(TargetConfig(label=device_id or f"target-{index}", device_id=device_id, static_ip=None))

    if not configs:
        device_id = str(getattr(args, "device_id", "") or "").strip().upper() or None
        static_ip = str(getattr(args, "static_ip", "") or "").strip() or None
        label = device_id or static_ip or "auto"
        configs.append(TargetConfig(label=label, device_id=device_id, static_ip=static_ip))

    return configs


async def heartbeat_loop(stop: asyncio.Event, link: EspTargetFanout) -> None:
    while not stop.is_set():
        try:
            await asyncio.wait_for(stop.wait(), timeout=HEARTBEAT_INTERVAL)
            break
        except asyncio.TimeoutError:
            await link.send_ping()


async def idle_fallback_loop(
    stop: asyncio.Event,
    link: EspTargetFanout,
    agg: StatusAggregator,
    idle_timeout_sec: float,
) -> None:
    if idle_timeout_sec <= 0:
        return

    while not stop.is_set():
        try:
            await asyncio.wait_for(stop.wait(), timeout=IDLE_FALLBACK_CHECK_INTERVAL)
            break
        except asyncio.TimeoutError:
            payload = agg.maybe_idle_fallback(idle_timeout_sec)
            if not payload:
                continue
            try:
                await link.send_json(payload)
                log.info("no hook updates for %.0fs -> fallback to idle", idle_timeout_sec)
            except Exception as exc:  # noqa: BLE001
                log.warning("idle fallback send failed: %s", exc)


async def send_shutdown_status(link: EspTargetFanout) -> None:
    """Best-effort tail status to reduce stale thinking/tool display on ESP32."""
    for payload in (
        {"state": "done", "msg": "Bridge shutting down"},
        {"state": "idle", "msg": "Bridge offline"},
    ):
        try:
            await link.send_json(payload)
        except Exception as exc:  # noqa: BLE001
            log.debug("shutdown status send skipped: %s", exc)


def load_config() -> SimpleNamespace:
    """读取同目录配置. 文件缺失则写默认模板."""
    if not os.path.exists(CONFIG_PATH):
        try:
            with open(CONFIG_PATH, "w", encoding="utf-8") as f:
                json.dump(DEFAULT_CONFIG, f, indent=2, ensure_ascii=False)
                f.write("\n")
            log.info("已创建默认配置: %s", CONFIG_PATH)
            log.info("请编辑该文件 (可填 device_id/device_ids/targets), 然后重新运行.")
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
    target_configs = build_target_configs(args)
    idle_timeout_sec = float(getattr(args, "idle_timeout_sec", 45) or 0)
    log.info("listen=%s:%d targets=%d source=%s",
             args.listen_host, args.listen_port, len(target_configs), args.source_name)
    if idle_timeout_sec > 0:
        log.info("idle fallback timeout: %.0fs", idle_timeout_sec)
    else:
        log.info("idle fallback timeout: disabled")
    for config in target_configs:
        log.info("target %s id=%s static_ip=%s tcp_port=%d",
                 config.label,
                 config.device_id or "(any)",
                 config.static_ip or "(discover)",
                 config.tcp_port)

    link = EspTargetFanout([
        EspTcpTarget(config, args.broadcast, args.source_name)
        for config in target_configs
    ])

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
    heartbeat_task = asyncio.create_task(heartbeat_loop(stop, link))
    idle_task = asyncio.create_task(idle_fallback_loop(stop, link, agg, idle_timeout_sec))
    try:
        await started
    except OSError as exc:
        if exc.errno == errno.EADDRINUSE:
            log.error("%s:%d is already in use; stop the existing bridge first",
                      args.listen_host, args.listen_port)
            heartbeat_task.cancel()
            idle_task.cancel()
            try:
                await server_task
            except OSError:
                pass
            try:
                await heartbeat_task
            except asyncio.CancelledError:
                pass
            try:
                await idle_task
            except asyncio.CancelledError:
                pass
            await link.close()
            return 1
        raise
    except Exception:
        server_task.cancel()
        heartbeat_task.cancel()
        idle_task.cancel()
        try:
            await server_task
        except asyncio.CancelledError:
            pass
        try:
            await heartbeat_task
        except asyncio.CancelledError:
            pass
        try:
            await idle_task
        except asyncio.CancelledError:
            pass
        await link.close()
        raise

    if args.connect_on_start:
        try:
            await link.send_json({"state": "idle", "msg": "Bridge ready"})
            log.info("initial TCP status sent")
        except Exception as exc:  # noqa: BLE001
            log.warning("initial TCP send failed (%s); will retry on first hook", exc)

    await stop.wait()
    await send_shutdown_status(link)
    server_task.cancel()
    heartbeat_task.cancel()
    idle_task.cancel()
    try:
        await server_task
    except asyncio.CancelledError:
        pass
    try:
        await heartbeat_task
    except asyncio.CancelledError:
        pass
    try:
        await idle_task
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
