# pc_client/AcController.py
import sys
import urllib.request
import urllib.error
from PySide6.QtWidgets import QApplication, QMainWindow, QPushButton, QLabel, QVBoxLayout, QHBoxLayout, QGridLayout, QWidget, QLineEdit
from PySide6.QtCore import QThread, Signal, Slot, QEventLoop, Qt

class HttpWorker(QThread):
    status_signal = Signal(str)

    def __init__(self):
        super().__init__()
        self.command_queue = []
        self.running = True

    def run(self):
        while self.running:
            if self.command_queue:
                host, cmd = self.command_queue.pop(0)
                self.send_http_command(host, cmd)
            self.msleep(100) # Sleep 100ms

    def send_http_command(self, host, cmd):
        # Mapped out corresponding to the enums on the ESP device
        cmd_map = {
            "OFF": 0, "ON": 1,
            "19C": 2, "20C": 3, "21C": 4, "22C": 5, "23C": 6, 
            "24C": 7, "25C": 8, "26C": 9, "27C": 10,
            "HOT": 11, "COLD": 12
        }
        
        cmd_idx = cmd_map.get(cmd, -1)
        if cmd_idx == -1:
            self.status_signal.emit(f"🔴 Error: Unknown command '{cmd}'")
            return
            
        # Make sure user input is properly formatted
        if not host.startswith("http://") and not host.startswith("https://"):
            host = "http://" + host

        try:
            url = f"{host}/ir?cmd={cmd_idx}"
            urllib.request.urlopen(url, timeout=3)
            self.status_signal.emit(f"🟢 Success - Last sent: {cmd}")
        except Exception as e:
            self.status_signal.emit(f"🔴 Error sending {cmd}: {str(e)}")

    def send_command(self, host, cmd):
        self.command_queue.append((host, cmd))

    def stop(self):
        self.running = False


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("ZJIOT Midea A/C Controller")
        self.resize(320, 400) # Made slightly taller to fit grid

        # UI Elements
        self.host_input = QLineEdit(self)
        self.host_input.setText("172.30.50.117")
        self.host_input.setPlaceholderText("ESP32 IP or Hostname")
        self.host_input.setToolTip("Use the local mDNS name or the IP address directly.")
        
        self.status_label = QLabel("Ready to Send Commands", self)
        
        # Power Buttons
        self.btn_off = QPushButton("Turn OFF", self)
        self.btn_on  = QPushButton("Turn ON", self)
        
        # Temp Buttons
        self.btn_19  = QPushButton("19°C", self)
        self.btn_20  = QPushButton("20°C", self)
        self.btn_21  = QPushButton("21°C", self)
        self.btn_22  = QPushButton("22°C", self)
        self.btn_23  = QPushButton("23°C", self)
        self.btn_24  = QPushButton("24°C", self)
        self.btn_25  = QPushButton("25°C", self)
        self.btn_26  = QPushButton("26°C", self)
        self.btn_27  = QPushButton("27°C", self)
        
        # Mode Buttons
        self.btn_hot = QPushButton("HOT Mode", self)
        self.btn_cold = QPushButton("COLD Mode", self)

        # Style layout sizes slightly
        self.btn_off.setStyleSheet("background-color: #ef4444; color: white; font-weight: bold;")
        self.btn_on.setStyleSheet("background-color: #10b981; color: white; font-weight: bold;")
        self.btn_hot.setStyleSheet("background-color: #f87171; color: white;")
        self.btn_cold.setStyleSheet("background-color: #38bdf8; color: white;")

        # Sub Layouts
        power_layout = QHBoxLayout()
        power_layout.addWidget(self.btn_off)
        power_layout.addWidget(self.btn_on)
        
        temp_layout = QGridLayout()
        temp_layout.addWidget(self.btn_19, 0, 0)
        temp_layout.addWidget(self.btn_20, 0, 1)
        temp_layout.addWidget(self.btn_21, 0, 2)
        temp_layout.addWidget(self.btn_22, 1, 0)
        temp_layout.addWidget(self.btn_23, 1, 1)
        temp_layout.addWidget(self.btn_24, 1, 2)
        temp_layout.addWidget(self.btn_25, 2, 0)
        temp_layout.addWidget(self.btn_26, 2, 1)
        temp_layout.addWidget(self.btn_27, 2, 2)
        
        mode_layout = QHBoxLayout()
        mode_layout.addWidget(self.btn_hot)
        mode_layout.addWidget(self.btn_cold)

        # Master Layout
        layout = QVBoxLayout()
        layout.addWidget(QLabel("Device Address:"))
        layout.addWidget(self.host_input)
        layout.addWidget(self.status_label)
        
        layout.addWidget(QLabel("Power Controls:", self))
        layout.addLayout(power_layout)
        
        layout.addWidget(QLabel("Temperature Selection:", self))
        layout.addLayout(temp_layout)
        
        layout.addWidget(QLabel("Mode Options:", self))
        layout.addLayout(mode_layout)

        container = QWidget()
        container.setLayout(layout)
        self.setCentralWidget(container)

        # Connect Signals - Triggers background HTTP requests securely
        self.btn_off.clicked.connect(lambda: self.http_worker.send_command(self.host_input.text(), "OFF"))
        self.btn_on.clicked.connect(lambda:  self.http_worker.send_command(self.host_input.text(), "ON"))
        
        self.btn_19.clicked.connect(lambda:  self.http_worker.send_command(self.host_input.text(), "19C"))
        self.btn_20.clicked.connect(lambda:  self.http_worker.send_command(self.host_input.text(), "20C"))
        self.btn_21.clicked.connect(lambda:  self.http_worker.send_command(self.host_input.text(), "21C"))
        self.btn_22.clicked.connect(lambda:  self.http_worker.send_command(self.host_input.text(), "22C"))
        self.btn_23.clicked.connect(lambda:  self.http_worker.send_command(self.host_input.text(), "23C"))
        self.btn_24.clicked.connect(lambda:  self.http_worker.send_command(self.host_input.text(), "24C"))
        self.btn_25.clicked.connect(lambda:  self.http_worker.send_command(self.host_input.text(), "25C"))
        self.btn_26.clicked.connect(lambda:  self.http_worker.send_command(self.host_input.text(), "26C"))
        self.btn_27.clicked.connect(lambda:  self.http_worker.send_command(self.host_input.text(), "27C"))
        
        self.btn_hot.clicked.connect(lambda:  self.http_worker.send_command(self.host_input.text(), "HOT"))
        self.btn_cold.clicked.connect(lambda:  self.http_worker.send_command(self.host_input.text(), "COLD"))

        # Start HTTP Background Thread
        self.http_worker = HttpWorker()
        self.http_worker.status_signal.connect(self.status_label.setText)
        self.http_worker.start()

    def closeEvent(self, event):
        self.http_worker.stop()
        self.http_worker.wait()
        event.accept()


def main():
    # Detect if a QApplication instance already exists
    app = QApplication.instance()
    is_standalone = False
    
    if not app:
        app = QApplication(sys.argv)
        is_standalone = True

    window = MainWindow()
    
    if is_standalone:
        window.show()
        sys.exit(app.exec())
    else:
        # If run from inside Comb.py, block the calling thread using a local event loop
        # so that the window isn't instantly garbage-collected when main() returns.
        window.setAttribute(Qt.WA_DeleteOnClose)
        loop = QEventLoop()
        window.destroyed.connect(loop.quit)
        window.show()
        loop.exec()

if __name__ == "__main__":
    main()