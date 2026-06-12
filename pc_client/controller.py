import sys
import asyncio
from PySide6.QtWidgets import QApplication, QMainWindow, QPushButton, QLabel, QVBoxLayout, QWidget
from PySide6.QtCore import QThread, Signal, Slot
from bleak import BleakClient, BleakScanner

DEVICE_NAME = "ESP32_AC_CTRL"
CHAR_UUID = "12345678-1234-5678-1234-56789abcdef1"

class BleWorker(QThread):
    connected_signal = Signal(bool)
    status_signal = Signal(str)

    def __init__(self):
        super().__init__()
        self.command_queue = asyncio.Queue()
        self.loop = None

    def run(self):
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self.loop.run_until_complete(self.ble_task())

    async def ble_task(self):
        self.status_signal.emit("Scanning for ESP32...")
        target_device = None
        
        while not target_device:
            devices = await BleakScanner.discover()
            for d in devices:
                if d.name == DEVICE_NAME:
                    target_device = d
                    break
            if not target_device:
                await asyncio.sleep(2)

        self.status_signal.emit(f"Connecting to {DEVICE_NAME}...")
        
        try:
            async with BleakClient(target_device) as client:
                self.connected_signal.emit(True)
                self.status_signal.emit("🟢 Connected to ESP32-S3")

                # Map text commands to the uint8 indexes expected by ESP32 C++
                cmd_map = {"OFF": 0, "ON": 1, "19C": 2, "24C": 3}

                while True:
                    cmd = await self.command_queue.get()
                    if cmd is None:
                        break # Exit
                    
                    val = bytes([cmd_map[cmd]])
                    await client.write_gatt_char(CHAR_UUID, val, response=False)
                    self.status_signal.emit(f"🟢 Connected - Last sent: {cmd}")
                    
        except Exception as e:
            self.status_signal.emit(f"🔴 Error: {str(e)}")
        finally:
            self.connected_signal.emit(False)

    def send_command(self, cmd):
        if self.loop:
            self.loop.call_soon_threadsafe(self.command_queue.put_nowait, cmd)

    def stop(self):
        if self.loop:
            self.loop.call_soon_threadsafe(self.command_queue.put_nowait, None)

class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("ZJIOT Midea A/C Controller")
        self.resize(300, 250)

        # UI Elements
        self.status_label = QLabel("🔴 Disconnected", self)
        self.btn_off = QPushButton("Turn OFF A/C", self)
        self.btn_on  = QPushButton("Turn ON A/C", self)
        self.btn_19  = QPushButton("Set 19°C", self)
        self.btn_24  = QPushButton("Set 24°C", self)

        # Disable buttons until connected
        for btn in [self.btn_off, self.btn_on, self.btn_19, self.btn_24]:
            btn.setEnabled(False)

        # Layout
        layout = QVBoxLayout()
        layout.addWidget(self.status_label)
        layout.addWidget(self.btn_on)
        layout.addWidget(self.btn_off)
        layout.addWidget(self.btn_19)
        layout.addWidget(self.btn_24)

        container = QWidget()
        container.setLayout(layout)
        self.setCentralWidget(container)

        # Connect Signals
        self.btn_off.clicked.connect(lambda: self.ble_worker.send_command("OFF"))
        self.btn_on.clicked.connect(lambda:  self.ble_worker.send_command("ON"))
        self.btn_19.clicked.connect(lambda:  self.ble_worker.send_command("19C"))
        self.btn_24.clicked.connect(lambda:  self.ble_worker.send_command("24C"))

        # Start BLE Background Thread
        self.ble_worker = BleWorker()
        self.ble_worker.connected_signal.connect(self.update_ui_state)
        self.ble_worker.status_signal.connect(self.status_label.setText)
        self.ble_worker.start()

    @Slot(bool)
    def update_ui_state(self, is_connected):
        for btn in [self.btn_off, self.btn_on, self.btn_19, self.btn_24]:
            btn.setEnabled(is_connected)

    def closeEvent(self, event):
        self.ble_worker.stop()
        self.ble_worker.wait()
        event.accept()

if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())