#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include <SPI.h>
#include <SD.h>

// =========================
// USER SETTINGS
// =========================
const char* ssid     = "Yasin Family Home";
const char* password = "September301993";

// Recording behavior
static const uint32_t CLIP_DURATION_MS   = 60UL * 1000UL;  // 60s per clip folder
static const uint8_t  MAX_CLIPS          = 10;             // keep last N clip folders (unless protected)
static const uint32_t FRAME_INTERVAL_MS  = 100;            // 10 fps (approx)

// Camera settings
static const framesize_t CAMERA_FRAMESIZE = FRAMESIZE_QVGA; // 320x240
static const int CAMERA_JPEG_QUALITY      = 15;             // lower = better quality/larger file (10-20 typical)
static const int CAMERA_FB_COUNT          = 2;

// SD (SPI) pins for XIAO ESP32-S3 (as you provided)
#define SD_CS    21
#define SD_MOSI  9
#define SD_MISO  8
#define SD_SCK   7

// =========================
// XIAO ESP32-S3 Sense camera pins (as you provided)
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

static char g_currentClipDir[16] = {0}; // "/rec_000"

// =========================
// Helpers: SD / filesystem
// =========================
static void makeClipDirName(uint8_t idx, char* out, size_t outLen) {
  // out like "/rec_000"
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

static bool deleteFileIfExists(const char* path) {
  if (SD.exists(path)) {
    return SD.remove(path);
  }
  return true;
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

  // remove the directory itself
  // ESP32 SD FS typically supports rmdir
  return SD.rmdir(dirPath);
}

static bool deleteOldestUnprotectedClipToMakeRoom(uint8_t targetIndexToOverwrite) {
  // When we're about to create clip folder for targetIndexToOverwrite,
  // delete that folder if it exists AND is not protected.
  char dirName[16];
  makeClipDirName(targetIndexToOverwrite, dirName, sizeof(dirName));

  if (!SD.exists(dirName)) return true;

  if (isClipProtected(dirName)) {
    // Can't delete; try to find some other unprotected clip to delete.
    // Scan all clips and delete the first unprotected one we find (oldest-ish).
    for (uint8_t i = 0; i < MAX_CLIPS; i++) {
      char scanDir[16];
      makeClipDirName(i, scanDir, sizeof(scanDir));
      if (SD.exists(scanDir) && !isClipProtected(scanDir)) {
        return deleteDirectoryRecursive(scanDir);
      }
    }
    // All protected
    return false;
  }

  return deleteDirectoryRecursive(dirName);
}

static bool startNewClip(uint8_t newIndex) {
  // Ensure we have room (delete something unprotected if needed)
  if (!deleteOldestUnprotectedClipToMakeRoom(newIndex)) {
    Serial.println("ERROR: SD full or all clips protected; cannot rotate.");
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

  // Filename: /rec_000/frame_000001.jpg
  char path[64];
  snprintf(path, sizeof(path), "%s/frame_%06lu.jpg", g_currentClipDir, (unsigned long)g_frameCounter);

  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;

  size_t written = f.write(fb->buf, fb->len);
  f.close();

  if (written != fb->len) {
    deleteFileIfExists(path);
    return false;
  }

  g_frameCounter++;
  return true;
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
        <button class="stop" onclick="resumeLoop()">Resume Loop Mode</button>
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
    if(j.recording){
      text += 'Recording: ON | ';
    }else{
      text += 'Recording: OFF | ';
    }

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

async function resumeLoop(){
  if(confirm('Resume loop recording mode (unprotect future overwrites)?')){
    await fetch('/control?action=resume');
    await refreshStatus();
  }
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
  char json[256];
  bool prot = isClipProtected(g_currentClipDir);

  snprintf(json, sizeof(json),
           "{\"recording\":%s,\"clipIndex\":%u,\"clipDir\":\"%s\",\"protected\":%s,\"frames\":%lu,\"maxClips\":%u}",
           g_recordingEnabled ? "true" : "false",
           (unsigned)g_currentClipIndex,
           g_currentClipDir,
           prot ? "true" : "false",
           (unsigned long)g_frameCounter,
           (unsigned)MAX_CLIPS);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t control_handler(httpd_req_t *req) {
  char buf[96] = {0};
  size_t len = httpd_req_get_url_query_len(req) + 1;

  if (len > 1 && len < sizeof(buf)) {
    httpd_req_get_url_query_str(req, buf, len);

    char action[24] = {0};
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
      } else if (strcmp(action, "resume") == 0) {
        // "resume" here simply means: do not create PROTECT automatically.
        // Existing protected clips remain protected until you delete PROTECT manually.
        Serial.println("Resume loop mode requested (no automatic protection).");
      }
    }
  }

  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t file_handler(httpd_req_t *req) {
  // /file?path=/rec_000/frame_000001.jpg
  char query[128] = {0};
  size_t qlen = httpd_req_get_url_query_len(req) + 1;
  if (qlen < 2 || qlen >= sizeof(query)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
    return ESP_FAIL;
  }

  httpd_req_get_url_query_str(req, query, qlen);

  char path[120] = {0};
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

  // Content type (only jpg supported here)
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

static esp_err_t clips_handler(httpd_req_t *req) {
  // Simple HTML listing of clip folders + frame links
  String html;
  html.reserve(4096);
  html += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta charset='utf-8'><title>Clips</title>";
  html += "<style>body{font-family:Arial;background:#111;color:#eee;padding:16px}";
  html += "a{color:#90caf9;text-decoration:none} .clip{padding:12px;border:1px solid #2a2a2a;border-radius:10px;margin:10px 0;background:#1b1b1b}";
  html += "</style></head><body>";
  html += "<h2>Saved Clips on SD</h2>";
  html += "<p>Click a clip to view frames. Protected clips are marked.</p>";

  // list /rec_### directories
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

    // list up to first ~50 frames as links (you can scroll more by editing)
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
    if (!fb) {
      Serial.println("Camera capture failed");
      return ESP_FAIL;
    }

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

    // Small delay to avoid starving other tasks
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

  httpd_uri_t uri_index = { .uri="/", .method=HTTP_GET, .handler=index_handler, .user_ctx=NULL };
  httpd_uri_t uri_status = { .uri="/status", .method=HTTP_GET, .handler=status_handler, .user_ctx=NULL };
  httpd_uri_t uri_control = { .uri="/control", .method=HTTP_GET, .handler=control_handler, .user_ctx=NULL };
  httpd_uri_t uri_stream = { .uri="/stream", .method=HTTP_GET, .handler=stream_handler, .user_ctx=NULL };
  httpd_uri_t uri_clips = { .uri="/clips", .method=HTTP_GET, .handler=clips_handler, .user_ctx=NULL };
  httpd_uri_t uri_file  = { .uri="/file",  .method=HTTP_GET, .handler=file_handler,  .user_ctx=NULL };

  httpd_register_uri_handler(httpd, &uri_index);
  httpd_register_uri_handler(httpd, &uri_status);
  httpd_register_uri_handler(httpd, &uri_control);
  httpd_register_uri_handler(httpd, &uri_stream);
  httpd_register_uri_handler(httpd, &uri_clips);
  httpd_register_uri_handler(httpd, &uri_file);

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
    Serial.println("ERROR: SD.begin failed. Check wiring, card format (FAT32), and CS pin.");
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
  if (!g_recordingEnabled) {
    delay(50);
    return;
  }

  const uint32_t now = millis();

  // Rotate clip by duration
  if ((uint32_t)(now - g_clipStartMs) >= CLIP_DURATION_MS) {
    uint8_t next = (uint8_t)((g_currentClipIndex + 1) % MAX_CLIPS);

    // If we cannot rotate (all protected or SD full), keep writing into current clip
    if (!startNewClip(next)) {
      // Keep current clip, just reset timer so we don't spam rotate attempts
      g_clipStartMs = now;
    }
  }

  // Capture frame periodically
  if ((uint32_t)(now - g_lastFrameMs) >= FRAME_INTERVAL_MS) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      // Save JPEG frame to SD
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
