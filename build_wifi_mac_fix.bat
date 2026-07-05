@echo off
call E:\Tools\ESP-IDF\Espressif\frameworks\esp-idf-v5.5.2\export.bat
idf.py build flash monitor -p COM5
