# Changelog

## 1.1.0 - 2026-10-22

### Added
- Read-only reader for DualSense, DualSense Edge and DualShock 4 controllers (USB and Bluetooth report layouts), with the button names `PS`, `TOUCHPAD` and `MUTE`. It opens a controller 10 seconds after the game starts and 5 seconds after the controller appears, and never writes to it.
- Unit tests for the controller report parsing and the button-name parser.

### Known problem
- Inside the running game the plugin does not see the controller, so the controller toggle does not work. `Controller = NONE` is the default. The keyboard toggle is unaffected.

Tested in game on 2026-10-22: Delete hides and shows the HUD in `draw` mode (6 toggles recorded in the log).

## 1.0.0 - 2026-10-22

- First version: Delete, or L3 + R3 on an Xbox-style controller, hides and shows the gameplay HUD.
- Two modes, `draw` and `gate`.
- The places to change are found by byte patterns and verified before use. Unexpected bytes: log and do nothing. A leftover change is restored.
