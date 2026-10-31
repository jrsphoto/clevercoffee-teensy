# Disabled CI workflows

These are upstream CleverCoffee's GitHub Actions, inherited when this repo was
forked. They are disabled here, not deleted, so they stay available if this
work is ever rebased toward upstream.

Why disabled:

- `format.yml` ran a clang-format check against upstream's style over the whole
  tree. This port adds Teensy and ESP32-S3 sources upstream does not have, so it
  failed on every push.
- `main.yml` ran a PlatformIO build with upstream's environment assumptions.
  This repo now has three environments (`teensy41`, `esp32_usb`, `esp32s3`),
  two of which upstream knows nothing about.

Between them they sent a failure email on every single push while testing
nothing that was actually true of this repo.

## If CI is wanted later

A workflow that builds all three environments would be genuinely useful - it
would have caught a real mistake during this port, where a commit landed with
the `esp32_usb` build broken. The shape would be roughly:

```yaml
name: Build
on: [push, pull_request]
jobs:
  build:
    runs-on: ubuntu-latest
    strategy:
      matrix:
        environment: [teensy41, esp32_usb, esp32s3]
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
      - run: pip install platformio
      - run: pio run -e ${{ matrix.environment }}
```

Worth enabling only once all three environments are reliably green, so it
reports real breakage rather than becoming noise again.
