# SafeBootloader

Bootloader for the ESP32 that maintains mirrored OTA (ota_0 / ota_1) and SPIFFS partitions and restores them automatically if corruption is detected.

This project is a pure ESP-IDF application and is currently based on and tested with v4.4.8.

## How to Compile
- Install ESP-IDF version 4.4.8.
- In the project directory run: `idf.py build`
- Use `idf.py menuconfig`, 'Bootloader config' if needed, for example to change log verbosity level

## How to Use
 - Flash the compiled bootloader and partition table to your ESP32 using esptool.py.
 - Use the provided partitions.csv in your application project.
 - Integrate the files safe_boot_functions.cpp and safe_boot_functions.h into your firmware.
 - Call the following functions from your application:
1) When the SPIFFS partition is modified (for example after writing files):
`update_spiffs_crc();`
2) When an OTA update writes a new firmware image (for example after a successful OTA update):
`update_ota_crc();`
These functions calculate and store CRC values used by the bootloader to verify the integrity of the partitions.
- from your application build do not flash bootloader, only the app!!!

## How It Works
- The bootloader provides a simple redundancy mechanism for firmware and filesystem data.
- SPIFFS Protection: Two SPIFFS partitions are used: spiffs and spiffs_backup. For each partition a CRC value is stored. During boot: If one partition is corrupted → it is automatically restored from the valid mirror.
- OTA Firmware Protection: Two firmware partitions are used: ota_0 and ota_1 (as usual). After each OTA update: the CRC of the updated partition is calculated
the value is stored in a dedicated CRC partition. During boot: The bootloader verifies the CRC of the currently selected OTA partition. If the CRC check fails → the bootloader automatically restores from the other OTA partition.
