@echo off
title AD5940 Impedance Data Logger
echo Starting AD5940 Impedance Logger...
python "%~dp0save_readings.py"
pause
