# Original X4 support

This branch targets the original ESP32-C3 Xteink X4, not the ESP32-S3 X4 Pro or X4
Classic. CrossMosa uses one C3 firmware image for the original X3 and X4; the
device profile is selected at boot by the hardware fingerprint and can be
overridden for recovery.

## Hardware profiles

| Device | Panel geometry | Display controller | Battery / optional hardware | Refresh policy |
| --- | ---: | --- | --- | --- |
| X4 | 480 x 800 | SSD1677 | ADC; no X3 RTC or IMU | normal FAST/HALF |
| X3 (older) | 528 x 792 | UC8253 | BQ27220, optional DS3231/QMI8658 | X3 differential grayscale |
| X3 (newer) | 528 x 792 | UC8279 | BQ27220, optional DS3231/QMI8658 | X3 differential grayscale |

The shared HAL must remain runtime-driven. Do not replace `renderer` dimensions
with X3 constants, and do not call X3 controller or grayscale paths for X4.

## XTC policy

XTC/XTCH is a pre-rendered, original-X4 format. It remains available on X4 and
is hidden/rejected on X3, where its 480 x 800 pages cannot be rendered safely.
EPUB, TXT/Markdown, BMP, networking, settings, screenshots, sleep, and firmware
update paths are shared features and must work on both profiles.

## Current implementation status

- The build already defines both `FREEINK_DEVICE_X4` and `FREEINK_DEVICE_X3`.
- The HAL already selects X3/X4 at runtime and preserves UC8253/UC8279 detection.
- Screenshot row storage is bounded for both runtime framebuffer geometries.
- XTC entry points are gated to the original X4.
- Hardware validation is still required before publishing an X4 release. The
  minimum matrix is one original X4, one UC8253 X3, and one UC8279 X3.

## Validation matrix

For each device, test clean first boot, warm reboot, deep-sleep wake, USB
charging, battery percentage, all buttons, screen orientation, EPUB text and
images, grayscale pages, TXT/Markdown, BMP, fonts, Wi-Fi/file transfer,
screenshots, custom sleep images, settings persistence, and firmware recovery.

X4-specific checks:

1. Confirm the first-boot probe selects SSD1677 and reports 480 x 800.
2. Capture a screenshot and verify the resulting portrait BMP is 480 x 800
   with no truncation or missing right edge.
3. Open both 1-bit XTC and 2-bit XTCH books and verify page orientation,
   grayscale levels, status-bar overlay, progress, and sleep-cover generation.
4. Verify ADC battery/USB detection and that X3-only clock, tilt, and UC8279
   behavior are absent.
5. Exercise the normal USB flasher path using the X4 target selector; use the
   CrossMosa SD rescue path only after the firmware is already installed.
