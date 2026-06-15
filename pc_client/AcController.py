# AcController.py
import sys
import urllib.request
import urllib.error
from PySide6.QtWidgets import QApplication, QMainWindow, QPushButton, QLabel, QVBoxLayout, QWidget, QLineEdit
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
        cmd_map = {"OFF": 0, "ON": 1, "19C": 2, "24C": 3}
        cmd_idx = cmd_map[cmd]
        
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
        self.resize(300, 270)

        # UI Elements
        self.host_input = QLineEdit(self)
        self.host_input.setText("http://esp32-ac-ctrl.local")
        self.host_input.setPlaceholderText("ESP32 IP or Hostname")
        self.host_input.setToolTip("Use the local mDNS name or the IP address directly.")
        
        self.status_label = QLabel("Ready to Send Commands", self)
        
        self.btn_off = QPushButton("Turn OFF A/C", self)
        self.btn_on  = QPushButton("Turn ON A/C", self)
        self.btn_19  = QPushButton("Set 19°C", self)
        self.btn_24  = QPushButton("Set 24°C", self)

        # Layout
        layout = QVBoxLayout()
        layout.addWidget(QLabel("Device Address:"))
        layout.addWidget(self.host_input)
        layout.addWidget(self.status_label)
        layout.addWidget(self.btn_on)
        layout.addWidget(self.btn_off)
        layout.addWidget(self.btn_19)
        layout.addWidget(self.btn_24)

        container = QWidget()
        container.setLayout(layout)
        self.setCentralWidget(container)

        # Connect Signals
        self.btn_off.clicked.connect(lambda: self.http_worker.send_command(self.host_input.text(), "OFF"))
        self.btn_on.clicked.connect(lambda:  self.http_worker.send_command(self.host_input.text(), "ON"))
        self.btn_19.clicked.connect(lambda:  self.http_worker.send_command(self.host_input.text(), "19C"))
        self.btn_24.clicked.connect(lambda:  self.http_worker.send_command(self.host_input.text(), "24C"))

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