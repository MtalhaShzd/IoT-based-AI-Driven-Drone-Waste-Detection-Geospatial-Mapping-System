import cv2
import numpy as np
import tensorflow as tf
import requests
import threading
import time
import json
import csv
import os
from datetime import datetime
from flask import Flask, jsonify, render_template_string, Response

app = Flask(__name__)

# ============================================
# Load Model
# ============================================
print("[MODEL] Loading model.keras ...")
try:
    model = tf.keras.models.load_model("model.keras")
    print("[MODEL] Loaded OK ✓")
except Exception as e:
    print(f"[MODEL] FAILED: {e}")
    exit(1)

classes = ['clean', 'garbage']

# ============================================
# Shared State (thread-safe)
# ============================================
current_result  = {"result": "waiting", "confidence": 0}
current_gps     = {"lat": 0.0, "lng": 0.0, "satellites": 0, "fix": False}
current_frame   = None
frame_lock      = threading.Lock()
result_lock     = threading.Lock()
gps_lock        = threading.Lock()

# Garbage detection history
garbage_log     = []          # list of dicts
garbage_lock    = threading.Lock()

# Stats
stats = {
    "frames_received" : 0,
    "frames_analyzed" : 0,
    "garbage_count"   : 0,
    "clean_count"     : 0,
    "stream_errors"   : 0,
    "gps_errors"      : 0,
    "start_time"      : datetime.now().strftime("%H:%M:%S")
}

ESP32_IP = "192.168.4.1"

# ============================================
# CSV Logger
# ============================================
LOG_FILE = "garbage_detections.csv"

def init_csv():
    if not os.path.exists(LOG_FILE):
        with open(LOG_FILE, 'w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow([
                "timestamp", "result", "confidence",
                "lat", "lng", "satellites", "maps_url"
            ])
        print(f"[LOG] Created {LOG_FILE}")

def log_detection(result, confidence, gps):
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    lat  = gps.get("lat", 0.0)
    lng  = gps.get("lng", 0.0)
    sats = gps.get("satellites", 0)
    maps_url = (
        f"https://maps.google.com/?q={lat},{lng}"
        if lat != 0 and lng != 0 else "no-fix"
    )

    row = {
        "timestamp"  : timestamp,
        "result"     : result,
        "confidence" : confidence,
        "lat"        : lat,
        "lng"        : lng,
        "satellites" : sats,
        "maps_url"   : maps_url
    }

    # Only log garbage detections to history
    if result == "garbage":
        with garbage_lock:
            garbage_log.append(row)
            # Keep last 100 entries in memory
            if len(garbage_log) > 100:
                garbage_log.pop(0)

        # Write to CSV
        try:
            with open(LOG_FILE, 'a', newline='') as f:
                writer = csv.writer(f)
                writer.writerow([
                    timestamp, result, confidence,
                    lat, lng, sats, maps_url
                ])
            print(f"[LOG] Garbage logged → {lat:.6f}, {lng:.6f}")
        except Exception as e:
            print(f"[LOG] Write error: {e}")

# ============================================
# AI Prediction (runs in separate thread pool)
# ============================================
predict_lock   = threading.Lock()
is_predicting  = False

def run_prediction(jpg_bytes):
    global current_result, is_predicting

    with predict_lock:
        if is_predicting:
            return           # skip if already predicting
        is_predicting = True

    try:
        np_arr = np.frombuffer(jpg_bytes, np.uint8)
        frame  = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)

        if frame is None:
            return

        # Preprocess
        img = cv2.resize(frame, (224, 224))
        img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
        img = img.astype(np.float32) / 255.0
        img = np.expand_dims(img, axis=0)

        # Predict
        prediction = model.predict(img, verbose=0)
        result     = classes[int(np.argmax(prediction))]
        confidence = round(float(np.max(prediction)) * 100, 2)

        with result_lock:
            current_result = {
                "result"     : result,
                "confidence" : confidence,
                "timestamp"  : datetime.now().strftime("%H:%M:%S")
            }

        # Update stats
        if result == "garbage":
            stats["garbage_count"] += 1
        else:
            stats["clean_count"] += 1
        stats["frames_analyzed"] += 1

        # Get current GPS snapshot
        with gps_lock:
            gps_snap = current_gps.copy()

        # Log every detection
        log_detection(result, confidence, gps_snap)

        label = "🗑️  GARBAGE" if result == "garbage" else "✅ CLEAN"
        print(f"[AI] {label}  {confidence:.1f}%  "
              f"| GPS: {gps_snap['lat']:.5f}, "
              f"{gps_snap['lng']:.5f} "
              f"({gps_snap['satellites']} sats)")

    except Exception as e:
        print(f"[AI] Prediction error: {e}")

    finally:
        with predict_lock:
            is_predicting = False

# ============================================
# MJPEG Stream Reader Thread
# ============================================
def read_stream():
    global current_frame
    stream_url = f"http://{ESP32_IP}/stream"
    print(f"[STREAM] Connecting to {stream_url}")

    ANALYZE_EVERY = 5    # analyze every N frames (lower = more CPU)

    while True:
        try:
            resp       = requests.get(
                stream_url,
                stream=True,
                timeout=20,
                headers={"Connection": "keep-alive"}
            )
            bytes_data = b""
            frame_count = 0

            print("[STREAM] Connected ✓")

            for chunk in resp.iter_content(chunk_size=8192):
                bytes_data += chunk

                # Extract all complete JPEG frames from buffer
                while True:
                    start = bytes_data.find(b'\xff\xd8')
                    end   = bytes_data.find(b'\xff\xd9')

                    if start == -1 or end == -1 or end <= start:
                        break

                    jpg        = bytes_data[start : end + 2]
                    bytes_data = bytes_data[end + 2:]
                    frame_count += 1
                    stats["frames_received"] += 1

                    # Store latest frame for browser
                    with frame_lock:
                        current_frame = jpg

                    # Run AI every N frames in background thread
                    if frame_count % ANALYZE_EVERY == 0:
                        t = threading.Thread(
                            target=run_prediction,
                            args=(jpg,),
                            daemon=True
                        )
                        t.start()

                    # Trim buffer if too large
                    if len(bytes_data) > 500000:
                        bytes_data = b""
                        print("[STREAM] Buffer trimmed")

        except requests.exceptions.ConnectionError:
            stats["stream_errors"] += 1
            print(f"[STREAM] Connection failed "
                  f"(attempt {stats['stream_errors']}) "
                  f"— retry in 3s...")
            time.sleep(3)

        except requests.exceptions.Timeout:
            stats["stream_errors"] += 1
            print("[STREAM] Timeout — retrying...")
            time.sleep(3)

        except Exception as e:
            stats["stream_errors"] += 1
            print(f"[STREAM] Error: {e} — retry in 3s...")
            time.sleep(3)

# ============================================
# GPS Polling Thread
# ============================================
def read_gps():
    global current_gps
    gps_url = f"http://{ESP32_IP}/gps"
    print(f"[GPS] Polling {gps_url}")

    GPS_INTERVAL = 3   # seconds between polls

    while True:
        try:
            resp = requests.get(gps_url, timeout=5)
            data = resp.json()

            with gps_lock:
                current_gps = {
                    "lat"        : float(data.get("lat", 0.0)),
                    "lng"        : float(data.get("lng", 0.0)),
                    "satellites" : int(data.get("satellites", 0)),
                    "fix"        : bool(data.get("fix", False))
                }

            fix_icon = "✓" if current_gps["fix"] else "✗"
            print(f"[GPS] {fix_icon} "
                  f"{current_gps['lat']:.6f}, "
                  f"{current_gps['lng']:.6f} "
                  f"| Sats: {current_gps['satellites']}")

        except requests.exceptions.ConnectionError:
            stats["gps_errors"] += 1
            print("[GPS] Connection failed")

        except Exception as e:
            stats["gps_errors"] += 1
            print(f"[GPS] Error: {e}")

        time.sleep(GPS_INTERVAL)

# ============================================
# Flask — Video Feed
# ============================================
def generate_frames():
    while True:
        with frame_lock:
            frame = current_frame

        if frame is not None:
            yield (
                b'--frame\r\n'
                b'Content-Type: image/jpeg\r\n\r\n'
                + frame +
                b'\r\n'
            )
        time.sleep(0.033)   # ~30 fps cap

@app.route('/video_feed')
def video_feed():
    return Response(
        generate_frames(),
        mimetype='multipart/x-mixed-replace; boundary=frame'
    )

# ============================================
# Flask — API Endpoints
# ============================================
@app.route('/result')
def get_result():
    with result_lock:
        return jsonify(current_result)

@app.route('/gps')
def get_gps():
    with gps_lock:
        return jsonify(current_gps)

@app.route('/history')
def get_history():
    with garbage_lock:
        return jsonify(garbage_log[-20:])   # last 20

@app.route('/stats')
def get_stats():
    uptime_s = int(time.time()) % 86400
    return jsonify({
        **stats,
        "garbage_log_count" : len(garbage_log),
        "gps_fix"           : current_gps.get("fix", False)
    })

# ============================================
# Flask — Main Web UI
# ============================================
@app.route('/')
def index():
    html = '''
<!DOCTYPE html>
<html>
<head>
<title>Drone Waste Detection</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }

body {
    font-family: Arial, sans-serif;
    background: #0d0d1a;
    color: #eee;
    padding: 16px;
}

h1 {
    text-align: center;
    font-size: 20px;
    color: #00d4ff;
    margin-bottom: 4px;
    line-height: 1.4;
}
.subtitle {
    text-align: center;
    font-size: 13px;
    color: #666;
    margin-bottom: 16px;
}

/* Grid layout */
.grid {
    display: grid;
    grid-template-columns: 1fr 340px;
    gap: 14px;
    max-width: 1100px;
    margin: 0 auto;
}
@media (max-width: 800px) {
    .grid { grid-template-columns: 1fr; }
}

.card {
    background: #16213e;
    border-radius: 12px;
    padding: 14px;
    border: 1px solid #1a2a4a;
}

/* Stream */
#stream-img {
    width: 100%;
    border-radius: 8px;
    border: 2px solid #1a2a4a;
    display: block;
}

/* Result box */
#result-box {
    border-radius: 12px;
    padding: 20px;
    text-align: center;
    transition: background 0.4s;
    margin-bottom: 12px;
}
.clean   { background: #0a3d0a; border: 2px solid #2ecc71; }
.garbage { background: #3d0a0a; border: 2px solid #e74c3c; }
.waiting { background: #1a1a2e; border: 2px solid #444;    }

#result-label {
    font-size: 42px;
    font-weight: bold;
    letter-spacing: 2px;
}
#confidence-label {
    font-size: 16px;
    color: #aaa;
    margin-top: 6px;
}
#timestamp-label {
    font-size: 11px;
    color: #555;
    margin-top: 4px;
}

/* GPS */
.gps-card { margin-bottom: 12px; }
.gps-row  {
    display: flex;
    justify-content: space-between;
    font-size: 13px;
    padding: 4px 0;
    border-bottom: 1px solid #1a2a4a;
}
.gps-row:last-child { border-bottom: none; }
.gps-val  { color: #00d4ff; font-weight: bold; }
#maps-btn {
    display: block;
    width: 100%;
    margin-top: 10px;
    padding: 8px;
    background: #0f3460;
    color: #fff;
    border: 1px solid #00d4ff;
    border-radius: 8px;
    cursor: pointer;
    font-size: 13px;
    text-decoration: none;
    text-align: center;
}
#maps-btn:hover { background: #00d4ff; color: #000; }

/* Stats */
.stat-grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 8px;
    margin-top: 8px;
}
.stat-item {
    background: #0d0d1a;
    border-radius: 8px;
    padding: 8px;
    text-align: center;
}
.stat-num  { font-size: 22px; font-weight: bold; color: #00d4ff; }
.stat-lbl  { font-size: 10px; color: #666; margin-top: 2px; }

/* History */
#history-list {
    max-height: 180px;
    overflow-y: auto;
    margin-top: 8px;
}
.h-item {
    font-size: 11px;
    padding: 5px 0;
    border-bottom: 1px solid #1a2a4a;
    color: #aaa;
}
.h-item a { color: #e74c3c; text-decoration: none; }
.h-item a:hover { text-decoration: underline; }

/* Indicators */
.dot {
    display: inline-block;
    width: 9px; height: 9px;
    border-radius: 50%;
    margin-right: 5px;
}
.dot-green  { background: #2ecc71; }
.dot-red    { background: #e74c3c; }
.dot-yellow { background: #f39c12; animation: blink 1s infinite; }
@keyframes blink {
    0%,100% { opacity:1; } 50% { opacity:0.2; }
}

.section-title {
    font-size: 12px;
    color: #555;
    text-transform: uppercase;
    letter-spacing: 1px;
    margin-bottom: 8px;
}
</style>
</head>
<body>

<h1>&#128247; IoT AI Drone Waste Detection</h1>
<p class="subtitle">Geospatial Mapping System — ESP32-S3-CAM + NEO-6M GPS</p>

<div class="grid">

    <!-- LEFT: Camera Stream -->
    <div>
        <div class="card">
            <div class="section-title">
                <span class="dot dot-yellow"></span>Live Camera Feed
            </div>
            <img id="stream-img"
                 src="/video_feed"
                 alt="Stream loading...">
        </div>
    </div>

    <!-- RIGHT: Panel -->
    <div>

        <!-- Detection Result -->
        <div id="result-box" class="waiting">
            <div id="result-label">CONNECTING</div>
            <div id="confidence-label">Starting up...</div>
            <div id="timestamp-label"></div>
        </div>

        <!-- GPS -->
        <div class="card gps-card">
            <div class="section-title">
                <span id="gps-dot" class="dot dot-red"></span>GPS Location
            </div>
            <div class="gps-row">
                <span>Latitude</span>
                <span class="gps-val" id="g-lat">—</span>
            </div>
            <div class="gps-row">
                <span>Longitude</span>
                <span class="gps-val" id="g-lng">—</span>
            </div>
            <div class="gps-row">
                <span>Satellites</span>
                <span class="gps-val" id="g-sats">—</span>
            </div>
            <div class="gps-row">
                <span>Fix Status</span>
                <span class="gps-val" id="g-fix">Waiting...</span>
            </div>
            <a id="maps-btn" href="#" target="_blank">
                &#128205; Open in Google Maps
            </a>
        </div>

        <!-- Stats -->
        <div class="card" style="margin-bottom:12px">
            <div class="section-title">Session Statistics</div>
            <div class="stat-grid">
                <div class="stat-item">
                    <div class="stat-num" id="s-frames">0</div>
                    <div class="stat-lbl">Frames</div>
                </div>
                <div class="stat-item">
                    <div class="stat-num" id="s-analyzed">0</div>
                    <div class="stat-lbl">Analyzed</div>
                </div>
                <div class="stat-item"
                     style="border:1px solid #3d0a0a">
                    <div class="stat-num"
                         id="s-garbage"
                         style="color:#e74c3c">0</div>
                    <div class="stat-lbl">Garbage Hits</div>
                </div>
                <div class="stat-item"
                     style="border:1px solid #0a3d0a">
                    <div class="stat-num"
                         id="s-clean"
                         style="color:#2ecc71">0</div>
                    <div class="stat-lbl">Clean Hits</div>
                </div>
            </div>
        </div>

        <!-- Garbage History -->
        <div class="card">
            <div class="section-title">
                &#128204; Recent Garbage Detections
            </div>
            <div id="history-list">
                <div style="color:#444;font-size:12px;
                            text-align:center;padding:10px">
                    No detections yet
                </div>
            </div>
        </div>

    </div><!-- end right panel -->
</div><!-- end grid -->

<script>
// ── Update detection result ──
function updateResult() {
    fetch('/result')
    .then(r => r.json())
    .then(d => {
        const box  = document.getElementById('result-box');
        const lbl  = document.getElementById('result-label');
        const conf = document.getElementById('confidence-label');
        const ts   = document.getElementById('timestamp-label');

        if (d.result === 'waiting') {
            box.className = 'waiting';
            lbl.innerText = 'CONNECTING';
            conf.innerText = 'Waiting for stream...';
            return;
        }

        box.className  = d.result;
        lbl.innerText  = d.result === 'garbage'
                         ? '🗑️ GARBAGE' : '✅ CLEAN';
        conf.innerText = 'Confidence: ' + d.confidence + '%';
        ts.innerText   = d.timestamp || '';
    })
    .catch(() => {});
}

// ── Update GPS ──
function updateGPS() {
    fetch('/gps')
    .then(r => r.json())
    .then(d => {
        const hasFix = d.fix && d.lat !== 0 && d.lng !== 0;
        const dot    = document.getElementById('gps-dot');

        document.getElementById('g-lat').innerText =
            hasFix ? d.lat.toFixed(6) + '°' : '—';
        document.getElementById('g-lng').innerText =
            hasFix ? d.lng.toFixed(6) + '°' : '—';
        document.getElementById('g-sats').innerText =
            d.satellites || '0';
        document.getElementById('g-fix').innerText =
            hasFix ? 'Fixed ✓' : 'Searching...';

        dot.className = hasFix
            ? 'dot dot-green' : 'dot dot-yellow';

        const btn = document.getElementById('maps-btn');
        if (hasFix) {
            btn.href = 'https://maps.google.com/?q='
                       + d.lat + ',' + d.lng;
            btn.style.opacity = '1';
        } else {
            btn.href = '#';
            btn.style.opacity = '0.4';
        }
    })
    .catch(() => {});
}

// ── Update stats ──
function updateStats() {
    fetch('/stats')
    .then(r => r.json())
    .then(d => {
        document.getElementById('s-frames').innerText =
            d.frames_received || 0;
        document.getElementById('s-analyzed').innerText =
            d.frames_analyzed || 0;
        document.getElementById('s-garbage').innerText =
            d.garbage_count || 0;
        document.getElementById('s-clean').innerText =
            d.clean_count || 0;
    })
    .catch(() => {});
}

// ── Update garbage history ──
function updateHistory() {
    fetch('/history')
    .then(r => r.json())
    .then(items => {
        const div = document.getElementById('history-list');
        if (!items || items.length === 0) {
            div.innerHTML =
                '<div style="color:#444;font-size:12px;'
                + 'text-align:center;padding:10px">'
                + 'No detections yet</div>';
            return;
        }
        // Show newest first
        const sorted = items.slice().reverse();
        div.innerHTML = sorted.map(item => {
            const hasGPS = item.lat !== 0 && item.lng !== 0;
            const loc = hasGPS
                ? '<a href="' + item.maps_url
                  + '" target="_blank">&#128205; '
                  + item.lat.toFixed(5) + ','
                  + item.lng.toFixed(5) + '</a>'
                : 'no GPS fix';
            return '<div class="h-item">'
                + item.timestamp + ' &nbsp;'
                + item.confidence + '% &nbsp;'
                + loc
                + '</div>';
        }).join('');
    })
    .catch(() => {});
}

// ── Poll intervals ──
setInterval(updateResult,  1000);   // result: every 1s
setInterval(updateGPS,     3000);   // GPS:    every 3s
setInterval(updateStats,   2000);   // stats:  every 2s
setInterval(updateHistory, 5000);   // history: every 5s

// Initial calls
updateResult();
updateGPS();
updateStats();
updateHistory();
</script>
</body>
</html>
'''
    return html

# ============================================
# MAIN
# ============================================
if __name__ == '__main__':
    init_csv()

    print("=" * 45)
    print("  Drone Waste Detection System Starting")
    print("=" * 45)
    print(f"  ESP32 IP  : {ESP32_IP}")
    print(f"  Stream    : http://{ESP32_IP}/stream")
    print(f"  GPS       : http://{ESP32_IP}/gps")
    print(f"  Dashboard : http://localhost:5000")
    print(f"  CSV Log   : {LOG_FILE}")
    print("=" * 45)

    # Start background threads
    threading.Thread(
        target=read_stream, daemon=True).start()
    threading.Thread(
        target=read_gps,    daemon=True).start()

    # Small delay so threads connect before browser hits
    time.sleep(1)

    app.run(
        host='0.0.0.0',
        port=5000,
        debug=False,
        threaded=True
    )