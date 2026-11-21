# TikPrint Firmware

Firmware for a small ESP32-S3 project. Work in progress.

## Build

Needs ESP-IDF.

```sh
idf.py build
idf.py -p <port> flash monitor
```

## Updates

Releases are built from `main` when the version number in `CMakeLists.txt` changes.
