#!/usr/bin/env python3
"""
MegaJoy bridge  -  Arduino Mega serial  ->  virtual gamepad (vJoy, Windows)

The MegaJoy sketch sends a small joystick packet over serial all the time.
This program reads it and shows it to Windows as a vJoy gamepad.
It works with the NORMAL Arduino USB firmware (no 16U2 flashing needed).

  python megajoy_bridge.py                 (auto-finds the Arduino)
  python megajoy_bridge.py --port COM10
  python megajoy_bridge.py --list          (show serial ports)
  python megajoy_bridge.py --dry-run       (no vJoy, just print what it reads)
"""
import argparse, ctypes, os, struct, sys, time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("Please install pyserial first:   pip install pyserial")

FRAME_LEN = 15            # 0xA5 + 13 report bytes + 1 xor byte
AXIS_IDS = (0x30, 0x31, 0x32, 0x35)          # X, Y, Z, Rz
HAT_NAMES = ["-", "N", "NE", "E", "SE", "S", "SW", "W", "NW"]


# ---------------------------------------------------------------- packet reader
def parse(buf):
    """Take every complete, valid packet out of buf (a bytearray). Returns a list of
    (buttons, (x, y, z, rz), hat). Text and broken bytes are skipped."""
    out = []
    while True:
        i = buf.find(b"\xa5")
        if i < 0:
            buf.clear()
            break
        if i > 0:
            del buf[:i]
        if len(buf) < FRAME_LEN:
            break
        x = 0
        for b in buf[1:14]:
            x ^= b
        if x == buf[14]:
            buttons, a0, a1, a2, a3, hat = struct.unpack("<IHHHHB", bytes(buf[1:14]))
            out.append((buttons, (a0, a1, a2, a3), hat))
            del buf[:FRAME_LEN]
        else:
            del buf[:1]
    return out


# ---------------------------------------------------------------- vJoy
class VJoy:
    def __init__(self, dev_id):
        paths = [os.environ.get("VJOY_DLL", ""),
                 r"C:\Program Files\vJoy\x64\vJoyInterface.dll",
                 r"C:\Program Files (x86)\vJoy\x64\vJoyInterface.dll",
                 r"C:\Program Files\vJoy\vJoyInterface.dll"]
        path = next((p for p in paths if p and os.path.exists(p)), None)
        if not path:
            raise RuntimeError("vJoy not found. Install vJoy first (see README.md).")
        d = ctypes.CDLL(path)
        d.SetAxis.argtypes = [ctypes.c_long, ctypes.c_uint, ctypes.c_uint]
        d.SetBtn.argtypes = [ctypes.c_int, ctypes.c_uint, ctypes.c_ubyte]
        d.SetContPov.argtypes = [ctypes.c_uint32, ctypes.c_uint, ctypes.c_ubyte]
        d.SetDiscPov.argtypes = [ctypes.c_int, ctypes.c_uint, ctypes.c_ubyte]
        self.d, self.id = d, dev_id
        if not d.vJoyEnabled():
            raise RuntimeError("The vJoy driver is not enabled. Reinstall vJoy and restart the PC.")
        status = d.GetVJDStatus(dev_id)      # 0 own, 1 free, 2 busy, 3 missing
        if status == 3:
            raise RuntimeError("vJoy device %d is not set up. Open 'Configure vJoy' and enable it." % dev_id)
        if status == 2 or not d.AcquireVJD(dev_id):
            raise RuntimeError("vJoy device %d is busy (another program uses it)." % dev_id)
        d.ResetVJD(dev_id)
        self.nbtn = d.GetVJDButtonNumber(dev_id)
        self.cont = d.GetVJDContPovNumber(dev_id) > 0
        self.disc = d.GetVJDDiscPovNumber(dev_id) > 0
        if self.nbtn < 32:
            print("Note: vJoy device %d has only %d buttons. Set it to 32 in 'Configure vJoy'." % (dev_id, self.nbtn))
        if not (self.cont or self.disc):
            print("Note: vJoy device %d has no hat. Add 1 POV hat in 'Configure vJoy' to use the hat." % dev_id)
        self.last = None

    def apply(self, rep):
        buttons, axes, hat = rep
        old = self.last or (None, (None,) * 4, None)
        d, i = self.d, self.id
        for n in range(min(32, self.nbtn)):
            on = (buttons >> n) & 1
            if old[0] is None or ((old[0] >> n) & 1) != on:
                d.SetBtn(on, i, n + 1)
        for k in range(4):
            if axes[k] != old[1][k]:
                d.SetAxis(min(32767, axes[k] * 32767 // 1023), i, AXIS_IDS[k])
        if hat != old[2]:
            if self.cont:
                d.SetContPov(0xFFFFFFFF if hat == 0 else (hat - 1) * 4500, i, 1)
            elif self.disc:
                d.SetDiscPov(-1 if hat == 0 else (0, 0, 1, 2, 2, 2, 3, 0)[hat - 1], i, 1)
        self.last = rep

    def release(self):
        """Let go of every button/axis (used when the board disconnects, so nothing stays stuck)."""
        try:
            self.d.ResetVJD(self.id)
        except Exception:
            pass
        self.last = None

    def close(self):
        try:
            self.d.ResetVJD(self.id)
            self.d.RelinquishVJD(self.id)
        except Exception:
            pass


class DryRun:
    def apply(self, rep): pass
    def release(self): pass
    def close(self): pass


# ---------------------------------------------------------------- main
def find_port():
    ports = list(serial.tools.list_ports.comports())
    def score(p):
        text = ((p.description or "") + " " + (p.manufacturer or "")).lower()
        if p.vid == 0x2341 and p.pid in (0x0042, 0x0010, 0x0242):   # Mega 2560 (original / R3)
            return 3
        if "mega" in text:
            return 3
        if "arduino" in text or p.vid == 0x2341:
            return 2
        if p.vid in (0x1A86, 0x0403, 0x10C4):                       # CH340 / FTDI / CP210x clones
            return 1
        return 0
    ports = sorted((p for p in ports if score(p)), key=lambda p: (-score(p), p.device))
    return ports[0].device if ports else None


def show(rep):
    buttons, axes, hat = rep
    pressed = [str(n + 1) for n in range(32) if (buttons >> n) & 1]
    line = "Buttons: %-40s X:%4d Y:%4d Z:%4d Rz:%4d  Hat: %s" % (
        " ".join(pressed) or "-", axes[0], axes[1], axes[2], axes[3], HAT_NAMES[hat] if hat < 9 else "?")
    print("\r" + line + " " * 6, end="", flush=True)


def run(port, baud, vj):
    # DTR off before opening: the Arduino is NOT reset when we connect
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, baud, 0.05
    ser.dtr = False
    ser.rts = False
    ser.open()
    print("Connected to %s. Press buttons on your controller. Ctrl+C to stop." % port)
    buf = bytearray()
    last_data = time.time()
    warned = False
    last_rep = None
    last_show = 0.0
    try:
        while True:
            data = ser.read(max(1, ser.in_waiting))
            if data:
                buf += data
                if len(buf) > 4096:
                    del buf[:-64]
                for rep in parse(buf):
                    last_data = time.time()
                    warned = False
                    if rep != last_rep:
                        vj.apply(rep)
                        last_rep = rep
                        now = time.time()
                        if now - last_show >= 0.05:      # console printing is slow; never let it lag the input
                            show(rep)
                            last_show = now
            elif time.time() - last_data > 3 and not warned:
                warned = True
                print("\nNo joystick data. Is the MegaJoy sketch on the Mega? "
                      "(If you flashed the MegaJoy 16U2 firmware you do not need this bridge.)")
    finally:
        ser.close()
        vj.release()                  # board gone or restarting: nothing may stay pressed


def main():
    ap = argparse.ArgumentParser(description="MegaJoy serial -> vJoy bridge")
    ap.add_argument("--port", help="serial port, e.g. COM10 (default: find the Arduino)")
    ap.add_argument("--baud", type=int, default=500000)
    ap.add_argument("--vjoy-id", type=int, default=1, help="vJoy device number (default 1)")
    ap.add_argument("--list", action="store_true", help="list serial ports and exit")
    ap.add_argument("--dry-run", action="store_true", help="do not use vJoy, just print")
    args = ap.parse_args()

    if args.list:
        for p in serial.tools.list_ports.comports():
            print(p.device, "-", p.description)
        return
    try:
        vj = DryRun() if args.dry_run else VJoy(args.vjoy_id)
    except RuntimeError as e:
        sys.exit("Error: " + str(e))
    try:
        while True:
            port = args.port or find_port()
            if not port:
                print("No Arduino found. Plug it in, or use --port COM10 (see --list).")
                time.sleep(2)
                continue
            try:
                run(port, args.baud, vj)
            except serial.SerialException as e:
                print("\nSerial problem: %s  (is the config page or another program using the port?)" % e)
                time.sleep(1.5)
    except KeyboardInterrupt:
        print("\nBye.")
    finally:
        vj.close()


if __name__ == "__main__":
    main()
