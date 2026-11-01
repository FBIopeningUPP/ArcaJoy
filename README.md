# MegaJoy

Turn an **Arduino Mega 2560** into a customizable gamepad on Windows (up to 32 buttons, 4 axes, 1 hat) using vJoy.

```
MegaJoy/
├─ configurator.html     Web page: map pins, calibrate, find pins, upload the sketch
├─ sketch/               Arduino sketch (MegaJoy.ino) + compiled MegaJoy_sketch.hex
├─ bridge/               Python bridge: Mega serial -> vJoy (run_bridge.bat)
├─ vjoy/                 vJoy installer (v2.2.2.0, Win10/11) – from github.com/BrunnerInnovation/vJoy
└─ optional_real_usb/    Optional: flash the USB chip so the Mega is a real gamepad (no bridge)
```

## Setup (once)
1. **vJoy:** run `vjoy/vJoySetup_v2.2.2.0_Win10_Win11.exe`, restart. Open *Configure vJoy* → Device 1:
   axes **X Y Z Rz**, **32 buttons**, **1 continuous POV hat** → **Apply**.
   (If buttons above 8 do nothing in Windows, the button count is still 8.)
2. **Python:** install Python 3 (64-bit). `run_bridge.bat` installs `pyserial` for you.
3. **Sketch:** open `configurator.html` in Chrome or Edge → **Upload to Mega** → Upload.
   (Or Arduino IDE: open `sketch/MegaJoy/MegaJoy.ino`, board *Mega 2560*.)

## Map your buttons
1. Open `configurator.html` → **Connect**.
2. **Buttons** tab → **Map all in order**, press each real button when asked (or **Learn** one at a time).
   **Pin finder** tab shows which pin lights up when you press something.
3. Wiring: one leg of each button to its pin, the other to **GND**. Avoid pins 0 and 1.
4. **Save to board**, then **Disconnect**.

## Play
- Double-click `bridge/run_bridge.bat`.
- Check: Win+R → `joy.cpl` → *vJoy Device* → Properties.
- `--port COM10` if it does not find the board, `--list` to show ports, `--dry-run` to test without vJoy.
- Only one program can use the serial port: disconnect the configurator before running the bridge.

## Optional: real USB gamepad (no bridge)
Flash the USB chip once with `optional_real_usb/MegaJoy_16U2.hex` (FLIP or dfu-programmer; touch RESET and GND on the 16U2's 6-pin header first).
Go back with `STOCK_16U2_restore.hex`. Source is in `optional_real_usb/source`.

## Notes
- vJoy is a separate project by its own authors; see https://github.com/BrunnerInnovation/vJoy for its license and source.
- Not yet tested with every wiring; open an issue with what you see.
