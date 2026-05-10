# ESP32 AD5933 Impedance Dashboard

Arduino IDE sketch for an ESP32 connected to an AD5933 impedance converter. The ESP32 connects to WiFi, hosts a dashboard on port 80, stores the latest sweep in RAM, and saves every measurement session as a CSV file in LittleFS.

## Required Libraries

- `WiFi.h`
- `WebServer.h`
- `Wire.h`
- `LittleFS.h`

These are available with the Arduino IDE ESP32 board package.

## Hardware Connections

Connect the ESP32 to the AD5933:

- SDA: GPIO26
- SCL: GPIO27
- 3.3V: AD5933 VCC
- GND: AD5933 GND

Current sketch settings:

- AD5933 I2C address: `0x0D`
- Calibration resistor: `16000` ohm
- PGA: x1
- Start frequency: `1000` Hz
- Frequency step: `1000` Hz
- Points: `100`

## WiFi Credentials

WiFi credentials are stored in `Impedance/secrets.h`. This file is ignored by git through `.gitignore` so credentials are not committed.

## Calibration

1. Connect a 16k resistor between AD5933 VOUT and VIN.
2. Upload the sketch.
3. Open the webpage hosted by the ESP32.
4. Press **Start Calibration**.

## Measurement

1. Replace the calibration resistor with the RC circuit or unknown impedance.
2. Press **Start Measurement**.
3. The latest sweep appears in the dashboard immediately.
4. A new persistent CSV file is saved in LittleFS under `/sessions/`.

## Open the Dashboard

1. Upload the code from Arduino IDE.
2. Open Serial Monitor at `115200` baud.
3. Copy the ESP32 IP address printed after WiFi connects.
4. Open `http://ESP32_IP/` in a browser.

## Download CSV

To download the latest RAM sweep, open:

```text
http://ESP32_IP/csv
```

To download a saved session, use the **Download CSV** button beside that session in the dashboard.

The CSV contains:

```text
Frequency,Real,Imag,Z,Phase,R,X
```

## LittleFS Sessions

The sketch uses LittleFS, not SPIFFS. On startup it mounts LittleFS and creates `/sessions` if the folder does not exist.

Each click on **Start Measurement** creates a new file:

```text
/sessions/session_YYYYMMDD_HHMMSS.csv
```

If real time is not available, the filename falls back to:

```text
/sessions/session_<millis>.csv
```

In Arduino IDE, select an ESP32 partition scheme that includes a filesystem area before uploading. No initial filesystem image is required for this sketch because it creates `/sessions` at runtime. If you later add files to a `data/` folder, use the ESP32 LittleFS upload tool/plugin to upload that filesystem image.

Saved sessions can be opened, downloaded, or deleted from the dashboard. New measurements do not delete older session files.

## Cloud Storage Placeholder

`cloud_config.h` contains placeholder Firebase/Supabase settings. The sketch includes `uploadPointToCloud(...)` and `uploadSessionToCloud(...)` TODO hooks. Cloud upload is disabled by default, so the dashboard and measurements work without cloud configuration.

## Warning

Recalibrate after reboot, after changing PGA, RFB, excitation range, or frequency settings.
