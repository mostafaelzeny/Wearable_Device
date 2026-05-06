#include <Wire.h>
#include <math.h>

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

void calibrateSystem() {
  Serial.println();
  Serial.println("=== Calibration started ===");
  Serial.println("Use a pure known resistor between VOUT and VIN.");
  Serial.println("Frequency,Real,Imag,Magnitude,RawPhaseDeg,GainFactor");

  setupSweep();

  for (int i = 0; i < NUM_POINTS; i++) {
    if (!waitForData()) {
      Serial.println("Data timeout during calibration");
      return;
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

  Serial.println("=== Calibration finished ===");
}

void measureUnknownRC() {
  Serial.println();
  Serial.println("=== Unknown RC measurement started ===");
  Serial.println("Frequency,Real,Imag,|Z| Ohm,Phase Deg,Resistance R Ohm,Reactance X Ohm");

  setupSweep();

  for (int i = 0; i < NUM_POINTS; i++) {
    if (!waitForData()) {
      Serial.println("Data timeout during measurement");
      return;
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

  Serial.println("=== Measurement finished ===");
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);

  Serial.println("AD5933 Impedance Measurement");
  Serial.println("Step 1: Connect known 16k resistor between VOUT and VIN.");
  Serial.println("Then send character: c");
  Serial.println("Step 2: Connect your unknown RC circuit.");
  Serial.println("Then send character: m");
}

void loop() {
  if (Serial.available()) {
    char cmd = Serial.read();

    if (cmd == 'c') {
      calibrateSystem();
      Serial.println("Now connect unknown RC circuit and send: m");
    }

    if (cmd == 'm') {
      measureUnknownRC();
    }
  }
}
