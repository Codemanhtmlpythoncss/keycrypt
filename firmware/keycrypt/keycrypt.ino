// KeyCrypt - Raspberry Pi Pico (RP2040) USB keyboard that encrypts / decrypts each line.
//
//  keyboard --(USB host, PIO on GP2/GP3)--> Pico --(native USB, HID keyboard)--> computer
//
// Keys pass straight through. On Enter, depending on the mode switch:
//   ENCRYPT: typed line is erased and re-typed as base64(nonce|AES-256-GCM ciphertext|tag)
//   DECRYPT: typed line (base64) is erased and re-typed as plaintext (left alone if it fails)
//   OFF:     plain keyboard
// Mode switch: GP14 to GND = encrypt, GP15 to GND = decrypt, neither (or both) = off.
// `keycrypt mode ...` can override the switch (saved in flash); `keycrypt mode switch` hands control back.
// Key is set over USB serial ("key <passphrase>") and saved in flash.
//
// Build: Board "Raspberry Pi Pico", USB Stack "Adafruit TinyUSB", CPU 120 MHz.

#include "pio_usb.h"
#include "Adafruit_TinyUSB.h"
#include <EEPROM.h>
#include <pico/util/queue.h>
#include <bearssl/bearssl.h>

// ---------- config ----------
#define HOST_PIN_DP     2        // USB D+ on GP2, D- must be GP3
#define PIN_SW_ENC      14       // switch 1 -> GND = encrypt
#define PIN_SW_DEC      15       // switch 2 -> GND = decrypt
#define STATUS_LED      25       // onboard LED (Pico, not Pico W)
#define MAX_LINE        512      // longest line we track
#define REPEAT_MS       400      // key held this long -> OS auto-repeat, stop tracking
#define PBKDF2_ITERS    50000
static const char PBKDF2_SALT[] = "keycrypt-v1";
static const uint32_t MAGIC = 0x4B435932;  // "KCY2"

enum Mode { MODE_OFF, MODE_ENCRYPT, MODE_DECRYPT };
static const char* const MODE_NAME[] = { "OFF", "ENCRYPT", "DECRYPT" };

// ---------- USB objects ----------
Adafruit_USBH_Host USBHost;
Adafruit_USBD_HID usb_hid;
static const uint8_t hid_desc[] = { TUD_HID_REPORT_DESC_KEYBOARD() };

static const uint8_t keycode2ascii[128][2] = { HID_KEYCODE_TO_ASCII };
static const uint8_t ascii2keycode[128][2] = { HID_ASCII_TO_KEYCODE };

// ---------- cross-core state ----------
struct KbReport { uint8_t mod; uint8_t keys[6]; };
static queue_t reportQ;
static volatile uint8_t ledState = 0;      // from computer (caps lock etc)
static volatile bool ledPending = false;
static volatile uint8_t kbAddr = 0, kbInst = 0;

// debug info about what the host port sees (written on core1)
static volatile uint16_t dbgVid = 0, dbgPid = 0;
static volatile uint8_t dbgHidCount = 0;
static volatile uint32_t dbgMounts = 0, dbgReports = 0;
static char dbgProtos[24] = "";

// ---------- settings (flash) ----------
// modeOverride: 0 = follow switch, 1 = off, 2 = encrypt, 3 = decrypt (set from CLI)
struct Settings { uint32_t magic; uint8_t hasKey; uint8_t key[32]; uint8_t modeOverride; };
static Settings cfg;

// ---------- line tracking (core0) ----------
static char line[MAX_LINE];
static int lineLen = 0;
static bool tracking = true;
static KbReport prev = {};
static bool suppressEnter = false;
static uint8_t lastKey = 0;
static uint32_t lastKeyAt = 0;

// =====================================================================
// helpers
// =====================================================================
static Mode readMode() {
  if (cfg.modeOverride >= 1 && cfg.modeOverride <= 3) return (Mode)(cfg.modeOverride - 1);
  bool enc = digitalRead(PIN_SW_ENC) == LOW;
  bool dec = digitalRead(PIN_SW_DEC) == LOW;
  if (enc && !dec) return MODE_ENCRYPT;
  if (dec && !enc) return MODE_DECRYPT;
  return MODE_OFF;
}

static bool hasKey(const KbReport& r, uint8_t k) {
  for (int i = 0; i < 6; i++) if (r.keys[i] == k) return true;
  return false;
}

static bool sendReport(uint8_t mod, const uint8_t keys[6]) {
  uint32_t t = millis();
  while (!usb_hid.ready()) {
    if (millis() - t > 100) return false;
    delay(1);
  }
  return usb_hid.keyboardReport(0, mod, (uint8_t*)keys);
}

static void tapKey(uint8_t mod, uint8_t key) {
  uint8_t k[6] = { key, 0, 0, 0, 0, 0 };
  uint8_t none[6] = { 0 };
  sendReport(mod, k);
  delay(2);
  sendReport(0, none);
  delay(2);
}

static void typeChar(char c) {
  if ((uint8_t)c >= 128) return;
  uint8_t shift = ascii2keycode[(uint8_t)c][0];
  uint8_t kc = ascii2keycode[(uint8_t)c][1];
  if (!kc) return;
  // computer caps lock flips letters
  if ((ledState & KEYBOARD_LED_CAPSLOCK) && isalpha((unsigned char)c)) shift = !shift;
  tapKey(shift ? KEYBOARD_MODIFIER_LEFTSHIFT : 0, kc);
}

static void loadSettings() {
  EEPROM.get(0, cfg);
  if (cfg.magic != MAGIC) { memset(&cfg, 0, sizeof cfg); cfg.magic = MAGIC; }
  if (cfg.modeOverride > 3) cfg.modeOverride = 0;
}

static void saveSettings() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// =====================================================================
// crypto
// =====================================================================
static void pbkdf2_sha256(const char* pass, uint8_t out[32]) {
  br_hmac_key_context kc;
  br_hmac_key_init(&kc, &br_sha256_vtable, pass, strlen(pass));
  uint8_t u[32], salt[sizeof(PBKDF2_SALT) - 1 + 4];
  memcpy(salt, PBKDF2_SALT, sizeof(PBKDF2_SALT) - 1);
  memcpy(salt + sizeof(PBKDF2_SALT) - 1, "\0\0\0\1", 4);  // block index 1
  br_hmac_context hc;
  br_hmac_init(&hc, &kc, 0);
  br_hmac_update(&hc, salt, sizeof salt);
  br_hmac_out(&hc, u);
  memcpy(out, u, 32);
  for (int i = 1; i < PBKDF2_ITERS; i++) {
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, u, 32);
    br_hmac_out(&hc, u);
    for (int j = 0; j < 32; j++) out[j] ^= u[j];
  }
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static String base64(const uint8_t* d, size_t n) {
  String s;
  s.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    s += B64[v >> 18 & 63];
    s += B64[v >> 12 & 63];
    s += i + 1 < n ? B64[v >> 6 & 63] : '=';
    s += i + 2 < n ? B64[v & 63] : '=';
  }
  return s;
}

// strict base64 decode; returns length or -1
static int unbase64(const char* s, int n, uint8_t* out, int cap) {
  if (n % 4) return -1;
  int o = 0;
  for (int i = 0; i < n; i += 4) {
    uint32_t v = 0;
    int pad = 0;
    for (int j = 0; j < 4; j++) {
      char c = s[i + j];
      const char* p = c ? strchr(B64, c) : nullptr;
      if (c == '=' && i + 4 == n && j >= 2) { pad++; v <<= 6; continue; }
      if (!p || pad) return -1;
      v = v << 6 | (p - B64);
    }
    for (int j = 0; j < 3 - pad; j++) {
      if (o >= cap) return -1;
      out[o++] = v >> (16 - 8 * j) & 0xFF;
    }
  }
  return o;
}

static void gcmInit(br_gcm_context& gc, br_aes_ct_ctr_keys& ctr, const uint8_t* nonce) {
  br_aes_ct_ctr_init(&ctr, cfg.key, 32);
  br_gcm_init(&gc, &ctr.vtable, br_ghash_ctmul32);
  br_gcm_reset(&gc, nonce, 12);
  br_gcm_flip(&gc);
}

// returns base64(nonce[12] | ciphertext | tag[16])
static String encryptText(const char* txt, size_t n) {
  static uint8_t buf[12 + MAX_LINE + 16];
  if (n > MAX_LINE) n = MAX_LINE;
  for (int i = 0; i < 12; i += 4) { uint32_t r = rp2040.hwrand32(); memcpy(buf + i, &r, 4); }
  memcpy(buf + 12, txt, n);
  br_aes_ct_ctr_keys ctr;
  br_gcm_context gc;
  gcmInit(gc, ctr, buf);
  br_gcm_run(&gc, 1, buf + 12, n);
  br_gcm_get_tag(&gc, buf + 12 + n);
  return base64(buf, 12 + n + 16);
}

// decrypts base64 text into out (NUL-terminated). returns plaintext length or -1.
static int decryptText(const char* b64, int n, char* out, int cap) {
  static uint8_t buf[MAX_LINE];
  int len = unbase64(b64, n, buf, sizeof buf);
  if (len < 12 + 16) return -1;
  int ptLen = len - 28;
  if (ptLen >= cap) return -1;
  br_aes_ct_ctr_keys ctr;
  br_gcm_context gc;
  gcmInit(gc, ctr, buf);
  br_gcm_run(&gc, 0, buf + 12, ptLen);
  if (!br_gcm_check_tag(&gc, buf + 12 + ptLen)) return -1;
  memcpy(out, buf + 12, ptLen);
  out[ptLen] = 0;
  return ptLen;
}

// =====================================================================
// keyboard logic (core0)
// =====================================================================
// erase the tracked line, type `text`, press Enter
static void replaceLine(const char* text, size_t n) {
  uint8_t none[6] = { 0 };
  sendReport(0, none);
  delay(2);
  for (int i = 0; i < lineLen; i++) tapKey(0, HID_KEY_BACKSPACE);
  for (size_t i = 0; i < n; i++) typeChar(text[i]);
  tapKey(0, HID_KEY_ENTER);
}

// returns true if Enter was handled (and should not be forwarded)
static bool onEnter() {
  if (!cfg.hasKey || !tracking || lineLen == 0) return false;
  Mode m = readMode();
  if (m == MODE_ENCRYPT) {
    String out = encryptText(line, lineLen);
    replaceLine(out.c_str(), out.length());
    return true;
  }
  if (m == MODE_DECRYPT) {
    static char pt[MAX_LINE];
    int n = decryptText(line, lineLen, pt, sizeof pt);
    if (n < 0) return false;  // not ours / wrong key: leave text, send Enter
    replaceLine(pt, n);
    return true;
  }
  return false;
}

static void resetLine() { lineLen = 0; tracking = true; lastKey = 0; }

static void handleReport(KbReport r) {
  bool ctrlAltGui = r.mod & ~(KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
  bool shift = r.mod & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);

  for (int i = 0; i < 6; i++) {
    uint8_t k = r.keys[i];
    if (!k || hasKey(prev, k)) continue;  // only new presses

    if (k == HID_KEY_ENTER || k == HID_KEY_KEYPAD_ENTER) {
      if (!ctrlAltGui && onEnter()) suppressEnter = true;
      resetLine();
      continue;
    }
    if (ctrlAltGui) { tracking = false; continue; }   // shortcuts: can't know text
    if (k == HID_KEY_BACKSPACE) {
      if (lineLen > 0) lineLen--;
      lastKey = k; lastKeyAt = millis();
      continue;
    }

    char c = k < 128 ? keycode2ascii[k][shift ? 1 : 0] : 0;
    if (c >= 32 && c < 127) {
      if ((ledState & KEYBOARD_LED_CAPSLOCK) && isalpha((unsigned char)c)) c ^= 0x20;
      if (lineLen < MAX_LINE) line[lineLen++] = c; else tracking = false;
      lastKey = k; lastKeyAt = millis();
    } else if (k != HID_KEY_CAPS_LOCK) {
      tracking = false;  // arrows, tab, esc, delete, etc
    }
  }

  // forward to computer, minus any Enter we already handled
  KbReport fwd = r;
  if (suppressEnter) {
    if (hasKey(r, HID_KEY_ENTER) || hasKey(r, HID_KEY_KEYPAD_ENTER)) {
      for (int i = 0; i < 6; i++)
        if (fwd.keys[i] == HID_KEY_ENTER || fwd.keys[i] == HID_KEY_KEYPAD_ENTER) fwd.keys[i] = 0;
    } else suppressEnter = false;
  }
  sendReport(fwd.mod, fwd.keys);
  prev = r;
}

static void checkAutoRepeat() {
  if (lastKey && hasKey(prev, lastKey) && millis() - lastKeyAt > REPEAT_MS) {
    tracking = false;
    lastKey = 0;
  }
}

// LED: solid = encrypt, blink = decrypt, off = off
static void updateLed() {
  Mode m = readMode();
  bool on = m == MODE_ENCRYPT || (m == MODE_DECRYPT && (millis() / 250) % 2);
  digitalWrite(STATUS_LED, on && cfg.hasKey);
}

// =====================================================================
// serial commands (core0)
// =====================================================================
static String cmdBuf;

static void printStatus() {
  Serial.printf("KeyCrypt: mode %s (%s), key %s, keyboard %s\r\n",
                MODE_NAME[readMode()], cfg.modeOverride ? "set by CLI" : "from switch", cfg.hasKey ? "set" : "NOT SET",
                kbAddr ? "connected" : "not connected");
  if (dbgVid || dbgHidCount)
    Serial.printf("  usb device %04x:%04x, hid interfaces %u (protocols: %s), reports %lu\r\n",
                  dbgVid, dbgPid, dbgHidCount, dbgProtos, (unsigned long)dbgReports);
  else
    Serial.printf("  no usb device seen on GP2/GP3 (mounts so far: %lu)\r\n", (unsigned long)dbgMounts);
}

static void runCommand(String c) {
  c.trim();
  if (c.startsWith("key ")) {
    String p = c.substring(4);
    Serial.println("deriving key...");
    pbkdf2_sha256(p.c_str(), cfg.key);
    cfg.hasKey = 1;
    saveSettings();
    Serial.println("key saved");
  } else if (c == "status") printStatus();
  else if (c.startsWith("test ")) {
    if (!cfg.hasKey) { Serial.println("no key set"); return; }
    String t = c.substring(5);
    Serial.println(encryptText(t.c_str(), t.length()));
  } else if (c.startsWith("untest ")) {
    if (!cfg.hasKey) { Serial.println("no key set"); return; }
    String t = c.substring(7);
    static char pt[MAX_LINE];
    Serial.println(decryptText(t.c_str(), t.length(), pt, sizeof pt) < 0 ? "[cannot decrypt]" : pt);
  } else if (c.startsWith("mode ")) {
    String m = c.substring(5);
    m.trim();
    m.toLowerCase();
    if (m == "switch") cfg.modeOverride = 0;
    else if (m == "off") cfg.modeOverride = 1;
    else if (m == "encrypt") cfg.modeOverride = 2;
    else if (m == "decrypt") cfg.modeOverride = 3;
    else { Serial.println("usage: mode switch|off|encrypt|decrypt"); return; }
    saveSettings();
    printStatus();
  } else if (c == "clearkey") { memset(cfg.key, 0, 32); cfg.hasKey = 0; saveSettings(); printStatus(); }
  else if (c.length()) {
    Serial.println("commands: key <passphrase> | status | mode switch|off|encrypt|decrypt | test <text> | untest <b64> | clearkey");
  }
}

static void pollSerial() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r' || ch == '\n') { runCommand(cmdBuf); cmdBuf = ""; }
    else if (ch == 8 || ch == 127) { if (cmdBuf.length()) cmdBuf.remove(cmdBuf.length() - 1); }
    else if (cmdBuf.length() < 300) cmdBuf += ch;
  }
}

// =====================================================================
// core0: device side
// =====================================================================
static void hid_set_report_cb(uint8_t, hid_report_type_t type, uint8_t const* buf, uint16_t n) {
  if (type == HID_REPORT_TYPE_OUTPUT && n >= 1) { ledState = buf[0]; ledPending = true; }
}

void setup() {
  pinMode(PIN_SW_ENC, INPUT_PULLUP);
  pinMode(PIN_SW_DEC, INPUT_PULLUP);
  pinMode(STATUS_LED, OUTPUT);
  queue_init(&reportQ, sizeof(KbReport), 64);
  EEPROM.begin(256);
  loadSettings();

  usb_hid.setBootProtocol(HID_ITF_PROTOCOL_KEYBOARD);
  usb_hid.setPollInterval(1);
  usb_hid.setReportDescriptor(hid_desc, sizeof(hid_desc));
  usb_hid.setStringDescriptor("KeyCrypt Keyboard");
  usb_hid.setReportCallback(NULL, hid_set_report_cb);
  usb_hid.begin();
  if (TinyUSBDevice.mounted()) { TinyUSBDevice.detach(); delay(10); TinyUSBDevice.attach(); }

  Serial.begin(115200);
}

void loop() {
  KbReport r;
  while (queue_try_remove(&reportQ, &r)) handleReport(r);
  checkAutoRepeat();
  updateLed();
  pollSerial();
}

// =====================================================================
// core1: USB host (PIO) for the physical keyboard
// =====================================================================
void setup1() {
  // PIO USB needs a clock that is a multiple of 12 MHz
  uint32_t hz = clock_get_hz(clk_sys);
  if (hz != 120000000UL && hz != 240000000UL) { while (1) delay(1); }
  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  pio_cfg.pin_dp = HOST_PIN_DP;
  USBHost.configure_pio_usb(1, &pio_cfg);
  USBHost.begin(1);
}

void loop1() {
  USBHost.task();
  if (ledPending && kbAddr) {
    static uint8_t leds;
    leds = ledState;
    ledPending = false;
    tuh_hid_set_report(kbAddr, kbInst, 0, HID_REPORT_TYPE_OUTPUT, &leds, 1);
  }
}

extern "C" {

void tuh_mount_cb(uint8_t dev) {
  uint16_t v = 0, p = 0;
  tuh_vid_pid_get(dev, &v, &p);
  dbgVid = v; dbgPid = p; dbgMounts++;
}

void tuh_umount_cb(uint8_t) {
  dbgVid = dbgPid = 0; dbgHidCount = 0; dbgProtos[0] = 0;
}

void tuh_hid_mount_cb(uint8_t dev, uint8_t inst, uint8_t const*, uint16_t) {
  // protocol: 0 none, 1 keyboard, 2 mouse
  size_t n = strlen(dbgProtos);
  if (n + 3 < sizeof dbgProtos) {
    if (n) dbgProtos[n++] = ',';
    dbgProtos[n] = '0' + tuh_hid_interface_protocol(dev, inst);
    dbgProtos[n + 1] = 0;
  }
  dbgHidCount++;
  if (tuh_hid_interface_protocol(dev, inst) == HID_ITF_PROTOCOL_KEYBOARD) {
    kbAddr = dev; kbInst = inst;
    ledPending = true;  // sync caps lock LED
  }
  tuh_hid_receive_report(dev, inst);
}

void tuh_hid_umount_cb(uint8_t dev, uint8_t inst) {
  if (dev == kbAddr && inst == kbInst) kbAddr = 0;
}

void tuh_hid_report_received_cb(uint8_t dev, uint8_t inst, uint8_t const* rep, uint16_t len) {
  dbgReports++;
  if (tuh_hid_interface_protocol(dev, inst) == HID_ITF_PROTOCOL_KEYBOARD && len >= 8) {
    KbReport r;
    r.mod = rep[0];
    memcpy(r.keys, rep + 2, 6);
    queue_try_add(&reportQ, &r);
  }
  tuh_hid_receive_report(dev, inst);
}

}  // extern "C"
