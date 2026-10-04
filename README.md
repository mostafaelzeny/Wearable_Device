# AD5941 Impedance Sweep Logger 🚀

Welcome to the AD5940/AD5941 Impedance Logger project! This repository allows you to perform highly accurate impedance sweeps using an ESP32 and save the resulting data automatically to CSV files on your computer.

> [!IMPORTANT]
> The Arduino IDE's Serial Monitor cannot save files! To actually save your sweeps as CSV files, you **must** use the provided python script or shell script.

## 🛠️ How to Use

Follow these steps to run sweeps and save your data:

### 1. Upload to ESP32
Open `AD5940_Impedance.ino` in the Arduino IDE. Compile and upload the sketch to your ESP32. 
**Make sure you close the Serial Monitor in the Arduino IDE after uploading!**

### 2. Run the Logger
Open your terminal in this folder and run the provided helper script:
```bash
./save_readings.sh
```
*(Alternatively, you can run `python3 save_readings.py` directly).*

### 3. Let it Sweep!
The script will automatically connect to your board and begin saving the impedance sweeps sequentially into separate CSV files (`sweep_1.csv`, `sweep_2.csv`, `sweep_3.csv`).

> [!TIP]
> After all sweeps complete, the script will pause. Press **Enter** in your terminal to restart another round of sweeps without having to physically reset the board!

---

## ⚙️ Adjusting the Configuration

All sweep configurations are located at the very top of the `AD5940_Impedance.ino` file under **USER CONFIGURABLE PARAMETERS**. Here is a breakdown of the things you can easily change to fit your needs:

### Sweep Parameters
| Parameter | Default | Description |
|---|---|---|
| `NUM_SWEEPS` | `3` | How many full frequency sweeps to execute before pausing. |
| `SWEEP_START_FREQ` | `100.0f` | The starting frequency in Hz. |
| `SWEEP_STOP_FREQ` | `100e3f` | The stopping frequency in Hz (e.g., 100,000 Hz or 100kHz). |
| `SWEEP_POINTS` | `101` | The number of measurement data points taken per sweep. |
| `SWEEP_LOG` | `bTRUE` | Set to `bTRUE` for logarithmic frequency spacing, or `bFALSE` for linear. |

### Analog Front-End (AFE) Parameters
| Parameter | Default | Description |
|---|---|---|
| `RTIA_RESISTOR` | `HSTIARTIA_5K` | High-Speed TIA Gain. Keep this lower than your lowest expected impedance to avoid clipping. |
| `DAC_VOLT_PP` | `200.0f` | AC excitation voltage in mVpp. For biological materials, keep under 200mV. |
| `RCAL_VALUE` | `10000.0f` | Calibration resistor on the board. Make sure this matches your physical hardware (Default is 10kΩ). |

> [!CAUTION]
> If your `RTIA_RESISTOR` is too high compared to the load, the AFE will saturate and output distorted/incorrect impedance values.

---

## 🔌 Hardware Port Details

*This project is based on a port of the official AD5940 library to ESP32. It supports SPI communications at 8MHz.*

**Connections:**
The electrode switch configuration supports a **4-wire** (Kelvin) setup for high accuracy measurements. The default mapping in the code is:
- **CE0**: Drive (Force +)
- **AIN1**: Positive Sense (Sense +)
- **AIN2**: Negative Sense (Sense -)
- **AIN3**: Current Return (Force -)

*(If you are using a different 4-wire pinout, you can easily change the `SWITCH_` macros under the USER CONFIGURABLE PARAMETERS section of `AD5940_Impedance.ino`)*

*Disclaimer: This library is not officially endorsed by Analog Devices.*
