# 🛸 IoT based AI-Driven Drone Waste Detection & Geospatial Mapping System


---

## 📌 Overview

An IoT-based system that uses an **ESP32-S3 CAM** module mounted on a drone to detect and classify waste (clean vs garbage) in real time using a deep learning model. The system streams live video to a local Python server, performs AI-based classification on each frame, and logs GPS coordinates to geospatially map detected waste locations.

---

## 🎯 Objectives

- Detect garbage in real-time using a drone-mounted camera
- Classify scenes as **clean** or **garbage** using a trained AI model
- Record GPS coordinates at the time of detection
- Display waste locations on an interactive map with green (clean) and red (garbage) markers
- Provide a live monitoring dashboard accessible via browser

---

## 🏗️ System Architecture

```
ESP32-S3 CAM (AP Mode)
        │
        │  WiFi (192.168.4.1)
        │
Python Flask Server (Laptop)
        │
        ├── AI Model (ResNet50 — TensorFlow)
        ├── GPS Polling (/gps endpoint)
        ├── Live Stream (/stream endpoint)
        └── Web Dashboard (localhost:5000)
```

---

## 🔧 Hardware Components

| Component | Description |
|-----------|-------------|
| GOOUUU ESP32-S3-CAM | Main microcontroller with OV3660 camera |
| NEO-6M GPS Module | U-blox GPS with external antenna |
| Jumper Wires | For GPIO connections |

---

## 💻 Software & Technologies

| Technology | Purpose |
|------------|---------|
| Python 3.11 | Backend server |
| TensorFlow / Keras | AI model training and inference |
| ResNet50 | Transfer learning for image classification |
| Flask | Web server and REST API |
| OpenCV | Image processing |
| Arduino IDE | ESP32 firmware development |
| TinyGPS++ | GPS NMEA parsing on ESP32 |
| Leaflet.js | Interactive map UI |

---

## 🤖 AI Model

- **Architecture:** ResNet50 (Transfer Learning)
- **Dataset:** Clean/Dirty Road Classification Dataset (Kaggle)
- **Classes:** `clean` / `garbage`
- **Training:** Google Colab (GPU)
- **Accuracy:** ~80% validation accuracy

---

## 📡 ESP32 Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/stream` | GET | MJPEG live video stream |
| `/gps` | GET | Current GPS coordinates (JSON) |

---

## 🚀 How It Works

1. ESP32-S3 starts as a WiFi Access Point (`ESP32_CAM`)
2. Laptop connects to ESP32 WiFi network
3. Python server reads the live MJPEG stream
4. Every 10 frames, the AI model classifies the scene
5. GPS coordinates are polled every 5 seconds
6. Results are displayed on the web dashboard at `localhost:5000`
7. Detected locations are plotted on an interactive map

---

## 📂 Project Structure

IoT based AI‑Driven Drone Waste Detection & Geospatial Mapping System/
├── IoT based AI‑Driven Drone Waste Detection & Geospatial Mapping System_ESP32_Code/
│   ├── IoT_Drone_Project.ino      # Main ESP32 firmware
│   ├── TinyGPSPlus.h              # GPS library header
│   └── TinyGPSPlus.cpp            # GPS library source
├── IoT based AI‑Driven Drone Waste Detection & Geospatial Mapping System_Python_Model/
│   └── server.py                  # Flask server + AI inference
├── .gitignore
└── README.md
```

---

## ⚙️ Setup & Installation

### ESP32
1. Open `IoT_Drone_Project.ino` in Arduino IDE
2. Select board: `ESP32S3 Dev Module`
3. Upload via TTL port
4. Switch to OTG port for Serial Monitor

### Python Server
```bash
# Create virtual environment
py -3.11 -m venv venv311
venv311\Scripts\activate

# Install dependencies
pip install flask tensorflow opencv-python numpy requests

# Run server
python server.py
```

### Connect
1. Connect laptop WiFi to `ESP32_CAM` (password: `12345678`)
2. Open browser at `http://localhost:5000`

---

## 👥 Team Members

| Name |
|------|
| Talha Shahzad |
| Muhammad Yousuf |
| Shoaib Akhter |
| Mubashir Ishaq |

---

## 🎓 Academic Context

6th Semester IoT Project — undergraduate level
Ghazi University, Dera Ghazi Khan.

---

## 📄 License

This project is for academic purposes only.
