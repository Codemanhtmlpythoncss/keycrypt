// KeyCrypt - Raspberry Pi Pico (RP2040) USB keyboard that encrypts / decrypts each line.
//
//  keyboard --(USB host, PIO on GP2/GP3)--> Pico --(native USB, HID keyboard)--> computer
//
// Keys pass straight through. On Enter, depending on the mode switch:
//   ENCRYPT: typed line is erased and re-typed encrypted (AES-256-GCM, ChaCha20-Poly1305 or both; see crypto)
//   DECRYPT: typed line (base64) is erased and re-typed as plaintext (left alone if it fails)
//   OFF:     plain keyboard
//   STEALTH: nothing reaches the computer while typing; on Enter only the ciphertext is typed
// Bypass: while Caps Lock is ON, Enter is sent normally (overrides CLI mode and switch).
// Mode switch: GP14 to GND = encrypt, GP15 to GND = decrypt, both = stealth, neither = off.
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

enum Mode { MODE_OFF, MODE_ENCRYPT, MODE_DECRYPT, MODE_STEALTH };
static const char* const MODE_NAME[] = { "OFF", "ENCRYPT", "DECRYPT", "STEALTH" };

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
// modeOverride: 0 = follow switch, 1 = off, 2 = encrypt, 3 = decrypt, 4 = stealth (set from CLI)
// layout: 0 = US, 1 = UK.  os: 0 = mac, 1 = windows, 2 = linux  (how the COMPUTER is set up)
// remap (Windows keyboard on a Mac): 0 = none,
//   1 = "win-mac":      Alt <-> Win swap (Alt = Command, Win = Option, Ctrl stays Control)
//   2 = "win-mac-ctrl": Ctrl -> Command, Win -> Option, Alt -> Control
struct Settings { uint32_t magic; uint8_t hasKey; uint8_t key[32]; uint8_t modeOverride; uint8_t layout; uint8_t os; uint8_t remap;
                  uint8_t cipher; uint8_t kdf; uint8_t salt[16]; };
// cipher: 0 = aes (AES-256-GCM), 1 = chacha (ChaCha20-Poly1305), 2 = cascade (AES-GCM inside ChaCha20-Poly1305)
// kdf:    0 = PBKDF2 on the Pico (fixed salt), 1 = scrypt on the computer (random salt, stored here)
static const char* const CIPHER_NAME[] = { "aes", "chacha", "cascade" };
static const char* const LAYOUT_NAME[] = { "us", "uk" };
static const char* const OS_NAME[] = { "mac", "windows", "linux" };
enum { OV_S = 1, OV_A = 2 };   // shift, alt/option
struct Override { uint8_t mods; uint8_t key; char c; };
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
  if (cfg.modeOverride >= 1 && cfg.modeOverride <= 4) return (Mode)(cfg.modeOverride - 1);
  bool enc = digitalRead(PIN_SW_ENC) == LOW;
  bool dec = digitalRead(PIN_SW_DEC) == LOW;
  if (enc && !dec) return MODE_ENCRYPT;
  if (dec && !enc) return MODE_DECRYPT;
  if (enc && dec) return MODE_STEALTH;
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

// ---------- keyboard layouts ----------
// Differences from US for the computer's layout. c = 0 means "not an ASCII char" (e.g. £).
static const Override OV_UK_PC[] = {        // UK on Windows / Linux (ISO)
  { OV_S, 0x1F, '"' }, { OV_S, 0x34, '@' }, { OV_S, 0x20, 0 },
  { 0, 0x32, '#' }, { OV_S, 0x32, '~' }, { 0, 0x31, '#' }, { OV_S, 0x31, '~' },
  { 0, 0x64, '\\' }, { OV_S, 0x64, '|' }, { OV_S, 0x35, 0 },
};
static const Override OV_UK_MAC[] = {       // "British" on macOS
  { OV_S, 0x20, 0 }, { OV_A, 0x20, '#' },
};

static const Override* overrides(size_t& n) {
  if (cfg.layout == 1 && cfg.os == 0) { n = sizeof OV_UK_MAC / sizeof *OV_UK_MAC; return OV_UK_MAC; }
  if (cfg.layout == 1) { n = sizeof OV_UK_PC / sizeof *OV_UK_PC; return OV_UK_PC; }
  n = 0; return nullptr;
}

// keycode + modifiers -> ASCII char the computer will produce, or 0 if unknown / not ASCII
static char keyToChar(uint8_t k, uint8_t mod) {
  if (mod & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL |
             KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI)) return 0;
  uint8_t m = ((mod & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) ? OV_S : 0) |
              ((mod & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)) ? OV_A : 0);
  size_t n; const Override* ov = overrides(n);
  for (size_t i = 0; i < n; i++) if (ov[i].key == k && ov[i].mods == m) return ov[i].c;
  if ((m & OV_A) || k >= 128) return 0;
  char c = keycode2ascii[k][m & OV_S ? 1 : 0];
  return (c >= 32 && c < 127) ? c : 0;
}

// ASCII char -> keycode + modifiers for the computer's layout. false if not typeable.
static bool charToKey(char c, uint8_t& mod, uint8_t& key) {
  size_t n; const Override* ov = overrides(n);
  for (size_t i = 0; i < n; i++) if (ov[i].c == c) {
    key = ov[i].key;
    mod = (ov[i].mods & OV_S ? KEYBOARD_MODIFIER_LEFTSHIFT : 0) | (ov[i].mods & OV_A ? KEYBOARD_MODIFIER_LEFTALT : 0);
    return true;
  }
  if ((uint8_t)c >= 128 || !ascii2keycode[(uint8_t)c][1]) return false;
  uint8_t s = ascii2keycode[(uint8_t)c][0] ? OV_S : 0;
  key = ascii2keycode[(uint8_t)c][1];
  for (size_t i = 0; i < n; i++) if (ov[i].key == key && ov[i].mods == s) return false;  // US key means something else here
  mod = s ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
  return true;
}

static void typeChar(char c) {
  uint8_t mod, kc;
  if (!charToKey(c, mod, kc)) return;
  // computer caps lock flips letters
  if ((ledState & KEYBOARD_LED_CAPSLOCK) && isalpha((unsigned char)c)) mod ^= KEYBOARD_MODIFIER_LEFTSHIFT;
  tapKey(mod, kc);
}

static void loadSettings() {
  EEPROM.get(0, cfg);
  if (cfg.magic != MAGIC) { memset(&cfg, 0, sizeof cfg); cfg.magic = MAGIC; }
  if (cfg.modeOverride > 4) cfg.modeOverride = 0;
  if (cfg.layout > 1) cfg.layout = 0;
  if (cfg.os > 2) cfg.os = 0;
  if (cfg.remap > 2) cfg.remap = 0;
  if (cfg.cipher > 2) cfg.cipher = 0;
  if (cfg.kdf > 1) { cfg.kdf = 0; cfg.hasKey = 0; }
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

// Formats
//   legacy (cipher aes + PBKDF2 key): base64( nonce[12] | AES-256-GCM ct | tag[16] )
//   v2: "k2." base64( hdr | body ),  hdr = cipher[1] kdf[1] (salt[16] if kdf = scrypt)
//       aes:     nonce[12] | AES-256-GCM(Ka) ct | tag[16]
//       chacha:  nonce[12] | ChaCha20-Poly1305(Kc) ct | tag[16]
//       cascade: ChaCha20-Poly1305(Kc) over the whole aes body
//       Ka = HMAC-SHA256(key, "keycrypt aes"), Kc = HMAC-SHA256(key, "keycrypt chacha"); hdr is AAD.
static const int MAX_CT = 18 + 12 + 12 + MAX_LINE + 16 + 16;

static void subkey(const char* label, uint8_t out[32]) {
  br_hmac_key_context kc;
  br_hmac_key_init(&kc, &br_sha256_vtable, cfg.key, 32);
  br_hmac_context hc;
  br_hmac_init(&hc, &kc, 0);
  br_hmac_update(&hc, label, strlen(label));
  br_hmac_out(&hc, out);
}

static void randBytes(uint8_t* p, int n) {
  for (int i = 0; i < n; i += 4) { uint32_t r = rp2040.hwrand32(); memcpy(p + i, &r, n - i < 4 ? n - i : 4); }
}

// in place: p = nonce | data[n] | tag ; encrypt fills nonce + tag. returns false on bad tag.
static bool aesGcm(const uint8_t key[32], uint8_t* p, size_t n, const uint8_t* aad, size_t aadLen, bool enc) {
  if (enc) randBytes(p, 12);
  br_aes_ct_ctr_keys ctr;
  br_gcm_context gc;
  br_aes_ct_ctr_init(&ctr, key, 32);
  br_gcm_init(&gc, &ctr.vtable, br_ghash_ctmul32);
  br_gcm_reset(&gc, p, 12);
  if (aadLen) br_gcm_aad_inject(&gc, aad, aadLen);
  br_gcm_flip(&gc);
  br_gcm_run(&gc, enc, p + 12, n);
  if (enc) { br_gcm_get_tag(&gc, p + 12 + n); return true; }
  return br_gcm_check_tag(&gc, p + 12 + n);
}

static bool chachaPoly(const uint8_t key[32], uint8_t* p, size_t n, const uint8_t* aad, size_t aadLen, bool enc) {
  if (enc) randBytes(p, 12);
  uint8_t tag[16];
  br_poly1305_ctmul_run(key, p, p + 12, n, aad, aadLen, tag, br_chacha20_ct_run, enc);
  if (enc) { memcpy(p + 12 + n, tag, 16); return true; }
  uint8_t d = 0;
  for (int i = 0; i < 16; i++) d |= tag[i] ^ p[12 + n + i];
  return d == 0;
}

static int headerLen(uint8_t kdf) { return kdf == 1 ? 18 : 2; }

static String encryptText(const char* txt, size_t n) {
  static uint8_t buf[MAX_CT];
  if (n > MAX_LINE) n = MAX_LINE;
  if (cfg.cipher == 0 && cfg.kdf == 0) {             // legacy format, master key directly
    memcpy(buf + 12, txt, n);
    aesGcm(cfg.key, buf, n, nullptr, 0, true);
    return base64(buf, 12 + n + 16);
  }
  int h = headerLen(cfg.kdf);
  buf[0] = cfg.cipher; buf[1] = cfg.kdf;
  if (cfg.kdf == 1) memcpy(buf + 2, cfg.salt, 16);
  uint8_t ka[32], kc[32];
  subkey("keycrypt aes", ka);
  subkey("keycrypt chacha", kc);
  uint8_t* body = buf + h;
  size_t len;
  if (cfg.cipher == 1) {
    memcpy(body + 12, txt, n);
    chachaPoly(kc, body, n, buf, h, true);
    len = 12 + n + 16;
  } else if (cfg.cipher == 2) {                       // inner aes body sits inside the chacha body
    memcpy(body + 24, txt, n);
    aesGcm(ka, body + 12, n, buf, h, true);
    chachaPoly(kc, body, 12 + n + 16, buf, h, true);
    len = 12 + 12 + n + 16 + 16;
  } else {
    memcpy(body + 12, txt, n);
    aesGcm(ka, body, n, buf, h, true);
    len = 12 + n + 16;
  }
  memset(ka, 0, 32); memset(kc, 0, 32);
  return "k2." + base64(buf, h + len);
}

// decrypts text into out (NUL-terminated). returns plaintext length or -1.
static int decryptText(const char* b64, int n, char* out, int cap) {
  static uint8_t buf[MAX_CT];
  const uint8_t* pt;
  int ptLen;
  if (n > 3 && !strncmp(b64, "k2.", 3)) {
    int len = unbase64(b64 + 3, n - 3, buf, sizeof buf);
    if (len < 2 || buf[0] > 2 || buf[1] != cfg.kdf) return -1;   // made with a different kind of key
    int h = headerLen(buf[1]);
    if (h == 18 && (len < 18 || memcmp(buf + 2, cfg.salt, 16))) return -1;  // different scrypt key
    uint8_t ka[32], kc[32];
    subkey("keycrypt aes", ka);
    subkey("keycrypt chacha", kc);
    uint8_t* body = buf + h;
    int bl = len - h;
    bool ok = false;
    if (buf[0] == 1 && bl >= 28) {
      ok = chachaPoly(kc, body, bl - 28, buf, h, false); pt = body + 12; ptLen = bl - 28;
    } else if (buf[0] == 2 && bl >= 56) {
      ok = chachaPoly(kc, body, bl - 28, buf, h, false) && aesGcm(ka, body + 12, bl - 56, buf, h, false);
      pt = body + 24; ptLen = bl - 56;
    } else if (buf[0] == 0 && bl >= 28) {
      ok = aesGcm(ka, body, bl - 28, buf, h, false); pt = body + 12; ptLen = bl - 28;
    }
    memset(ka, 0, 32); memset(kc, 0, 32);
    if (!ok) return -1;
  } else {
    if (cfg.kdf != 0) return -1;                      // legacy text needs a PBKDF2 key
    int len = unbase64(b64, n, buf, sizeof buf);
    if (len < 28) return -1;
    ptLen = len - 28;
    if (!aesGcm(cfg.key, buf, ptLen, nullptr, 0, false)) return -1;
    pt = buf + 12;
  }
  if (ptLen >= cap) return -1;
  memcpy(out, pt, ptLen);
  out[ptLen] = 0;
  return ptLen;
}

// =====================================================================
// keyboard logic (core0)
// =====================================================================
// erase the tracked line, type `text`, press Enter
static void typeLine(const char* text, size_t n, int backspaces) {
  uint8_t none[6] = { 0 };
  sendReport(0, none);
  delay(2);
  for (int i = 0; i < backspaces; i++) tapKey(0, HID_KEY_BACKSPACE);
  for (size_t i = 0; i < n; i++) typeChar(text[i]);
  tapKey(0, HID_KEY_ENTER);
}

static void replaceLine(const char* text, size_t n) { typeLine(text, n, lineLen); }

// returns true if Enter was handled (and should not be forwarded)
static bool onEnter() {
  if (ledState & KEYBOARD_LED_CAPSLOCK) return false;  // caps lock on = bypass (beats CLI + switch)
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

static void resetLine() {
  lineLen = 0; tracking = true; lastKey = 0;
}

static volatile uint8_t dbgModRaw = 0, dbgModSent = 0;

// map each modifier bit (L and R side) through a table: index 0 ctrl, 1 shift, 2 alt, 3 gui
static uint8_t remapMods(uint8_t m) {
  static const uint8_t MAPS[3][4] = {
    { 0, 1, 2, 3 },   // none
    { 0, 1, 3, 2 },   // win-mac:      alt <-> gui
    { 3, 1, 0, 2 },   // win-mac-ctrl: ctrl->gui, alt->ctrl, gui->alt
  };
  const uint8_t* map = MAPS[cfg.remap <= 2 ? cfg.remap : 0];
  uint8_t out = 0;
  for (int side = 0; side < 2; side++)
    for (int b = 0; b < 4; b++)
      if (m & (1 << (side * 4 + b))) out |= 1 << (side * 4 + map[b]);
  if (m) { dbgModRaw = m; dbgModSent = out; }
  return out;
}

// ---------- stealth mode ----------
// Nothing typed reaches the computer until Enter; then only the ciphertext is typed.
// Keys we keep (chars, Enter, and Backspace/Esc while the buffer has text) are "captured"
// and stay hidden until released; everything else (arrows, shortcuts...) passes through.
static uint8_t captured[6];
static bool wasStealth = false;

static bool isCaptured(uint8_t k) {
  for (int i = 0; i < 6; i++) if (captured[i] == k) return true;
  return false;
}

static void handleStealth(KbReport r) {
  bool ctrlGui = r.mod & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL |
                          KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI);
  // forget captured keys that were released
  for (int i = 0; i < 6; i++) if (captured[i] && !hasKey(r, captured[i])) captured[i] = 0;

  for (int i = 0; i < 6; i++) {
    uint8_t k = r.keys[i];
    if (!k || hasKey(prev, k)) continue;  // only new presses
    bool take = false;
    if ((k == HID_KEY_ENTER || k == HID_KEY_KEYPAD_ENTER) && !ctrlGui) {
      take = true;
      if (lineLen > 0) {
        String out = encryptText(line, lineLen);
        typeLine(out.c_str(), out.length(), 0);
      } else {
        tapKey(0, HID_KEY_ENTER);
      }
      lineLen = 0;
    } else if (ctrlGui) {
      take = false;                                   // shortcuts pass through
    } else if (k == HID_KEY_BACKSPACE || k == HID_KEY_ESCAPE) {
      take = lineLen > 0;                             // edit the hidden buffer
      if (take) lineLen = k == HID_KEY_ESCAPE ? 0 : lineLen - 1;
    } else {
      char c = keyToChar(k, r.mod);
      if (c) {
        take = true;
        if (lineLen < MAX_LINE) line[lineLen++] = c;  // full: extra chars dropped, never leaked
      }
    }
    if (take)
      for (int j = 0; j < 6; j++) if (!captured[j]) { captured[j] = k; break; }
  }

  KbReport fwd = r;
  for (int i = 0; i < 6; i++) if (fwd.keys[i] && isCaptured(fwd.keys[i])) fwd.keys[i] = 0;
  sendReport(fwd.mod, fwd.keys);
  prev = r;
}

static void handleReport(KbReport r) {
  r.mod = remapMods(r.mod);
  bool stealth = readMode() == MODE_STEALTH && cfg.hasKey && !(ledState & KEYBOARD_LED_CAPSLOCK);
  if (stealth != wasStealth) {   // entering/leaving stealth: pending hidden text is discarded
    wasStealth = stealth;
    resetLine();
    memset(captured, 0, sizeof captured);
  }
  if (stealth) { handleStealth(r); return; }
  bool ctrlAltGui = r.mod & ~(KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
  bool ctrlGui = r.mod & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL |
                          KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI);

  for (int i = 0; i < 6; i++) {
    uint8_t k = r.keys[i];
    if (!k || hasKey(prev, k)) continue;  // only new presses

    if (k == HID_KEY_ENTER || k == HID_KEY_KEYPAD_ENTER) {
      if (!ctrlAltGui && onEnter()) suppressEnter = true;
      resetLine();
      continue;
    }
    if (ctrlGui) { tracking = false; continue; }   // shortcuts: can't know text
    if (k == HID_KEY_BACKSPACE) {
      if (lineLen > 0) lineLen--;
      lastKey = k; lastKeyAt = millis();
      continue;
    }

    char c = keyToChar(k, r.mod);
    if (c) {
      if ((ledState & KEYBOARD_LED_CAPSLOCK) && isalpha((unsigned char)c)) c ^= 0x20;
      if (lineLen < MAX_LINE) line[lineLen++] = c; else tracking = false;
      lastKey = k; lastKeyAt = millis();
    } else if (k != HID_KEY_CAPS_LOCK && !(k >= HID_KEY_CONTROL_LEFT && k <= HID_KEY_GUI_RIGHT)) {
      tracking = false;  // arrows, tab, esc, delete, £, option-combos, etc
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
  if (wasStealth) return;  // nothing is forwarded in stealth, so no OS auto-repeat
  if (lastKey && hasKey(prev, lastKey) && millis() - lastKeyAt > REPEAT_MS) {
    tracking = false;
    lastKey = 0;
  }
}

// LED: solid = encrypt, blink = decrypt, off = off
static void updateLed() {
  Mode m = readMode();
  bool on = m == MODE_ENCRYPT || (m == MODE_DECRYPT && (millis() / 250) % 2) ||
            (m == MODE_STEALTH && (lineLen == 0 || (millis() / 80) % 2));  // stealth: flicker = text waiting
  digitalWrite(STATUS_LED, on && cfg.hasKey && !(ledState & KEYBOARD_LED_CAPSLOCK));
}

// =====================================================================
// serial commands (core0)
// =====================================================================
static String cmdBuf;

static void printStatus() {
  Serial.printf("KeyCrypt: mode %s (%s)%s, key %s, keyboard %s\r\n",
                MODE_NAME[readMode()], cfg.modeOverride ? "set by CLI" : "from switch",
                (ledState & KEYBOARD_LED_CAPSLOCK) ? " [CAPS LOCK ON: bypassed]" : "", cfg.hasKey ? "set" : "NOT SET",
                kbAddr ? "connected" : "not connected");
  Serial.printf("  computer layout %s, os %s, modifier remap %s\r\n", LAYOUT_NAME[cfg.layout], OS_NAME[cfg.os],
                cfg.remap == 1 ? "win-mac" : cfg.remap == 2 ? "win-mac-ctrl" : "off");
  Serial.printf("  cipher %s, key made with %s\r\n", CIPHER_NAME[cfg.cipher],
                cfg.kdf ? "scrypt (on computer, random salt)" : "PBKDF2 (on Pico)");
  Serial.printf("  last modifiers: from keyboard 0x%02x -> sent 0x%02x\r\n", dbgModRaw, dbgModSent);
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
    cfg.kdf = 0;
    saveSettings();
    Serial.println("key saved");
  } else if (c.startsWith("rawkey ")) {     // rawkey <64 hex key> <32 hex salt>: key made by scrypt on the computer
    String a = c.substring(7); a.trim();
    uint8_t tmp[48];
    bool ok = a.length() == 64 + 1 + 32 && a[64] == ' ';
    for (int i = 0, j = 0; ok && i < 48; i++, j += 2) {
      if (j == 64) j++;
      char hx[3] = { a[j], a[j + 1], 0 };
      char* e;
      tmp[i] = strtol(hx, &e, 16);
      ok = *e == 0 && isxdigit((unsigned char)hx[0]) && isxdigit((unsigned char)hx[1]);
    }
    if (!ok) { Serial.println("usage: rawkey <64 hex> <32 hex>"); return; }
    memcpy(cfg.key, tmp, 32); memcpy(cfg.salt, tmp + 32, 16); memset(tmp, 0, sizeof tmp);
    cfg.hasKey = 1; cfg.kdf = 1;
    saveSettings();
    Serial.println("key saved");
  } else if (c.startsWith("cipher ")) {
    String v = c.substring(7); v.trim(); v.toLowerCase();
    if (v == "aes") cfg.cipher = 0; else if (v == "chacha") cfg.cipher = 1; else if (v == "cascade") cfg.cipher = 2;
    else { Serial.println("usage: cipher aes|chacha|cascade"); return; }
    saveSettings(); printStatus();
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
    else if (m == "stealth") cfg.modeOverride = 4;
    else { Serial.println("usage: mode switch|off|encrypt|decrypt|stealth"); return; }
    saveSettings();
    printStatus();
  } else if (c.startsWith("layout ")) {
    String v = c.substring(7); v.trim(); v.toLowerCase();
    if (v == "us") cfg.layout = 0; else if (v == "uk") cfg.layout = 1;
    else { Serial.println("usage: layout us|uk"); return; }
    saveSettings(); printStatus();
  } else if (c.startsWith("os ")) {
    String v = c.substring(3); v.trim(); v.toLowerCase();
    if (v == "mac") cfg.os = 0; else if (v == "windows") cfg.os = 1; else if (v == "linux") cfg.os = 2;
    else { Serial.println("usage: os mac|windows|linux"); return; }
    saveSettings(); printStatus();
  } else if (c.startsWith("remap ")) {
    String v = c.substring(6); v.trim(); v.toLowerCase();
    if (v == "off") cfg.remap = 0; else if (v == "win-mac") cfg.remap = 1; else if (v == "win-mac-ctrl") cfg.remap = 2;
    else { Serial.println("usage: remap off|win-mac|win-mac-ctrl"); return; }
    saveSettings(); printStatus();
  } else if (c == "clearkey") { memset(cfg.key, 0, 32); memset(cfg.salt, 0, 16); cfg.hasKey = 0; cfg.kdf = 0; saveSettings(); printStatus(); }
  else if (c.length()) {
    Serial.println("commands: key <passphrase> | rawkey <hex> <salthex> | cipher aes|chacha|cascade | status | mode switch|off|encrypt|decrypt|stealth | layout us|uk | os mac|windows|linux | remap off|win-mac|win-mac-ctrl | test <text> | untest <b64> | clearkey");
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
