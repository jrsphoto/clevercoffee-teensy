# Argenta WIP (not built)

Files parked here are NOT compiled (PlatformIO only builds `src/` and `lib/`).

## TempSensorMAX31865.{h,cpp}
Sample PT100/MAX31865 temp-sensor driver for the Argenta board. NOT yet integrated:
- Needs the `Adafruit_MAX31865` library added to `platformio.ini` lib_deps:
    adafruit/Adafruit MAX31865 library
- Was written against the OLD CleverCoffee TempSensor interface; the current fork's
  interface changed (Timer-based update + error counting) — port it to match
  `src/hardware/tempsensors/TempSensor.h` (use TempSensorTSIC/Dallas as the pattern).
Do this during Phase 1 firmware, when the board is in hand to test against.
