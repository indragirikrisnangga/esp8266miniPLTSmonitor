#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <time.h>
#include <EEPROM.h>

#define MAX_WIFI 5
#define EEPROM_SIZE 512

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

// --- Setup ---
void setup() {
  Serial.begin(115200);
  Wire.begin();

  if (!aht.begin()) {
    Serial.println("AHT10 not detected!");
    while (1) delay(10);
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
  server.begin();

  Serial.println("Webserver started.");
}

// --- Loop ---
void loop() {
  server.handleClient();
}
