/*
 * MegaJoy - customizable USB joystick firmware for Arduino Mega 2560
 * (works together with the 16U2 firmware in usb_16u2/ and configurator.html)
 *
 * What it does
 *  - up to 32 buttons on any pin 2..69 (54..69 = A0..A15), any button -> any joystick button number
 *  - per button: normal / inverted / toggle
 *  - 4 axes: analog pot, OR two digital pins (stick as axis)
 *  - 1 hat (4 pins)
 *  - settings saved in EEPROM, edit them live with configurator.html
 *
 * Default: 24 buttons on pins 22..45 (to GND, internal pull-ups).
 * Do NOT use pins 0 and 1 (they talk to the USB chip).
 *
 * Serial text commands (500000 baud, one per line):
 *   HELLO | GET | SAVE | DEFAULTS | STREAM 0/1 | FIND 0/1
 *   SET DEB <ms>
 *   SET BTN <i 0-31> <pin|255> <mode 0-3> <target 1-32>      mode bit0=invert, bit1=toggle
 *   SET AXIS <i 0-3> <type 0 off,1 analog,2 two-pins> <pinA> <pinB> <invert> <min> <max> <dead>
 *   SET HAT <up> <right> <down> <left>
 */
#include <EEPROM.h>

#define NUM_BTN   32
#define NUM_AXIS  4
#define PIN_OFF   255
#define MIN_PIN   2
#define MAX_PIN   69
#define CFG_MAGIC 0x4A4D
#define CFG_VER   1
#define FRAME_START 0xA5

struct AxisCfg {
  uint8_t  type;     // 0 off, 1 analog, 2 two digital pins
  uint8_t  pinA;     // analog: 54..69 (A0..A15) | two pins: "minus" pin
  uint8_t  pinB;     // two pins: "plus" pin
  uint8_t  invert;
  uint16_t minV;     // analog calibration
  uint16_t maxV;
  uint8_t  dead;     // dead zone around center (0..100, in 0..1023 units)
};

struct Config {
  uint16_t magic;
  uint8_t  ver;
  uint8_t  debounce;          // ms
  uint8_t  pin[NUM_BTN];
  uint8_t  mode[NUM_BTN];
  uint8_t  target[NUM_BTN];   // joystick button number 1..32
  AxisCfg  axis[NUM_AXIS];
  uint8_t  hat[4];            // up, right, down, left
  uint8_t  crc;
};

Config cfg;

// ---------- state ----------
bool     lastRaw[NUM_BTN], stable[NUM_BTN], tog[NUM_BTN];
uint32_t changeT[NUM_BTN];

uint32_t outButtons = 0;
uint16_t outAxis[NUM_AXIS], rawAxis[NUM_AXIS];
uint8_t  outHat = 0;

uint8_t  lastFrame[14];
uint32_t lastSendMs = 0;

bool     streamOn = false, findOn = false;
uint32_t lastStreamMs = 0;

int16_t  dumpStep = -1;
uint32_t lastDumpUs = 0;

char     lineBuf[96];
uint8_t  lineLen = 0;

// ---------- helpers ----------
void sendLine(const char* s) {            // one write call so frames never split a line
  uint8_t n = strlen(s);
  Serial.write('\n');                      // leading newline: keeps the line clean even if stray bytes came before it
  Serial.write((const uint8_t*)s, n);
}

uint8_t calcCrc(const Config& c) {
  const uint8_t* p = (const uint8_t*)&c;
  uint8_t x = 0x5A;
  for (uint16_t i = 0; i < sizeof(Config) - 1; i++) x = (uint8_t)((x << 1) | (x >> 7)) ^ p[i];
  return x;
}

bool validPin(int p) { return p == PIN_OFF || (p >= MIN_PIN && p <= MAX_PIN); }

void setDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = CFG_MAGIC;
  cfg.ver = CFG_VER;
  cfg.debounce = 5;
  for (uint8_t i = 0; i < NUM_BTN; i++) {
    cfg.pin[i] = (i < 24) ? (22 + i) : PIN_OFF;
    cfg.mode[i] = 0;
    cfg.target[i] = i + 1;
  }
  for (uint8_t a = 0; a < NUM_AXIS; a++) {
    cfg.axis[a].type = 0;
    cfg.axis[a].pinA = PIN_OFF;
    cfg.axis[a].pinB = PIN_OFF;
    cfg.axis[a].invert = 0;
    cfg.axis[a].minV = 0;
    cfg.axis[a].maxV = 1023;
    cfg.axis[a].dead = 0;
  }
  for (uint8_t h = 0; h < 4; h++) cfg.hat[h] = PIN_OFF;
}

bool configLooksOk() {
  if (cfg.magic != CFG_MAGIC || cfg.ver != CFG_VER || cfg.crc != calcCrc(cfg)) return false;
  for (uint8_t i = 0; i < NUM_BTN; i++)
    if (!validPin(cfg.pin[i]) || cfg.target[i] < 1 || cfg.target[i] > 32) return false;
  return true;
}

void loadConfig() {
  EEPROM.get(0, cfg);
  if (!configLooksOk()) setDefaults();
}

void saveConfig() {
  cfg.crc = calcCrc(cfg);
  EEPROM.put(0, cfg);             // put() only writes bytes that changed
}

void applyPins() {
  for (uint8_t p = MIN_PIN; p <= MAX_PIN; p++) pinMode(p, INPUT);
  for (uint8_t i = 0; i < NUM_BTN; i++)
    if (cfg.pin[i] != PIN_OFF) pinMode(cfg.pin[i], INPUT_PULLUP);
  for (uint8_t h = 0; h < 4; h++)
    if (cfg.hat[h] != PIN_OFF) pinMode(cfg.hat[h], INPUT_PULLUP);
  for (uint8_t a = 0; a < NUM_AXIS; a++) {
    if (cfg.axis[a].type == 2) {
      if (cfg.axis[a].pinA != PIN_OFF) pinMode(cfg.axis[a].pinA, INPUT_PULLUP);
      if (cfg.axis[a].pinB != PIN_OFF) pinMode(cfg.axis[a].pinB, INPUT_PULLUP);
    }
  }
  memset(lastRaw, 0, sizeof(lastRaw));
  memset(stable, 0, sizeof(stable));
  memset(tog, 0, sizeof(tog));
}

void applyFindPins() {            // pin finder: every pin gets a pull-up
  for (uint8_t p = MIN_PIN; p <= MAX_PIN; p++) pinMode(p, INPUT_PULLUP);
}

// ---------- reading inputs ----------
void scanButtons() {
  uint32_t now = millis();
  uint32_t mask = 0;
  for (uint8_t i = 0; i < NUM_BTN; i++) {
    uint8_t p = cfg.pin[i];
    if (p == PIN_OFF) continue;
    bool r = (digitalRead(p) == LOW);
    if (cfg.mode[i] & 1) r = !r;                       // invert
    if (r != lastRaw[i]) { lastRaw[i] = r; changeT[i] = now; }
    else if (r != stable[i] && (now - changeT[i]) >= cfg.debounce) {
      stable[i] = r;
      if ((cfg.mode[i] & 2) && r) tog[i] = !tog[i];    // toggle on press
    }
    bool out = (cfg.mode[i] & 2) ? tog[i] : stable[i];
    if (out) mask |= (1UL << (cfg.target[i] - 1));
  }
  outButtons = mask;
}

uint16_t readAxis(uint8_t a) {
  AxisCfg& c = cfg.axis[a];
  uint16_t v = 512;
  rawAxis[a] = 0;
  if (c.type == 1 && c.pinA != PIN_OFF) {
    uint16_t raw = analogRead(c.pinA);
    rawAxis[a] = raw;
    uint16_t lo = c.minV, hi = c.maxV;
    if (hi <= lo + 1) { lo = 0; hi = 1023; }
    int32_t s = ((int32_t)raw - lo) * 1023L / (hi - lo);
    if (s < 0) s = 0;
    if (s > 1023) s = 1023;
    v = (uint16_t)s;
    if (c.dead && abs((int16_t)v - 512) <= c.dead) v = 512;
  } else if (c.type == 2) {
    bool neg = (c.pinA != PIN_OFF) && digitalRead(c.pinA) == LOW;
    bool pos = (c.pinB != PIN_OFF) && digitalRead(c.pinB) == LOW;
    rawAxis[a] = (neg ? 1 : 0) | (pos ? 2 : 0);
    if (neg && !pos) v = 0;
    else if (pos && !neg) v = 1023;
  }
  if (c.invert) v = 1023 - v;
  return v;
}

uint8_t readHat() {
  bool u = cfg.hat[0] != PIN_OFF && digitalRead(cfg.hat[0]) == LOW;
  bool r = cfg.hat[1] != PIN_OFF && digitalRead(cfg.hat[1]) == LOW;
  bool d = cfg.hat[2] != PIN_OFF && digitalRead(cfg.hat[2]) == LOW;
  bool l = cfg.hat[3] != PIN_OFF && digitalRead(cfg.hat[3]) == LOW;
  if (u && d) u = d = false;
  if (l && r) l = r = false;
  if (u && r) return 2;
  if (r && d) return 4;
  if (d && l) return 6;
  if (l && u) return 8;
  if (u) return 1;
  if (r) return 3;
  if (d) return 5;
  if (l) return 7;
  return 0;
}

void sendFrame() {
  uint8_t f[15];
  f[0] = FRAME_START;
  memcpy(f + 1, &outButtons, 4);
  for (uint8_t a = 0; a < NUM_AXIS; a++) memcpy(f + 5 + a * 2, &outAxis[a], 2);
  f[13] = outHat;
  uint8_t x = 0;
  for (uint8_t i = 1; i <= 13; i++) x ^= f[i];
  f[14] = x;

  uint32_t now = millis();
  bool changed = memcmp(f + 1, lastFrame, 14) != 0;
  if ((changed && now - lastSendMs >= 2) || now - lastSendMs >= 50) {
    Serial.write(f, 15);
    memcpy(lastFrame, f + 1, 14);
    lastSendMs = now;
  }
}

// ---------- serial commands ----------
void cmdGetLine(int16_t step) {
  char b[64];
  if (step == 0) snprintf(b, sizeof(b), "DEB %u\n", cfg.debounce);
  else if (step <= NUM_BTN) {
    uint8_t i = step - 1;
    snprintf(b, sizeof(b), "BTN %u %u %u %u\n", i, cfg.pin[i], cfg.mode[i], cfg.target[i]);
  } else if (step <= NUM_BTN + NUM_AXIS) {
    uint8_t a = step - NUM_BTN - 1;
    AxisCfg& c = cfg.axis[a];
    snprintf(b, sizeof(b), "AXIS %u %u %u %u %u %u %u %u\n", a, c.type, c.pinA, c.pinB, c.invert, c.minV, c.maxV, c.dead);
  } else if (step == NUM_BTN + NUM_AXIS + 1) {
    snprintf(b, sizeof(b), "HAT %u %u %u %u\n", cfg.hat[0], cfg.hat[1], cfg.hat[2], cfg.hat[3]);
  } else {
    snprintf(b, sizeof(b), "END\n");
  }
  sendLine(b);
}

long arg(char*& p) {                         // next number from the line
  char* t = strtok_r(NULL, " ", &p);
  if (!t) return -1;
  return atol(t);
}

void handleLine(char* line) {
  char* sp = NULL;
  char* cmd = strtok_r(line, " ", &sp);
  if (!cmd) return;

  if (!strcmp(cmd, "HELLO")) { sendLine("MEGAJOY OK 1\n"); return; }
  if (!strcmp(cmd, "GET"))   { dumpStep = 0; lastDumpUs = 0; return; }
  if (!strcmp(cmd, "SAVE"))  { saveConfig(); sendLine("OK saved\n"); return; }
  if (!strcmp(cmd, "DEFAULTS")) { setDefaults(); applyPins(); sendLine("OK defaults\n"); return; }
  if (!strcmp(cmd, "STREAM")) { streamOn = arg(sp) > 0; sendLine("OK\n"); return; }
  if (!strcmp(cmd, "FIND")) {
    findOn = arg(sp) > 0;
    if (findOn) applyFindPins(); else applyPins();
    sendLine("OK\n");
    return;
  }
  if (!strcmp(cmd, "SET")) {
    char* what = strtok_r(NULL, " ", &sp);
    if (!what) { sendLine("ERR set\n"); return; }

    if (!strcmp(what, "DEB")) {
      long v = arg(sp);
      if (v < 0 || v > 100) { sendLine("ERR deb\n"); return; }
      cfg.debounce = v;
    } else if (!strcmp(what, "BTN")) {
      long i = arg(sp), p = arg(sp), m = arg(sp), t = arg(sp);
      if (i < 0 || i >= NUM_BTN || !validPin(p) || m < 0 || m > 3 || t < 1 || t > 32) { sendLine("ERR btn\n"); return; }
      cfg.pin[i] = p; cfg.mode[i] = m; cfg.target[i] = t;
    } else if (!strcmp(what, "AXIS")) {
      long a = arg(sp), ty = arg(sp), pa = arg(sp), pb = arg(sp), inv = arg(sp), mn = arg(sp), mx = arg(sp), dz = arg(sp);
      if (a < 0 || a >= NUM_AXIS || ty < 0 || ty > 2 || !validPin(pa) || !validPin(pb) ||
          inv < 0 || mn < 0 || mx < 0 || mn > 1023 || mx > 1023 || dz < 0 || dz > 100) { sendLine("ERR axis\n"); return; }
      if (ty == 1 && pa != PIN_OFF && pa < 54) { sendLine("ERR axis pin must be A0-A15\n"); return; }
      AxisCfg& c = cfg.axis[a];
      c.type = ty; c.pinA = pa; c.pinB = pb; c.invert = inv ? 1 : 0; c.minV = mn; c.maxV = mx; c.dead = dz;
    } else if (!strcmp(what, "HAT")) {
      long h0 = arg(sp), h1 = arg(sp), h2 = arg(sp), h3 = arg(sp);
      if (!validPin(h0) || !validPin(h1) || !validPin(h2) || !validPin(h3)) { sendLine("ERR hat\n"); return; }
      cfg.hat[0] = h0; cfg.hat[1] = h1; cfg.hat[2] = h2; cfg.hat[3] = h3;
    } else { sendLine("ERR set what\n"); return; }

    if (findOn) applyFindPins(); else applyPins();
    sendLine("OK\n");
    return;
  }
  sendLine("ERR unknown\n");
}

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen) { lineBuf[lineLen] = 0; handleLine(lineBuf); lineLen = 0; }
    } else if (c >= 32 && c < 127) {
      if (lineLen < sizeof(lineBuf) - 1) lineBuf[lineLen++] = c; else lineLen = 0;
    }
  }
}

void doStreams() {
  uint32_t now = millis();

  if (dumpStep >= 0) {                        // paced so the USB chip buffer never overflows
    uint32_t us = micros();
    if (us - lastDumpUs >= 3000) {
      lastDumpUs = us;
      cmdGetLine(dumpStep);
      if (dumpStep == NUM_BTN + NUM_AXIS + 2) dumpStep = -1; else dumpStep++;
    }
    return;
  }

  if ((streamOn || findOn) && now - lastStreamMs >= 33) {
    lastStreamMs = now;
    char b[96];
    if (streamOn) {
      snprintf(b, sizeof(b), "ST %08lX %u %u %u %u %u %u %u %u %u\n", (unsigned long)outButtons, outHat,
               outAxis[0], outAxis[1], outAxis[2], outAxis[3], rawAxis[0], rawAxis[1], rawAxis[2], rawAxis[3]);
      sendLine(b);
    }
    if (findOn) {
      uint32_t w0 = 0, w1 = 0, w2 = 0;
      for (uint8_t p = MIN_PIN; p <= MAX_PIN; p++) {
        if (digitalRead(p) == LOW) {
          if (p < 32) w0 |= 1UL << p;
          else if (p < 64) w1 |= 1UL << (p - 32);
          else w2 |= 1UL << (p - 64);
        }
      }
      snprintf(b, sizeof(b), "PN %08lX %08lX %08lX\n", (unsigned long)w0, (unsigned long)w1, (unsigned long)w2);
      sendLine(b);
    }
  }
}

// ---------- main ----------
void setup() {
  Serial.begin(500000);
  loadConfig();
  applyPins();
  memset(lastFrame, 0xFF, sizeof(lastFrame));
  delay(300);
  sendLine("MEGAJOY READY 1\n");
}

void loop() {
  readSerial();
  scanButtons();
  for (uint8_t a = 0; a < NUM_AXIS; a++) outAxis[a] = readAxis(a);
  outHat = readHat();
  sendFrame();
  doStreams();
}
