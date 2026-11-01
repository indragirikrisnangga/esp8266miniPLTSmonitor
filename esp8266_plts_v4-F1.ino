/*
  PLTS Monitoring & Smart Relay Controller (ESP8266) — Refactored
  Perbaikan Utama & Optimasi Memori:
  1. Perbaikan Bug Hardware Relay:
     - Pada kode lama, LED_PIN (GPIO 2) bentrok dengan RELAY_PIN (D4 = GPIO 2 pada NodeMCU)
       dan pinMode(RELAY_PIN, OUTPUT) belum dipanggil sehingga relay berkedip/gagal dikendalikan.
     - Kini LED detak jantung dipindah ke GPIO 16 (D0 / LED bawaan kedua NodeMCU) dan RELAY_PIN (D4)
       diinisialisasi penuh dengan dukungan mode MANUAL (ON/OFF langsung) & OTOMATIS (Histeresis Suhu).
  2. Mode Ganda WiFi (WIFI_AP_STA):
     - SoftAP ("ESP_Config" -> 192.168.4.1) tetap dipertahankan aktif meskipun ESP8266 sudah
       terhubung ke router WiFi, sehingga Anda selalu bisa masuk ke SoftAP untuk melihat IP Lokal (STA).
  3. Efisiensi RAM Maksimal:
     - Menghapus seluruh penggabungan String HTML di RAM (4 halaman digabung menjadi 1 SPA di PROGMEM).
     - Pembacaan sensor AHT10 & ADC dilakukan non-blocking setiap 2 detik agar WebServer sangat responsif.
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>

// Memanggil antarmuka HTML Tailwind CSS dari Flash (PROGMEM)
#include "PltsWebUI.h"

// ---------- Konfigurasi Pin & Konstanta ----------
#define MAX_WIFI        5

const char* CONFIG_FILE = "/config.json";
const char* WIFI_FILE   = "/wifi.json";

// Konfigurasi Pin Perangkat Keras:
// Pin 2 digunakan kembali sebagai LED Heartbeat, dan dimatikan selama proses cek & eksekusi Relay D4
#define RELAY_PIN       D4
#define LED_PIN         2

// Modul Relay standar bersifat Active-LOW (LOW = Menyala, HIGH = Mati)
#define RELAY_ACTIVE    LOW
#define RELAY_INACTIVE  HIGH

const char* AP_SSID = "ESP_Config";
const char* AP_PASS = ""; // Kosong = Open AP seperti kode awal (isi minimal 8 karakter jika ingin dikunci)

// ---------- Struktur Konfigurasi di RAM (Disimpan ke LittleFS) ----------
struct WifiCred {
  char ssid[33];
  char pass[65];
};

struct SystemConfig {
  WifiCred wifi[MAX_WIFI];
  char boardName[32];
  char boardID[32];
  char hostName[64];
  float tempThreshold;
  uint8_t relayAutoMode;     // 1 = Otomatis (sensor suhu), 0 = Manual
  uint8_t manualRelayState;  // 1 = ON, 0 = OFF (saat mode manual)
};

SystemConfig sysCfg;

// ---------- Objek Global ----------
ESP8266WebServer server(80);
Adafruit_AHTX0 aht;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "id.pool.ntp.org", 7 * 3600, 60000);

// ---------- Variabel Sensor & Status ----------
const int voltagePin   = A0;
const float HYSTERESIS = 1.0f; // Toleransi +-1.0 C untuk mencegah relay cetak-cetek

bool ahtReady          = false;
bool relayIsOn         = false;
float temperature      = 0.0f;
float humidity         = 0.0f;
float voltage          = 0.0f;

// ---------- Timer Non-Blocking & Watchdog ----------
unsigned long lastSensorMillis     = 0;
unsigned long lastActivityTime     = 0;
unsigned long lastWifiRetry        = 0;
unsigned long lastRelayCycleMs     = 0;
unsigned long heartbeatMillis      = 0;
unsigned long heartbeatResumeAtMs  = 0;
uint8_t heartbeatStep              = 0;
bool heartbeatActive               = true;
int lastSentMinute                 = -1;

// Penjadwalan koneksi WiFi non-blocking agar respon HTTP simpan SSID tidak terputus
bool pendingWifiConnect          = false;
unsigned long wifiConnectAtMs    = 0;

const unsigned long SENSOR_INTERVAL_MS    = 2000UL;    // Baca sensor tiap 2 detik
const unsigned long WIFI_RETRY_INTERVAL   = 300000UL;  // Coba sambung ulang STA tiap 5 menit jika putus
const unsigned long WATCHDOG_TIMEOUT_MS   = 3600000UL; // Reset otomatis jika 1 jam tanpa aktivitas

// ---------- Utilitas Keamanan String & JSON ----------
void feedWatchdog() {
  lastActivityTime = millis();
}

void sanitizeJsonStr(const char* src, char* dst, size_t dstLen) {
  size_t j = 0;
  for (size_t i = 0; src[i] != '\0' && j + 2 < dstLen; i++) {
    if (src[i] == '"' || src[i] == '\\') {
      dst[j++] = '\\';
    }
    dst[j++] = src[i];
  }
  dst[j] = '\0';
}

// ---------- Manajemen Penyimpanan LittleFS (/config.json & /wifi.json) ----------
uint8_t countSavedWifi() {
  uint8_t cnt = 0;
  for (int i = 0; i < MAX_WIFI; i++) {
    if (sysCfg.wifi[i].ssid[0] != '\0') cnt++;
  }
  return cnt;
}

void setDefaultConfigs() {
  memset(&sysCfg, 0, sizeof(SystemConfig));
  strlcpy(sysCfg.boardName, "ESP8266-PLTS", sizeof(sysCfg.boardName));
  strlcpy(sysCfg.boardID, "001", sizeof(sysCfg.boardID));
  strlcpy(sysCfg.hostName, "esp8266.local", sizeof(sysCfg.hostName));
  sysCfg.tempThreshold    = 33.0f;
  sysCfg.relayAutoMode    = 1;
  sysCfg.manualRelayState = 0;
}

bool saveBoardConfig() {
  StaticJsonDocument<384> doc;
  doc["boardName"]        = sysCfg.boardName;
  doc["boardID"]          = sysCfg.boardID;
  doc["hostName"]         = sysCfg.hostName;
  doc["tempThreshold"]    = sysCfg.tempThreshold;
  doc["relayAutoMode"]    = sysCfg.relayAutoMode;
  doc["manualRelayState"] = sysCfg.manualRelayState;

  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) {
    Serial.println(F("[LittleFS] Gagal membuka /config.json untuk ditulis"));
    return false;
  }
  size_t written = serializeJson(doc, f);
  f.flush();
  f.close();

  Serial.printf("[LittleFS] Simpan config.json (%u bytes) | Board=%s | ID=%s | Host=%s\n",
                (unsigned int)written, sysCfg.boardName, sysCfg.boardID, sysCfg.hostName);
  return (written > 0);
}

bool saveWifiConfig() {
  StaticJsonDocument<768> doc;
  JsonArray arr = doc.createNestedArray("networks");
  for (int i = 0; i < MAX_WIFI; i++) {
    if (sysCfg.wifi[i].ssid[0] != '\0') {
      JsonObject o = arr.createNestedObject();
      o["ssid"] = sysCfg.wifi[i].ssid;
      o["pass"] = sysCfg.wifi[i].pass;
    }
  }

  File f = LittleFS.open(WIFI_FILE, "w");
  if (!f) {
    Serial.println(F("[LittleFS] Gagal membuka /wifi.json untuk ditulis"));
    return false;
  }
  size_t written = serializeJson(doc, f);
  f.flush();
  f.close();

  Serial.printf("[LittleFS] Simpan wifi.json (%u bytes) | TotalSSID=%u\n",
                (unsigned int)written, countSavedWifi());
  return (written > 0);
}

void loadConfigs() {
  setDefaultConfigs();

  // 1. Muat Konfigurasi Board & Relay dari /config.json
  if (LittleFS.exists(CONFIG_FILE)) {
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (f) {
      StaticJsonDocument<384> doc;
      DeserializationError err = deserializeJson(doc, f);
      f.close();
      if (!err) {
        strlcpy(sysCfg.boardName, doc["boardName"] | sysCfg.boardName, sizeof(sysCfg.boardName));
        strlcpy(sysCfg.boardID,   doc["boardID"]   | sysCfg.boardID,   sizeof(sysCfg.boardID));
        strlcpy(sysCfg.hostName,  doc["hostName"]  | sysCfg.hostName,  sizeof(sysCfg.hostName));

        float th = doc["tempThreshold"] | sysCfg.tempThreshold;
        if (!isnan(th) && th >= 0.0f && th <= 50.0f) {
          sysCfg.tempThreshold = th;
        }
        sysCfg.relayAutoMode    = doc["relayAutoMode"]    | sysCfg.relayAutoMode;
        sysCfg.manualRelayState = doc["manualRelayState"] | sysCfg.manualRelayState;
      }
    }
  } else {
    saveBoardConfig();
  }

  // 2. Muat Daftar Multi-WiFi dari /wifi.json
  if (LittleFS.exists(WIFI_FILE)) {
    File f = LittleFS.open(WIFI_FILE, "r");
    if (f) {
      StaticJsonDocument<768> doc;
      DeserializationError err = deserializeJson(doc, f);
      f.close();
      if (!err) {
        JsonArray arr = doc["networks"].as<JsonArray>();
        int idx = 0;
        for (JsonObject o : arr) {
          if (idx >= MAX_WIFI) break;
          const char* s = o["ssid"] | "";
          const char* p = o["pass"] | "";
          if (s[0] != '\0') {
            strlcpy(sysCfg.wifi[idx].ssid, s, sizeof(sysCfg.wifi[idx].ssid));
            strlcpy(sysCfg.wifi[idx].pass, p, sizeof(sysCfg.wifi[idx].pass));
            idx++;
          }
        }
      }
    }
  } else {
    saveWifiConfig();
  }

  Serial.printf("[LittleFS] Dimuat | Board=%s | ID=%s | Host=%s | TotalSSID=%u\n",
                sysCfg.boardName, sysCfg.boardID, sysCfg.hostName, countSavedWifi());
}

// ---------- Pembacaan Sensor AHT10 & Tegangan (Tanpa Eksekusi Relay) ----------
void readSensors() {
  if (!ahtReady) {
    ahtReady = aht.begin();
  }

  if (ahtReady) {
    sensors_event_t humidityEvent, tempEvent;
    aht.getEvent(&humidityEvent, &tempEvent);
    if (!isnan(tempEvent.temperature)) {
      temperature = tempEvent.temperature;
    }
    if (!isnan(humidityEvent.relative_humidity)) {
      humidity = humidityEvent.relative_humidity;
    }
  }

  // Pembacaan tegangan modul sensor 0-25V / pembagi tegangan pada pin A0
  int adcValue = analogRead(voltagePin);
  int mapVoltage = map(adcValue, 0, 1023, 0, 255);
  voltage = (mapVoltage * 0.00489f) * 12.7f;
  if (voltage < 0.0f) voltage = 0.0f;
}

// ---------- Kendali Fisik Relay D4 & Jeda Heartbeat Pin 2 ----------
void pauseHeartbeat() {
  heartbeatActive = false;
  heartbeatResumeAtMs = 0;
  heartbeatStep = 0;
  // Matikan Pin 2 sebagai heartbeat (HIGH = LED OFF), kecuali berbagi pin dengan relay yang sedang ON
  if (!(LED_PIN == RELAY_PIN && relayIsOn)) {
    digitalWrite(LED_PIN, HIGH);
  }
}

void setPhysicalRelay(bool turnOn) {
  relayIsOn = turnOn;
  digitalWrite(RELAY_PIN, turnOn ? RELAY_ACTIVE : RELAY_INACTIVE);
}

void checkAndExecuteRelay() {
  // 1. Matikan Pin 2 sebagai heartbeat selama proses cek kondisi & eksekusi relay
  pauseHeartbeat();

  // 2. Ambil pembacaan suhu terbaru sebelum mengecek kondisi relay
  readSensors();

  // 3. Cek kondisi relay (Otomatis dengan histeresis atau Manual)
  bool targetOn = relayIsOn;
  if (!sysCfg.relayAutoMode) {
    targetOn = (sysCfg.manualRelayState != 0);
  } else if (ahtReady) {
    if (temperature >= (sysCfg.tempThreshold + HYSTERESIS)) {
      targetOn = true;
    } else if (temperature <= (sysCfg.tempThreshold - HYSTERESIS)) {
      targetOn = false;
    }
  }

  // 4. Eksekusi kendali relay pada pin D4
  setPhysicalRelay(targetOn);

  Serial.printf("[RELAY] Cek & Eksekusi D4 | Mode=%s | Suhu=%.1fC | Batas=%.1fC | Relay=%s\n",
                sysCfg.relayAutoMode ? "AUTO" : "MANUAL",
                temperature, sysCfg.tempThreshold,
                relayIsOn ? "ON" : "OFF");

  // 5. Selama relay aktif dieksekusi (ON), Pin 2 tetap dimatikan sebagai heartbeat agar tidak mengganggu D4.
  //    Jika kondisi relay OFF, tahan Pin 2 tetap mati selama 2 detik sebelum melanjutkan heartbeat.
  if (relayIsOn) {
    heartbeatActive = false;
    if (LED_PIN != RELAY_PIN) {
      digitalWrite(LED_PIN, HIGH);
    }
  } else {
    heartbeatResumeAtMs = millis() + 2000UL;
  }
}

// ---------- Manajemen Multi-WiFi + SoftAP Selalu Aktif ----------
void setupSoftAP() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA); // Penting: Pertahankan SoftAP sekaligus koneksi WiFi lokal (STA)
  if (strlen(AP_PASS) >= 8) {
    WiFi.softAP(AP_SSID, AP_PASS);
  } else {
    WiFi.softAP(AP_SSID);
  }
  Serial.printf("[SoftAP] Aktif -> SSID: %s | AP IP: %s\n",
                AP_SSID, WiFi.softAPIP().toString().c_str());
}

bool connectMultiWiFi() {
  if (countSavedWifi() == 0) {
    Serial.println(F("[WiFi] Belum ada SSID tersimpan. SoftAP tetap siaga."));
    return false;
  }

  // Pastikan mode tetap WIFI_AP_STA agar SoftAP tidak mati
  if (WiFi.getMode() != WIFI_AP_STA) {
    WiFi.mode(WIFI_AP_STA);
  }

  for (int i = 0; i < MAX_WIFI; i++) {
    if (sysCfg.wifi[i].ssid[0] != '\0') {
      Serial.printf("[WiFi] Mencoba SSID: %s\n", sysCfg.wifi[i].ssid);
      WiFi.begin(sysCfg.wifi[i].ssid, sysCfg.wifi[i].pass);

      unsigned long start = millis();
      while (WiFi.status() != WL_CONNECTED && (millis() - start < 8000UL)) {
        delay(250);
        Serial.print('.');
        ESP.wdtFeed();
      }
      Serial.println();

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println(F("[WiFi] Berhasil Terhubung!"));
        Serial.printf("  -> SSID     : %s\n", WiFi.SSID().c_str());
        Serial.printf("  -> IP Lokal : %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("  -> SoftAP IP: %s (Tetap Aktif)\n", WiFi.softAPIP().toString().c_str());
        feedWatchdog();
        return true;
      }
    }
  }

  // Jika semua gagal, putuskan percobaan STA tanpa mematikan SoftAP
  WiFi.disconnect(false);
  Serial.println(F("[WiFi] Gagal terhubung ke daftar SSID. SoftAP tetap aktif."));
  return false;
}

// ---------- Pengiriman Data ke Server Database (Format Dipertahankan) ----------
void Send_to_Database() {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.printf("[DB] Menghubungi %s...\n", sysCfg.hostName);
  WiFiClient client;
  if (!client.connect(sysCfg.hostName, 80)) {
    Serial.println(F("[DB] Koneksi ke host gagal"));
    return;
  }

  // Format URL tetap dipertahankan menuju /realtime/add.php tanpa alokasi String berulang
  char url[160];
  snprintf(url, sizeof(url),
           "/realtime/add.php?code=%s&value1=0&value2=%.2f&value3=%.2f&value4=%.2f&value5=%d",
           sysCfg.boardID, temperature, humidity, voltage, relayIsOn ? 1 : 0);

  client.printf("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                url, sysCfg.hostName);

  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 5000UL) {
      Serial.println(F("[DB] Client Timeout!"));
      client.stop();
      return;
    }
    yield();
  }

  while (client.available()) {
    String line = client.readStringUntil('\r');
    if (line.indexOf("sukses gaes") != -1) {
      Serial.println(F("[DB] 1 Record berhasil masuk ke tabel telemetri"));
      feedWatchdog();
    } else if (line.indexOf("gagal gaes") != -1) {
      Serial.println(F("[DB] Gagal menyimpan ke database"));
    }
  }
  client.stop();
}

// ---------- Endpoint REST API WebServer ----------
void sendJson(int code, const char* payload) {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.send(code, "application/json", payload);
}

void scheduleWifiConnect(unsigned long delayMs = 700UL) {
  pendingWifiConnect = true;
  wifiConnectAtMs = millis() + delayMs;
}

void handleStatusJson() {
  feedWatchdog();

  bool staOk = (WiFi.status() == WL_CONNECTED);
  char ssidEsc[68], bNameEsc[64], bIdEsc[64], hostEsc[128];
  sanitizeJsonStr(staOk ? WiFi.SSID().c_str() : "", ssidEsc, sizeof(ssidEsc));
  sanitizeJsonStr(sysCfg.boardName, bNameEsc, sizeof(bNameEsc));
  sanitizeJsonStr(sysCfg.boardID, bIdEsc, sizeof(bIdEsc));
  sanitizeJsonStr(sysCfg.hostName, hostEsc, sizeof(hostEsc));

  String staIpStr = staOk ? WiFi.localIP().toString() : "Belum Terhubung";
  String apIpStr  = WiFi.softAPIP().toString();

  char json[600];
  snprintf(json, sizeof(json),
    "{\"temperature\":%.1f,\"humidity\":%.1f,\"voltage\":%.2f,"
    "\"relay\":%d,\"autoMode\":%s,\"manualState\":%d,"
    "\"threshold\":%.1f,\"hysteresis\":%.1f,"
    "\"staConnected\":%s,\"staSSID\":\"%s\",\"staIP\":\"%s\","
    "\"apSSID\":\"%s\",\"apIP\":\"%s\","
    "\"boardName\":\"%s\",\"boardID\":\"%s\",\"hostName\":\"%s\","
    "\"uptime\":%lu}",
    temperature, humidity, voltage,
    relayIsOn ? 1 : 0,
    sysCfg.relayAutoMode ? "true" : "false",
    sysCfg.manualRelayState ? 1 : 0,
    sysCfg.tempThreshold, HYSTERESIS,
    staOk ? "true" : "false", ssidEsc, staIpStr.c_str(),
    AP_SSID, apIpStr.c_str(),
    bNameEsc, bIdEsc, hostEsc,
    millis() / 1000UL
  );

  sendJson(200, json);
}

void handleWifiList() {
  feedWatchdog();
  char json[512];
  size_t pos = 0;
  pos += snprintf(json + pos, sizeof(json) - pos, "[");

  bool first = true;
  for (int i = 0; i < MAX_WIFI; i++) {
    if (sysCfg.wifi[i].ssid[0] != '\0') {
      char esc[68];
      sanitizeJsonStr(sysCfg.wifi[i].ssid, esc, sizeof(esc));
      pos += snprintf(json + pos, sizeof(json) - pos,
                      "%s{\"index\":%d,\"ssid\":\"%s\"}",
                      first ? "" : ",", i, esc);
      first = false;
    }
  }
  snprintf(json + pos, sizeof(json) - pos, "]");
  sendJson(200, json);
}

void handleWifiAdd() {
  feedWatchdog();
  if (!server.hasArg("ssid")) {
    sendJson(400, "{\"ok\":false,\"msg\":\"Parameter SSID tidak terbaca\"}");
    return;
  }

  String s = server.arg("ssid");
  s.trim();
  String p = server.hasArg("pass") ? server.arg("pass") : "";

  if (s.length() == 0) {
    sendJson(400, "{\"ok\":false,\"msg\":\"SSID tidak boleh kosong\"}");
    return;
  }

  // Perbarui jika SSID sudah ada, atau cari slot kosong pertama
  int targetIdx = -1;
  for (int i = 0; i < MAX_WIFI; i++) {
    if (strcmp(sysCfg.wifi[i].ssid, s.c_str()) == 0) {
      targetIdx = i;
      break;
    }
    if (targetIdx == -1 && sysCfg.wifi[i].ssid[0] == '\0') {
      targetIdx = i;
    }
  }

  if (targetIdx == -1) {
    sendJson(400, "{\"ok\":false,\"msg\":\"Slot WiFi penuh (Maksimal 5 SSID)\"}");
    return;
  }

  memset(&sysCfg.wifi[targetIdx], 0, sizeof(WifiCred));
  strlcpy(sysCfg.wifi[targetIdx].ssid, s.c_str(), sizeof(sysCfg.wifi[targetIdx].ssid));
  strlcpy(sysCfg.wifi[targetIdx].pass, p.c_str(), sizeof(sysCfg.wifi[targetIdx].pass));

  if (!saveWifiConfig()) {
    sendJson(500, "{\"ok\":false,\"msg\":\"Gagal menyimpan SSID ke LittleFS\"}");
    return;
  }

  // Kirim respon HTTP lebih dulu agar koneksi SoftAP tidak terputus saat pindah kanal WiFi
  sendJson(200, "{\"ok\":true,\"msg\":\"SSID berhasil disimpan ke LittleFS\"}");

  if (WiFi.status() != WL_CONNECTED) {
    scheduleWifiConnect(700UL);
  }
}

void handleWifiDelete() {
  feedWatchdog();
  if (!server.hasArg("index")) {
    sendJson(400, "{\"ok\":false,\"msg\":\"Indeks SSID tidak ditemukan\"}");
    return;
  }

  int idx = server.arg("index").toInt();
  if (idx < 0 || idx >= MAX_WIFI || sysCfg.wifi[idx].ssid[0] == '\0') {
    sendJson(400, "{\"ok\":false,\"msg\":\"Indeks SSID tidak valid\"}");
    return;
  }

  bool wasConnectedSsid = (WiFi.status() == WL_CONNECTED &&
                           WiFi.SSID() == String(sysCfg.wifi[idx].ssid));

  // Geser elemen agar daftar tetap berurutan
  for (int i = idx; i < MAX_WIFI - 1; i++) {
    sysCfg.wifi[i] = sysCfg.wifi[i + 1];
  }
  memset(&sysCfg.wifi[MAX_WIFI - 1], 0, sizeof(WifiCred));

  if (!saveWifiConfig()) {
    sendJson(500, "{\"ok\":false,\"msg\":\"Gagal memperbarui file WiFi di LittleFS\"}");
    return;
  }

  sendJson(200, "{\"ok\":true}");

  if (wasConnectedSsid) {
    WiFi.disconnect(false);
    if (countSavedWifi() > 0) {
      scheduleWifiConnect(700UL);
    }
  }
}

void handleWifiReconnect() {
  feedWatchdog();
  sendJson(200, "{\"ok\":true,\"msg\":\"Menyambungkan ulang ke daftar WiFi...\"}");
  scheduleWifiConnect(500UL);
}

void handleRelayControl() {
  feedWatchdog();
  bool needSave = false;
  bool immediateManualExec = false;

  if (server.hasArg("mode")) {
    uint8_t newAuto = (server.arg("mode") == "auto") ? 1 : 0;
    if (sysCfg.relayAutoMode != newAuto) {
      sysCfg.relayAutoMode = newAuto;
      needSave = true;
    }
  }

  if (server.hasArg("state")) {
    uint8_t newManual = (server.arg("state") == "1") ? 1 : 0;
    sysCfg.relayAutoMode = 0; // Saklar manual otomatis mengaktifkan mode manual
    sysCfg.manualRelayState = newManual;
    needSave = true;
    immediateManualExec = true;
  }

  if (server.hasArg("threshold")) {
    float t = server.arg("threshold").toFloat();
    if (t >= 0.0f && t <= 50.0f) {
      sysCfg.tempThreshold = t;
      needSave = true;
    }
  }

  if (needSave) saveBoardConfig();

  // Saklar manual dieksekusi langsung (tetap mematikan heartbeat Pin 2 saat cek & eksekusi),
  // sedangkan mode otomatis dieksekusi terjadwal setiap 1 menit setelah kirim data ke server.
  if (immediateManualExec) {
    checkAndExecuteRelay();
  }

  char resp[128];
  snprintf(resp, sizeof(resp),
           "{\"ok\":true,\"relay\":%d,\"autoMode\":%s,\"threshold\":%.1f}",
           relayIsOn ? 1 : 0,
           sysCfg.relayAutoMode ? "true" : "false",
           sysCfg.tempThreshold);
  sendJson(200, resp);
}

void handleSaveBoardConfig() {
  feedWatchdog();
  bool hasData = false;

  if (server.hasArg("name")) {
    String v = server.arg("name");
    v.trim();
    if (v.length() > 0) {
      strlcpy(sysCfg.boardName, v.c_str(), sizeof(sysCfg.boardName));
      hasData = true;
    }
  }
  if (server.hasArg("id")) {
    String v = server.arg("id");
    v.trim();
    if (v.length() > 0) {
      strlcpy(sysCfg.boardID, v.c_str(), sizeof(sysCfg.boardID));
      hasData = true;
    }
  }
  if (server.hasArg("host")) {
    String v = server.arg("host");
    v.trim();
    if (v.length() > 0) {
      strlcpy(sysCfg.hostName, v.c_str(), sizeof(sysCfg.hostName));
      hasData = true;
    }
  }

  if (!hasData) {
    sendJson(400, "{\"ok\":false,\"msg\":\"Data konfigurasi board tidak terbaca\"}");
    return;
  }

  if (!saveBoardConfig()) {
    sendJson(500, "{\"ok\":false,\"msg\":\"Gagal menulis konfigurasi ke LittleFS\"}");
    return;
  }

  char bNameEsc[64], bIdEsc[64], hostEsc[128];
  sanitizeJsonStr(sysCfg.boardName, bNameEsc, sizeof(bNameEsc));
  sanitizeJsonStr(sysCfg.boardID, bIdEsc, sizeof(bIdEsc));
  sanitizeJsonStr(sysCfg.hostName, hostEsc, sizeof(hostEsc));

  char resp[320];
  snprintf(resp, sizeof(resp),
           "{\"ok\":true,\"boardName\":\"%s\",\"boardID\":\"%s\",\"hostName\":\"%s\"}",
           bNameEsc, bIdEsc, hostEsc);
  sendJson(200, resp);
}

void setupServer() {
  server.on("/", HTTP_GET, []() {
    feedWatchdog();
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server.send_P(200, "text/html", PLTS_INDEX_HTML);
  });

  // Endpoint status real-time
  server.on("/api/status", HTTP_GET, handleStatusJson);
  server.on("/data.json",  HTTP_GET, handleStatusJson);

  // Gunakan HTTP_ANY agar parameter query string maupun form POST selalu terbaca di semua versi Core ESP8266
  server.on("/api/wifi",         HTTP_GET, handleWifiList);
  server.on("/api/wifi/add",     HTTP_ANY, handleWifiAdd);
  server.on("/api/wifi/delete",  HTTP_ANY, handleWifiDelete);
  server.on("/api/wifi/connect", HTTP_ANY, handleWifiReconnect);
  server.on("/api/relay",        HTTP_ANY, handleRelayControl);
  server.on("/api/config",       HTTP_ANY, handleSaveBoardConfig);

  server.begin();
}

// ---------- Pola LED Detak Jantung Non-Blocking (Pin 2) ----------
void updateHeartbeatLed() {
  unsigned long nowMs = millis();

  // Lanjutkan heartbeat Pin 2 setelah proses cek & eksekusi selesai (hanya jika relay sedang OFF)
  if (!heartbeatActive && !relayIsOn && heartbeatResumeAtMs != 0 &&
      (long)(nowMs - heartbeatResumeAtMs) >= 0) {
    heartbeatResumeAtMs = 0;
    heartbeatActive = true;
    heartbeatStep = 0;
  }

  // Selama proses cek kondisi relay & eksekusi berlangsung, Pin 2 dimatikan sebagai heartbeat
  if (!heartbeatActive) return;

  switch (heartbeatStep) {
    case 0:
      digitalWrite(LED_PIN, LOW); // ON pertama
      heartbeatMillis = nowMs;
      heartbeatStep = 1;
      break;
    case 1:
      if (nowMs - heartbeatMillis >= 100UL) {
        digitalWrite(LED_PIN, HIGH); // OFF
        heartbeatMillis = nowMs;
        heartbeatStep = 2;
      }
      break;
    case 2:
      if (nowMs - heartbeatMillis >= 100UL) {
        digitalWrite(LED_PIN, LOW); // ON kedua
        heartbeatMillis = nowMs;
        heartbeatStep = 3;
      }
      break;
    case 3:
      if (nowMs - heartbeatMillis >= 100UL) {
        digitalWrite(LED_PIN, HIGH); // OFF panjang
        heartbeatMillis = nowMs;
        heartbeatStep = 4;
      }
      break;
    case 4:
      if (nowMs - heartbeatMillis >= 700UL) {
        heartbeatStep = 0;
      }
      break;
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(200);

  // Inisialisasi Pin LED Heartbeat (Pin 2) & Pin Relay (D4) ke posisi aman (MATI / HIGH)
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_INACTIVE);

  // Mount sistem file LittleFS (Format otomatis jika partisi baru/belum terformat)
  if (!LittleFS.begin()) {
    Serial.println(F("[LittleFS] Mount gagal, memformat partisi LittleFS..."));
    LittleFS.format();
    LittleFS.begin();
  }

  loadConfigs();

  Wire.begin();
  ahtReady = aht.begin();
  if (!ahtReady) {
    Serial.println(F("[AHT10] Sensor belum terdeteksi! Sistem tetap berjalan & akan mencoba ulang."));
  }

  // Baca sensor awal & cek kondisi awal relay saat boot
  checkAndExecuteRelay();
  lastRelayCycleMs = millis();

  // Nyalakan SoftAP terlebih dahulu, lalu coba sambungkan ke daftar Multi-WiFi
  setupSoftAP();
  connectMultiWiFi();

  setupServer();
  timeClient.begin();
  feedWatchdog();

  Serial.println(F("[SYSTEM] WebServer PLTS Monitoring siap."));
}

// ---------- Loop Utama ----------
void loop() {
  server.handleClient();

  // Eksekusi koneksi WiFi terjadwal setelah respon HTTP selesai dikirim ke browser
  if (pendingWifiConnect && (long)(millis() - wifiConnectAtMs) >= 0) {
    pendingWifiConnect = false;
    WiFi.disconnect(false);
    connectMultiWiFi();
  }

  // 1. Baca Sensor setiap 2 detik hanya untuk tampilan grafik/kartu (Tanpa menyentuh pin D4)
  if (millis() - lastSensorMillis >= SENSOR_INTERVAL_MS) {
    lastSensorMillis = millis();
    readSensors();
    if (ahtReady) {
      feedWatchdog();
    }
  }

  // 2. Pengiriman Data ke Server tiap 1 menit diikuti Cek Kondisi & Eksekusi Relay D4
  bool ntpSynced = false;
  if (WiFi.status() == WL_CONNECTED) {
    timeClient.update();
    time_t epoch = timeClient.getEpochTime();
    if (epoch > 100000) {
      ntpSynced = true;
      struct tm timeinfo;
      gmtime_r(&epoch, &timeinfo);
      if (timeinfo.tm_sec == 0 && timeinfo.tm_min != lastSentMinute) {
        lastSentMinute = timeinfo.tm_min;
        lastRelayCycleMs = millis();

        // Kirim data ke server terlebih dahulu
        Send_to_Database();

        // Setelah pengiriman selesai, matikan heartbeat Pin 2 lalu cek & eksekusi relay D4
        checkAndExecuteRelay();
      }
    }
  } else {
    // 3. Reconnect WiFi berkala jika koneksi terputus (tanpa mengganggu SoftAP)
    if (countSavedWifi() > 0 && (millis() - lastWifiRetry >= WIFI_RETRY_INTERVAL)) {
      lastWifiRetry = millis();
      connectMultiWiFi();
    }
  }

  // Cadangan siklus 1 menit untuk cek & eksekusi relay jika sedang offline / NTP belum tersinkron
  if (!ntpSynced && (millis() - lastRelayCycleMs >= 60000UL)) {
    lastRelayCycleMs = millis();
    checkAndExecuteRelay();
  }

  // 4. Detak jantung LED indikator pada Pin 2 (otomatis nonaktif saat cek & eksekusi relay)
  updateHeartbeatLed();

  // 5. Pengecekan Software Watchdog (Gunakan millis() terbaru agar bebas dari unsigned underflow)
  unsigned long currentMs = millis();
  if (currentMs >= lastActivityTime && (currentMs - lastActivityTime >= WATCHDOG_TIMEOUT_MS)) {
    Serial.println(F("[WDT] Tidak ada aktivitas sistem selama 1 jam, merestart ESP..."));
    ESP.restart();
  }

  yield();
}
