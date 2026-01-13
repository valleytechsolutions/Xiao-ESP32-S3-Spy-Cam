#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include <SPI.h>
#include <SD.h>

// =========================
// USER SETTINGS
// =========================
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Recording behavior
static const uint32_t CLIP_DURATION_MS   = 60UL * 1000UL;  // 60s per clip folder
static const uint8_t  MAX_CLIPS          = 10;             // keep last N clip folders (unless protected)
static const uint32_t FRAME_INTERVAL_MS  = 100;            // ~10 fps

// Camera settings (normal mode: stream + record JPEG)
static const framesize_t CAMERA_FRAMESIZE = FRAMESIZE_QVGA; // 320x240
static const int CAMERA_JPEG_QUALITY      = 15;             // 10-20 typical
static const int CAMERA_FB_COUNT          = 2;

// Motion detection settings
static bool     MOTION_ENABLED           = true;
static const uint32_t MOTION_CHECK_MS    = 250;            // how often to check
static uint32_t MOTION_THRESHOLD         = 18;             // higher = less sensitive
static const uint32_t POST_MOTION_MS     = 10UL * 1000UL;  // keep recording this long after motion stops

// Motion capture mode (for detection only)
static const framesize_t MOTION_FRAMESIZE = FRAMESIZE_QQVGA; // 160x120

// SD (SPI) pins for XIAO ESP32-S3
#define SD_CS    21
#define SD_MOSI  9
#define SD_MISO  8
#define SD_SCK   7

// =========================
// XIAO ESP32-S3 Sense camera pins
// =========================
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39
#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

// =========================
// Globals
// =========================
static httpd_handle_t httpd = NULL;

static bool     g_recordingEnabled = true;
static uint8_t  g_currentClipIndex = 0;
static uint32_t g_clipStartMs      = 0;
static uint32_t g_lastFrameMs      = 0;
static uint32_t g_frameCounter     = 0;

static uint32_t g_lastMotionCheckMs = 0;
static uint32_t g_motionActiveUntil = 0;

static char g_currentClipDir[16] = {0}; // "/rec_000"

// Motion buffer (QQVGA 160x120 = 19200 bytes)
static uint8_t* g_prevGray = nullptr;
static size_t   g_prevGrayLen = 0;

// =========================
// Helpers: SD / filesystem
// =========================
static void makeClipDirName(uint8_t idx, char* out, size_t outLen) {
  snprintf(out, outLen, "/rec_%03u", (unsigned)idx);
}

static bool isClipProtected(const char* clipDir) {
  String p = String(clipDir) + "/PROTECT";
  return SD.exists(p.c_str());
}

static bool ensureDir(const char* path) {
  if (SD.exists(path)) return true;
  return SD.mkdir(path);
}

static bool deleteDirectoryRecursive(const char* dirPath) {
  File dir = SD.open(dirPath);
  if (!dir) return false;
  if (!dir.isDirectory()) {
    dir.close();
    return false;
  }

  File entry;
  while ((entry = dir.openNextFile())) {
    String fullPath = String(dirPath) + "/" + entry.name();
    if (entry.isDirectory()) {
      entry.close();
      deleteDirectoryRecursive(fullPath.c_str());
    } else {
      entry.close();
      SD.remove(fullPath.c_str());
    }
  }
  dir.close();

  return SD.rmdir(dirPath);
}

static bool deleteOldestUnprotectedClipToMakeRoom(uint8_t targetIndexToOverwrite) {
  char dirName[16];
  makeClipDirName(targetIndexToOverwrite, dirName, sizeof(dirName));

  if (!SD.exists(dirName)) return true;

  if (isClipProtected(dirName)) {
    // delete any other unprotected clip
    for (uint8_t i = 0; i < MAX_CLIPS; i++) {
      char scanDir[16];
      makeClipDirName(i, scanDir, sizeof(scanDir));
      if (SD.exists(scanDir) && !isClipProtected(scanDir)) {
        return deleteDirectoryRecursive(scanDir);
      }
    }
    // all protected
    return false;
  }

  return deleteDirectoryRecursive(dirName);
}

static bool startNewClip(uint8_t newIndex) {
  if (!deleteOldestUnprotectedClipToMakeRoom(newIndex)) {
    Serial.println("ERROR: All clips protected or cannot free space; cannot rotate.");
    return false;
  }

  g_currentClipIndex = newIndex;
  makeClipDirName(g_currentClipIndex, g_currentClipDir, sizeof(g_currentClipDir));

  if (!ensureDir(g_currentClipDir)) {
    Serial.println("ERROR: Failed to create clip directory.");
    return false;
  }

  g_frameCounter = 0;
  g_clipStartMs  = millis();

  Serial.printf("Started clip: %s\n", g_currentClipDir);
  return true;
}

static bool saveCurrentFrameAsJpg(camera_fb_t* fb) {
  if (!fb || fb->format != PIXFORMAT_JPEG) return false;

  char path[64];
  snprintf(path, sizeof(path), "%s/frame_%06lu.jpg", g_currentClipDir, (unsigned long)g_frameCounter);

  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;

  size_t written = f.write(fb->buf, fb->len);
  f.close();

  if (written != fb->len) {
    SD.remove(path);
    return false;
  }

  g_frameCounter++;
  return true;
}

// =========================
// TAR streaming (single-click download)
// =========================
static void tarOctal(char* out, size_t outLen, uint32_t val) {
  // tar numeric fields are octal, NUL or space terminated
  // ensure fixed width with leading zeros
  snprintf(out, outLen, "%0*o", (int)(outLen - 1), (unsigned)val);
}

static void tarChecksum(char* header) {
  // checksum field treated as spaces while computing
  memset(header + 148, ' ', 8);
  uint32_t sum = 0;
  for (int i = 0; i < 512; i++) sum += (uint8_t)header[i];
  // write checksum as octal, then NUL + space
  snprintf(header + 148, 8, "%06o", (unsigned)sum);
  header[154] = '\0';
  header[155] = ' ';
}

static bool tarSendHeader(httpd_req_t* req, const char* name, uint32_t size, char typeflag) {
  char h[512];
  memset(h, 0, sizeof(h));

  // name (100)
  strncpy(h, name, 100);

  // mode, uid, gid
  strncpy(h + 100, "0000777", 7);
  strncpy(h + 108, "0000000", 7);
  strncpy(h + 116, "0000000", 7);

  // size (12), mtime (12)
  tarOctal(h + 124, 12, size);
  tarOctal(h + 136, 12, (uint32_t)time(NULL));

  // typeflag
  h[156] = typeflag;

  // magic + version
  memcpy(h + 257, "ustar", 5);
  memcpy(h + 263, "00", 2);

  tarChecksum(h);

  return (httpd_resp_send_chunk(req, h, 512) == ESP_OK);
}

static bool tarSendFile(httpd_req_t* req, const char* tarName, const char* sdPath) {
  File f = SD.open(sdPath, FILE_READ);
  if (!f) return false;

  uint32_t size = (uint32_t)f.size();
  if (!tarSendHeader(req, tarName, size, '0')) {
    f.close();
    return false;
  }

  uint8_t buf[1024];
  uint32_t sent = 0;
  while (sent < size) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    if (httpd_resp_send_chunk(req, (const char*)buf, n) != ESP_OK) {
      f.close();
      return false;
    }
    sent += (uint32_t)n;
  }
  f.close();

  // pad to 512 boundary
  uint32_t pad = (512 - (size % 512)) % 512;
  if (pad) {
    char z[512];
    memset(z, 0, sizeof(z));
    if (httpd_resp_send_chunk(req, z, pad) != ESP_OK) return false;
  }

  return (sent == size);
}

// =========================
// Motion detection
// =========================
static bool setCameraMode(pixformat_t fmt, framesize_t fs, int quality) {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return false;

  if (s->set_framesize(s, fs) != 0) return false;
  if (s->set_pixformat(s, fmt) != 0) return false;

  if (fmt == PIXFORMAT_JPEG) {
    s->set_quality(s, quality);
  }

  // allow sensor settle
  delay(10);
  return true;
}

static bool motionCheckAndUpdate(uint32_t nowMs) {
  // Switch to grayscale small frame
  if (!setCameraMode(PIXFORMAT_GRAYSCALE, MOTION_FRAMESIZE, CAMERA_JPEG_QUALITY)) {
    // restore normal mode best-effort
    setCameraMode(PIXFORMAT_JPEG, CAMERA_FRAMESIZE, CAMERA_JPEG_QUALITY);
    return false;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    setCameraMode(PIXFORMAT_JPEG, CAMERA_FRAMESIZE, CAMERA_JPEG_QUALITY);
    return false;
  }

  bool motion = false;

  if (fb->format == PIXFORMAT_GRAYSCALE && fb->len > 0) {
    if (!g_prevGray) {
      g_prevGray = (uint8_t*)ps_malloc(fb->len);
      if (g_prevGray) {
        memcpy(g_prevGray, fb->buf, fb->len);
        g_prevGrayLen = fb->len;
      }
    } else if (g_prevGrayLen == fb->len) {
      // Compute mean absolute difference (fast)
      uint32_t sad = 0;
      const uint8_t* cur = fb->buf;
      const uint8_t* prev = g_prevGray;

      // sample every 2 bytes to reduce CPU (still works)
      for (size_t i = 0; i < fb->len; i += 2) {
        int d = (int)cur[i] - (int)prev[i];
        if (d < 0) d = -d;
        sad += (uint32_t)d;
      }

      uint32_t samples = (uint32_t)(fb->len / 2);
      uint32_t meanDiff = (samples > 0) ? (sad / samples) : 0;

      if (meanDiff >= MOTION_THRESHOLD) motion = true;

      // update baseline slowly (helps lighting changes)
      // blend: prev = (prev*3 + cur)/4
      for (size_t i = 0; i < fb->len; i += 2) {
        g_prevGray[i] = (uint8_t)((((uint16_t)g_prevGray[i]) * 3 + cur[i]) / 4);
      }
    } else {
      // length mismatch: re-init
      free(g_prevGray);
      g_prevGray = (uint8_t*)ps_malloc(fb->len);
      if (g_prevGray) {
        memcpy(g_prevGray, fb->buf, fb->len);
        g_prevGrayLen = fb->len;
      }
    }
  }

  esp_camera_fb_return(fb);

  // Restore normal JPEG mode
  setCameraMode(PIXFORMAT_JPEG, CAMERA_FRAMESIZE, CAMERA_JPEG_QUALITY);

  if (motion) {
    g_motionActiveUntil = nowMs + POST_MOTION_MS;
  }

  return motion;
}

// =========================
// Web UI (HTML)
// =========================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>XIAO ESP32-S3 Security Cam</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; margin:0; background:#111; color:#eee; text-align:center; }
    .wrap { padding:16px; max-width: 980px; margin: 0 auto; }
    h1 { margin: 12px 0 6px; font-size: 22px; }
    .card { background:#1b1b1b; border:1px solid #2a2a2a; border-radius:12px; padding:12px; margin:12px 0; }
    img { width:100%; max-width: 960px; border-radius:12px; border:1px solid #2a2a2a; }
    button {
      background:#2e7d32; color:#fff; border:0; border-radius:10px;
      padding:12px 14px; margin:6px; font-size:15px; cursor:pointer;
    }
    button.warn { background:#ef6c00; }
    button.stop { background:#c62828; }
    a { color:#90caf9; text-decoration:none; }
    .row { display:flex; flex-wrap:wrap; gap:8px; justify-content:center; }
    .status { font-size:14px; padding:10px; background:#0f0f0f; border-radius:10px; border:1px solid #2a2a2a; }
    code { color:#8bc34a; }
  </style>
</head>
<body>
  <div class="wrap">
    <h1>Security Camera</h1>

    <div class="card">
      <div class="status" id="status">Loading status...</div>
      <div style="margin-top:10px;">
        <img id="stream" src="/stream" alt="Live Stream">
      </div>
    </div>

    <div class="card">
      <div class="row">
        <button onclick="toggleRecording()">Start or Stop Recording</button>
        <button class="warn" onclick="protectClip()">Protect Current Clip</button>
        <button onclick="toggleMotion()">Toggle Motion</button>
        <button onclick="downloadCurrent()">Download Current Clip</button>
        <button onclick="openClips()">List Clips</button>
      </div>
      <div style="margin-top:10px; font-size:13px;">
        Stream URL: <code>/stream</code><br>
        Clips Browser: <code>/clips</code>
      </div>
    </div>
  </div>

<script>
async function refreshStatus(){
  try{
    const r = await fetch('/status');
    const j = await r.json();
    const s = document.getElementById('status');

    let text = '';
    text += 'Recording: ' + (j.recording ? 'ON' : 'OFF') + ' | ';
    text += 'Motion: ' + (j.motionEnabled ? 'ON' : 'OFF') + ' | ';
    text += 'Current clip: ' + j.clipDir + ' | ';
    text += 'Protected: ' + (j.protected ? 'YES' : 'NO') + ' | ';
    text += 'Frames in clip: ' + j.frames;

    s.textContent = text;
  }catch(e){
    document.getElementById('status').textContent = 'Status error';
  }
}

async function toggleRecording(){
  await fetch('/control?action=toggle');
  await refreshStatus();
}

async function protectClip(){
  if(confirm('Protect current clip from overwrite?')){
    await fetch('/control?action=protect');
    await refreshStatus();
  }
}

async function toggleMotion(){
  await fetch('/control?action=motion_toggle');
  await refreshStatus();
}

function downloadCurrent(){
  window.location.href = '/download?clip=current';
}

function openClips(){
  window.open('/clips', '_blank');
}

setInterval(refreshStatus, 2000);
refreshStatus();
</script>
</body>
</html>
)rawliteral";

// =========================
// HTTP handlers
// =========================
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req) {
  char json[320];
  bool prot = isClipProtected(g_currentClipDir);

  snprintf(json, sizeof(json),
           "{\"recording\":%s,\"motionEnabled\":%s,\"motionUntil\":%lu,"
           "\"clipIndex\":%u,\"clipDir\":\"%s\",\"protected\":%s,\"frames\":%lu,\"maxClips\":%u,\"motionThreshold\":%lu}",
           g_recordingEnabled ? "true" : "false",
           MOTION_ENABLED ? "true" : "false",
           (unsigned long)g_motionActiveUntil,
           (unsigned)g_currentClipIndex,
           g_currentClipDir,
           prot ? "true" : "false",
           (unsigned long)g_frameCounter,
           (unsigned)MAX_CLIPS,
           (unsigned long)MOTION_THRESHOLD);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t control_handler(httpd_req_t *req) {
  char buf[128] = {0};
  size_t len = httpd_req_get_url_query_len(req) + 1;

  if (len > 1 && len < sizeof(buf)) {
    httpd_req_get_url_query_str(req, buf, len);

    char action[32] = {0};
    if (httpd_query_key_value(buf, "action", action, sizeof(action)) == ESP_OK) {
      if (strcmp(action, "toggle") == 0) {
        g_recordingEnabled = !g_recordingEnabled;
        Serial.printf("Recording toggled: %s\n", g_recordingEnabled ? "ON" : "OFF");
      } else if (strcmp(action, "protect") == 0) {
        String p = String(g_currentClipDir) + "/PROTECT";
        File f = SD.open(p.c_str(), FILE_WRITE);
        if (f) {
          f.println("protected");
          f.close();
          Serial.printf("Protected clip: %s\n", g_currentClipDir);
        }
      } else if (strcmp(action, "motion_toggle") == 0) {
        MOTION_ENABLED = !MOTION_ENABLED;
        Serial.printf("Motion toggled: %s\n", MOTION_ENABLED ? "ON" : "OFF");
      } else if (strcmp(action, "motion_threshold") == 0) {
        char v[16] = {0};
        if (httpd_query_key_value(buf, "value", v, sizeof(v)) == ESP_OK) {
          uint32_t nv = (uint32_t)atoi(v);
          if (nv < 1) nv = 1;
          if (nv > 80) nv = 80;
          MOTION_THRESHOLD = nv;
          Serial.printf("Motion threshold set: %lu\n", (unsigned long)MOTION_THRESHOLD);
        }
      }
    }
  }

  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t file_handler(httpd_req_t *req) {
  // /file?path=/rec_000/frame_000001.jpg
  char query[160] = {0};
  size_t qlen = httpd_req_get_url_query_len(req) + 1;
  if (qlen < 2 || qlen >= sizeof(query)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
    return ESP_FAIL;
  }

  httpd_req_get_url_query_str(req, query, qlen);

  char path[140] = {0};
  if (httpd_query_key_value(query, "path", path, sizeof(path)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
    return ESP_FAIL;
  }

  if (!SD.exists(path)) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
  }

  File f = SD.open(path, FILE_READ);
  if (!f) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Open failed");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");

  uint8_t buf[1024];
  while (true) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    if (httpd_resp_send_chunk(req, (const char*)buf, n) != ESP_OK) {
      f.close();
      httpd_resp_sendstr_chunk(req, NULL);
      return ESP_FAIL;
    }
  }
  f.close();
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t download_handler(httpd_req_t* req) {
  // /download?clip=current OR /download?clip=/rec_000
  char query[96] = {0};
  size_t qlen = httpd_req_get_url_query_len(req) + 1;

  String clipDir;

  if (qlen > 1 && qlen < sizeof(query)) {
    httpd_req_get_url_query_str(req, query, qlen);
    char clip[40] = {0};
    if (httpd_query_key_value(query, "clip", clip, sizeof(clip)) == ESP_OK) {
      if (strcmp(clip, "current") == 0) {
        clipDir = String(g_currentClipDir);
      } else {
        clipDir = String(clip);
      }
    }
  }

  if (clipDir.length() == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing clip");
    return ESP_FAIL;
  }

  if (!clipDir.startsWith("/rec_")) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid clip");
    return ESP_FAIL;
  }

  if (!SD.exists(clipDir.c_str())) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Clip not found");
    return ESP_FAIL;
  }

  // Content disposition for download
  String fname = clipDir.substring(1) + ".tar"; // "rec_000.tar"
  String disp = "attachment; filename=\"" + fname + "\"";

  httpd_resp_set_type(req, "application/x-tar");
  httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());

  File dir = SD.open(clipDir.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Open dir failed");
    return ESP_FAIL;
  }

  // Stream all files in the clip directory into tar
  File e;
  while ((e = dir.openNextFile())) {
    if (!e.isDirectory()) {
      String nm = e.name();
      e.close();

      // create tar path like "rec_000/frame_000001.jpg"
      String tarName = clipDir.substring(1) + "/" + nm;
      String sdPath  = clipDir + "/" + nm;

      if (!tarSendFile(req, tarName.c_str(), sdPath.c_str())) {
        dir.close();
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_FAIL;
      }
    } else {
      e.close();
    }
  }
  dir.close();

  // Two 512-byte zero blocks = end of tar
  char z[512];
  memset(z, 0, sizeof(z));
  if (httpd_resp_send_chunk(req, z, 512) != ESP_OK) return ESP_FAIL;
  if (httpd_resp_send_chunk(req, z, 512) != ESP_OK) return ESP_FAIL;

  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t clips_handler(httpd_req_t *req) {
  String html;
  html.reserve(6000);
  html += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta charset='utf-8'><title>Clips</title>";
  html += "<style>body{font-family:Arial;background:#111;color:#eee;padding:16px}";
  html += "a{color:#90caf9;text-decoration:none} .clip{padding:12px;border:1px solid #2a2a2a;border-radius:10px;margin:10px 0;background:#1b1b1b}";
  html += "</style></head><body>";
  html += "<h2>Saved Clips on SD</h2>";
  html += "<p>Click frames to open. Use Download to grab the entire clip as a TAR.</p>";

  for (uint8_t i = 0; i < MAX_CLIPS; i++) {
    char dirName[16];
    makeClipDirName(i, dirName, sizeof(dirName));
    if (!SD.exists(dirName)) continue;

    bool prot = isClipProtected(dirName);

    html += "<div class='clip'>";
    html += "<b>";
    html += dirName;
    html += "</b>";
    if (prot) html += " [PROTECTED]";
    html += "<br>";
    html += "<a href='/download?clip=";
    html += dirName;
    html += "'>Download</a><br><br>";

    File dir = SD.open(dirName);
    if (dir && dir.isDirectory()) {
      int shown = 0;
      File e;
      while ((e = dir.openNextFile())) {
        if (!e.isDirectory()) {
          String nm = e.name();
          if (nm.endsWith(".jpg")) {
            String full = String(dirName) + "/" + nm;
            html += "<a href='/file?path=" + full + "' target='_blank'>" + nm + "</a><br>";
            shown++;
            if (shown >= 50) {
              html += "<i>Showing first 50 frames...</i><br>";
              e.close();
              break;
            }
          }
        }
        e.close();
      }
      dir.close();
    } else {
      if (dir) dir.close();
      html += "Unable to open directory.<br>";
    }

    html += "</div>";
  }

  html += "<p><a href='/'>Back</a></p>";
  html += "</body></html>";

  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html.c_str(), html.length());
}

static esp_err_t stream_handler(httpd_req_t *req) {
  static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
  static const char* _STREAM_BOUNDARY = "\r\n--frame\r\n";
  static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

  httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);

  char partBuf[64];

  while (true) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return ESP_FAIL;

    size_t hlen = (size_t)snprintf(partBuf, sizeof(partBuf), _STREAM_PART, (unsigned)fb->len);

    if (httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY)) != ESP_OK) {
      esp_camera_fb_return(fb);
      return ESP_FAIL;
    }
    if (httpd_resp_send_chunk(req, partBuf, hlen) != ESP_OK) {
      esp_camera_fb_return(fb);
      return ESP_FAIL;
    }
    if (httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len) != ESP_OK) {
      esp_camera_fb_return(fb);
      return ESP_FAIL;
    }

    esp_camera_fb_return(fb);
    vTaskDelay(1);
  }

  return ESP_OK;
}

static void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;

  if (httpd_start(&httpd, &config) != ESP_OK) {
    Serial.println("ERROR: httpd_start failed");
    return;
  }

  httpd_uri_t uri_index    = { .uri="/",        .method=HTTP_GET, .handler=index_handler,   .user_ctx=NULL };
  httpd_uri_t uri_status   = { .uri="/status",  .method=HTTP_GET, .handler=status_handler,  .user_ctx=NULL };
  httpd_uri_t uri_control  = { .uri="/control", .method=HTTP_GET, .handler=control_handler, .user_ctx=NULL };
  httpd_uri_t uri_stream   = { .uri="/stream",  .method=HTTP_GET, .handler=stream_handler,  .user_ctx=NULL };
  httpd_uri_t uri_clips    = { .uri="/clips",   .method=HTTP_GET, .handler=clips_handler,   .user_ctx=NULL };
  httpd_uri_t uri_file     = { .uri="/file",    .method=HTTP_GET, .handler=file_handler,    .user_ctx=NULL };
  httpd_uri_t uri_download = { .uri="/download",.method=HTTP_GET, .handler=download_handler,.user_ctx=NULL };

  httpd_register_uri_handler(httpd, &uri_index);
  httpd_register_uri_handler(httpd, &uri_status);
  httpd_register_uri_handler(httpd, &uri_control);
  httpd_register_uri_handler(httpd, &uri_stream);
  httpd_register_uri_handler(httpd, &uri_clips);
  httpd_register_uri_handler(httpd, &uri_file);
  httpd_register_uri_handler(httpd, &uri_download);

  Serial.println("Web server started on port 80");
}

// =========================
// Camera init
// =========================
static bool initCamera() {
  camera_config_t config;
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

  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  config.frame_size   = CAMERA_FRAMESIZE;
  config.jpeg_quality = CAMERA_JPEG_QUALITY;
  config.fb_count     = CAMERA_FB_COUNT;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("ERROR: esp_camera_init failed: 0x%x\n", err);
    return false;
  }
  return true;
}

// =========================
// Arduino setup/loop
// =========================
void setup() {
  Serial.begin(115200);
  delay(250);

  Serial.println();
  Serial.println("XIAO ESP32-S3 Sense Security Cam Starting...");

  // SD init (SPI)
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial.println("ERROR: SD.begin failed. Use FAT32 and verify CS pin/wiring.");
    while (true) delay(1000);
  }
  Serial.println("SD card mounted.");

  // Camera init
  if (!initCamera()) {
    Serial.println("ERROR: Camera init failed.");
    while (true) delay(1000);
  }
  Serial.println("Camera initialized.");

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());

  // Start recording first clip
  g_clipStartMs = millis();
  g_lastFrameMs = millis();
  startNewClip(0);

  // Start web server
  startWebServer();

  Serial.println("Open:");
  Serial.print("  UI:     http://");
  Serial.print(WiFi.localIP());
  Serial.println("/");
  Serial.print("  Stream: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/stream");
  Serial.print("  Clips:  http://");
  Serial.print(WiFi.localIP());
  Serial.println("/clips");
}

void loop() {
  const uint32_t now = millis();

  // Motion check
  if (MOTION_ENABLED && (uint32_t)(now - g_lastMotionCheckMs) >= MOTION_CHECK_MS) {
    motionCheckAndUpdate(now);
    g_lastMotionCheckMs = now;
  }

  // Determine whether we should write frames
  bool shouldWriteFrames = g_recordingEnabled;
  if (MOTION_ENABLED) {
    // record only when motion is active (or in hold window)
    shouldWriteFrames = shouldWriteFrames && ((int32_t)(g_motionActiveUntil - now) > 0);
  }

  // Rotate clips by time only while actively writing frames (prevents empty clip spam)
  if (shouldWriteFrames && (uint32_t)(now - g_clipStartMs) >= CLIP_DURATION_MS) {
    uint8_t next = (uint8_t)((g_currentClipIndex + 1) % MAX_CLIPS);
    if (!startNewClip(next)) {
      g_clipStartMs = now; // prevent rapid retry
    }
  }

  // Write a frame periodically only if we should
  if (shouldWriteFrames && (uint32_t)(now - g_lastFrameMs) >= FRAME_INTERVAL_MS) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      if (!saveCurrentFrameAsJpg(fb)) {
        Serial.println("WARN: Failed to save frame to SD.");
      }
      esp_camera_fb_return(fb);
    } else {
      Serial.println("WARN: Camera frame capture failed.");
    }
    g_lastFrameMs = now;
  }

  delay(1);
}
