#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include <HardwareSerial.h>
#include "TinyGPSPlus.h"

// ============================================
// WiFi - AP Mode (no router needed)
// ============================================
const char* ssid     = "ESP32_CAM_GPS";
const char* password = "12345678";

// ============================================
// Board Pin Definitions
// ============================================
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    15
#define SIOD_GPIO_NUM     4
#define SIOC_GPIO_NUM     5
#define Y9_GPIO_NUM      16
#define Y8_GPIO_NUM      17
#define Y7_GPIO_NUM      18
#define Y6_GPIO_NUM      12
#define Y5_GPIO_NUM      10
#define Y4_GPIO_NUM       8
#define Y3_GPIO_NUM       9
#define Y2_GPIO_NUM      11
#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM    13
#define LED_GPIO_NUM     21   // Onboard LED confirmed

// ============================================
// GPS Pins - Verified FREE on this board
// Camera: 4,5,6,7,8,9,10,11,12,13,15,16,17,18
// LED:    21
// USB:    19,20,43,44
// FREE:   14 ✓  47 ✓
// ============================================
#define GPS_RX_PIN  14    // NEO-6M TX → GPIO 14
#define GPS_TX_PIN  47    // NEO-6M RX → GPIO 47
#define GPS_BAUD  9600

// ============================================
// GPS Objects
// ============================================
TinyGPSPlus    gps;
HardwareSerial GPSSerial(1);   // UART1

double        currentLat       = 0.0;
double        currentLng       = 0.0;
int           currentSats      = 0;
bool          gpsFix           = false;
unsigned long gpsCharsReceived = 0;
unsigned long lastStatusPrint  = 0;

// ============================================
// Camera control variables
// ============================================
int        jpeg_quality = 12;
int        frame_delay  = 30;
framesize_t frame_size  = FRAMESIZE_VGA;

// ============================================
// HTTP Server
// ============================================
httpd_handle_t camera_httpd = NULL;

// ============================================
// READ GPS — call as often as possible
// ============================================
void readGPS() {
    while (GPSSerial.available()) {
        char c = GPSSerial.read();
        gps.encode(c);
        gpsCharsReceived++;
    }

    if (gps.location.isValid()
        && gps.location.age() < 2000
        && gps.satellites.isValid()
        && gps.satellites.value() > 0)
    {
        currentLat  = gps.location.lat();
        currentLng  = gps.location.lng();
        currentSats = (int)gps.satellites.value();
        gpsFix      = true;
    }
}

// ============================================
// GPS JSON Endpoint — /gps
// ============================================
static esp_err_t gps_handler(httpd_req_t *req) {
    readGPS();  // fresh read before responding

    char json[256];
    snprintf(json, sizeof(json),
        "{"
          "\"lat\":%.6f,"
          "\"lng\":%.6f,"
          "\"satellites\":%d,"
          "\"fix\":%s,"
          "\"chars_received\":%lu"
        "}",
        currentLat,
        currentLng,
        currentSats,
        gpsFix ? "true" : "false",
        gpsCharsReceived
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr(req, json);

    Serial.printf("[GPS-API] lat:%.6f lng:%.6f sats:%d fix:%s\n",
        currentLat, currentLng, currentSats,
        gpsFix ? "YES" : "NO");

    return ESP_OK;
}

// ============================================
// Capture Single Image — /capture
// ============================================
static esp_err_t capture_handler(httpd_req_t *req) {
    digitalWrite(LED_GPIO_NUM, HIGH);

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        digitalWrite(LED_GPIO_NUM, LOW);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, (const char*)fb->buf, fb->len);

    esp_camera_fb_return(fb);
    digitalWrite(LED_GPIO_NUM, LOW);
    return ESP_OK;
}

// ============================================
// MJPEG Stream — /stream
// ============================================
static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t* fb  = NULL;
    esp_err_t    res = ESP_OK;
    char         part_buf[64];

    Serial.println("[STREAM] Client connected");

    httpd_resp_set_type(req,
        "multipart/x-mixed-replace;boundary=frame");
    httpd_resp_set_hdr(req,
        "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req,
        "Cache-Control", "no-cache");

    uint32_t frameCount = 0;
    uint32_t failCount  = 0;

    while (true) {
        // Keep GPS alive during streaming
        readGPS();

        fb = esp_camera_fb_get();
        if (!fb) {
            failCount++;
            Serial.printf("[STREAM] Frame fail #%u\n", failCount);
            if (failCount > 10) {
                Serial.println("[STREAM] Too many fails, stopping");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        failCount = 0;

        // Send boundary
        res = httpd_resp_send_chunk(req, "--frame\r\n", 9);
        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }

        // Send MIME header
        size_t hlen = snprintf(part_buf, sizeof(part_buf),
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %u\r\n\r\n",
            (uint32_t)fb->len);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }

        // Send frame
        res = httpd_resp_send_chunk(req,
            (const char*)fb->buf, fb->len);
        esp_camera_fb_return(fb);
        if (res != ESP_OK) break;

        // Trailing CRLF
        res = httpd_resp_send_chunk(req, "\r\n", 2);
        if (res != ESP_OK) break;

        frameCount++;
        if (frameCount % 50 == 0) {
            Serial.printf("[STREAM] %u frames | sats:%d\n",
                frameCount, currentSats);
        }

        vTaskDelay(frame_delay / portTICK_PERIOD_MS);
    }

    Serial.println("[STREAM] Client disconnected");
    return res;
}

// ============================================
// Web UI — /
// ============================================
static esp_err_t index_handler(httpd_req_t *req) {
    const char* html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>ESP32-S3 CAM + GPS</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: Arial; margin: 20px; background: #1a1a2e; color: #eee; }
  h1   { color: #00d4ff; }
  h3   { color: #aaa; margin-bottom: 8px; }
  .box { background: #16213e; border-radius: 10px;
         padding: 15px; margin: 10px 0; }
  button {
    padding: 8px 15px; margin: 4px;
    background: #0f3460; color: white;
    border: 1px solid #00d4ff;
    border-radius: 6px; cursor: pointer;
  }
  button:hover { background: #00d4ff; color: #000; }
  #stream { max-width: 100%; border-radius: 10px;
            border: 2px solid #00d4ff; }
  #gpsBox { font-size: 14px; line-height: 1.8; }
  .fix-yes { color: #00ff88; font-weight: bold; }
  .fix-no  { color: #ff4444; font-weight: bold; }
  #mapLink { color: #00d4ff; }
</style>
</head>
<body>
<h1>&#128247; ESP32-S3 CAM + GPS</h1>

<div class="box">
  <h3>&#128246; GPS Status</h3>
  <div id="gpsBox">Loading GPS...</div>
</div>

<div class="box">
  <h3>&#127909; Live Stream</h3>
  <img id="stream" src="/stream">
</div>

<div class="box">
  <h3>&#9881; Quality</h3>
  <button onclick="setQuality(5)">High</button>
  <button onclick="setQuality(12)">Medium</button>
  <button onclick="setQuality(20)">Low</button>
</div>

<div class="box">
  <h3>&#9889; Speed</h3>
  <button onclick="setSpeed(10)">Fast</button>
  <button onclick="setSpeed(30)">Normal</button>
  <button onclick="setSpeed(60)">Slow</button>
</div>

<div class="box">
  <h3>&#128250; Resolution</h3>
  <button onclick="setRes(5)">QVGA 320x240</button>
  <button onclick="setRes(6)">VGA 640x480</button>
  <button onclick="setRes(8)">SVGA 800x600</button>
</div>

<script>
// Poll GPS every 3 seconds
function updateGPS() {
  fetch('/gps')
    .then(r => r.json())
    .then(d => {
      let fixClass = d.fix ? 'fix-yes' : 'fix-no';
      let fixText  = d.fix ? 'YES ✓'  : 'NO ✗';
      let mapHtml  = d.fix
        ? '<br>&#128205; <a id="mapLink" href="https://maps.google.com/?q='
          + d.lat + ',' + d.lng
          + '" target="_blank">Open in Google Maps</a>'
        : '';
      document.getElementById('gpsBox').innerHTML =
        'Fix: <span class="' + fixClass + '">' + fixText + '</span><br>' +
        'Latitude:   ' + d.lat.toFixed(6) + '°<br>' +
        'Longitude:  ' + d.lng.toFixed(6) + '°<br>' +
        'Satellites: ' + d.satellites +
        mapHtml;
    })
    .catch(() => {
      document.getElementById('gpsBox').innerHTML =
        '<span class="fix-no">GPS fetch failed</span>';
    });
}

updateGPS();
setInterval(updateGPS, 3000);

function setQuality(q) { fetch('/control?quality=' + q); }
function setSpeed(s)   { fetch('/control?speed='   + s); }
function setRes(r) {
  fetch('/control?resolution=' + r).then(() => {
    document.getElementById('stream').src =
      '/stream?' + Date.now();
  });
}
</script>
</body>
</html>
)rawliteral";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

// ============================================
// Camera Control — /control
// ============================================
static esp_err_t control_handler(httpd_req_t *req) {
    char query[200];
    if (httpd_req_get_url_query_str(req, query,
            sizeof(query)) == ESP_OK) {
        char param[32];

        if (httpd_query_key_value(query, "quality",
                param, sizeof(param)) == ESP_OK) {
            jpeg_quality = atoi(param);
            sensor_t* s = esp_camera_sensor_get();
            if (s) s->set_quality(s, jpeg_quality);
            Serial.printf("[CAM] Quality → %d\n", jpeg_quality);
        }

        if (httpd_query_key_value(query, "speed",
                param, sizeof(param)) == ESP_OK) {
            frame_delay = atoi(param);
            Serial.printf("[CAM] Delay → %dms\n", frame_delay);
        }

        if (httpd_query_key_value(query, "resolution",
                param, sizeof(param)) == ESP_OK) {
            framesize_t new_size = (framesize_t)atoi(param);
            sensor_t* s = esp_camera_sensor_get();
            if (s) s->set_framesize(s, new_size);
            Serial.printf("[CAM] Resolution → %d\n", new_size);
        }
    }

    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

// ============================================
// Start HTTP Server
// ============================================
void startServer() {
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 10;
    cfg.stack_size       = 10240;

    if (httpd_start(&camera_httpd, &cfg) != ESP_OK) {
        Serial.println("[SERVER] Failed to start!");
        return;
    }

    // All URI handlers
    static const httpd_uri_t uri_index = {
        .uri="/",         .method=HTTP_GET,
        .handler=index_handler,   .user_ctx=NULL };

    static const httpd_uri_t uri_capture = {
        .uri="/capture",  .method=HTTP_GET,
        .handler=capture_handler, .user_ctx=NULL };

    static const httpd_uri_t uri_stream = {
        .uri="/stream",   .method=HTTP_GET,
        .handler=stream_handler,  .user_ctx=NULL };

    static const httpd_uri_t uri_control = {
        .uri="/control",  .method=HTTP_GET,
        .handler=control_handler, .user_ctx=NULL };

    static const httpd_uri_t uri_gps = {
        .uri="/gps",      .method=HTTP_GET,
        .handler=gps_handler,     .user_ctx=NULL };

    httpd_register_uri_handler(camera_httpd, &uri_index);
    httpd_register_uri_handler(camera_httpd, &uri_capture);
    httpd_register_uri_handler(camera_httpd, &uri_stream);
    httpd_register_uri_handler(camera_httpd, &uri_control);
    httpd_register_uri_handler(camera_httpd, &uri_gps);

    Serial.println("[SERVER] All endpoints registered:");
    Serial.println("[SERVER]  /         → Web UI");
    Serial.println("[SERVER]  /stream   → MJPEG Stream");
    Serial.println("[SERVER]  /capture  → Single JPEG");
    Serial.println("[SERVER]  /control  → Camera Settings");
    Serial.println("[SERVER]  /gps      → GPS JSON");
}

// ============================================
// Camera Init
// ============================================
bool startCamera() {
    Serial.println("[CAMERA] Initializing...");

    camera_config_t config;
    memset(&config, 0, sizeof(config));

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = frame_size;
    config.jpeg_quality = jpeg_quality;
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAMERA] FAILED: 0x%x\n", err);
        return false;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 1);
        s->set_saturation(s, 0);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_exposure_ctrl(s, 1);
    }

    Serial.println("[CAMERA] OK ✓");
    return true;
}

// ============================================
// Wait For GPS Fix (blocking before camera)
// ============================================
void waitForGPSFix() {
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.println("[GPS] Waiting for fix...");
    Serial.printf( "[GPS] RX=GPIO%d ← NEO-6M TX\n", GPS_RX_PIN);
    Serial.printf( "[GPS] TX=GPIO%d → NEO-6M RX\n", GPS_TX_PIN);
    Serial.println("[GPS] Point antenna to open sky!");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

    unsigned long lastPrint = 0;
    unsigned long startMs   = millis();

    while (true) {
        readGPS();

        unsigned long now = millis();
        if (now - lastPrint >= 2000) {
            lastPrint = now;
            int sats = gps.satellites.isValid()
                       ? (int)gps.satellites.value() : 0;

            Serial.printf(
                "[GPS] %lus elapsed | Chars:%lu | "
                "Sentences:%lu | Sats:%d\n",
                (now - startMs) / 1000,
                gpsCharsReceived,
                gps.passedChecksum(),
                sats
            );

            // No data at all
            if (gpsCharsReceived == 0
                && (now - startMs) > 8000) {
                Serial.println("[GPS] ✗ ZERO chars received!");
                Serial.println("[GPS]   Check NEO-6M TX → GPIO14");
                Serial.println("[GPS]   Check 3.3V power");
                Serial.println("[GPS]   Check GND");
            }

            // Data but no fix
            if (gpsCharsReceived > 100 && sats == 0) {
                Serial.println("[GPS] ℹ Data OK, awaiting fix");
                Serial.println("[GPS]   Can take 1-5 min outdoors");
            }

            // Got fix!
            if (gpsFix) {
                Serial.println();
                Serial.println("╔═══════════════════════════════╗");
                Serial.println("║      GPS FIX ACQUIRED ✓       ║");
                Serial.printf( "║  LAT  : %11.6f°        ║\n",
                               currentLat);
                Serial.printf( "║  LNG  : %11.6f°        ║\n",
                               currentLng);
                Serial.printf( "║  SATS : %-2d                     ║\n",
                               currentSats);
                Serial.println("╚═══════════════════════════════╝");
                return;
            }
        }
        delay(20);
    }
}

// ============================================
// SETUP
// ============================================
void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println();
    Serial.println("╔═══════════════════════════════╗");
    Serial.println("║  ESP32-S3-CAM N16R8 + GPS     ║");
    Serial.println("╚═══════════════════════════════╝");

    // LED init
    pinMode(LED_GPIO_NUM, OUTPUT);
    digitalWrite(LED_GPIO_NUM, LOW);

    // STEP 1 — GPS UART on safe pins
    Serial.printf("\n[1/4] GPS UART1 RX=GPIO%d TX=GPIO%d\n",
                  GPS_RX_PIN, GPS_TX_PIN);
    GPSSerial.begin(GPS_BAUD, SERIAL_8N1,
                    GPS_RX_PIN, GPS_TX_PIN);
    delay(500);

    // STEP 2 — Wait for GPS fix BEFORE camera
    Serial.println("[2/4] Acquiring GPS fix...");
    waitForGPSFix();

    // STEP 3 — WiFi AP (no router needed)
    Serial.println("[3/4] Starting WiFi AP...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, password);
    delay(500);
    Serial.print("[WIFI] SSID: ");
    Serial.println(ssid);
    Serial.print("[WIFI] IP:   ");
    Serial.println(WiFi.softAPIP());

    // STEP 4 — Camera
    Serial.println("[4/4] Starting Camera...");
    if (!startCamera()) {
        Serial.println("[ERROR] Camera failed! Reboot in 5s");
        delay(5000);
        ESP.restart();
    }

    // Start server
    startServer();

    // Blink LED to signal ready
    for (int i = 0; i < 3; i++) {
        digitalWrite(LED_GPIO_NUM, HIGH);
        delay(200);
        digitalWrite(LED_GPIO_NUM, LOW);
        delay(200);
    }

    Serial.println();
    Serial.println("╔═══════════════════════════════╗");
    Serial.println("║        SYSTEM READY ✓         ║");
    Serial.printf( "║  WiFi  : %-21s║\n", ssid);
    Serial.printf( "║  Pass  : %-21s║\n", password);
    Serial.println("║  Open  : 192.168.4.1          ║");
    Serial.println("║  Stream: 192.168.4.1/stream   ║");
    Serial.println("║  GPS   : 192.168.4.1/gps      ║");
    Serial.println("╚═══════════════════════════════╝");
}

// ============================================
// LOOP
// ============================================
void loop() {
    // GPS is highest priority — always read first
    readGPS();

    // Status every 5 seconds
    if (millis() - lastStatusPrint >= 5000) {
        lastStatusPrint = millis();

        Serial.println("┌──────────── STATUS ──────────┐");
        Serial.printf( "│ GPS Chars : %-17lu│\n",
                       gpsCharsReceived);
        Serial.printf( "│ Sentences : %-17lu│\n",
                       gps.passedChecksum());
        Serial.printf( "│ Satellites: %-17d│\n",
                       currentSats);
        if (gpsFix) {
            Serial.printf("│ LAT: %-25.6f│\n", currentLat);
            Serial.printf("│ LNG: %-25.6f│\n", currentLng);
            Serial.println("│ Fix: YES ✓                   │");
        } else {
            Serial.println("│ Fix: NO — searching...       │");
        }
        Serial.printf("│ WiFi Clients: %-15d│\n",
                      WiFi.softAPgetStationNum());
        Serial.println("└──────────────────────────────┘");
    }

    delay(10);
}