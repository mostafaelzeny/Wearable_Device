"""
=============================================================================
AD5940 Impedance Sweep Serial Logger
=============================================================================
This script automatically connects to your ESP32, captures the impedance
readings, and saves 3 separate CSV files for each sweep in this folder:
  - Sweep 1: reading_1.csv, reading_2.csv, reading_3.csv
  - Sweep 2: sweep_2_reading_1.csv, sweep_2_reading_2.csv, ...
  (Each file contains: Frequency_Hz, Real_Ohm, Imag_Ohm)

Usage:
  Double-click save_readings.bat (or run python save_readings.py)
=============================================================================
"""

import os
import sys
import re
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Error: pyserial is not installed. Installing it now...")
    os.system(f'"{sys.executable}" -m pip install pyserial')
    import serial
    import serial.tools.list_ports

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

def find_esp32_port():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        return None

    for p in ports:
        desc = p.description.lower()
        if any(keyword in desc for keyword in ["cp210", "ch340", "ftdi", "usb-serial", "uart", "esp32"]):
            return p.device

    non_bt = [p.device for p in ports if "bluetooth" not in p.description.lower()]
    if non_bt:
        return non_bt[0]

    return ports[0].device

def main():
    print("=" * 65)
    print("  AD5940 Impedance Measurement Data Logger")
    print("=" * 65)
    print(f"Target folder: {SCRIPT_DIR}")

    port = find_esp32_port()
    if not port:
        print("\n[!] No serial COM port detected.")
        print("Please plug in your ESP32 board via USB and run this script again.")
        input("\nPress Enter to exit...")
        sys.exit(1)

    print(f"\n[+] Selected port: {port}")
    print("[+] Connecting at 115200 baud...")

    try:
        ser = serial.Serial(port, 115200, timeout=1.0)
    except Exception as e:
        print(f"\n[!] Could not open port {port}: {e}")
        print("Make sure the Arduino IDE Serial Monitor is closed so the port is free!")
        input("\nPress Enter to exit...")
        sys.exit(1)

    time.sleep(1.0)

    operation_number = 1
    file_handles = {}
    saved_files = []

    reading_pattern = re.compile(
        r"Freq:\s*([-\d\.]+)\s*Hz\s*\|\s*Real:\s*([-\d\.]+)\s*Ohm\s*\|\s*Imag:\s*([-\d\.]+)\s*Ohm\s*\(Sweep\s*(\d+)/(\d+)\)",
        re.IGNORECASE
    )

    print("[+] Listening for impedance sweep data from ESP32...")
    print("    (Press Ctrl+C at any time to stop logging)\n")

    try:
        while True:
            line_bytes = ser.readline()
            if not line_bytes:
                continue
            
            line = line_bytes.decode("utf-8", errors="ignore").strip()
            if not line:
                continue

            # Print to console
            print(line)

            # Match individual reading: Freq, Real, Imag (Sweep X/N)
            m_read = reading_pattern.search(line)
            if m_read:
                freq = m_read.group(1)
                real = m_read.group(2)
                imag = m_read.group(3)
                sweep_idx = int(m_read.group(4))

                if operation_number == 1:
                    filename = f"sweep_{sweep_idx}.csv"
                else:
                    filename = f"op_{operation_number}_sweep_{sweep_idx}.csv"

                file_path = os.path.join(SCRIPT_DIR, filename)
                if sweep_idx not in file_handles:
                    file_handles[sweep_idx] = open(file_path, "w", encoding="utf-8")
                    file_handles[sweep_idx].write("Frequency_Hz,Real_Ohm,Imag_Ohm\n")
                    saved_files.append(filename)

                file_handles[sweep_idx].write(f"{freq},{real},{imag}\n")
                file_handles[sweep_idx].flush()

            if "--- All Sweeps Complete ---" in line:
                print("\n" + "=" * 65)
                print(f"[SUCCESS] Operation {operation_number} (3 sweeps) complete! Saved files in this folder:")
                for fname in saved_files:
                    print(f"  -> {fname}")
                print("=" * 65 + "\n")

                # Close file handles for this operation
                for f in file_handles.values():
                    f.close()
                file_handles.clear()
                saved_files.clear()
                operation_number += 1
                
                print("Press ENTER to restart the sweeps, or Ctrl+C to quit...")
                try:
                    # Clear serial buffer before waiting
                    ser.reset_input_buffer()
                    input()
                    print("[+] Restarting...")
                    ser.write(b'R')
                except KeyboardInterrupt:
                    print("\n[+] Stopped logging.")
                    break

    except KeyboardInterrupt:
        print("\n[+] Stopped logging.")
    finally:
        for f in file_handles.values():
            f.close()
        ser.close()
        print("[+] Serial port closed.")

if __name__ == "__main__":
    main()
