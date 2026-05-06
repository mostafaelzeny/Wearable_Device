#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <math.h>

#if __has_include(<esp_eap_client.h>)
#include <esp_eap_client.h>
#define USE_ESP_EAP_CLIENT 1
#else
#include <esp_wpa2.h>
#define USE_ESP_EAP_CLIENT 0
#endif

/*
  MAX30102 pulse oximeter sketch for ESP32.

  Connections used by the previous sketch:
    SDA -> GPIO25
    SCL -> GPIO26

  Serial output:
    NO_FINGER,0,0
    BPM,SPO2

  BPM is printed every second. SpO2 is recalculated every 5 seconds after the
  signal has settled, and the most recent final SpO2 value is printed beside
  each BPM reading. The same values are published to MQTT as JSON.

  SpO2 is an estimate from the common MAX3010x ratio-of-ratios equation:
    R = (ACred/DCred) / (ACir/DCir)
    SpO2 ~= 110 - 25 * R

  This is suitable for experiments and display projects. It is not a medical
  measurement and must be calibrated against known references for real use.
*/

static const uint8_t SDA_PIN = 25;
static const uint8_t SCL_PIN = 26;
static const uint32_t I2C_CLOCK_HZ = 400000;

static const bool WIFI_ENTERPRISE = false;
static const char *WIFI_SSID = "Mostafa's S22 Ultra";
static const char *WIFI_USERNAME = "";
static const char *WIFI_PASSWORD = "12345678";

// PC hostname where Mosquitto is running. On Linux this is usually:
//   <output-of-hostname>.local
// The fallback IP is used if mDNS is not available on the PC/network.
static const char *MQTT_HOSTNAME = "asus";
static const char *MQTT_FALLBACK_IP = "192.168.0.126";
static const uint16_t MQTT_PORT = 1883;
static const char *MQTT_CLIENT_ID = "esp32-max30102";
static const char *MQTT_TOPIC = "wearable/vitals";

static const uint8_t MAX30102_ADDR = 0x57;  // Datasheet slave ID 0b1010111.

static const uint8_t REG_INTR_STATUS_1 = 0x00;
static const uint8_t REG_INTR_STATUS_2 = 0x01;
static const uint8_t REG_FIFO_WR_PTR = 0x04;
static const uint8_t REG_OVF_COUNTER = 0x05;
static const uint8_t REG_FIFO_RD_PTR = 0x06;
static const uint8_t REG_FIFO_DATA = 0x07;
static const uint8_t REG_FIFO_CONFIG = 0x08;
static const uint8_t REG_MODE_CONFIG = 0x09;
static const uint8_t REG_SPO2_CONFIG = 0x0A;
static const uint8_t REG_LED1_PA = 0x0C;    // Red LED pulse amplitude.
static const uint8_t REG_LED2_PA = 0x0D;    // IR LED pulse amplitude.
static const uint8_t REG_MULTI_LED_1 = 0x11;
static const uint8_t REG_PART_ID = 0xFF;

static const uint8_t EXPECTED_PART_ID = 0x15;

static const uint16_t SAMPLE_RATE_HZ = 100;
static const uint16_t SPO2_WINDOW = SAMPLE_RATE_HZ * 4;  // 4 seconds of samples.
static const uint32_t SPO2_SETTLE_MS = 8000;
static const uint32_t SPO2_UPDATE_MS = 5000;
static const uint8_t SPO2_HISTORY_SIZE = 5;
static const uint8_t SPO2_REQUIRED_VALID_WINDOWS = 3;
static const int SPO2_CALIBRATION_OFFSET = 6;
static const uint32_t FINGER_THRESHOLD = 50000;
static const float MIN_BEAT_THRESHOLD = 18.0f;
static const uint16_t MIN_BEAT_MS = 300;       // 200 BPM upper limit.
static const uint16_t MAX_BEAT_MS = 1500;      // 40 BPM lower limit.

static uint32_t redBuffer[SPO2_WINDOW];
static uint32_t irBuffer[SPO2_WINDOW];
static uint16_t sampleCount = 0;
static uint16_t sampleIndex = 0;
static uint32_t fingerStartMs = 0;
static uint32_t lastPrintMs = 0;
static uint32_t lastSpo2UpdateMs = 0;

static float bpmAverage = 0.0f;
static float lastBpm = 0.0f;
static float bpmHistory[4] = {0, 0, 0, 0};
static uint8_t bpmIndex = 0;
static uint8_t bpmValidCount = 0;
static int spo2History[SPO2_HISTORY_SIZE] = {0, 0, 0, 0, 0};
static uint8_t spo2Index = 0;
static uint8_t spo2ValidCount = 0;
static int finalSpo2 = 0;

static float irDc = 0.0f;
static float prevFilteredIr = 0.0f;
static float filteredIr = 0.0f;
static float signalMeanAbs = 0.0f;
static bool armedForBeat = true;
static uint32_t lastBeatMs = 0;

struct Spo2Result {
  bool valid;
  int value;
};

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

static IPAddress resolveMqttServer() {
  IPAddress brokerIp = MDNS.queryHost(MQTT_HOSTNAME);
  if (brokerIp != INADDR_NONE) {
    return brokerIp;
  }

  brokerIp.fromString(MQTT_FALLBACK_IP);
  return brokerIp;
}

static bool scanForConfiguredWiFi() {
  Serial.println("STATUS,WiFi scanning");
  int networkCount = WiFi.scanNetworks();
  bool found = false;

  for (int i = 0; i < networkCount; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid == WIFI_SSID) {
      found = true;
      Serial.print("STATUS,WiFi SSID found,RSSI=");
      Serial.println(WiFi.RSSI(i));
      break;
    }
  }

  if (!found) {
    Serial.print("STATUS,WiFi SSID not found,name=");
    Serial.println(WIFI_SSID);
  }

  WiFi.scanDelete();
  return found;
}

static bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  if (WIFI_ENTERPRISE && strlen(WIFI_USERNAME) == 0) {
    Serial.println("STATUS,WiFi error,eduroam requires WIFI_USERNAME");
    delay(5000);
    return false;
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  scanForConfiguredWiFi();

  if (!WIFI_ENTERPRISE) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  } else {
#if USE_ESP_EAP_CLIENT
    esp_eap_client_set_identity((uint8_t *)WIFI_USERNAME, strlen(WIFI_USERNAME));
    esp_eap_client_set_username((uint8_t *)WIFI_USERNAME, strlen(WIFI_USERNAME));
    esp_eap_client_set_password((uint8_t *)WIFI_PASSWORD, strlen(WIFI_PASSWORD));
    esp_wifi_sta_enterprise_enable();
#else
    esp_wifi_sta_wpa2_ent_set_identity((uint8_t *)WIFI_USERNAME, strlen(WIFI_USERNAME));
    esp_wifi_sta_wpa2_ent_set_username((uint8_t *)WIFI_USERNAME, strlen(WIFI_USERNAME));
    esp_wifi_sta_wpa2_ent_set_password((uint8_t *)WIFI_PASSWORD, strlen(WIFI_PASSWORD));
    esp_wifi_sta_wpa2_ent_enable();
#endif

    WiFi.begin(WIFI_SSID);
  }

  Serial.print("STATUS,WiFi connecting");
  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 60) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println();
    Serial.print("STATUS,WiFi failed,status=");
    Serial.println(WiFi.status());
    if (WIFI_ENTERPRISE) {
      Serial.println("STATUS,WiFi check enterprise username,password,and EAP method support");
    } else {
      Serial.println("STATUS,WiFi check hotspot name,password,2.4GHz,and WPA2 compatibility");
    }
    return false;
  }

  Serial.println();
  Serial.print("STATUS,WiFi connected,IP=");
  Serial.println(WiFi.localIP());

  if (!MDNS.begin("esp32-max30102")) {
    Serial.println("STATUS,mDNS start failed");
  }
  return true;
}

static bool connectMqtt() {
  if (mqttClient.connected()) {
    return true;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("STATUS,MQTT skipped,WiFi disconnected");
    return false;
  }

  while (!mqttClient.connected()) {
    IPAddress brokerIp = resolveMqttServer();
    mqttClient.setServer(brokerIp, MQTT_PORT);

    Serial.print("STATUS,MQTT connecting...");
    if (mqttClient.connect(MQTT_CLIENT_ID)) {
      Serial.println("connected");
      mqttClient.publish(MQTT_TOPIC, "{\"status\":\"online\"}", true);
      return true;
    } else {
      Serial.print("failed,rc=");
      Serial.println(mqttClient.state());
      delay(2000);
    }
  }
  return true;
}

static void publishVitals(int bpm, int spo2, bool fingerDetected) {
  if (!connectWiFi() || !connectMqtt()) {
    return;
  }

  char payload[128];
  snprintf(payload, sizeof(payload),
           "{\"bpm\":%d,\"spo2\":%d,\"finger\":%s,\"uptime_ms\":%lu}",
           bpm, spo2, fingerDetected ? "true" : "false",
           (unsigned long)millis());

  mqttClient.publish(MQTT_TOPIC, payload);
}

static bool writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MAX30102_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool readRegister(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(MAX30102_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(MAX30102_ADDR, (uint8_t)1) != 1) {
    return false;
  }

  value = Wire.read();
  return true;
}

static uint8_t availableSamples() {
  uint8_t writePtr = 0;
  uint8_t readPtr = 0;

  if (!readRegister(REG_FIFO_WR_PTR, writePtr)) {
    return 0;
  }
  if (!readRegister(REG_FIFO_RD_PTR, readPtr)) {
    return 0;
  }

  writePtr &= 0x1F;
  readPtr &= 0x1F;
  return (writePtr >= readPtr) ? (writePtr - readPtr) : (32 + writePtr - readPtr);
}

static bool readFifoSample(uint32_t &red, uint32_t &ir) {
  Wire.beginTransmission(MAX30102_ADDR);
  Wire.write(REG_FIFO_DATA);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  // SpO2 mode puts one red sample and one IR sample in each FIFO entry.
  if (Wire.requestFrom(MAX30102_ADDR, (uint8_t)6) != 6) {
    return false;
  }

  red = ((uint32_t)Wire.read() << 16) | ((uint32_t)Wire.read() << 8) | Wire.read();
  ir = ((uint32_t)Wire.read() << 16) | ((uint32_t)Wire.read() << 8) | Wire.read();

  red &= 0x3FFFF;  // 18-bit ADC sample.
  ir &= 0x3FFFF;
  return true;
}

static void resetAlgorithm() {
  sampleCount = 0;
  sampleIndex = 0;
  fingerStartMs = 0;
  lastSpo2UpdateMs = 0;
  bpmAverage = 0.0f;
  lastBpm = 0.0f;
  bpmIndex = 0;
  bpmValidCount = 0;
  spo2Index = 0;
  spo2ValidCount = 0;
  finalSpo2 = 0;
  irDc = 0.0f;
  prevFilteredIr = 0.0f;
  filteredIr = 0.0f;
  signalMeanAbs = 0.0f;
  armedForBeat = true;
  lastBeatMs = 0;

  for (uint8_t i = 0; i < 4; i++) {
    bpmHistory[i] = 0.0f;
  }
  for (uint8_t i = 0; i < SPO2_HISTORY_SIZE; i++) {
    spo2History[i] = 0;
  }
}

static bool setupMax30102() {
  uint8_t partId = 0;
  if (!readRegister(REG_PART_ID, partId) || partId != EXPECTED_PART_ID) {
    return false;
  }

  writeRegister(REG_MODE_CONFIG, 0x40);  // Reset.
  delay(100);

  uint8_t modeConfig = 0x40;
  uint32_t resetStart = millis();
  while ((modeConfig & 0x40) && (millis() - resetStart < 1000)) {
    readRegister(REG_MODE_CONFIG, modeConfig);
    delay(10);
  }

  // Clear pending interrupts by reading both status registers.
  readRegister(REG_INTR_STATUS_1, modeConfig);
  readRegister(REG_INTR_STATUS_2, modeConfig);

  writeRegister(REG_FIFO_WR_PTR, 0x00);
  writeRegister(REG_OVF_COUNTER, 0x00);
  writeRegister(REG_FIFO_RD_PTR, 0x00);

  // Sample average 4, FIFO rollover enabled, almost-full threshold 17 samples.
  writeRegister(REG_FIFO_CONFIG, 0b01011111);

  // SpO2 mode: red + IR channels.
  writeRegister(REG_MODE_CONFIG, 0x03);

  // ADC range 4096nA, 100 samples/s, 411us pulse width for 18-bit samples.
  writeRegister(REG_SPO2_CONFIG, 0b00100111);

  // Start moderately. Increase if the signal is low; decrease if saturated.
  writeRegister(REG_LED1_PA, 0x24);
  writeRegister(REG_LED2_PA, 0x24);

  // Slot 1 = red, slot 2 = IR. Useful if board firmware enters multi-LED path.
  writeRegister(REG_MULTI_LED_1, 0x21);

  resetAlgorithm();
  return true;
}

static void updateHeartRate(uint32_t ir) {
  if (irDc == 0.0f) {
    irDc = (float)ir;
  }

  irDc = (0.95f * irDc) + (0.05f * (float)ir);
  prevFilteredIr = filteredIr;
  filteredIr = (float)ir - irDc;
  signalMeanAbs = (0.95f * signalMeanAbs) + (0.05f * fabsf(filteredIr));

  const float threshold = max(MIN_BEAT_THRESHOLD, signalMeanAbs * 0.55f);

  if (filteredIr < -threshold * 0.5f) {
    armedForBeat = true;
  }

  const bool upwardCrossing = (prevFilteredIr <= threshold) && (filteredIr > threshold);
  if (armedForBeat && upwardCrossing) {
    uint32_t now = millis();
    uint32_t delta = now - lastBeatMs;

    if (lastBeatMs != 0 && delta >= MIN_BEAT_MS && delta <= MAX_BEAT_MS) {
      float bpm = 60000.0f / (float)delta;
      lastBpm = bpm;
      bpmHistory[bpmIndex] = bpm;
      bpmIndex = (bpmIndex + 1) % 4;
      if (bpmValidCount < 4) {
        bpmValidCount++;
      }

      float sum = 0.0f;
      for (uint8_t i = 0; i < bpmValidCount; i++) {
        sum += bpmHistory[i];
      }
      bpmAverage = sum / (float)bpmValidCount;
    }

    lastBeatMs = now;
    armedForBeat = false;
  }
}

static Spo2Result calculateSpo2Window() {
  if (sampleCount < SPO2_WINDOW) {
    return {false, 0};
  }

  double redMean = 0.0;
  double irMean = 0.0;
  uint32_t redMin = 0xFFFFFFFF;
  uint32_t redMax = 0;
  uint32_t irMin = 0xFFFFFFFF;
  uint32_t irMax = 0;
  for (uint16_t i = 0; i < SPO2_WINDOW; i++) {
    redMean += redBuffer[i];
    irMean += irBuffer[i];
    if (redBuffer[i] < redMin) redMin = redBuffer[i];
    if (redBuffer[i] > redMax) redMax = redBuffer[i];
    if (irBuffer[i] < irMin) irMin = irBuffer[i];
    if (irBuffer[i] > irMax) irMax = irBuffer[i];
  }
  redMean /= SPO2_WINDOW;
  irMean /= SPO2_WINDOW;

  if (redMean < 1.0 || irMean < 1.0) {
    return {false, 0};
  }

  // Reject weak contact, saturation, and large baseline movement.
  if (redMean < 50000.0 || irMean < 50000.0 || redMean > 240000.0 || irMean > 240000.0) {
    return {false, 0};
  }
  if ((redMax - redMin) > 25000 || (irMax - irMin) > 25000) {
    return {false, 0};
  }

  double redAcSq = 0.0;
  double irAcSq = 0.0;
  for (uint16_t i = 0; i < SPO2_WINDOW; i++) {
    double redAc = (double)redBuffer[i] - redMean;
    double irAc = (double)irBuffer[i] - irMean;
    redAcSq += redAc * redAc;
    irAcSq += irAc * irAc;
  }

  double redRms = sqrt(redAcSq / SPO2_WINDOW);
  double irRms = sqrt(irAcSq / SPO2_WINDOW);
  if (redRms < 15.0 || irRms < 15.0) {
    return {false, 0};
  }

  double redPerfusion = redRms / redMean;
  double irPerfusion = irRms / irMean;
  if (redPerfusion < 0.00015 || irPerfusion < 0.00015 ||
      redPerfusion > 0.03 || irPerfusion > 0.03) {
    return {false, 0};
  }

  double ratio = redPerfusion / irPerfusion;
  if (ratio < 0.4 || ratio > 3.0) {
    return {false, 0};
  }

  int spo2 = (int)round(110.0 - (25.0 * ratio)) + SPO2_CALIBRATION_OFFSET;

  if (spo2 > 100) {
    spo2 = 100;
  }
  if (spo2 < 70) {
    spo2 = 70;
  }
  return {true, spo2};
}

static int updateFinalSpo2() {
  if (fingerStartMs == 0 || millis() - fingerStartMs < SPO2_SETTLE_MS) {
    return 0;
  }

  if (lastSpo2UpdateMs != 0 && millis() - lastSpo2UpdateMs < SPO2_UPDATE_MS) {
    return finalSpo2;
  }

  Spo2Result result = calculateSpo2Window();
  if (!result.valid) {
    return finalSpo2;
  }

  lastSpo2UpdateMs = millis();
  spo2History[spo2Index] = result.value;
  spo2Index = (spo2Index + 1) % SPO2_HISTORY_SIZE;
  if (spo2ValidCount < SPO2_HISTORY_SIZE) {
    spo2ValidCount++;
  }

  if (spo2ValidCount < SPO2_REQUIRED_VALID_WINDOWS) {
    return 0;
  }

  int sum = 0;
  for (uint8_t i = 0; i < spo2ValidCount; i++) {
    sum += spo2History[i];
  }
  finalSpo2 = (int)round((float)sum / (float)spo2ValidCount);
  return finalSpo2;
}

static void storeSample(uint32_t red, uint32_t ir) {
  redBuffer[sampleIndex] = red;
  irBuffer[sampleIndex] = ir;
  sampleIndex = (sampleIndex + 1) % SPO2_WINDOW;
  if (sampleCount < SPO2_WINDOW) {
    sampleCount++;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  connectWiFi();
  connectMqtt();

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(I2C_CLOCK_HZ);

  if (!setupMax30102()) {
    Serial.println("STATUS,MAX30102 not found");
    while (true) {
      delay(1000);
    }
  }

  Serial.println("STATUS,MAX30102 ready");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    connectMqtt();
  }
  if (mqttClient.connected()) {
    mqttClient.loop();
  }

  uint8_t samples = availableSamples();
  if (samples == 0) {
    delay(5);
    return;
  }

  while (samples--) {
    uint32_t red = 0;
    uint32_t ir = 0;

    if (!readFifoSample(red, ir)) {
      Serial.println("STATUS,FIFO read error");
      delay(100);
      return;
    }

    if (ir < FINGER_THRESHOLD || red < FINGER_THRESHOLD) {
      resetAlgorithm();
      if (millis() - lastPrintMs >= 1000) {
        Serial.println("NO_FINGER,0,0");
        publishVitals(0, 0, false);
        lastPrintMs = millis();
      }
      continue;
    }

    if (fingerStartMs == 0) {
      fingerStartMs = millis();
    }

    storeSample(red, ir);
    updateHeartRate(ir);
    int spo2 = updateFinalSpo2();

    if (millis() - lastPrintMs >= 1000) {
      int bpm = (int)round(bpmAverage > 0.0f ? bpmAverage : lastBpm);
      Serial.print(bpm);
      Serial.print(",");
      Serial.println(spo2);
      publishVitals(bpm, spo2, true);
      lastPrintMs = millis();
    }
  }
}
