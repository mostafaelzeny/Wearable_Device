#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <FS.h>
#include <LittleFS.h>
#include <math.h>
#include <time.h>

#include "secrets.h"
#include "cloud_config.h"
#include "dashboard_page.h"

// ESP32 I2C pins
#define SDA_PIN 26
#define SCL_PIN 27

#define AD5933_ADDR 0x0D

// AD5933 registers
#define REG_CONTROL_HB      0x80
#define REG_CONTROL_LB      0x81
#define REG_START_FREQ      0x82
#define REG_FREQ_INC        0x85
#define REG_NUM_INC         0x88
#define REG_SETTLING_CYCLES 0x8A
#define REG_STATUS          0x8F
#define REG_REAL_DATA       0x94
#define REG_IMAG_DATA       0x96

// AD5933 commands
#define CMD_INIT_START_FREQ 0x10
#define CMD_START_SWEEP    0x20
#define CMD_INC_FREQ       0x30
#define CMD_POWER_DOWN     0xA0
#define CMD_STANDBY        0xB0

// Status bits
#define STATUS_DATA_VALID  0x02
#define STATUS_SWEEP_DONE  0x04

// AD5933 internal clock
#define MCLK 16776000.0

// Choose PGA here
#define PGA_X5 0x00   // D8 = 0
#define PGA_X1 0x01   // D8 = 1


#define PGA_GAIN PGA_X1   // recommended for your 16kΩ test

// Sweep settings
const double START_FREQ = 1000.0;      // 1 kHz
const double FREQ_STEP  = 1000.0;      // 1 kHz step
const int NUM_POINTS = 100;            // 1 kHz to 100 kHz

// Calibration resistor value
const double CALIBRATION_RESISTOR = 16000.0; // 16k ohm
double gainFactor[NUM_POINTS];
double systemPhase[NUM_POINTS];
bool isCalibrated = false;
bool hasMeasurements = false;
String latestSessionFile = "";

WebServer server(80);

struct SweepReading {
  double frequency;
  int16_t real;
  int16_t imag;
  double zMagnitude;
  double phaseDeg;
  double resistance;
  double reactance;
};

SweepReading latestReadings[NUM_POINTS];

String csvHeader();
void appendReadingCsvLine(String &csv, const SweepReading &reading);
String makeSessionFilename();
bool isValidSessionPath(const String &path);
String saveSessionCsv();
void initLittleFS();

void uploadPointToCloud(const SweepReading &reading) {
  (void)reading;
  if (!CLOUD_UPLOAD_ENABLED) {
    return;
  }

  // TODO: Connect Firebase/Supabase point upload here.
}

void uploadSessionToCloud(const SweepReading readings[], int count) {
  (void)readings;
  (void)count;
  if (!CLOUD_UPLOAD_ENABLED) {
    return;
  }

  // TODO: Connect Firebase/Supabase session upload here.
}

void writeRegister(byte reg, byte value) {
  Wire.beginTransmission(AD5933_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

byte readRegister(byte reg) {
  Wire.beginTransmission(AD5933_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);

  Wire.requestFrom(AD5933_ADDR, 1);
  return Wire.read();
}

void write24(byte reg, long value) {
  writeRegister(reg,     (value >> 16) & 0xFF);
  writeRegister(reg + 1, (value >> 8) & 0xFF);
  writeRegister(reg + 2, value & 0xFF);
}

void write16(byte reg, int value) {
  writeRegister(reg,     (value >> 8) & 0xFF);
  writeRegister(reg + 1, value & 0xFF);
}

int16_t read16(byte reg) {
  byte highByte = readRegister(reg);
  byte lowByte  = readRegister(reg + 1);

  return (int16_t)((highByte << 8) | lowByte);
}

long frequencyCode(double frequency) {
  return (long)((frequency * pow(2, 27)) / (MCLK / 4.0));
}

void setControl(byte command) {
  writeRegister(REG_CONTROL_HB, command | PGA_GAIN);
  writeRegister(REG_CONTROL_LB, 0x00); // internal clock
}

bool waitForData() {
  unsigned long startTime = millis();

  while (millis() - startTime < 2000) {
    byte status = readRegister(REG_STATUS);

    if (status & STATUS_DATA_VALID) {
      return true;
    }

    delay(5);
  }

  return false;
}

void setupSweep() {
  long startCode = frequencyCode(START_FREQ);
  long incCode   = frequencyCode(FREQ_STEP);

  setControl(CMD_STANDBY);

  write24(REG_START_FREQ, startCode);
  write24(REG_FREQ_INC, incCode);
  write16(REG_NUM_INC, NUM_POINTS - 1);

  // Settling cycles
  write16(REG_SETTLING_CYCLES, 15);

  setControl(CMD_INIT_START_FREQ);
  delay(100);

  setControl(CMD_START_SWEEP);
  delay(100);
}

bool calibrateSystem() {
  Serial.println();
  Serial.println("=== Calibration started ===");
  Serial.println("Use a pure known resistor between VOUT and VIN.");
  Serial.println("Frequency,Real,Imag,Magnitude,RawPhaseDeg,GainFactor");

  setupSweep();

  for (int i = 0; i < NUM_POINTS; i++) {
    if (!waitForData()) {
      Serial.println("Data timeout during calibration");
      isCalibrated = false;
      setControl(CMD_POWER_DOWN);
      return false;
    }

    int16_t realValue = read16(REG_REAL_DATA);
    int16_t imagValue = read16(REG_IMAG_DATA);

    double magnitude = sqrt((double)realValue * realValue +
                            (double)imagValue * imagValue);

    double phase = atan2((double)imagValue, (double)realValue);

    gainFactor[i] = (1.0 / CALIBRATION_RESISTOR) / magnitude;
    systemPhase[i] = phase;

    double freq = START_FREQ + i * FREQ_STEP;

    Serial.print(freq);
    Serial.print(",");
    Serial.print(realValue);
    Serial.print(",");
    Serial.print(imagValue);
    Serial.print(",");
    Serial.print(magnitude);
    Serial.print(",");
    Serial.print(phase * 180.0 / PI);
    Serial.print(",");
    Serial.println(gainFactor[i], 12);

    if (i < NUM_POINTS - 1) {
      setControl(CMD_INC_FREQ);
      delay(20);
    }
  }

  setControl(CMD_POWER_DOWN);

  isCalibrated = true;
  Serial.println("=== Calibration finished ===");
  return true;
}

bool measureUnknownRC() {
  Serial.println();
  Serial.println("=== Unknown RC measurement started ===");
  Serial.println("Frequency,Real,Imag,|Z| Ohm,Phase Deg,Resistance R Ohm,Reactance X Ohm");

  if (!isCalibrated) {
    Serial.println("Measurement stopped: calibration is required first.");
    return false;
  }

  setupSweep();

  for (int i = 0; i < NUM_POINTS; i++) {
    if (!waitForData()) {
      Serial.println("Data timeout during measurement");
      hasMeasurements = false;
      setControl(CMD_POWER_DOWN);
      return false;
    }

    int16_t realValue = read16(REG_REAL_DATA);
    int16_t imagValue = read16(REG_IMAG_DATA);

    double magnitude = sqrt((double)realValue * realValue +
                            (double)imagValue * imagValue);

    double measuredPhase = atan2((double)imagValue, (double)realValue);

    double zMagnitude = 1.0 / (gainFactor[i] * magnitude);

    double impedancePhase = measuredPhase - systemPhase[i];

    double resistance = zMagnitude * cos(impedancePhase);
    double reactance  = zMagnitude * sin(impedancePhase);

    double freq = START_FREQ + i * FREQ_STEP;

    latestReadings[i].frequency = freq;
    latestReadings[i].real = realValue;
    latestReadings[i].imag = imagValue;
    latestReadings[i].zMagnitude = zMagnitude;
    latestReadings[i].phaseDeg = impedancePhase * 180.0 / PI;
    latestReadings[i].resistance = resistance;
    latestReadings[i].reactance = reactance;

    uploadPointToCloud(latestReadings[i]);

    Serial.print(freq);
    Serial.print(",");
    Serial.print(realValue);
    Serial.print(",");
    Serial.print(imagValue);
    Serial.print(",");
    Serial.print(zMagnitude);
    Serial.print(",");
    Serial.print(impedancePhase * 180.0 / PI);
    Serial.print(",");
    Serial.print(resistance);
    Serial.print(",");
    Serial.println(reactance);

    if (i < NUM_POINTS - 1) {
      setControl(CMD_INC_FREQ);
      delay(20);
    }
  }

  setControl(CMD_POWER_DOWN);

  hasMeasurements = true;
  latestSessionFile = saveSessionCsv();
  uploadSessionToCloud(latestReadings, NUM_POINTS);

  Serial.println("=== Measurement finished ===");
  if (latestSessionFile.length() > 0) {
    Serial.print("Saved session: ");
    Serial.println(latestSessionFile);
  } else {
    Serial.println("Session save failed");
  }
  return true;
}

String csvHeader() {
  return "Frequency,Real,Imag,Z,Phase,R,X\n";
}

void appendReadingCsvLine(String &csv, const SweepReading &reading) {
  csv += String(reading.frequency, 0);
  csv += ",";
  csv += String(reading.real);
  csv += ",";
  csv += String(reading.imag);
  csv += ",";
  csv += String(reading.zMagnitude, 6);
  csv += ",";
  csv += String(reading.phaseDeg, 6);
  csv += ",";
  csv += String(reading.resistance, 6);
  csv += ",";
  csv += String(reading.reactance, 6);
  csv += "\n";
}

String makeSessionFilename() {
  struct tm timeInfo;
  if (getLocalTime(&timeInfo, 100)) {
    char path[48];
    strftime(path, sizeof(path), "/sessions/session_%Y%m%d_%H%M%S.csv", &timeInfo);
    String candidate = String(path);
    while (LittleFS.exists(candidate)) {
      delay(1000);
      if (!getLocalTime(&timeInfo, 100)) {
        break;
      }
      strftime(path, sizeof(path), "/sessions/session_%Y%m%d_%H%M%S.csv", &timeInfo);
      candidate = String(path);
    }
    if (!LittleFS.exists(candidate)) {
      return candidate;
    }
  }

  String candidate = "/sessions/session_" + String(millis()) + ".csv";
  while (LittleFS.exists(candidate)) {
    delay(1);
    candidate = "/sessions/session_" + String(millis()) + ".csv";
  }
  return candidate;
}

bool isValidSessionPath(const String &path) {
  if (!path.startsWith("/sessions/session_") || !path.endsWith(".csv")) {
    return false;
  }

  if (path.indexOf("..") >= 0 || path.indexOf('\\') >= 0) {
    return false;
  }

  return path.indexOf('/', 10) < 0;
}

String saveSessionCsv() {
  String path = makeSessionFilename();
  File file = LittleFS.open(path, FILE_WRITE);
  if (!file) {
    return "";
  }

  file.print(csvHeader());
  for (int i = 0; i < NUM_POINTS; i++) {
    String line;
    line.reserve(96);
    appendReadingCsvLine(line, latestReadings[i]);
    file.print(line);
  }

  file.close();
  return path;
}

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleCalibrate() {
  bool ok = calibrateSystem();
  if (ok) {
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"Calibration finished\"}");
  } else {
    server.send(500, "application/json", "{\"ok\":false,\"message\":\"Calibration failed\"}");
  }
}

void handleMeasure() {
  bool ok = measureUnknownRC();
  if (ok) {
    String json = "{\"ok\":true,\"message\":\"";
    json += latestSessionFile.length() > 0 ? "Measurement finished and session saved" : "Measurement finished but session save failed";
    json += "\",\"latestSession\":\"";
    json += latestSessionFile;
    json += "\"}";
    server.send(200, "application/json", json);
  } else {
    server.send(409, "application/json", "{\"ok\":false,\"message\":\"Measurement failed or calibration is required\"}");
  }
}

void appendJsonNumber(String &json, double value, int decimals) {
  json += String(value, decimals);
}

String readingsJson() {
  String json;
  json.reserve(16000);
  json += "{\"calibrated\":";
  json += isCalibrated ? "true" : "false";
  json += ",\"hasMeasurements\":";
  json += hasMeasurements ? "true" : "false";
  json += ",\"message\":\"";
  json += hasMeasurements ? "Latest sweep loaded" : "No measurement data yet";
  json += "\",\"latestSession\":\"";
  json += latestSessionFile;
  json += "\",\"readings\":[";

  if (hasMeasurements) {
    for (int i = 0; i < NUM_POINTS; i++) {
      if (i > 0) {
        json += ",";
      }
      json += "{\"frequency\":";
      appendJsonNumber(json, latestReadings[i].frequency, 0);
      json += ",\"real\":";
      json += latestReadings[i].real;
      json += ",\"imag\":";
      json += latestReadings[i].imag;
      json += ",\"zMagnitude\":";
      appendJsonNumber(json, latestReadings[i].zMagnitude, 6);
      json += ",\"phaseDeg\":";
      appendJsonNumber(json, latestReadings[i].phaseDeg, 6);
      json += ",\"resistance\":";
      appendJsonNumber(json, latestReadings[i].resistance, 6);
      json += ",\"reactance\":";
      appendJsonNumber(json, latestReadings[i].reactance, 6);
      json += "}";
    }
  }

  json += "]}";
  return json;
}

void handleData() {
  server.send(200, "application/json", readingsJson());
}

String readingsCsv() {
  String csv;
  csv.reserve(10000);
  csv += csvHeader();

  if (hasMeasurements) {
    for (int i = 0; i < NUM_POINTS; i++) {
      appendReadingCsvLine(csv, latestReadings[i]);
    }
  }

  return csv;
}

void handleCsv() {
  server.sendHeader("Content-Disposition", "attachment; filename=ad5933_latest_sweep.csv");
  server.send(200, "text/csv", readingsCsv());
}

void handleSessions() {
  File root = LittleFS.open("/sessions");
  if (!root || !root.isDirectory()) {
    server.send(200, "application/json", "{\"sessions\":[]}");
    return;
  }

  String json;
  json.reserve(4096);
  json += "{\"sessions\":[";

  bool first = true;
  File file = root.openNextFile();
  while (file) {
    String path = file.name();
    if (!path.startsWith("/")) {
      path = "/sessions/" + path;
    }

    if (!file.isDirectory() && isValidSessionPath(path)) {
      if (!first) {
        json += ",";
      }
      first = false;
      json += "{\"file\":\"";
      json += path;
      json += "\",\"size\":";
      json += String((unsigned long)file.size());
      json += "}";
    }

    file.close();
    file = root.openNextFile();
  }

  root.close();
  json += "]}";
  server.send(200, "application/json", json);
}

bool getRequestedSessionPath(String &path) {
  if (!server.hasArg("file")) {
    server.send(400, "application/json", "{\"ok\":false,\"message\":\"Missing file parameter\"}");
    return false;
  }

  path = server.arg("file");
  if (!isValidSessionPath(path)) {
    server.send(400, "application/json", "{\"ok\":false,\"message\":\"Invalid session file\"}");
    return false;
  }

  if (!LittleFS.exists(path)) {
    server.send(404, "application/json", "{\"ok\":false,\"message\":\"Session file not found\"}");
    return false;
  }

  return true;
}

void handleSession() {
  String path;
  if (!getRequestedSessionPath(path)) {
    return;
  }

  File file = LittleFS.open(path, FILE_READ);
  if (!file) {
    server.send(500, "application/json", "{\"ok\":false,\"message\":\"Could not open session file\"}");
    return;
  }

  String json;
  json.reserve(16000);
  json += "{\"file\":\"";
  json += path;
  json += "\",\"readings\":[";

  bool first = true;
  bool header = true;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
      continue;
    }
    if (header) {
      header = false;
      continue;
    }

    int p1 = line.indexOf(',');
    int p2 = line.indexOf(',', p1 + 1);
    int p3 = line.indexOf(',', p2 + 1);
    int p4 = line.indexOf(',', p3 + 1);
    int p5 = line.indexOf(',', p4 + 1);
    int p6 = line.indexOf(',', p5 + 1);
    if (p1 < 0 || p2 < 0 || p3 < 0 || p4 < 0 || p5 < 0 || p6 < 0) {
      continue;
    }

    if (!first) {
      json += ",";
    }
    first = false;

    json += "{\"frequency\":";
    json += line.substring(0, p1);
    json += ",\"real\":";
    json += line.substring(p1 + 1, p2);
    json += ",\"imag\":";
    json += line.substring(p2 + 1, p3);
    json += ",\"zMagnitude\":";
    json += line.substring(p3 + 1, p4);
    json += ",\"phaseDeg\":";
    json += line.substring(p4 + 1, p5);
    json += ",\"resistance\":";
    json += line.substring(p5 + 1, p6);
    json += ",\"reactance\":";
    json += line.substring(p6 + 1);
    json += "}";
  }

  file.close();
  json += "]}";
  server.send(200, "application/json", json);
}

void handleDownload() {
  String path;
  if (!getRequestedSessionPath(path)) {
    return;
  }

  File file = LittleFS.open(path, FILE_READ);
  if (!file) {
    server.send(500, "application/json", "{\"ok\":false,\"message\":\"Could not open session file\"}");
    return;
  }

  String filename = path.substring(path.lastIndexOf('/') + 1);
  server.sendHeader("Content-Disposition", String("attachment; filename=") + filename);
  server.streamFile(file, "text/csv");
  file.close();
}

void handleDelete() {
  String path;
  if (!getRequestedSessionPath(path)) {
    return;
  }

  if (LittleFS.remove(path)) {
    if (latestSessionFile == path) {
      latestSessionFile = "";
    }
    server.send(200, "application/json", "{\"ok\":true,\"message\":\"Session deleted\"}");
  } else {
    server.send(500, "application/json", "{\"ok\":false,\"message\":\"Could not delete session\"}");
  }
}

void initLittleFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return;
  }

  if (!LittleFS.exists("/sessions")) {
    if (LittleFS.mkdir("/sessions")) {
      Serial.println("LittleFS /sessions folder created");
    } else {
      Serial.println("LittleFS /sessions folder create failed");
    }
  }

  Serial.println("LittleFS initialized");
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/calibrate", HTTP_GET, handleCalibrate);
  server.on("/measure", HTTP_GET, handleMeasure);
  server.on("/data", HTTP_GET, handleData);
  server.on("/csv", HTTP_GET, handleCsv);
  server.on("/sessions", HTTP_GET, handleSessions);
  server.on("/session", HTTP_GET, handleSession);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/delete", HTTP_GET, handleDelete);
  server.begin();
  Serial.println("WebServer started on port 80");
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);

  initLittleFS();
  connectWiFi();
  setupWebServer();

  Serial.println("AD5933 Impedance Measurement");
  Serial.println("Step 1: Connect known 16k resistor between VOUT and VIN.");
  Serial.println("Then send character: c");
  Serial.println("Step 2: Connect your unknown RC circuit.");
  Serial.println("Then send character: m");
}

void loop() {
  server.handleClient();

  if (Serial.available()) {
    char cmd = Serial.read();

    if (cmd == 'c') {
      if (calibrateSystem()) {
        Serial.println("Now connect unknown RC circuit and send: m");
      }
    }

    if (cmd == 'm') {
      measureUnknownRC();
    }
  }
}
