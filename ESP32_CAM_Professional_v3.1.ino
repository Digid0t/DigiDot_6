/*
 * ==========================================
 * ESP32-CAM PROFESSIONAL WebServer
 * FINAL VERSION - ALLE BOARD VERSIONEN
 * ==========================================
 * 
 * FEATURES:
 * - Kompatibel mit ESP32 Board Version 2.x UND 3.x
 * - Dual-Mode: WLAN-Client ODER Access Point (automatischer Fallback)
 * - HTTP Basic Authentication (Passwortschutz)
 * - Live-Video-Stream (MJPEG)
 * - Foto-Aufnahme und Speicherung auf SD-Karte
 * - Robuste Fehlerbehandlung
 * - Status-LED Anzeigen
 * - Automatische Wiederverbindung
 * 
 * HARDWARE:
 * - ESP32-CAM (AI-Thinker)
 * - MicroSD-Karte (optional für Foto-Speicherung)
 * 
 * BOARD-EINSTELLUNGEN:
 * - Board: "AI Thinker ESP32-CAM"
 * - Upload Speed: 115200
 * - Partition Scheme: "Huge APP (3MB No OTA/1MB SPIFFS)"
 * 
 * Version: 3.1 Final (Board Version 2.x & 3.x kompatibel)
 * Datum: 2025-11-05
 * Status: Production Ready
 * ==========================================
 */

#include "esp_camera.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "Arduino.h"
#include "fb_gfx.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_http_server.h"
#include <WiFi.h>
#include "FS.h"
#include "SD_MMC.h"

// ===== KONFIGURATION =====

// WLAN-Client-Modus (verbindet sich mit bestehendem WLAN)
const char* WIFI_SSID = "Wilde_Hilde";              // Dein WLAN-Name
const char* WIFI_PASSWORD = "_____";   // Dein WLAN-Passwort

// Access Point Modus (Fallback wenn WLAN nicht erreichbar)
const char* AP_SSID = "ESP32-CAM-Wilde";            // Name des ESP32-CAM AP
const char* AP_PASSWORD = "____";              // AP-Passwort (min. 8 Zeichen!)

// Webserver Authentifizierung
const char* HTTP_USERNAME = "moni";                // Web-Login Benutzername
const char* HTTP_PASSWORD = "_____";            // Web-Login Passwort

// Timeout-Einstellungen
#define WIFI_CONNECT_TIMEOUT 20000  // 20 Sekunden Timeout für WLAN-Verbindung

// ===== KAMERA PIN DEFINITIONEN (AI-Thinker ESP32-CAM) =====
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
#define LED_GPIO_NUM       4

// ===== GLOBALE VARIABLEN =====
httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;
bool sd_card_available = false;
int photo_counter = 0;
bool is_access_point_mode = false;
unsigned long last_wifi_check = 0;

// LED-Kanal für beide Board-Versionen
#define LED_CHANNEL 7

// ===== HILFSFUNKTIONEN =====

/*
 * LED-Flash Steuerung
 * Kompatibel mit ESP32 Board Version 2.x und 3.x
 */
void setupLedFlash(int pin) {
  #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    // Neue API für ESP32 Board Version 3.x
    ledcAttach(pin, 5000, 8);
    ledcWrite(pin, 0);
    Serial.println("[LED] Flash initialisiert (Board v3.x)");
  #else
    // Alte API für ESP32 Board Version 2.x
    ledcSetup(LED_CHANNEL, 5000, 8);
    ledcAttachPin(pin, LED_CHANNEL);
    ledcWrite(LED_CHANNEL, 0);
    Serial.println("[LED] Flash initialisiert (Board v2.x)");
  #endif
}

void setLedBrightness(int brightness) {
  #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    // Neue API für ESP32 Board Version 3.x
    ledcWrite(LED_GPIO_NUM, brightness);
  #else
    // Alte API für ESP32 Board Version 2.x
    ledcWrite(LED_CHANNEL, brightness);
  #endif
}

/*
 * Base64 Encoding (für HTTP Auth)
 * Einfache Implementation ohne externe Bibliothek
 */
String base64_encode(String input) {
  const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String output = "";
  int val = 0;
  int valb = -6;
  
  for (unsigned char c : input) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      output += base64_chars[(val >> valb) & 0x3F];
      valb -= 6;
    }
  }
  if (valb > -6) {
    output += base64_chars[((val << 8) >> (valb + 8)) & 0x3F];
  }
  while (output.length() % 4) {
    output += '=';
  }
  return output;
}

String base64_decode(String input) {
  const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String output = "";
  int T[256];
  
  for (int i = 0; i < 256; i++) T[i] = -1;
  for (int i = 0; i < 64; i++) T[(int)base64_chars[i]] = i;
  
  int val = 0;
  int valb = -8;
  
  for (unsigned char c : input) {
    if (T[c] == -1) break;
    val = (val << 6) + T[c];
    valb += 6;
    if (valb >= 0) {
      output += char((val >> valb) & 0xFF);
      valb -= 8;
    }
  }
  return output;
}

/*
 * HTTP Basic Authentication Check
 */
bool checkAuthentication(httpd_req_t *req) {
  char auth_header[128];
  size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");
  
  if (auth_len > 0 && auth_len < sizeof(auth_header)) {
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) == ESP_OK) {
      if (strncmp(auth_header, "Basic ", 6) == 0) {
        String auth_b64 = String(auth_header + 6);
        String decoded = base64_decode(auth_b64);
        String expected = String(HTTP_USERNAME) + ":" + String(HTTP_PASSWORD);
        
        if (decoded == expected) {
          return true;
        }
      }
    }
  }
  return false;
}

esp_err_t sendAuthRequired(httpd_req_t *req) {
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32-CAM\"");
  
  const char* resp = "<html><body><h1>401 Unauthorized</h1><p>Bitte anmelden.</p></body></html>";
  httpd_resp_send(req, resp, strlen(resp));
  return ESP_OK;
}

/*
 * SD-Karte initialisieren
 */
bool initSDCard() {
  Serial.println("[SD] Initialisiere SD-Karte...");
  
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("[SD] WARNUNG: SD-Karte nicht gefunden");
    Serial.println("[SD] Fotos können nicht gespeichert werden");
    return false;
  }
  
  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[SD] Keine SD-Karte eingelegt");
    return false;
  }
  
  Serial.print("[SD] ✓ SD-Karte Typ: ");
  if (cardType == CARD_MMC) Serial.println("MMC");
  else if (cardType == CARD_SD) Serial.println("SDSC");
  else if (cardType == CARD_SDHC) Serial.println("SDHC");
  else Serial.println("UNKNOWN");
  
  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  Serial.printf("[SD] Größe: %lluMB\n", cardSize);
  
  if (!SD_MMC.exists("/photos")) {
    if (SD_MMC.mkdir("/photos")) {
      Serial.println("[SD] Ordner '/photos' erstellt");
    }
  }
  
  File counterFile = SD_MMC.open("/photo_counter.txt", FILE_READ);
  if (counterFile) {
    String counter_str = counterFile.readStringUntil('\n');
    photo_counter = counter_str.toInt();
    counterFile.close();
    Serial.printf("[SD] Letzter Foto-Counter: %d\n", photo_counter);
  }
  
  return true;
}

/*
 * Foto auf SD-Karte speichern
 */
bool savePhoto() {
  if (!sd_card_available) {
    Serial.println("[FOTO] FEHLER: SD-Karte nicht verfügbar");
    return false;
  }
  
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[FOTO] FEHLER: Kamera-Frame konnte nicht erfasst werden");
    return false;
  }
  
  setLedBrightness(255);
  delay(100);
  setLedBrightness(0);
  
  photo_counter++;
  char filename[32];
  snprintf(filename, sizeof(filename), "/photos/photo_%04d.jpg", photo_counter);
  
  File file = SD_MMC.open(filename, FILE_WRITE);
  if (!file) {
    Serial.printf("[FOTO] FEHLER: Konnte Datei nicht öffnen: %s\n", filename);
    esp_camera_fb_return(fb);
    return false;
  }
  
  file.write(fb->buf, fb->len);
  file.close();
  esp_camera_fb_return(fb);
  
  File counterFile = SD_MMC.open("/photo_counter.txt", FILE_WRITE);
  if (counterFile) {
    counterFile.println(photo_counter);
    counterFile.close();
  }
  
  Serial.printf("[FOTO] ✓ Foto gespeichert: %s\n", filename);
  return true;
}

// ===== HTTP HANDLER =====

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req) {
  if (!checkAuthentication(req)) {
    return sendAuthRequired(req);
  }
  
  camera_fb_t* fb = NULL;
  esp_err_t res = ESP_OK;
  size_t jpg_buf_len = 0;
  uint8_t* jpg_buf = NULL;
  char part_buf[64];
  
  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  
  Serial.println("[STREAM] Client verbunden");
  
  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[STREAM] Kamera-Frame-Fehler");
      res = ESP_FAIL;
      break;
    }
    
    if (fb->format != PIXFORMAT_JPEG) {
      bool jpeg_converted = frame2jpg(fb, 80, &jpg_buf, &jpg_buf_len);
      esp_camera_fb_return(fb);
      fb = NULL;
      if (!jpeg_converted) {
        Serial.println("[STREAM] JPEG-Konvertierung fehlgeschlagen");
        res = ESP_FAIL;
        break;
      }
    } else {
      jpg_buf_len = fb->len;
      jpg_buf = fb->buf;
    }
    
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    }
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, 64, STREAM_PART, jpg_buf_len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char*)jpg_buf, jpg_buf_len);
    }
    
    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      jpg_buf = NULL;
    } else if (jpg_buf) {
      free(jpg_buf);
      jpg_buf = NULL;
    }
    
    if (res != ESP_OK) break;
  }
  
  Serial.println("[STREAM] Client getrennt");
  return res;
}

static esp_err_t capture_handler(httpd_req_t *req) {
  if (!checkAuthentication(req)) {
    return sendAuthRequired(req);
  }
  
  Serial.println("[CAPTURE] Einzelbild wird aufgenommen");
  
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[CAPTURE] Kamera-Frame-Fehler");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  
  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  
  if (res == ESP_OK) {
    Serial.println("[CAPTURE] ✓ Einzelbild gesendet");
  }
  
  return res;
}

static esp_err_t save_photo_handler(httpd_req_t *req) {
  if (!checkAuthentication(req)) {
    return sendAuthRequired(req);
  }
  
  bool success = savePhoto();
  
  httpd_resp_set_type(req, "application/json");
  if (success) {
    char response[128];
    snprintf(response, sizeof(response), 
             "{\"success\":true,\"message\":\"Foto gespeichert\",\"filename\":\"photo_%04d.jpg\",\"count\":%d}", 
             photo_counter, photo_counter);
    httpd_resp_send(req, response, strlen(response));
  } else {
    const char* response = "{\"success\":false,\"message\":\"Fehler beim Speichern\"}";
    httpd_resp_send(req, response, strlen(response));
  }
  
  return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req) {
  if (!checkAuthentication(req)) {
    return sendAuthRequired(req);
  }
  
  char response[512];
  snprintf(response, sizeof(response),
           "{\"mode\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"sd_card\":%s,\"photos\":%d,\"heap\":%d}",
           is_access_point_mode ? "AP" : "Client",
           is_access_point_mode ? AP_SSID : WIFI_SSID,
           WiFi.localIP().toString().c_str(),
           WiFi.RSSI(),
           sd_card_available ? "true" : "false",
           photo_counter,
           ESP.getFreeHeap());
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, response, strlen(response));
  return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req) {
  if (!checkAuthentication(req)) {
    return sendAuthRequired(req);
  }
  
  Serial.println("[WEB] Hauptseite wird geladen");
  
  const char* html = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-CAM Wilde Hilde</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: Arial, sans-serif;
            background: linear-gradient(135deg, #1a1a1a 0%, #2d2d2d 100%);
            color: #fff;
            padding: 20px;
        }
        .container {
            max-width: 1200px;
            margin: 0 auto;
            background: rgba(255, 255, 255, 0.05);
            border-radius: 20px;
            padding: 30px;
            backdrop-filter: blur(10px);
        }
        h1 {
            text-align: center;
            color: #00ff88;
            margin-bottom: 30px;
            text-shadow: 0 0 20px rgba(0, 255, 136, 0.5);
        }
        .status {
            background: rgba(0, 255, 136, 0.1);
            padding: 15px;
            border-radius: 10px;
            margin-bottom: 20px;
            border: 1px solid rgba(0, 255, 136, 0.3);
        }
        .video-container {
            position: relative;
            width: 100%;
            background: #000;
            border-radius: 15px;
            overflow: hidden;
            margin-bottom: 20px;
        }
        .video-container img {
            width: 100%;
            height: auto;
            display: block;
        }
        .controls {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 15px;
            margin-top: 20px;
        }
        button {
            padding: 15px 25px;
            font-size: 16px;
            font-weight: 600;
            color: #000;
            background: linear-gradient(135deg, #00ff88 0%, #00cc6f 100%);
            border: none;
            border-radius: 10px;
            cursor: pointer;
            transition: all 0.3s ease;
            box-shadow: 0 4px 15px rgba(0, 255, 136, 0.3);
        }
        button:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 20px rgba(0, 255, 136, 0.4);
        }
        button:active {
            transform: translateY(0);
        }
        .info {
            background: rgba(255, 255, 255, 0.03);
            padding: 20px;
            border-radius: 10px;
            margin-top: 20px;
        }
        .info-row {
            display: flex;
            justify-content: space-between;
            padding: 8px 0;
            border-bottom: 1px solid rgba(255, 255, 255, 0.1);
        }
        .info-row:last-child { border-bottom: none; }
        .label { color: #888; }
        .value { color: #00ff88; font-weight: bold; }
        #notification {
            position: fixed;
            top: 20px;
            right: 20px;
            padding: 15px 25px;
            background: #00ff88;
            color: #000;
            border-radius: 10px;
            font-weight: bold;
            display: none;
            animation: slideIn 0.3s ease;
        }
        @keyframes slideIn {
            from { transform: translateX(400px); opacity: 0; }
            to { transform: translateX(0); opacity: 1; }
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>📹 ESP32-CAM Wilde Hilde</h1>
        
        <div class="status" id="status">
            <strong>Status:</strong> <span id="statusText">Lädt...</span>
        </div>
        
        <div class="video-container">
            <img id="stream" src="/stream" alt="Video Stream">
        </div>
        
        <div class="controls">
            <button onclick="reloadStream()">🔄 Stream Neu Laden</button>
            <button onclick="capturePhoto()">📸 Foto Anzeigen</button>
            <button onclick="savePhoto()">💾 Foto Speichern</button>
            <button onclick="updateStatus()">ℹ️ Status Aktualisieren</button>
        </div>
        
        <div class="info" id="info">
            <h3 style="color: #00ff88; margin-bottom: 15px;">System-Information</h3>
            <div class="info-row">
                <span class="label">Modus:</span>
                <span class="value" id="mode">-</span>
            </div>
            <div class="info-row">
                <span class="label">SSID:</span>
                <span class="value" id="ssid">-</span>
            </div>
            <div class="info-row">
                <span class="label">IP-Adresse:</span>
                <span class="value" id="ip">-</span>
            </div>
            <div class="info-row">
                <span class="label">Signal:</span>
                <span class="value" id="rssi">-</span>
            </div>
            <div class="info-row">
                <span class="label">SD-Karte:</span>
                <span class="value" id="sdcard">-</span>
            </div>
            <div class="info-row">
                <span class="label">Gespeicherte Fotos:</span>
                <span class="value" id="photos">-</span>
            </div>
            <div class="info-row">
                <span class="label">Freier Speicher:</span>
                <span class="value" id="heap">-</span>
            </div>
        </div>
    </div>
    
    <div id="notification"></div>
    
    <script>
        function showNotification(message) {
            const notif = document.getElementById('notification');
            notif.textContent = message;
            notif.style.display = 'block';
            setTimeout(() => { notif.style.display = 'none'; }, 3000);
        }
        
        function reloadStream() {
            document.getElementById('stream').src = '/stream?' + new Date().getTime();
            showNotification('Stream wird neu geladen...');
        }
        
        function capturePhoto() {
            window.open('/capture', '_blank');
        }
        
        async function savePhoto() {
            try {
                const response = await fetch('/save_photo');
                const data = await response.json();
                if (data.success) {
                    showNotification('✓ Foto gespeichert: ' + data.filename);
                    updateStatus();
                } else {
                    showNotification('✗ Fehler: ' + data.message);
                }
            } catch (error) {
                showNotification('✗ Fehler beim Speichern');
            }
        }
        
        async function updateStatus() {
            try {
                const response = await fetch('/status');
                const data = await response.json();
                
                document.getElementById('mode').textContent = data.mode;
                document.getElementById('ssid').textContent = data.ssid;
                document.getElementById('ip').textContent = data.ip;
                document.getElementById('rssi').textContent = data.rssi + ' dBm';
                document.getElementById('sdcard').textContent = data.sd_card ? 'Verfügbar ✓' : 'Nicht verfügbar ✗';
                document.getElementById('photos').textContent = data.photos;
                document.getElementById('heap').textContent = (data.heap / 1024).toFixed(1) + ' KB';
                
                document.getElementById('statusText').textContent = '✓ Online - ' + data.mode + ' Modus';
            } catch (error) {
                console.error('Status-Fehler:', error);
            }
        }
        
        document.getElementById('stream').onload = function() {
            document.getElementById('statusText').textContent = '✓ Stream läuft';
        };
        
        document.getElementById('stream').onerror = function() {
            document.getElementById('statusText').textContent = '⚠ Stream-Fehler';
        };
        
        setInterval(updateStatus, 10000);
        setTimeout(updateStatus, 1000);
    </script>
</body>
</html>
)rawliteral";
  
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html, strlen(html));
}

void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  
  httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
  httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
  httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
  httpd_uri_t save_photo_uri = { .uri = "/save_photo", .method = HTTP_GET, .handler = save_photo_handler, .user_ctx = NULL };
  httpd_uri_t status_uri = { .uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };
  
  Serial.println("[SERVER] Starte HTTP-Server...");
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &save_photo_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    Serial.println("[SERVER] ✓ HTTP-Server gestartet");
  }
  
  config.server_port = 81;
  config.ctrl_port = 32769;
  
  Serial.println("[SERVER] Starte Stream-Server...");
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    Serial.println("[SERVER] ✓ Stream-Server gestartet");
  }
}

bool initCamera() {
  Serial.println("[KAMERA] Initialisiere Kamera...");
  
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_UXGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  
  if (psramFound()) {
    Serial.println("[KAMERA] PSRAM gefunden - Hohe Qualität aktiviert");
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    Serial.println("[KAMERA] WARNUNG: Kein PSRAM - Reduzierte Qualität");
    config.frame_size = FRAMESIZE_SVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }
  
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[KAMERA] ✗ FEHLER: Init fehlgeschlagen mit Error 0x%x\n", err);
    Serial.println("[KAMERA] Bitte ESP32-CAM überprüfen");
    return false;
  }
  
  Serial.println("[KAMERA] ✓ Kamera erfolgreich initialisiert");
  
  sensor_t* s = esp_camera_sensor_get();
  if (s != NULL) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_special_effect(s, 0);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 0);
    s->set_gain_ctrl(s, 1);
    s->set_agc_gain(s, 0);
    s->set_gainceiling(s, (gainceiling_t)0);
    s->set_bpc(s, 0);
    s->set_wpc(s, 1);
    s->set_raw_gma(s, 1);
    s->set_lenc(s, 1);
    s->set_hmirror(s, 0);
    s->set_vflip(s, 0);
    s->set_dcw(s, 1);
    s->set_colorbar(s, 0);
    Serial.println("[KAMERA] Sensor-Parameter optimiert");
  }
  
  return true;
}

bool connectToWiFi() {
  Serial.println("========================================");
  Serial.println("[WLAN] Versuche Verbindung herzustellen...");
  Serial.printf("[WLAN] SSID: %s\n", WIFI_SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false);
  
  unsigned long start_time = millis();
  int attempt = 0;
  
  while (WiFi.status() != WL_CONNECTED && (millis() - start_time) < WIFI_CONNECT_TIMEOUT) {
    delay(500);
    Serial.print(".");
    setLedBrightness((attempt % 2) * 128);
    attempt++;
    
    if (attempt % 40 == 0) {
      Serial.println();
      Serial.printf("[WLAN] Versuche weiter... (%d Sekunden)\n", (millis() - start_time) / 1000);
    }
  }
  
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    setLedBrightness(0);
    Serial.println("[WLAN] ✓ Verbindung erfolgreich!");
    Serial.print("[WLAN] IP-Adresse: ");
    Serial.println(WiFi.localIP());
    Serial.print("[WLAN] Signal-Stärke: ");
    long rssi = WiFi.RSSI();
    Serial.print(rssi);
    Serial.print(" dBm ");
    
    if (rssi > -50) Serial.println("(Ausgezeichnet)");
    else if (rssi > -60) Serial.println("(Sehr gut)");
    else if (rssi > -70) Serial.println("(Gut)");
    else if (rssi > -80) Serial.println("(Schwach)");
    else Serial.println("(Sehr schwach)");
    
    return true;
  } else {
    Serial.println("[WLAN] ✗ Verbindung fehlgeschlagen!");
    return false;
  }
}

void startAccessPoint() {
  Serial.println("========================================");
  Serial.println("[AP] Starte Access Point Modus...");
  Serial.printf("[AP] SSID: %s\n", AP_SSID);
  Serial.printf("[AP] Passwort: %s\n", AP_PASSWORD);
  
  WiFi.mode(WIFI_AP);
  bool success = WiFi.softAP(AP_SSID, AP_PASSWORD);
  
  if (success) {
    IPAddress IP = WiFi.softAPIP();
    Serial.println("[AP] ✓ Access Point gestartet!");
    Serial.print("[AP] IP-Adresse: ");
    Serial.println(IP);
    is_access_point_mode = true;
    
    for (int i = 0; i < 6; i++) {
      setLedBrightness(255);
      delay(200);
      setLedBrightness(0);
      delay(200);
    }
  } else {
    Serial.println("[AP] ✗ FEHLER: Access Point konnte nicht gestartet werden!");
    delay(10000);
    ESP.restart();
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  delay(1000);
  
  Serial.println();
  Serial.println("========================================");
  Serial.println("  ESP32-CAM PROFESSIONAL v3.1");
  Serial.println("  Wilde Hilde Edition");
  #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  Serial.println("  Board Version: 3.x");
  #else
  Serial.println("  Board Version: 2.x");
  #endif
  Serial.println("========================================");
  Serial.println();
  
  setupLedFlash(LED_GPIO_NUM);
  setLedBrightness(0);
  
  if (!initCamera()) {
    Serial.println("[SYSTEM] KRITISCHER FEHLER: Kamera!");
    while (true) {
      setLedBrightness(255);
      delay(100);
      setLedBrightness(0);
      delay(100);
    }
  }
  
  sd_card_available = initSDCard();
  
  bool wifi_connected = connectToWiFi();
  
  if (!wifi_connected) {
    Serial.println("[SYSTEM] Wechsle zu Access Point Modus...");
    delay(1000);
    startAccessPoint();
  } else {
    is_access_point_mode = false;
  }
  
  startWebServer();
  
  Serial.println("========================================");
  Serial.println("  ✓ ESP32-CAM BEREIT!");
  Serial.println("========================================");
  
  if (is_access_point_mode) {
    Serial.println("  MODUS: Access Point");
    Serial.printf("  SSID: %s\n", AP_SSID);
    Serial.println("  IP: 192.168.4.1");
    Serial.println("  Verbinde mit ESP32-CAM WLAN");
  } else {
    Serial.println("  MODUS: WLAN Client");
    Serial.printf("  Verbunden mit: %s\n", WIFI_SSID);
    Serial.print("  IP: ");
    Serial.println(WiFi.localIP());
    Serial.printf("  Zugriff: http://%s\n", WiFi.localIP().toString().c_str());
  }
  
  Serial.printf("  Login: %s / %s\n", HTTP_USERNAME, HTTP_PASSWORD);
  Serial.printf("  SD-Karte: %s\n", sd_card_available ? "Verfügbar" : "Nicht verfügbar");
  Serial.println("========================================");
}

void loop() {
  if (!is_access_point_mode) {
    unsigned long current_time = millis();
    
    if (current_time - last_wifi_check > 10000) {
      last_wifi_check = current_time;
      
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WLAN] Verbindung verloren! Versuche neu...");
        WiFi.disconnect();
        delay(1000);
        
        bool reconnected = connectToWiFi();
        
        if (!reconnected) {
          Serial.println("[WLAN] Wechsle zu AP-Modus...");
          
          if (camera_httpd) {
            httpd_stop(camera_httpd);
            camera_httpd = NULL;
          }
          if (stream_httpd) {
            httpd_stop(stream_httpd);
            stream_httpd = NULL;
          }
          
          startAccessPoint();
          startWebServer();
        }
      }
    }
  }
  
  delay(100);
}
