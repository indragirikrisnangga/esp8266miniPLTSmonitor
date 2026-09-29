#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>

#define CONFIG_MAGIC 0x41485431 // Magic key untuk verifikasi validitas file sistem
#define MAX_WIFI_NETWORKS 5
#define VOLTAGE_DIVIDER_RATIO 5.0 // Divider ratio module 0-25V (5:1)
#define ADC_REF_VOLTAGE 3.3        // Voltase referensi internal NodeMCU

// NTP Time Configuration (WIB = UTC+7 / 25200 detik)
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 25200
#define DAYLIGHT_OFFSET_SEC 0

// Inisialisasi LCD 1602 I2C (Alamat I2C umum 0x27)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// WiFi Credential structure
struct WiFiCredential {
  char ssid[32];
  char password[64];
  bool active;
};

// Main System Configuration stored in LittleFS File System
struct SystemConfig {
  uint32_t magic;
  char board_id[32];
  char board_name[32];
  char host_server[128];
  WiFiCredential wifis[MAX_WIFI_NETWORKS];
};

SystemConfig config;
ESP8266WebServer server(80);
Adafruit_AHTX0 aht;

bool aht_present = false;

// Variable Global Sensor & Timer
float current_temp = 0.0;
float current_hum = 0.0;
float current_volt = 0.0;

unsigned long last_sensor_read = 0;
const unsigned long SENSOR_INTERVAL = 1000; // Interval pembacaan sensor (1 detik)

unsigned long lcd_page_timer = 0;
bool showing_page2 = false;

// Variable Global Heartbeat LED
unsigned long last_heartbeat = 0;
bool heartbeat_state = false;
const unsigned long HEARTBEAT_INTERVAL = 500; // Blink setiap 500 ms (1 Hz)

// Prototipe Fungsi (Forward Declarations untuk mencegah error di Arduino IDE)
void loadConfiguration();
void saveConfiguration();
void setupWiFi();
void readSensors();
void updateLCDDisplay();
void handleHeartbeat();
void setupWebServer();

// Fungsi untuk membaca konfigurasi dari LittleFS File System
void loadConfiguration() {
  if (!LittleFS.begin()) {
    Serial.println("[LittleFS] Gagal mounting file system. Memformat...");
    LittleFS.format();
    LittleFS.begin();
  }

  // Cek jika file /config.bin tersedia di dalam LittleFS
  if (LittleFS.exists("/config.bin")) {
    File configFile = LittleFS.open("/config.bin", "r");
    if (configFile) {
      configFile.read((uint8_t*)&config, sizeof(SystemConfig));
      configFile.close();
      if (config.magic == CONFIG_MAGIC) {
        Serial.println("[LittleFS] Konfigurasi berhasil dimuat dari file system.");
        return;
      }
    }
  }

  // Jika file belum ada atau corrupt, gunakan konfigurasi default
  Serial.println("[LittleFS] File konfigurasi tidak ditemukan. Membuat default...");
  memset(&config, 0, sizeof(SystemConfig));
  config.magic = CONFIG_MAGIC;
  strncpy(config.board_id, "RF033001099", sizeof(config.board_id));
  strncpy(config.board_name, "ESP8266-SENS-01", sizeof(config.board_name));
  strncpy(config.host_server, "202.173.16.251", sizeof(config.host_server));
  
  // Set default WiFi credential
  strncpy(config.wifis[0].ssid, "SmartHome_WiFi", sizeof(config.wifis[0].ssid));
  strncpy(config.wifis[0].password, "12345678", sizeof(config.wifis[0].password));
  config.wifis[0].active = true;

  saveConfiguration();
}

// Fungsi untuk menyimpan konfigurasi ke LittleFS File System
void saveConfiguration() {
  File configFile = LittleFS.open("/config.bin", "w");
  if (configFile) {
    configFile.write((uint8_t*)&config, sizeof(SystemConfig));
    configFile.close();
    Serial.println("[LittleFS] Konfigurasi berhasil disimpan ke /config.bin");
  } else {
    Serial.println("[LittleFS] Gagal menyimpan file konfigurasi.");
  }
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  bool connected = false;

  Serial.println("\n[WiFi] Memulai pencarian jaringan WiFi tersimpan...");

  for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
    if (config.wifis[i].active && strlen(config.wifis[i].ssid) > 0) {
      Serial.printf("[WiFi] Mencoba terhubung ke: %s\n", config.wifis[i].ssid);
      WiFi.begin(config.wifis[i].ssid, config.wifis[i].password);

      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 15) {
        delay(500);
        Serial.print(".");
        ESP.wdtFeed(); // Feed Watchdog
        attempts++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        connected = true;
        Serial.printf("\n[WiFi] Berhasil terhubung! IP: %s\n", WiFi.localIP().toString().c_str());
        
        // Inisialisasi NTP Server setelah WiFi terhubung (WIB GMT+7)
        configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER, "time.nist.gov");
        Serial.println("[NTP] Memulai sinkronisasi waktu NTP...");
        break;
      } else {
        Serial.println("\n[WiFi] Gagal terhubung.");
      }
    }
  }

  // Fallback ke Access Point Mode menggunakan config.board_id
  if (!connected) {
    Serial.println("[WiFi] Tidak dapat terhubung ke WiFi manapun. Membuka Access Point Mode...");
    WiFi.mode(WIFI_AP);
    String ap_name = "SmartHome-Setup-" + String(config.board_id);
    WiFi.softAP(ap_name.c_str(), "12345678");
    Serial.printf("[WiFi] AP Aktif: %s, IP: %s\n", ap_name.c_str(), WiFi.softAPIP().toString().c_str());
  }
}

void readSensors() {
  // Read AHT10 Temperature & Humidity
  if (aht_present) {
    sensors_event_t humidity, temp;
    aht.getEvent(&humidity, &temp);
    current_temp = temp.temperature;
    current_hum = humidity.relative_humidity;
  } else {
    // Simulated values if AHT10 sensor is disconnected
    current_temp = 25.0 + random(-5, 5) / 10.0;
    current_hum = 60.0 + random(-10, 10) / 10.0;
  }

  // Read Voltage Divider module from ADC pin A0
  int rawADC = analogRead(A0);
  float voltageA0 = (rawADC / 1023.0) * ADC_REF_VOLTAGE;
  current_volt = voltageA0 * VOLTAGE_DIVIDER_RATIO;
}

// Fungsi Heartbeat LED untuk indikator sistem normal (Non-blocking)
void handleHeartbeat() {
  unsigned long currentMillis = millis();
  if (currentMillis - last_heartbeat >= HEARTBEAT_INTERVAL) {
    last_heartbeat = currentMillis;
    heartbeat_state = !heartbeat_state;
    // Pada mayoritas board ESP8266 (NodeMCU/WeMos), LED_BUILTIN bersifat Active-LOW
    digitalWrite(LED_BUILTIN, heartbeat_state ? LOW : HIGH);
  }
}

// Fungsi untuk memperbarui tampilan LCD 1602 I2C (Pergantian Halaman 1 & 2)
void updateLCDDisplay() {
  unsigned long currentMillis = millis();

  // Switch display mode: 5s Page 1, 2s Page 2
  if (!showing_page2 && (currentMillis - lcd_page_timer >= 5000)) {
    showing_page2 = true;
    lcd_page_timer = currentMillis;
    lcd.clear();
  } else if (showing_page2 && (currentMillis - lcd_page_timer >= 2000)) {
    showing_page2 = false;
    lcd_page_timer = currentMillis;
    lcd.clear();
  }

  if (!showing_page2) {
    // Halaman 1:
    // Baris 1: Suhu di kiri (e.g. "T:25.4C"), Jam (HH:MM) di 5 karakter paling kanan
    lcd.setCursor(0, 0);
    char line1_left[10];
    snprintf(line1_left, sizeof(line1_left), "T:%.1fC", current_temp);
    lcd.print(line1_left);

    // Ambil waktu NTP HH:MM
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    char timeStr[6] = "--:--";
    if (timeinfo && timeinfo->tm_year > 70) {
      strftime(timeStr, sizeof(timeStr), "%H:%M", timeinfo);
    }

    // Karakter paling kanan (kolom index 11 pada LCD 16 karakter)
    lcd.setCursor(11, 0);
    lcd.print(timeStr);

    // Baris 2: Kelembaban paling kiri, Tegangan paling kanan
    lcd.setCursor(0, 1);
    char humStr[9];
    snprintf(humStr, sizeof(humStr), "H:%.1f%%", current_hum);
    lcd.print(humStr);

    char voltStr[9];
    snprintf(voltStr, sizeof(voltStr), "V:%.2fV", current_volt);
    int voltLen = strlen(voltStr);
    lcd.setCursor(16 - voltLen, 1);
    lcd.print(voltStr);
  } else {
    // Halaman 2: SSID WiFi & IP Address
    lcd.setCursor(0, 0);
    if (WiFi.status() == WL_CONNECTED) {
      String ssidStr = WiFi.SSID();
      if (ssidStr.length() > 16) ssidStr = ssidStr.substring(0, 16);
      lcd.print(ssidStr);

      lcd.setCursor(0, 1);
      String ipStr = WiFi.localIP().toString();
      lcd.print(ipStr);
    } else {
      lcd.print("AP Mode Active");
      lcd.setCursor(0, 1);
      lcd.print(WiFi.softAPIP().toString());
    }
  }
}

void Send_to_Database() {
  Serial.print("connecting to ");
  Serial.println(String(config.host_server));
 
  WiFiClient client;
  const int httpPort = 80;
  if (!client.connect(String(config.host_server), httpPort)) {
    Serial.println("connection failed");
    return;
  }
 
  // We now create a URI for the request
  String url = "/realtime/add.php?";
  url += "code=";
  url += String(config.board_id);
  url += "&value1=";
  url += 0;
  url += "&value2=";
  url += current_temp;
  url += "&value3=";
  url += current_hum;
  url += "&value4=";
  url += current_volt;
  url += "&value5=";
  url += 0;
 
  Serial.print("Requesting URL: ");
  Serial.println(url);
 
  // This will send the request to the server
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + String(config.host_server) + "\r\n" +
               "Connection: close\r\n\r\n");
 
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 5000) {
      Serial.println(">>> Client Timeout !");
      client.stop();
      return;
    }
  }
 
  // Read all the lines of the reply from server and print them to Serial
  while (client.available()) {
    String line = client.readStringUntil('\r');
 
    if (line.indexOf("sukses gaes") != -1) {
      Serial.println("1 Record successfully insert into telemetri table");
    } else if (line.indexOf("gagal gaes") != -1) {
      Serial.println("Failled insert data to databases");
    }
  }

  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);

  if (timeinfo && timeinfo->tm_year > 70) {
    // Jika waktu NTP sudah tersinkronisasi
    Serial.printf("closing connection at %02d:%02d:%02d\n", 
                  timeinfo->tm_hour, 
                  timeinfo->tm_min, 
                  timeinfo->tm_sec);
  } else {
    // Jika waktu NTP belum tersinkronisasi
    Serial.println("closing connection at --:--:--");
  }

  Serial.println();
}

const char HTML_DASHBOARD[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id" class="dark">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Smart Home IoT Monitor</title>
  <script src="https://cdn.tailwindcss.com"></script>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <script>
    tailwind.config = {
      darkMode: 'class',
      theme: {
        extend: {
          colors: {
            glass: 'rgba(255, 255, 255, 0.1)',
            glassBorder: 'rgba(255, 255, 255, 0.2)',
            glassDark: 'rgba(15, 23, 42, 0.65)'
          }
        }
      }
    }
  </script>
  <style>
    body {
      background: linear-gradient(135deg, #0f172a 0%, #1e1b4b 50%, #311042 100%);
      min-height: 100vh;
    }
    .glass-card {
      background: rgba(255, 255, 255, 0.07);
      backdrop-filter: blur(16px);
      -webkit-backdrop-filter: blur(16px);
      border: 1px solid rgba(255, 255, 255, 0.15);
      box-shadow: 0 20px 40px rgba(0, 0, 0, 0.3);
    }
    .glass-card:hover {
      border-color: rgba(255, 255, 255, 0.3);
      transform: translateY(-2px);
      transition: all 0.3s ease;
    }
  </style>
</head>
<body class="text-slate-100 font-sans p-4 md:p-8">
  <div class="max-w-7xl mx-auto space-y-6">
    
    <!-- Navigation / Header -->
    <header class="glass-card rounded-2xl p-6 flex flex-col md:flex-row justify-between items-center gap-4">
      <div>
        <h1 class="text-2xl md:text-3xl font-extrabold bg-gradient-to-r from-cyan-400 via-sky-300 to-indigo-400 bg-clip-text text-transparent" id="lblBoardName">
          Smart Home Node
        </h1>
        <p class="text-xs text-slate-400 mt-1">ID Board: <span id="lblBoardID" class="font-mono text-cyan-300">--</span></p>
      </div>
      <div class="flex items-center gap-3">
        <span class="inline-flex items-center px-3 py-1 rounded-full text-xs font-semibold bg-emerald-500/20 text-emerald-300 border border-emerald-500/30">
          <span class="w-2 h-2 rounded-full bg-emerald-400 animate-ping mr-2"></span> System Active
        </span>
        <a href="/settings" class="px-4 py-2 bg-indigo-600/80 hover:bg-indigo-500 rounded-xl text-xs font-bold transition backdrop-blur-md border border-indigo-400/30">
          ⚙️ Pengaturan
        </a>
      </div>
    </header>

    <!-- Translucent Floating Metric Cards -->
    <div class="grid grid-cols-1 md:grid-cols-3 gap-6">
      
      <!-- Temperature Card -->
      <div class="glass-card rounded-2xl p-6 flex flex-col justify-between relative overflow-hidden group">
        <div class="absolute -right-6 -bottom-6 w-28 h-28 bg-rose-500/20 rounded-full blur-2xl group-hover:bg-rose-500/30 transition"></div>
        <div class="flex justify-between items-start">
          <span class="text-xs font-semibold uppercase tracking-wider text-slate-400">Suhu Ruangan</span>
          <span class="p-2 rounded-xl bg-rose-500/20 text-rose-300">🌡️</span>
        </div>
        <div class="my-4">
          <span id="valTemp" class="text-4xl md:text-5xl font-black text-rose-400">0.0</span>
          <span class="text-xl text-slate-400">°C</span>
        </div>
        <div class="text-xs text-slate-400">AHT10 Precision Sensor</div>
      </div>

      <!-- Humidity Card -->
      <div class="glass-card rounded-2xl p-6 flex flex-col justify-between relative overflow-hidden group">
        <div class="absolute -right-6 -bottom-6 w-28 h-28 bg-sky-500/20 rounded-full blur-2xl group-hover:bg-sky-500/30 transition"></div>
        <div class="flex justify-between items-start">
          <span class="text-xs font-semibold uppercase tracking-wider text-slate-400">Kelembaban</span>
          <span class="p-2 rounded-xl bg-sky-500/20 text-sky-300">💧</span>
        </div>
        <div class="my-4">
          <span id="valHum" class="text-4xl md:text-5xl font-black text-sky-400">0.0</span>
          <span class="text-xl text-slate-400">%</span>
        </div>
        <div class="text-xs text-slate-400">Relative Humidity</div>
      </div>

      <!-- Voltage Card -->
      <div class="glass-card rounded-2xl p-6 flex flex-col justify-between relative overflow-hidden group">
        <div class="absolute -right-6 -bottom-6 w-28 h-28 bg-amber-500/20 rounded-full blur-2xl group-hover:bg-amber-500/30 transition"></div>
        <div class="flex justify-between items-start">
          <span class="text-xs font-semibold uppercase tracking-wider text-slate-400">Tegangan Masukan</span>
          <span class="p-2 rounded-xl bg-amber-500/20 text-amber-300">⚡</span>
        </div>
        <div class="my-4">
          <span id="valVolt" class="text-4xl md:text-5xl font-black text-amber-400">0.00</span>
          <span class="text-xl text-slate-400">V</span>
        </div>
        <div class="text-xs text-slate-400">Pembagi Tegangan A0</div>
      </div>

    </div>

    <!-- Live Dynamic Charts -->
    <div class="grid grid-cols-1 lg:grid-cols-3 gap-6">
      
      <!-- Temp Chart -->
      <div class="glass-card rounded-2xl p-5 flex flex-col justify-between">
        <h3 class="text-sm font-bold text-rose-300 mb-3 flex items-center gap-2">
          <span>📈</span> Grafik Suhu (°C)
        </h3>
        <div class="relative h-48 w-full">
          <canvas id="chartTemp"></canvas>
        </div>
      </div>

      <!-- Humidity Chart -->
      <div class="glass-card rounded-2xl p-5 flex flex-col justify-between">
        <h3 class="text-sm font-bold text-sky-300 mb-3 flex items-center gap-2">
          <span>📊</span> Grafik Kelembaban (%)
        </h3>
        <div class="relative h-48 w-full">
          <canvas id="chartHum"></canvas>
        </div>
      </div>

      <!-- Voltage Chart -->
      <div class="glass-card rounded-2xl p-5 flex flex-col justify-between">
        <h3 class="text-sm font-bold text-amber-300 mb-3 flex items-center gap-2">
          <span>⚡</span> Grafik Tegangan (V)
        </h3>
        <div class="relative h-48 w-full">
          <canvas id="chartVolt"></canvas>
        </div>
      </div>

    </div>
  </div>

  <script>
    const maxDataPoints = 20;
    const timeLabels = Array(maxDataPoints).fill('');

    const createChartConfig = (label, colorHex, bgColorHex) => {
      return {
        type: 'line',
        data: {
          labels: timeLabels,
          datasets: [{
            label: label,
            data: Array(maxDataPoints).fill(null),
            borderColor: colorHex,
            backgroundColor: bgColorHex,
            borderWidth: 2,
            fill: true,
            tension: 0.4,
            pointRadius: 2
          }]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          plugins: { legend: { display: false } },
          scales: {
            x: { display: false },
            y: {
              grid: { color: 'rgba(255, 255, 255, 0.05)' },
              ticks: { color: 'rgba(255, 255, 255, 0.5)', font: { size: 10 } }
            }
          }
        }
      };
    };

    const chartTemp = new Chart(document.getElementById('chartTemp'), createChartConfig('Suhu', '#f43f5e', 'rgba(244, 63, 94, 0.1)', 0, 50));
    const chartHum  = new Chart(document.getElementById('chartHum'), createChartConfig('Kelembaban', '#38bdf8', 'rgba(56, 189, 248, 0.1)', 0, 100));
    const chartVolt = new Chart(document.getElementById('chartVolt'), createChartConfig('Tegangan', '#fbbf24', 'rgba(251, 191, 36, 0.1)', 0, 25));

    const pushData = (chart, value) => {
      chart.data.datasets[0].data.push(value);
      if (chart.data.datasets[0].data.length > maxDataPoints) {
        chart.data.datasets[0].data.shift();
      }
      chart.update('none');
    };

    const fetchSensorData = () => {
      fetch('/api/data')
        .then(res => {
          if (!res.ok) return null;
          return res.json();
        })
        .then(data => {
          if (!data) return;
          document.getElementById('lblBoardName').innerText = data.board_name;
          document.getElementById('lblBoardID').innerText = data.board_id;

          document.getElementById('valTemp').innerText = data.temp.toFixed(1);
          document.getElementById('valHum').innerText = data.hum.toFixed(1);
          document.getElementById('valVolt').innerText = data.volt.toFixed(2);

          const now = new Date().toLocaleTimeString('id-ID');
          timeLabels.push(now);
          if (timeLabels.length > maxDataPoints) timeLabels.shift();

          pushData(chartTemp, data.temp);
          pushData(chartHum, data.hum);
          pushData(chartVolt, data.volt);
        })
        .catch(err => {
          console.error("Gagal memperbarui data sensor:", err);
        });
    };

    // Auto update dynamic charts every 1 second
    setInterval(fetchSensorData, 1000);
    fetchSensorData();
  </script>
</body>
</html>
)rawliteral";

const char HTML_SETTINGS[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id" class="dark">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Pengaturan Smart Home</title>
  <script src="https://cdn.tailwindcss.com"></script>
  <style>
    body {
      background: linear-gradient(135deg, #0f172a 0%, #1e1b4b 50%, #311042 100%);
      min-height: 100vh;
    }
    .glass-card {
      background: rgba(255, 255, 255, 0.07);
      backdrop-filter: blur(16px);
      -webkit-backdrop-filter: blur(16px);
      border: 1px solid rgba(255, 255, 255, 0.15);
      box-shadow: 0 20px 40px rgba(0, 0, 0, 0.3);
    }
  </style>
</head>
<body class="text-slate-100 font-sans p-4 md:p-8">
  <div class="max-w-4xl mx-auto space-y-6">
    
    <header class="glass-card rounded-2xl p-6 flex justify-between items-center">
      <div>
        <h1 class="text-2xl font-bold bg-gradient-to-r from-cyan-400 to-indigo-400 bg-clip-text text-transparent">Pengaturan Sistem</h1>
        <p class="text-xs text-slate-400">Konfigurasi WiFi Multi-SSID dan Identitas Board</p>
      </div>
      <a href="/" class="px-4 py-2 bg-slate-700/80 hover:bg-slate-600 rounded-xl text-xs font-bold transition border border-slate-500/30">
        ← Kembali
      </a>
    </header>

    <!-- Multi WiFi Manager -->
    <div class="glass-card rounded-2xl p-6 space-y-4">
      <h2 class="text-lg font-bold text-sky-300">📶 Pengelola Multi WiFi (Max 5)</h2>
      
      <div id="wifiList" class="space-y-2">
        <p class="text-xs text-slate-400">Memuat daftar WiFi...</p>
      </div>

      <form id="formAddWifi" class="pt-4 border-t border-white/10 grid grid-cols-1 md:grid-cols-3 gap-3">
        <input type="text" id="addSSID" placeholder="SSID WiFi Baru" required class="bg-slate-900/60 border border-slate-700 rounded-xl px-4 py-2 text-sm focus:outline-none focus:border-sky-400">
        <input type="password" id="addPass" placeholder="Password WiFi" class="bg-slate-900/60 border border-slate-700 rounded-xl px-4 py-2 text-sm focus:outline-none focus:border-sky-400">
        <button type="submit" class="bg-emerald-600/80 hover:bg-emerald-500 text-white font-semibold py-2 px-4 rounded-xl text-sm transition border border-emerald-400/30">
          + Tambah WiFi
        </button>
      </form>
    </div>

    <!-- Device Identity & Server Configuration -->
    <div class="glass-card rounded-2xl p-6 space-y-4">
      <h2 class="text-lg font-bold text-indigo-300">⚙️ Identitas Board & Host Server</h2>
      
      <form id="formConfig" class="space-y-4">
        <div>
          <label class="block text-xs text-slate-400 mb-1">ID Board (Unik)</label>
          <input type="text" id="cfgBoardID" required class="w-full bg-slate-900/60 border border-slate-700 rounded-xl px-4 py-2 text-sm focus:outline-none focus:border-indigo-400">
        </div>
        <div>
          <label class="block text-xs text-slate-400 mb-1">Nama Board</label>
          <input type="text" id="cfgBoardName" required class="w-full bg-slate-900/60 border border-slate-700 rounded-xl px-4 py-2 text-sm focus:outline-none focus:border-indigo-400">
        </div>
        <div>
          <label class="block text-xs text-slate-400 mb-1">Host Server Endpoint URL</label>
          <input type="text" id="cfgHostServer" required class="w-full bg-slate-900/60 border border-slate-700 rounded-xl px-4 py-2 text-sm focus:outline-none focus:border-indigo-400">
        </div>
        <button type="submit" class="w-full bg-indigo-600/80 hover:bg-indigo-500 text-white font-bold py-2.5 rounded-xl text-sm transition border border-indigo-400/30">
          💾 Simpan Konfigurasi
        </button>
      </form>
    </div>

  </div>

  <script>
    const loadSettings = () => {
      fetch('/api/config')
        .then(res => res.json())
        .then(data => {
          document.getElementById('cfgBoardID').value = data.board_id;
          document.getElementById('cfgBoardName').value = data.board_name;
          document.getElementById('cfgHostServer').value = data.host_server;

          const wifiListContainer = document.getElementById('wifiList');
          wifiListContainer.innerHTML = '';

          data.wifis.forEach((item, index) => {
            if (item.ssid.length > 0) {
              const div = document.createElement('div');
              div.className = "flex justify-between items-center bg-slate-800/40 border border-white/5 p-3 rounded-xl";
              div.innerHTML = `
                <div>
                  <span class="text-sm font-semibold">${item.ssid}</span>
                  <span class="ml-2 text-xs text-slate-400">${item.active ? '(Aktif)' : '(Non-aktif)'}</span>
                </div>
                <button onclick="deleteWifi(${index})" class="bg-rose-600/80 hover:bg-rose-500 text-white text-xs px-3 py-1.5 rounded-lg border border-rose-400/30 transition">
                  Hapus
                </button>
              `;
              wifiListContainer.appendChild(div);
            }
          });
        })
        .catch(err => console.error("Gagal memuat pengaturan:", err));
    };

    document.getElementById('formAddWifi').addEventListener('submit', (e) => {
      e.preventDefault();
      const ssid = document.getElementById('addSSID').value;
      const pass = document.getElementById('addPass').value;

      fetch('/api/wifi/add', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: `ssid=${encodeURIComponent(ssid)}&pass=${encodeURIComponent(pass)}`
      }).then(() => {
        document.getElementById('addSSID').value = '';
        document.getElementById('addPass').value = '';
        loadSettings();
      });
    });

    const deleteWifi = (index) => {
      fetch('/api/wifi/delete', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: `index=${index}`
      }).then(() => {
        loadSettings();
      });
    };

    document.getElementById('formConfig').addEventListener('submit', (e) => {
      e.preventDefault();
      const board_id = document.getElementById('cfgBoardID').value;
      const board_name = document.getElementById('cfgBoardName').value;
      const host_server = document.getElementById('cfgHostServer').value;

      fetch('/api/config/save', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: `board_id=${encodeURIComponent(board_id)}&board_name=${encodeURIComponent(board_name)}&host_server=${encodeURIComponent(host_server)}`
      }).then(() => {
        alert('Konfigurasi berhasil disimpan! ESP8266 akan memuat ulang.');
        location.reload();
      });
    });

    loadSettings();
  </script>
</body>
</html>
)rawliteral";

void setupWebServer() {
  // Page Endpoints
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", HTML_DASHBOARD);
  });

  server.on("/settings", HTTP_GET, []() {
    server.send(200, "text/html", HTML_SETTINGS);
  });

  // REST API Endpoints
  server.on("/api/data", HTTP_GET, []() {
    String json = "{";
    json += "\"temp\":" + String(current_temp, 1) + ",";
    json += "\"hum\":" + String(current_hum, 1) + ",";
    json += "\"volt\":" + String(current_volt, 2) + ",";
    json += "\"board_id\":\"" + String(config.board_id) + "\",";
    json += "\"board_name\":\"" + String(config.board_name) + "\"";
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/api/config", HTTP_GET, []() {
    String json = "{";
    json += "\"board_id\":\"" + String(config.board_id) + "\",";
    json += "\"board_name\":\"" + String(config.board_name) + "\",";
    json += "\"host_server\":\"" + String(config.host_server) + "\",";
    json += "\"wifis\":[";
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
      json += "{";
      json += "\"ssid\":\"" + String(config.wifis[i].ssid) + "\",";
      json += "\"active\":" + String(config.wifis[i].active ? "true" : "false");
      json += "}";
      if (i < MAX_WIFI_NETWORKS - 1) json += ",";
    }
    json += "]}";
    server.send(200, "application/json", json);
  });

  server.on("/api/wifi/add", HTTP_POST, []() {
    if (server.hasArg("ssid")) {
      String ssid = server.arg("ssid");
      String pass = server.arg("pass");

      for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        if (!config.wifis[i].active || strlen(config.wifis[i].ssid) == 0) {
          strncpy(config.wifis[i].ssid, ssid.c_str(), sizeof(config.wifis[i].ssid));
          strncpy(config.wifis[i].password, pass.c_str(), sizeof(config.wifis[i].password));
          config.wifis[i].active = true;
          saveConfiguration();
          break;
        }
      }
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/api/wifi/delete", HTTP_POST, []() {
    if (server.hasArg("index")) {
      int idx = server.arg("index").toInt();
      if (idx >= 0 && idx < MAX_WIFI_NETWORKS) {
        memset(&config.wifis[idx], 0, sizeof(WiFiCredential));
        saveConfiguration();
      }
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/api/config/save", HTTP_POST, []() {
    if (server.hasArg("board_id")) {
      strncpy(config.board_id, server.arg("board_id").c_str(), sizeof(config.board_id));
      strncpy(config.board_name, server.arg("board_name").c_str(), sizeof(config.board_name));
      strncpy(config.host_server, server.arg("host_server").c_str(), sizeof(config.host_server));
      saveConfiguration();
    }
    server.send(200, "text/plain", "OK");
  });

  server.begin();
  Serial.println("[WebServer] Server HTTP telah diaktifkan.");
}

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("\n[System] Memulai ESP8266 Smart Home IoT Monitor...");

  // Inisialisasi LED Built-in untuk Heartbeat System
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // LED mati saat inisialisasi (Active-LOW)

  // Enable ESP8266 Hardware Watchdog Timer
  ESP.wdtEnable(WDTO_8S);

  // Initialize Wire / I2C (SDA: D2/GPIO4, SCL: D1/GPIO5 pada NodeMCU)
  Wire.begin(4, 5);

  // Inisialisasi LCD 1602 I2C
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Smart Home IoT");
  lcd.setCursor(0, 1);
  lcd.print("Memulai System...");

  // Initialize LittleFS & Load Configuration
  loadConfiguration();

  // Inisialisasi AHT10 Sensor
  if (aht.begin()) {
    aht_present = true;
    Serial.println("[Sensor] Sensor AHT10 terdeteksi dan terinisialisasi.");
  } else {
    aht_present = false;
    Serial.println("[Sensor] WARNING: Sensor AHT10 tidak ditemukan! Menggunakan mode simulasi.");
  }

  // Setup Multi WiFi connection
  setupWiFi();

  // Setup Embedded Web Server
  setupWebServer();

  lcd_page_timer = millis();
}

void loop() {
  // Feed the watchdog to prevent unexpected system reset
  ESP.wdtFeed();

  // Process incoming web HTTP requests
  server.handleClient();

  // Ambil waktu sekarang dari NTP
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);

  // Jika detik = 0 (awal menit)
  if (timeinfo->tm_sec == 0) {
    static time_t lastSend = 0;
    if (now != lastSend) {
      // Baca sensor
      readSensors();

      // Kirim data ke server
      Send_to_Database();

      // Simpan waktu terakhir agar tidak kirim berulang di detik yang sama
      lastSend = now;
    }
  }

  // Non-blocking Heartbeat LED indicator
  handleHeartbeat();

  // Non-blocking sensor reading every 1 second
  unsigned long currentMillis = millis();
  if (currentMillis - last_sensor_read >= SENSOR_INTERVAL) {
    last_sensor_read = currentMillis;
    readSensors();
  }

  // Perbarui Tampilan LCD 1602
  updateLCDDisplay();
}
