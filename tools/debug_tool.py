#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32S3-TFT-BS 调试工具
支持:
  - JSON 模式: 发送 WS2812/电池/电压等 JSON 命令
  - RS485-1 模拟: 模拟 DD/77 帧格式电池 BMS 应答
  - RS485-2 模拟: 模拟 0xAA 帧格式电池 BMS 应答
"""

import sys
import os
import struct
import json
import time
from functools import partial

# 解决 PyQt5 在特殊路径下找不到 xcb 插件的问题
try:
    import PyQt5
    _qt_dir = os.path.join(os.path.dirname(PyQt5.__file__), 'Qt5', 'plugins')
    if os.path.isdir(_qt_dir):
        os.environ.setdefault('QT_QPA_PLATFORM_PLUGIN_PATH', _qt_dir)
        os.environ.setdefault('QT_PLUGIN_PATH', _qt_dir)
except Exception:
    pass

from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QTabWidget, QGroupBox, QLabel, QComboBox, QPushButton, QLineEdit,
    QTextEdit, QSpinBox, QDoubleSpinBox, QCheckBox, QSlider, QGridLayout,
    QFormLayout, QSplitter, QMessageBox
)
from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtGui import QFont
import serial
import serial.tools.list_ports


class SerialManager:
    """串口管理"""
    def __init__(self):
        self.ser = None

    def open(self, port, baud):
        self.close()
        # 先不打开，手动控制 DTR/RTS 避免 CH340X 触发 ESP32 复位
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.1
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        return True

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()
        self.ser = None

    @property
    def is_open(self):
        return self.ser is not None and self.ser.is_open

    def write(self, data: bytes):
        if self.is_open:
            self.ser.write(data)

    def read_all(self) -> bytes:
        if self.is_open and self.ser.in_waiting:
            return self.ser.read(self.ser.in_waiting)
        return b''

    def set_baudrate(self, baud):
        if self.is_open:
            self.ser.baudrate = baud


class JsonTab(QWidget):
    """JSON 命令发送面板"""
    def __init__(self, serial_mgr, log_fn):
        super().__init__()
        self.serial_mgr = serial_mgr
        self.log = log_fn
        self._init_ui()

    def _init_ui(self):
        layout = QVBoxLayout(self)

        # 快捷命令区
        grp = QGroupBox("快捷命令")
        grid = QGridLayout(grp)

        # 电池状态
        grid.addWidget(QLabel("电压 (V):"), 0, 0)
        self.voltage_spin = QDoubleSpinBox()
        self.voltage_spin.setRange(18.0, 29.4)
        self.voltage_spin.setValue(24.0)
        self.voltage_spin.setSingleStep(0.1)
        grid.addWidget(self.voltage_spin, 0, 1)

        grid.addWidget(QLabel("电量 (%):"), 0, 2)
        self.battery_spin = QSpinBox()
        self.battery_spin.setRange(0, 100)
        self.battery_spin.setValue(50)
        grid.addWidget(self.battery_spin, 0, 3)

        self.charging_check = QCheckBox("充电中")
        grid.addWidget(self.charging_check, 0, 4)

        btn_batt = QPushButton("发送电池状态")
        btn_batt.clicked.connect(self._send_battery)
        grid.addWidget(btn_batt, 0, 5)

        # WS2812 控制
        grid.addWidget(QLabel("通道:"), 1, 0)
        self.ch_combo = QComboBox()
        self.ch_combo.addItems(["0", "1", "2", "3", "255 (广播)"])
        grid.addWidget(self.ch_combo, 1, 1)

        grid.addWidget(QLabel("模式:"), 1, 2)
        self.mode_combo = QComboBox()
        modes = ["0-关闭", "1-静态", "2-呼吸灯", "3-跑马灯", "4-彩虹",
                 "5-波浪", "6-闪烁", "7-自动循环", "8-电量显示"]
        self.mode_combo.addItems(modes)
        grid.addWidget(self.mode_combo, 1, 3)

        grid.addWidget(QLabel("亮度:"), 1, 4)
        self.bright_spin = QSpinBox()
        self.bright_spin.setRange(0, 255)
        self.bright_spin.setValue(128)
        grid.addWidget(self.bright_spin, 1, 5)

        # 颜色
        grid.addWidget(QLabel("R:"), 2, 0)
        self.r_spin = QSpinBox(); self.r_spin.setRange(0, 255); self.r_spin.setValue(255)
        grid.addWidget(self.r_spin, 2, 1)
        grid.addWidget(QLabel("G:"), 2, 2)
        self.g_spin = QSpinBox(); self.g_spin.setRange(0, 255); self.g_spin.setValue(0)
        grid.addWidget(self.g_spin, 2, 3)
        grid.addWidget(QLabel("B:"), 2, 4)
        self.b_spin = QSpinBox(); self.b_spin.setRange(0, 255); self.b_spin.setValue(0)
        grid.addWidget(self.b_spin, 2, 5)

        btn_ws = QPushButton("发送灯效命令")
        btn_ws.clicked.connect(self._send_ws2812)
        grid.addWidget(btn_ws, 3, 0, 1, 3)

        btn_batt_ch = QPushButton("设置电量显示通道")
        btn_batt_ch.clicked.connect(self._send_battery_channel)
        grid.addWidget(btn_batt_ch, 3, 3, 1, 3)

        layout.addWidget(grp)

        # 自定义 JSON
        grp2 = QGroupBox("自定义 JSON")
        vl = QVBoxLayout(grp2)
        self.json_edit = QTextEdit()
        self.json_edit.setMaximumHeight(120)
        self.json_edit.setPlaceholderText('例: {"channel": 0, "mode": 1, "color": {"r":255,"g":0,"b":0}}')
        vl.addWidget(self.json_edit)
        btn_send = QPushButton("发送自定义 JSON")
        btn_send.clicked.connect(self._send_custom)
        vl.addWidget(btn_send)
        layout.addWidget(grp2)

        layout.addStretch()

    def _get_channel(self):
        txt = self.ch_combo.currentText()
        return 255 if "255" in txt else int(txt)

    @staticmethod
    def _round_floats(obj):
        """递归将所有 float 保留两位小数"""
        if isinstance(obj, float):
            return round(obj, 2)
        if isinstance(obj, dict):
            return {k: JsonTab._round_floats(v) for k, v in obj.items()}
        if isinstance(obj, list):
            return [JsonTab._round_floats(v) for v in obj]
        return obj

    def _send_json(self, obj):
        if not self.serial_mgr.is_open:
            self.log("[错误] 串口未打开")
            return
        obj = self._round_floats(obj)
        text = json.dumps(obj, ensure_ascii=False)
        self.serial_mgr.write(text.encode('utf-8'))
        self.log(f"[TX JSON] {text}")

    def _send_battery(self):
        self._send_json({
            "voltage": self.voltage_spin.value(),
            "battery": self.battery_spin.value(),
            "charging": self.charging_check.isChecked()
        })

    def _send_ws2812(self):
        obj = {
            "channel": self._get_channel(),
            "mode": self.mode_combo.currentIndex(),
            "brightness": self.bright_spin.value(),
            "color": {"r": self.r_spin.value(), "g": self.g_spin.value(), "b": self.b_spin.value()}
        }
        self._send_json(obj)

    def _send_battery_channel(self):
        self._send_json({"battery_channel": self._get_channel()})

    def _send_custom(self):
        text = self.json_edit.toPlainText().strip()
        if not text:
            return
        try:
            obj = json.loads(text)
            self._send_json(obj)
        except json.JSONDecodeError as e:
            self.log(f"[错误] JSON 解析失败: {e}")


class RS485_1_Tab(QWidget):
    """RS485-1 电池模拟器 (DD/77 帧格式)
    
    模拟 BMS: 监听 ESP32 发来的查询帧 (DD A5 03 00 ... 77),
    自动用面板上的参数构造应答帧返回。
    """
    def __init__(self, serial_mgr, log_fn):
        super().__init__()
        self.serial_mgr = serial_mgr
        self.log = log_fn
        self.auto_reply = False
        self._init_ui()

    def _init_ui(self):
        layout = QVBoxLayout(self)

        grp = QGroupBox("RS485-1 电池参数 (模拟 BMS 应答)")
        form = QFormLayout(grp)

        self.voltage_spin = QDoubleSpinBox()
        self.voltage_spin.setRange(0, 100); self.voltage_spin.setValue(24.0); self.voltage_spin.setSingleStep(0.1)
        form.addRow("总电压 (V):", self.voltage_spin)

        self.current_spin = QDoubleSpinBox()
        self.current_spin.setRange(-100, 100); self.current_spin.setValue(0.5); self.current_spin.setSingleStep(0.1)
        form.addRow("电流 (A, 正=充电):", self.current_spin)

        self.remain_spin = QSpinBox()
        self.remain_spin.setRange(0, 65535); self.remain_spin.setValue(800)
        form.addRow("剩余容量 (×10mAh):", self.remain_spin)

        self.full_spin = QSpinBox()
        self.full_spin.setRange(0, 65535); self.full_spin.setValue(1000)
        form.addRow("标称容量 (×10mAh):", self.full_spin)

        self.soc_spin = QSpinBox()
        self.soc_spin.setRange(0, 100); self.soc_spin.setValue(80)
        form.addRow("SOC (%):", self.soc_spin)

        self.strings_spin = QSpinBox()
        self.strings_spin.setRange(1, 32); self.strings_spin.setValue(7)
        form.addRow("电池串数:", self.strings_spin)

        self.temp_spin = QDoubleSpinBox()
        self.temp_spin.setRange(-40, 120); self.temp_spin.setValue(25.0); self.temp_spin.setSingleStep(0.5)
        form.addRow("温度 (℃):", self.temp_spin)

        layout.addWidget(grp)

        hl = QHBoxLayout()
        self.auto_check = QCheckBox("自动应答查询帧")
        self.auto_check.toggled.connect(self._toggle_auto)
        hl.addWidget(self.auto_check)

        btn_send = QPushButton("手动发送一次应答")
        btn_send.clicked.connect(self._send_response)
        hl.addWidget(btn_send)
        layout.addLayout(hl)

        layout.addStretch()

    def _toggle_auto(self, on):
        self.auto_reply = on
        self.log(f"[RS485-1] 自动应答: {'开' if on else '关'}")

    def _build_response(self):
        """构造 0x03 基本信息应答帧"""
        # payload: voltage(2B, ×10mV, big-endian), current(2B, ×10mA signed),
        #          remain_cap(2B), full_cap(2B), cycles(2B), prod_date(2B),
        #          balance_lo(2B), balance_hi(2B), protect(2B), sw_ver(1B),
        #          soc(1B), fet(1B), strings(1B), ntc_count(1B), temps...
        v_raw = int(self.voltage_spin.value() * 100)  # ×10mV
        i_raw = int(self.current_spin.value() * 100) & 0xFFFF
        remain = self.remain_spin.value()
        full = self.full_spin.value()
        soc = self.soc_spin.value()
        strings = self.strings_spin.value()
        temp_raw = int(self.temp_spin.value() * 10 + 2731)

        payload = struct.pack('>HhHH', v_raw, int(self.current_spin.value() * 100), remain, full)
        payload += struct.pack('>HH', 100, 0)  # cycles, prod_date
        payload += struct.pack('>HH', 0, 0)    # balance
        payload += struct.pack('>H', 0)         # protect
        payload += struct.pack('B', 0x10)       # sw_ver
        payload += struct.pack('B', soc)
        payload += struct.pack('B', 0x03)       # fet: charge+discharge
        payload += struct.pack('B', strings)
        payload += struct.pack('B', 1)          # 1 个温度探头
        payload += struct.pack('>H', temp_raw)

        # 帧: DD 03 00 LEN DATA CHECKSUM(2B) 77
        data_len = len(payload)
        # checksum = ~(cmd + len + sum(payload)) + 1 只取低16位
        cs_sum = 0x03 + data_len
        for b in payload:
            cs_sum += b
        checksum = (~cs_sum + 1) & 0xFFFF

        frame = bytes([0xDD, 0x03, 0x00, data_len]) + payload
        frame += struct.pack('>H', checksum)
        frame += bytes([0x77])
        return frame

    def _send_response(self):
        if not self.serial_mgr.is_open:
            self.log("[错误] 串口未打开")
            return
        frame = self._build_response()
        self.serial_mgr.write(frame)
        self.log(f"[TX RS485-1] {frame.hex(' ')}")

    def check_and_reply(self, data: bytes):
        """检查收到的数据是否包含查询帧, 自动应答"""
        if not self.auto_reply:
            return
        # 查询帧: DD A5 03 00 FF FD 77
        if b'\xdd\xa5\x03' in data:
            self.log("[RS485-1] 检测到查询帧, 自动应答")
            self._send_response()


class RS485_2_Tab(QWidget):
    """RS485-2 电池模拟器 (0xAA 帧格式, 小端)"""
    def __init__(self, serial_mgr, log_fn):
        super().__init__()
        self.serial_mgr = serial_mgr
        self.log = log_fn
        self.auto_reply = False
        self._init_ui()

    def _init_ui(self):
        layout = QVBoxLayout(self)

        grp = QGroupBox("RS485-2 电池参数 (模拟 BMS 应答)")
        form = QFormLayout(grp)

        self.voltage_spin = QDoubleSpinBox()
        self.voltage_spin.setRange(0, 100); self.voltage_spin.setValue(24.0); self.voltage_spin.setSingleStep(0.1); self.voltage_spin.setDecimals(3)
        form.addRow("总电压 (V):", self.voltage_spin)

        self.current_spin = QDoubleSpinBox()
        self.current_spin.setRange(-100, 100); self.current_spin.setValue(0.5); self.current_spin.setSingleStep(0.1); self.current_spin.setDecimals(3)
        form.addRow("电流 (A, 正=充电):", self.current_spin)

        self.soc_spin = QSpinBox()
        self.soc_spin.setRange(0, 100); self.soc_spin.setValue(80)
        form.addRow("SOC (%):", self.soc_spin)

        self.soh_spin = QSpinBox()
        self.soh_spin.setRange(0, 100); self.soh_spin.setValue(100)
        form.addRow("SOH (%):", self.soh_spin)

        self.remain_spin = QSpinBox()
        self.remain_spin.setRange(0, 999999); self.remain_spin.setValue(8000)
        form.addRow("剩余容量 (mAh):", self.remain_spin)

        self.full_spin = QSpinBox()
        self.full_spin.setRange(0, 999999); self.full_spin.setValue(10000)
        form.addRow("满充容量 (mAh):", self.full_spin)

        self.cycles_spin = QSpinBox()
        self.cycles_spin.setRange(0, 65535); self.cycles_spin.setValue(50)
        form.addRow("循环次数:", self.cycles_spin)

        self.temp_spin = QDoubleSpinBox()
        self.temp_spin.setRange(-40, 120); self.temp_spin.setValue(25.0)
        form.addRow("电芯温度 (℃, 4路相同):", self.temp_spin)

        self.mos_temp_spin = QSpinBox()
        self.mos_temp_spin.setRange(-40, 120); self.mos_temp_spin.setValue(30)
        form.addRow("MOS温度 (℃):", self.mos_temp_spin)

        self.env_temp_spin = QSpinBox()
        self.env_temp_spin.setRange(-40, 120); self.env_temp_spin.setValue(25)
        form.addRow("环境温度 (℃):", self.env_temp_spin)

        layout.addWidget(grp)

        hl = QHBoxLayout()
        self.auto_check = QCheckBox("自动应答查询帧")
        self.auto_check.toggled.connect(self._toggle_auto)
        hl.addWidget(self.auto_check)

        btn_send = QPushButton("手动发送一次应答")
        btn_send.clicked.connect(self._send_response)
        hl.addWidget(btn_send)

        btn_handshake = QPushButton("发送握手应答")
        btn_handshake.clicked.connect(self._send_handshake_reply)
        hl.addWidget(btn_handshake)
        layout.addLayout(hl)

        layout.addStretch()

    def _toggle_auto(self, on):
        self.auto_reply = on
        self.log(f"[RS485-2] 自动应答: {'开' if on else '关'}")

    def _build_frame(self, cmd, payload: bytes) -> bytes:
        """构造 RS485-2 帧: 0xAA CMD LEN DATA SUM_L SUM_H"""
        length = len(payload)
        header = bytes([0xAA, cmd, length])
        checksum = cmd + length
        for b in payload:
            checksum += b
        checksum &= 0xFFFF
        return header + payload + struct.pack('<H', checksum)

    def _build_info_payload(self) -> bytes:
        """构造 0x21 电池信息 26 字节 payload (小端)"""
        mv = int(self.voltage_spin.value() * 1000)
        ma = int(self.current_spin.value() * 1000)
        soc = self.soc_spin.value()
        soh = self.soh_spin.value()
        remain = self.remain_spin.value()
        full = self.full_spin.value()
        cycles = self.cycles_spin.value()
        temp = int(self.temp_spin.value())
        mos_temp = self.mos_temp_spin.value()
        env_temp = self.env_temp_spin.value()

        # [0..3]voltage(mV) [4..7]current(mA) [8]SOC [9]SOH
        # [10..13]remain [14..17]full [18..19]cycles
        # [20..23]temp1..4 [24]MOS_temp [25]env_temp
        payload = struct.pack('<IiBB', mv, ma, soc, soh)
        payload += struct.pack('<II', remain, full)
        payload += struct.pack('<H', cycles)
        payload += struct.pack('4b', temp, temp, temp, temp)
        payload += struct.pack('b', mos_temp)
        payload += struct.pack('b', env_temp)
        return payload

    def _send_response(self):
        if not self.serial_mgr.is_open:
            self.log("[错误] 串口未打开")
            return
        frame = self._build_frame(0x21, self._build_info_payload())
        self.serial_mgr.write(frame)
        self.log(f"[TX RS485-2] {frame.hex(' ')}")

    def _send_handshake_reply(self):
        if not self.serial_mgr.is_open:
            self.log("[错误] 串口未打开")
            return
        frame = self._build_frame(0x00, b'')
        self.serial_mgr.write(frame)
        self.log(f"[TX RS485-2 握手] {frame.hex(' ')}")

    def check_and_reply(self, data: bytes):
        if not self.auto_reply:
            return
        # 扫描 0xAA 帧头
        for i, b in enumerate(data):
            if b != 0xAA or i + 2 >= len(data):
                continue
            cmd = data[i + 1]
            if cmd == 0x00:
                self.log("[RS485-2] 检测到握手帧, 自动应答")
                self._send_handshake_reply()
            elif cmd == 0x21:
                self.log("[RS485-2] 检测到电池查询帧, 自动应答")
                self._send_response()


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("ESP32S3-TFT-BS 调试工具")
        self.resize(900, 650)
        self.serial_mgr = SerialManager()

        central = QWidget()
        self.setCentralWidget(central)
        main_layout = QVBoxLayout(central)

        # 串口配置
        ser_grp = QGroupBox("串口配置")
        hl = QHBoxLayout(ser_grp)

        hl.addWidget(QLabel("端口:"))
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(200)
        hl.addWidget(self.port_combo)

        btn_refresh = QPushButton("刷新")
        btn_refresh.clicked.connect(self._refresh_ports)
        hl.addWidget(btn_refresh)

        hl.addWidget(QLabel("波特率:"))
        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["9600", "115200"])
        self.baud_combo.setCurrentText("115200")
        hl.addWidget(self.baud_combo)

        self.btn_open = QPushButton("打开")
        self.btn_open.clicked.connect(self._toggle_serial)
        hl.addWidget(self.btn_open)

        self.ser_status = QLabel("● 未连接")
        self.ser_status.setStyleSheet("color: red; font-weight: bold;")
        hl.addWidget(self.ser_status)

        main_layout.addWidget(ser_grp)

        # 功能 Tab
        splitter = QSplitter(Qt.Vertical)

        self.tabs = QTabWidget()
        self.json_tab = JsonTab(self.serial_mgr, self._log)
        self.rs485_1_tab = RS485_1_Tab(self.serial_mgr, self._log)
        self.rs485_2_tab = RS485_2_Tab(self.serial_mgr, self._log)
        self.tabs.addTab(self.json_tab, "JSON 模式 (115200)")
        self.tabs.addTab(self.rs485_1_tab, "RS485-1 模拟BMS (9600)")
        self.tabs.addTab(self.rs485_2_tab, "RS485-2 模拟BMS (9600)")
        self.tabs.currentChanged.connect(self._on_tab_changed)
        splitter.addWidget(self.tabs)

        # 日志
        self.log_edit = QTextEdit()
        self.log_edit.setReadOnly(True)
        self.log_edit.setFont(QFont("Consolas", 9))
        splitter.addWidget(self.log_edit)
        splitter.setSizes([400, 250])

        main_layout.addWidget(splitter)

        # 定时读取串口
        self.rx_timer = QTimer()
        self.rx_timer.timeout.connect(self._poll_rx)
        self.rx_timer.start(50)

        self._refresh_ports()

    def _refresh_ports(self):
        self.port_combo.clear()
        for p in serial.tools.list_ports.comports():
            self.port_combo.addItem(f"{p.device} - {p.description}", p.device)

    def _toggle_serial(self):
        if self.serial_mgr.is_open:
            self.serial_mgr.close()
            self.btn_open.setText("打开")
            self.ser_status.setText("● 未连接")
            self.ser_status.setStyleSheet("color: red; font-weight: bold;")
            self._log("[串口] 已关闭")
        else:
            port = self.port_combo.currentData()
            if not port:
                QMessageBox.warning(self, "错误", "请选择串口")
                return
            baud = int(self.baud_combo.currentText())
            try:
                self.serial_mgr.open(port, baud)
                self.btn_open.setText("关闭")
                self.ser_status.setText(f"● {port} @ {baud}")
                self.ser_status.setStyleSheet("color: green; font-weight: bold;")
                self._log(f"[串口] 已打开 {port} @ {baud}")
            except Exception as e:
                QMessageBox.critical(self, "串口错误", str(e))

    def _on_tab_changed(self, idx):
        """切换 Tab 时自动建议波特率"""
        if idx == 0:
            self.baud_combo.setCurrentText("115200")
        else:
            self.baud_combo.setCurrentText("9600")
        # 如果串口已打开, 自动切换波特率
        if self.serial_mgr.is_open:
            new_baud = int(self.baud_combo.currentText())
            self.serial_mgr.set_baudrate(new_baud)
            self._log(f"[串口] 波特率切换为 {new_baud}")

    def _poll_rx(self):
        data = self.serial_mgr.read_all()
        if not data:
            return
        # 尝试文本显示
        try:
            text = data.decode('utf-8', errors='replace')
            self._log(f"[RX] {text.strip()}")
        except Exception:
            self._log(f"[RX HEX] {data.hex(' ')}")

        # 转发给 RS485 模拟器检查自动应答
        self.rs485_1_tab.check_and_reply(data)
        self.rs485_2_tab.check_and_reply(data)

    def _log(self, msg):
        self.log_edit.append(f"[{time.strftime('%H:%M:%S')}] {msg}")
        sb = self.log_edit.verticalScrollBar()
        sb.setValue(sb.maximum())

    def closeEvent(self, event):
        self.serial_mgr.close()
        super().closeEvent(event)


def main():
    app = QApplication(sys.argv)
    win = MainWindow()
    win.show()
    sys.exit(app.exec_())


if __name__ == '__main__':
    main()
