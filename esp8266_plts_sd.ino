#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <time.h>
#include <EEPROM.h>
#include <SD.h>
#include <SPI.h>

#define MAX_WIFI 5
#define EEPROM_SIZE 512

#define SD_CS_PIN D8   // sesuaikan dengan pin CS modul SD

ESP8266WiFiMulti wifiMulti;
ESP8266WebServer server(80);
Adafruit_AHTX0 aht;
sensors_event_t humidityEvent, tempEvent;

const int voltagePin = A0;
String boardName = "ESP8266-miniPLTS";
float voltage, temperature, humidity;

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600;
const int daylightOffset_sec = 0;

struct WifiConfig {
  char ssid[32];
  char pass[32];
};
WifiConfig wifiConfigs[MAX_WIFI];

// --- EEPROM ---
void saveWiFiConfigs() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, wifiConfigs);
  EEPROM.commit();
  EEPROM.end();
}

void loadWiFiConfigs() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, wifiConfigs);
  EEPROM.end();
  for (int i = 0; i < MAX_WIFI; i++) {
    if (strlen(wifiConfigs[i].ssid) > 0) {
      wifiMulti.addAP(wifiConfigs[i].ssid, wifiConfigs[i].pass);
    }
  }
}

// --- Sensor ---
void readSensors() {
  aht.getEvent(&humidityEvent, &tempEvent);
  temperature = tempEvent.temperature;
  humidity = humidityEvent.relative_humidity;
  int adcValue = analogRead(voltagePin);
  voltage = (adcValue / 1023.0) * 25.0;
}

// --- Endpoint JSON untuk data sensor ---
void handleDataJson() {
  readSensors();
  String json = "{";
  json += "\"voltage\":" + String(voltage,2) + ",";
  json += "\"temperature\":" + String(temperature,1) + ",";
  json += "\"humidity\":" + String(humidity,1);
  json += "}";
  server.send(200, "application/json", json);
}

// --- Endpoint JSON untuk grafik 24 jam ---
void handleLogsJson() {
  File file = SD.open("/datalog.csv", FILE_READ);
  if (!file) {
    server.send(500, "application/json", "{\"error\":\"Failed to open datalog.csv\"}");
    return;
  }

  time_t now = time(nullptr);
  time_t cutoff = now - 24*60*60; // 24 jam terakhir

  String json = "[";
  bool first = true;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.startsWith("Date")) continue; // skip header

    // Parse CSV sederhana
    int firstComma = line.indexOf(',');
    int secondComma = line.indexOf(',', firstComma+1);
    String dateStr = line.substring(0, firstComma);
    String timeStr = line.substring(firstComma+1, secondComma);

    // Gabungkan date+time → ubah ke time_t
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    strptime((dateStr + " " + timeStr).c_str(), "%Y-%m-%d %H:%M:%S", &tm);
    time_t logTime = mktime(&tm);

    if (logTime >= cutoff) {
      if (!first) json += ",";
      first = false;

      // Ambil nilai sensor
      String rest = line.substring(secondComma+1);
      json += "{";
      json += "\"datetime\":\"" + dateStr + " " + timeStr + "\",";
      json += "\"voltage\":" + rest.substring(0, rest.indexOf(','));
      // Ambil temperature & humidity juga
      int tComma = rest.indexOf(',');
      int hComma = rest.indexOf(',', tComma+1);
      String tempStr = rest.substring(tComma+1, hComma);
      int h2Comma = rest.indexOf(',', hComma+1);
      String humStr = rest.substring(hComma+1, h2Comma);
      json += ",\"temperature\":" + tempStr;
      json += ",\"humidity\":" + humStr;
      json += "}";
    }
  }
  file.close();
  json += "]";
  server.send(200, "application/json", json);
}

// --- Halaman Utama ---
void handleRoot() {
  readSensors();
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  char timeString[30];
  strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", timeinfo);

  String ipAddress = WiFi.localIP().toString();
  String macAddress = WiFi.macAddress(); // 

  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<title>Monitoring Board</title>";
  html += "<link rel='stylesheet' href='https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css'>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
  html += "</head><body class='container mt-4'>";
  html += "<h2>Board: " + boardName + "</h2>";
  html += "<p><b>IP Address:</b> " + ipAddress + "</p>";
  html += "<p><b>MAC Address:</b> " + macAddress + "</p>"; // 
  html += "<p><b>Time (NTP):</b> " + String(timeString) + "</p>";

  // Canvas grafik speedometer
  html += "<div class='row'>";
  html += "<div class='col-md-4'><canvas id='voltageGauge'></canvas></div>";
  html += "<div class='col-md-4'><canvas id='tempGauge'></canvas></div>";
  html += "<div class='col-md-4'><canvas id='humGauge'></canvas></div>";
  html += "</div>";

  // Script Chart.js + AJAX untuk update real-time
  html += "<script>";
  html += "function createGauge(id, value, max, label){";
  html += "return new Chart(document.getElementById(id), {type:'doughnut',data:{labels:[label],datasets:[{data:[value,max-value],backgroundColor:['#007bff','#e9ecef'],borderWidth:0}]},options:{circumference:180,rotation:270,cutout:'70%',plugins:{legend:{display:false},tooltip:{enabled:false},title:{display:true,text:label+': '+value}}}});}";
  html += "var voltageChart = createGauge('voltageGauge'," + String(voltage,2) + ",25,'Voltage (V)');";
  html += "var tempChart = createGauge('tempGauge'," + String(temperature,1) + ",100,'Temperature (°C)');";
  html += "var humChart = createGauge('humGauge'," + String(humidity,1) + ",100,'Humidity (%)');";

  html += "setInterval(()=>{fetch('/data.json').then(r=>r.json()).then(data=>{";
  html += "voltageChart.data.datasets[0].data=[data.voltage,25-data.voltage];";
  html += "voltageChart.options.plugins.title.text='Voltage (V): '+data.voltage;";
  html += "voltageChart.update();";
  html += "tempChart.data.datasets[0].data=[data.temperature,100-data.temperature];";
  html += "tempChart.options.plugins.title.text='Temperature (°C): '+data.temperature;";
  html += "tempChart.update();";
  html += "humChart.data.datasets[0].data=[data.humidity,100-data.humidity];";
  html += "humChart.options.plugins.title.text='Humidity (%): '+data.humidity;";
  html += "humChart.update();";
  html += "});},2000);";
  html += "</script>";

  html += "<h3>Grafik 24 Jam Terakhir</h3>";
  html += "<canvas id='lineChart'></canvas>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
  html += "<script>";
  html += "fetch('/logs.json').then(r=>r.json()).then(data=>{";
  html += "const labels = data.map(d=>d.datetime);";
  html += "const voltages = data.map(d=>d.voltage);";
  html += "const temps = data.map(d=>d.temperature);";
  html += "const hums = data.map(d=>d.humidity);";
  html += "new Chart(document.getElementById('lineChart'),{type:'line',data:{labels:labels,datasets:[";
  html += "{label:'Voltage (V)',data:voltages,borderColor:'blue',fill:false},";
  html += "{label:'Temperature (°C)',data:temps,borderColor:'red',fill:false},";
  html += "{label:'Humidity (%)',data:hums,borderColor:'green',fill:false}";
  html += "]}});});";
  html += "</script>";


  html += "<hr><a href='/wifi' class='btn btn-secondary'>Manage WiFi</a>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

// --- Halaman WiFi ---
void handleWifi() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<title>WiFi Config</title>";
  html += "<link rel='stylesheet' href='https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css'>";
  html += "</head><body class='container mt-4'>";
  html += "<h2>Stored WiFi Networks</h2><ul class='list-group'>";

  for (int i = 0; i < MAX_WIFI; i++) {
    if (strlen(wifiConfigs[i].ssid) > 0) {
      html += "<li class='list-group-item d-flex justify-content-between align-items-center'>";
      html += wifiConfigs[i].ssid;
      html += " <a href='/deletewifi?index=" + String(i) + "' class='btn btn-danger btn-sm'>Delete</a></li>";
    }
  }

  html += "</ul><br><h3>Add WiFi</h3>";
  html += "<form method='POST' action='/addwifi'>";
  html += "SSID: <input type='text' name='ssid'><br>";
  html += "Password: <input type='text' name='pass'><br>";
  html += "<button type='submit' class='btn btn-primary btn-sm'>Save</button></form>";
  html += "<br><a href='/' class='btn btn-secondary'>Back</a>";

  // Toast container
  html += "<div class='position-fixed bottom-0 end-0 p-3' style='z-index: 11'>";
  html += "<div id='liveToast' class='toast' role='alert' aria-live='assertive' aria-atomic='true'>";
  html += "<div class='toast-header'><strong class='me-auto'>WiFi Config</strong>";
  html += "<small>Now</small><button type='button' class='btn-close' data-bs-dismiss='toast'></button></div>";
  html += "<div class='toast-body'>Action completed successfully!</div></div></div>";

  html += "<script src='https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/js/bootstrap.bundle.min.js'></script>";
  html += "<script>";
  html += "const urlParams = new URLSearchParams(window.location.search);";
  html += "if(urlParams.get('toast')==='1'){";
  html += "var toastEl = document.getElementById('liveToast');";
  html += "var toast = new bootstrap.Toast(toastEl);";
  html += "toast.show();}";
  html += "</script>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

// --- Tambah WiFi ---
void handleAddWifi() {
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    for (int i = 0; i < MAX_WIFI; i++) {
      if (strlen(wifiConfigs[i].ssid) == 0) {
        strncpy(wifiConfigs[i].ssid, server.arg("ssid").c_str(), sizeof(wifiConfigs[i].ssid));
        strncpy(wifiConfigs[i].pass, server.arg("pass").c_str(), sizeof(wifiConfigs[i].pass));
        break;
      }
    }
    saveWiFiConfigs();
  }
  server.sendHeader("Location", "/wifi?toast=1");
  server.send(303);
}

// --- Hapus WiFi ---
void handleDeleteWifi() {
  if (server.hasArg("index")) {
    int idx = server.arg("index").toInt();
    if (idx >= 0 && idx < MAX_WIFI) {
      memset(&wifiConfigs[idx], 0, sizeof(WifiConfig));
      saveWiFiConfigs();
    }
  }
  server.sendHeader("Location", "/wifi?toast=1");
  server.send(303);
}

// --- Ubah Nama Board ---
void handleSetName() {
  if (server.hasArg("name")) {
    boardName = server.arg("name");
  }
  server.sendHeader("Location", "/?toast=1");
  server.send(303);
}

// --- Save SD Card ---
void logToSD() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);

  char dateStr[12], timeStr[12];
  strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", timeinfo);
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", timeinfo);

  String ipAddress = WiFi.localIP().toString();
  String macAddress = WiFi.macAddress();

  File file = SD.open("/datalog.csv", FILE_WRITE);
  if (file) {
    file.print(dateStr); file.print(",");
    file.print(timeStr); file.print(",");
    file.print(voltage, 2); file.print(",");
    file.print(temperature, 1); file.print(",");
    file.print(humidity, 1); file.print(",");
    file.print(ipAddress); file.print(",");
    file.println(macAddress);
    file.close();
    Serial.println("Data logged to SD card.");
  } else {
    Serial.println("Failed to open datalog.csv");
  }
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  Wire.begin();

  if (!aht.begin()) {
    Serial.println("AHT10 not detected!");
    while (1) delay(10);
  }

  // Inisialisasi SD card
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD Card initialization failed!");
  } else {
    Serial.println("SD Card ready.");
    // Buat file log jika belum ada
    if (!SD.exists("/datalog.csv")) {
      File file = SD.open("/datalog.csv", FILE_WRITE);
      if (file) {
        file.println("Date,Time,Voltage,Temperature,Humidity,IP,MAC");
        file.close();
      }
    }
  }

  EEPROM.begin(EEPROM_SIZE);
  loadWiFiConfigs();

  Serial.println("Connecting...");
  while (wifiMulti.run() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  server.on("/", handleRoot);
  server.on("/wifi", handleWifi);
  server.on("/addwifi", HTTP_POST, handleAddWifi);
  server.on("/deletewifi", handleDeleteWifi);
  server.on("/setname", HTTP_POST, handleSetName);
  server.on("/data.json", handleDataJson);
  server.on("/logs.json", handleLogsJson);
  server.begin();

  Serial.println("Webserver started.");
}

// --- Loop ---
void loop() {
  server.handleClient();

  // Ambil waktu sekarang
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);

  // Jika tepat menit 0 atau 30, dan detik 0
  if ((timeinfo->tm_min == 0 || timeinfo->tm_min == 30) && timeinfo->tm_sec == 0) {
    static time_t lastLog = 0;
    // Pastikan tidak log berulang kali dalam detik yang sama
    if (now != lastLog) {
      readSensors();
      logToSD();
      lastLog = now;
    }
  }
}

