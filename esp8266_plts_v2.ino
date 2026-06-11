#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <time.h>

#define EEPROM_SIZE 512
ESP8266WebServer server(80);
Adafruit_AHTX0 aht;

struct WifiConfig {
  char ssid[32];
  char pass[32];
};
WifiConfig wifiConfig;

float voltage, temperature, humidity;
const int voltagePin = A0;

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600;   // GMT+7
const int daylightOffset_sec = 0;

unsigned long lastActivityTime = 0;          // waktu terakhir aktivitas
const unsigned long watchdogTimeout = 3600000UL; // 1 jam = 3600000 ms

// --- Variabel konfigurasi tambahan ---
String boardName = "ESP8266-Board";
String boardID   = "ESP8266-ID";
String hostName  = "202.173.16.251";

// --- EEPROM ---
void saveWiFiConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, wifiConfig);
  EEPROM.commit();
  EEPROM.end();
}
void loadWiFiConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, wifiConfig);
  EEPROM.end();
}

// --- Sensor ---
void readSensors() {
  sensors_event_t humidityEvent, tempEvent;
  aht.getEvent(&humidityEvent, &tempEvent);
  temperature = tempEvent.temperature;
  humidity = humidityEvent.relative_humidity;
  int adcValue = analogRead(voltagePin);
  int mapVoltage = map(adcValue, 0, 1023, 0, 255);
  voltage = ((mapVoltage * 0.00489) * 12.7);
}

// --- Toast Helper ---
void sendToastPage(String message, String type="success") {
  String html = R"rawliteral(
  <!DOCTYPE html><html><head><meta charset='UTF-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1'>
  <title>WiFi Setup</title>
  <link rel='stylesheet' href='https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css'>
  </head><body class='container mt-4'>
    <h2>WiFi Setup</h2>
    <div class='position-fixed bottom-0 end-0 p-3' style='z-index:11'>
      <div id='liveToast' class='toast align-items-center text-bg-)rawliteral" + type + R"rawliteral( border-0' role='alert' aria-live='assertive' aria-atomic='true'>
        <div class='d-flex'>
          <div class='toast-body'>)rawliteral" + message + R"rawliteral(</div>
          <button type='button' class='btn-close btn-close-white me-2 m-auto' data-bs-dismiss='toast'></button>
        </div>
      </div>
    </div>
    <script src='https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/js/bootstrap.bundle.min.js'></script>
    <script>
      const toastLiveExample=document.getElementById('liveToast');
      const toast=new bootstrap.Toast(toastLiveExample);
      toast.show();
      setTimeout(()=>{window.location.href='/'},2000);
    </script>
  </body></html>
  )rawliteral";
  server.send(200,"text/html",html);
}

// --- Halaman Setup WiFi ---
void handleRootSetup() {
  String html = R"rawliteral(
  <!DOCTYPE html><html><head><meta charset='UTF-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1'>
  <title>WiFi Setup</title>
  <link rel='stylesheet' href='https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css'>
  </head><body class='container mt-4'>
    <h2>Configure WiFi</h2>
    <form method='POST' action='/save' class='mb-3'>
      <label>SSID:</label>
      <input type='text' name='ssid' class='form-control'>
      <label>Password:</label>
      <input type='password' name='pass' class='form-control'>
      <button type='submit' class='btn btn-primary mt-2'>Save</button>
      <a href='/' class='btn btn-secondary mt-2'>Back</a>
    </form>
    <hr>
    <h4>Stored WiFi Configuration</h4>
  )rawliteral";

  if (strlen(wifiConfig.ssid) > 0) {
    html += "<ul class='list-group'>";
    html += "<li class='list-group-item d-flex justify-content-between align-items-center'>";
    html += String(wifiConfig.ssid);
    html += " <a href='/deletewifi' class='btn btn-danger btn-sm'>Delete</a></li>";
    html += "</ul>";
  } else {
    html += "<p><i>No WiFi configuration stored.</i></p>";
  }

  html += R"rawliteral(
    </body></html>
  )rawliteral";

  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    strncpy(wifiConfig.ssid, server.arg("ssid").c_str(), sizeof(wifiConfig.ssid));
    strncpy(wifiConfig.pass, server.arg("pass").c_str(), sizeof(wifiConfig.pass));
    saveWiFiConfig();
    sendToastPage("✅ WiFi configuration saved! Restarting...","success");
    delay(2000);
    ESP.restart();
  } else {
    sendToastPage("❌ Failed to save WiFi configuration","danger");
  }
}

void handleDeleteWifi() {
  memset(&wifiConfig,0,sizeof(wifiConfig));
  saveWiFiConfig();
  sendToastPage("🗑️ WiFi configuration deleted","warning");
}

// --- Halaman konfigurasi tambahan ---
void handleConfigPage() {
  String html = R"rawliteral(
  <!DOCTYPE html><html><head><meta charset='UTF-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1'>
  <title>Board Configuration</title>
  <link rel='stylesheet' href='https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css'>
  </head><body class='container mt-4'>
    <h2>Board Configuration</h2>
    <form method='POST' action='/saveconfig'>
      <div class='mb-3'>
        <label class='form-label'>Board Name</label>
        <input type='text' name='name' class='form-control' value=')rawliteral" + boardName + R"rawliteral('>
      </div>
      <div class='mb-3'>
        <label class='form-label'>Board ID</label>
        <input type='text' name='id' class='form-control' value=')rawliteral" + boardID + R"rawliteral('>
      </div>
      <div class='mb-3'>
        <label class='form-label'>Host</label>
        <input type='text' name='host' class='form-control' value=')rawliteral" + hostName + R"rawliteral('>
      </div>
      <button type='submit' class='btn btn-primary'>Save</button>
    </form>
    <br><a href='/' class='btn btn-secondary'>Back to Dashboard</a>
  </body></html>
  )rawliteral";

  server.send(200,"text/html",html);
}

// --- Handler simpan konfigurasi ---
void handleSaveConfig() {
  if(server.hasArg("name")) boardName = server.arg("name");
  if(server.hasArg("id"))   boardID   = server.arg("id");
  if(server.hasArg("host")) hostName  = server.arg("host");

  // Simpan ke EEPROM (contoh sederhana)
  EEPROM.begin(EEPROM_SIZE);
  int addr = sizeof(WifiConfig); // simpan setelah data WiFi
  EEPROM.put(addr, boardName);
  addr += sizeof(boardName);
  EEPROM.put(addr, boardID);
  addr += sizeof(boardID);
  EEPROM.put(addr, hostName);
  EEPROM.commit();
  EEPROM.end();

  sendToastPage("⚙️ Board configuration updated","success");
}

// --- Dashboard ---
void handleDashboard() {
  readSensors();
  String html = R"rawliteral(
  <!DOCTYPE html><html><head><meta charset='UTF-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1'>
  <title>Monitoring Dashboard</title>
  <link rel='stylesheet' href='https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css'>
  <style>
    .card { margin-bottom: 15px; }
    .progress { height: 25px; }
    .progress-bar { font-weight: bold; transition: width 1s; }
    .icon { font-size: 1.5rem; margin-right: 8px; }
  </style>
  </head><body class='container mt-4'>
    <h2>Monitoring Dashboard</h2>

    <!-- Voltage -->
    <div class='card' id='voltageCard'><div class='card-body'>
      <h5 class='card-title'><span class='icon'>⚡</span>Voltage: <span id='voltageVal'></span></h5>
      <div class='progress'><div id='voltageBar' class='progress-bar bg-info'></div></div>
    </div></div>

    <!-- Temperature -->
    <div class='card' id='tempCard'><div class='card-body'>
      <h5 class='card-title'><span class='icon'>🌡️</span>Temperature: <span id='tempVal'></span></h5>
      <div class='progress'><div id='tempBar' class='progress-bar bg-danger'></div></div>
    </div></div>

    <!-- Humidity -->
    <div class='card' id='humCard'><div class='card-body'>
      <h5 class='card-title'><span class='icon'>💧</span>Humidity: <span id='humVal'></span></h5>
      <div class='progress'><div id='humBar' class='progress-bar bg-success'></div></div>
    </div></div>

    <!-- NTP Time -->
    <div class='card' id='ntpCard'><div class='card-body'>
      <h5 class='card-title'>🕒 NTP Time: <span id='ntpTime'></span></h5>
      <p>Server: pool.ntp.org</p>
    </div></div>

    <!-- WiFi Info -->
    <div class='card' id='wifiCard'><div class='card-body'>
      <h5 class='card-title'>📶 WiFi Status</h5>
      <p><b>SSID:</b> <span id='ssid'></span></p>
      <p><b>Status:</b> <span id='status'></span></p>
      <p><b>IP Address:</b> <span id='ip'></span></p>
      <p><b>MAC Address:</b> <span id='mac'></span></p>
      <hr><!-- Tombol menuju WiFi Setup -->
      <a href='/wifisetup' class='btn btn-outline-warning'>WiFi Setup</a>
      <a href='/config' class='btn btn-outline-success'>Board Config</a>
    </div></div>

    <!-- Footer -->
    <footer class='text-center mt-4 mb-2'>
      <hr>
      <p>&copy; 2026 ESP8266 Monitoring Dashboard</p>
    </footer>

    <!-- AJAX Auto Refresh -->
    <script>
      function updateData(){
        fetch('/data.json').then(r=>r.json()).then(d=>{
          // Voltage
          document.getElementById('voltageVal').innerText = d.voltage.toFixed(2)+" V";
          let vbar=document.getElementById('voltageBar');
          vbar.style.width=(d.voltage/25*100)+"%";
          vbar.innerText=d.voltage.toFixed(2)+" V";

          // Temperature
          document.getElementById('tempVal').innerText = d.temperature.toFixed(1)+" °C";
          let tbar=document.getElementById('tempBar');
          tbar.style.width=d.temperature+"%";
          tbar.innerText=d.temperature.toFixed(1)+" °C";

          // Humidity
          document.getElementById('humVal').innerText = d.humidity.toFixed(1)+" %";
          let hbar=document.getElementById('humBar');
          hbar.style.width=d.humidity+"%";
          hbar.innerText=d.humidity.toFixed(1)+" %";

          // NTP Time
          document.getElementById('ntpTime').innerText = d.ntptime;

          // WiFi Info
          document.getElementById('ssid').innerText = d.ssid;
          document.getElementById('status').innerText = d.status;
          document.getElementById('ip').innerText = d.ip;
          document.getElementById('mac').innerText = d.mac;
        });
      }
      setInterval(updateData,2000);
      updateData();
    </script>
  </body></html>
  )rawliteral";

  server.send(200,"text/html",html);
}

void handleDataJson() {
  readSensors();

  // Ambil waktu NTP
  struct tm timeinfo;
  String ntpTime = "";
  if (getLocalTime(&timeinfo)) {
    char buf[30];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ntpTime = String(buf);
  }

  String json = "{";
  json += "\"voltage\":" + String(voltage,2) + ",";
  json += "\"temperature\":" + String(temperature,1) + ",";
  json += "\"humidity\":" + String(humidity,1) + ",";
  json += "\"ntptime\":\"" + ntpTime + "\",";
  json += "\"ssid\":\"" + String(WiFi.SSID()) + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"mac\":\"" + WiFi.macAddress() + "\",";
  json += "\"status\":\"" + String(WiFi.status()==WL_CONNECTED ? "Connected" : "Disconnected") + "\"";
  json += "}";
  server.send(200,"application/json",json);
}

void feedWatchdog() {
  lastActivityTime = millis();
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  Wire.begin();
  if(!aht.begin()) {
    Serial.println("AHT10 not detected!");
  }

  // Load konfigurasi WiFi dari EEPROM
  loadWiFiConfig();

  if(strlen(wifiConfig.ssid) == 0) {
    // Belum ada konfigurasi → masuk mode SoftAP
    WiFi.softAP("ESP8266-Setup","12345678");
    Serial.println("SoftAP started. Connect to SSID ESP8266-Setup, password 12345678");
    Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

    // Routing halaman setup
    server.on("/", handleRootSetup);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/deletewifi", handleDeleteWifi);
    server.on("/config", handleConfigPage);
    server.on("/saveconfig", HTTP_POST, handleSaveConfig);
  } else {
    // Sudah ada konfigurasi → coba koneksi ke WiFi
    WiFi.begin(wifiConfig.ssid, wifiConfig.pass);
    Serial.print("Connecting to "); Serial.println(wifiConfig.ssid);
    while(WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

    // NTP server
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    // Routing dashboard
    server.on("/", handleDashboard);
    server.on("/data.json", handleDataJson);
  }

  feedWatchdog(); // catat aktivitas awal

  server.begin();
  Serial.println("Webserver started.");
}

// --- Loop ---
void loop() {
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

  // setiap kali ada aktivitas, panggil feedWatchdog()
  feedWatchdog();

  // --- Watchdog cek ---
  if (millis() - lastActivityTime >= watchdogTimeout) {
    Serial.println("Watchdog: Tidak ada aktivitas 1 jam, reset ESP...");
    ESP.restart();
  }

  // feed hardware watchdog bawaan
  yield();
}

void Send_to_Database() {
  Serial.print("connecting to ");
  Serial.println(hostName);
 
  WiFiClient client;
  const int httpPort = 80;
  if (!client.connect(hostName, httpPort)) {
    Serial.println("connection failed");
    return;
  }
 
  // We now create a URI for the request
  String url = "/realtime/add.php?";
  url += "code=";
  url += boardID;
  url += "&value1=";
  url += 0;
  url += "&value2=";
  url += temperature;
  url += "&value3=";
  url += humidity;
  url += "&value4=";
  url += voltage;
  url += "&value5=";
  url += 0;
 
  Serial.print("Requesting URL: ");
  Serial.println(url);
 
  // This will send the request to the server
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + hostName + "\r\n" +
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
 
  Serial.println("closing connection at %02d:%02d:%02d\n");
  Serial.println();
}

