#!/bin/bash
echo "Starting AD5940 Impedance Logger..."
python3 "$(dirname "$0")/save_readings.py"
