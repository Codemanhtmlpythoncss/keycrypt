# pico-desktop-encryptor (KeyCrypt)

A Raspberry Pi Pico that sits between your USB keyboard and your computer.
Keys pass straight through as you type. When you press **Enter**, the Pico
erases the line you just typed and re-types it **encrypted** (AES-256-GCM, shown as
base64), then presses Enter. A 2-position switch picks the mode:
**off**, **encrypt**, **decrypt**, or **stealth** (nothing reaches the computer until
Enter, then only the ciphertext is typed, so a keylogger on the computer never sees your text).

```
keyboard ──USB──▶ [ Pico ] ──USB──▶ computer
                     ▲
              2-position switch
```

Example: you type `meet at 6` and press Enter. The chat box gets
`Oz6cFc6aCrqW2vvDGwhoJ6sGRhT+OX9GaRnfvHynyq66Pve…`.

---

## 1. What you need

| Part | Notes |
|---|---|
| Raspberry Pi Pico (RP2040) | Plain Pico is best. A Pico W works too, but its onboard LED won't light. |
| USB-A **female** socket / breakout | The keyboard plugs into this. |
| Micro-USB cable | Pico to computer. |
| 2-position DIP or toggle switch | Picks the mode. |
| Jumper wires, soldering iron | Keep the USB wires short (under 10 cm). |

## 2. Wiring

Pico physical pin numbers count from pin 1, at the top-left corner next to the USB port.

### Keyboard socket (USB-A female)

| USB-A socket pin | Usual wire colour | Pico pin |
|---|---|---|
| 1 VBUS (+5 V) | red | **VBUS**, pin 40 |
| 2 D− | white | **GP3**, pin 5 |
| 3 D+ | green | **GP2**, pin 4 |
| 4 GND | black | **GND**, pin 38 |

> Swapping D+ and D− is the most common mistake. If no keyboard is detected, check those two first.

### Mode switch

Pins 18, 19 and 20 sit next to each other, so this is easy:

| Switch | Pico pin |
|---|---|
| common side of **both** switches | **GND**, pin 18 |
| other side of switch **1** (ENC) | **GP14**, pin 19 |
| other side of switch **2** (DEC) | **GP15**, pin 20 |
| *(optional)* switch **3** (PASSWORD): one side | **GP13**, pin 17 |
| *(optional)* switch **3**: other side | **GND**, pin 18 (same as above) |

Switch 3 is optional. With only 2 switches (or switch 3 off) nothing changes, and you can still
use password mode with `keycrypt mode password`. Switch 3 on beats switches 1 and 2.

| Switch 1 | Switch 2 | Mode | Pico LED |
|---|---|---|---|
| off | off | **Off** (normal keyboard) | solid |
| **on** | off | **Encrypt** | solid |
| off | **on** | **Decrypt** | solid, 1 short dip each second |
| **on** | **on** | **Stealth** | solid; flickers while text is waiting |
| switch 3 on | (any) | **Password** | solid, 2 short dips each second; flickers while text is waiting |

The LED is lit whenever the Pico is running, so you can see it is working. Until a key is set
(step 5) it blinks slowly. `keycrypt status` always shows the exact mode.

**No switch? Or want to override it?** Set the mode from the computer. It's saved on the device and overrides the switch:
```bash
keycrypt mode encrypt
```
Use `keycrypt mode decrypt`, `keycrypt mode stealth`, `keycrypt mode password` or `keycrypt mode off` the same way. To hand control back to the switch:
```bash
keycrypt mode switch
```

## 3. Install the `keycrypt` command

**macOS**

```bash
brew install pipx
```
```bash
pipx ensurepath
```
Close and reopen Terminal, then:
```bash
pipx install git+https://github.com/Codemanhtmlpythoncss/pico-desktop-encryptor
```

**Linux / Windows:** install Python 3.9+ first, then:
```bash
python3 -m pip install --user pipx
```
```bash
python3 -m pipx ensurepath
```
Reopen your terminal, then:
```bash
pipx install git+https://github.com/Codemanhtmlpythoncss/pico-desktop-encryptor
```

Check it works:
```bash
keycrypt --help
```

## 4. Flash the Pico

1. Hold the **BOOTSEL** button on the Pico.
2. While holding it, plug the Pico into your computer. Let go.
   A drive called **RPI-RP2** appears.
3. Run:
   ```bash
   keycrypt flash
   ```
The Pico restarts as KeyCrypt. On a Mac, a "Keyboard Setup Assistant" window may pop up. Click **Quit**.

To update later, run `keycrypt flash` again. BOOTSEL isn't needed after the first time.

## 5. Set your key

Pick **one** of these. The key is stored in the Pico's flash, so it survives unplugging.

**Default: passphrase** (the same passphrase decrypts on any computer):
```bash
keycrypt setkey
```
Type it twice. It takes a few seconds: the key is worked out on the Pico with PBKDF2-SHA256
(50 000 rounds). This is the simplest option, but a short or guessable passphrase can be
dictionary-attacked, so use a long one (5+ random words).

**Strongest: a random key** (no passphrase to guess, so no dictionary attack is possible):
```bash
keycrypt setkey --random
```
It prints a key string like `KC2-ABCDE-FGHIJ-…`. Whoever decrypts your messages imports it once:
```bash
keycrypt importkey KC2-ABCDE-FGHIJ-...
```
Anyone with that string can read your messages, so hand it over in person, not over chat.

**Passphrase + secret code:**
```bash
keycrypt setkey --code
```
KeyCrypt makes a random 128-bit **secret code** and mixes it into your passphrase before scrypt
(memory-hard). The way your key is encoded is then unique to you: without the code, a dictionary
attack on your passphrase gets nowhere, even though this code is open source. The code is saved on
this computer (the passphrase never is) and printed as `KC2-…`. To decrypt on another computer you
need **both** the code (`keycrypt importkey`) and the passphrase. Back the code up: if you lose it,
the passphrase alone can't decrypt.

**Passphrase with scrypt:** `keycrypt setkey --plain`. Like the default, the passphrase alone decrypts anywhere,
but scrypt makes each guess far slower for an attacker than PBKDF2.

### Reading the key back

```bash
keycrypt readkey
```
The Pico only hands over its key after you **press the BOOTSEL button on the Pico** within 15 seconds
(the LED blinks fast while it waits). Malware on the computer can't press a button, so it can't steal
the key. It prints the key as a `KC2-…` string and saves it on this computer.

To put a key you were given onto a Pico: `keycrypt importkey KC2-... --to-device`.

Saved keys and codes live in `~/.config/keycrypt/keyring.json` (readable only by you).
`keycrypt decrypt` picks the right one from the ciphertext automatically and only asks for a passphrase when it needs one.

```bash
keycrypt status
```
This prints the mode, the cipher, and the key type.

### Stronger encryption

Pick the cipher from the computer (it's not on the switch):

```bash
keycrypt cipher cascade
```

| Cipher | What it does | Output length |
|---|---|---|
| `aes` (default) | AES-256-GCM | shortest |
| `chacha` | ChaCha20-Poly1305 | about the same |
| `cascade` | AES-256-GCM inside ChaCha20-Poly1305, with two separate keys. Stays safe even if one of the two ciphers is ever broken | ~40 characters longer |

The ciphertext records which cipher made it, so `keycrypt decrypt` and the Pico's decrypt mode
work whatever the current setting is. `keycrypt status` shows the cipher and how the key was made.

The weakest link is almost always the **passphrase**, not the cipher. Use a long one (5+ random words).

```bash
keycrypt status
```
This prints something like `KeyCrypt: mode ENCRYPT, key set, keyboard connected`.

### Tell it your computer's layout and OS

The Pico has to know how your computer turns keys into characters. Otherwise symbols like
`£ # @ " \ ~` come out wrong. Set the layout the **computer** uses (on a Mac, this is System Settings → Keyboard → Input Sources):

```bash
keycrypt layout uk
```
```bash
keycrypt os mac
```
Layouts: `us` (default), `uk`. OS: `mac` (default), `windows`, `linux`. Both are saved on the Pico.
On a UK layout, `£` and Option-combos can't be tracked. Typing one sends that line unencrypted, so check it.

### Windows keyboard on a Mac

Two remaps are available. Keyboards differ, so try one, and if the keys come out wrong, use the other:

```bash
keycrypt remap win-mac
```
`win-mac` swaps Alt ↔ Windows key, so Alt acts as Command and Win as Option.
On the AULA F2088 this gives **Ctrl → Command, Alt → Control, Win → Option**.

```bash
keycrypt remap win-mac-ctrl
```
`win-mac-ctrl` sends Ctrl as Command, Win as Option, and Alt as Control.

To turn remapping off:
```bash
keycrypt remap off
```
Not sure what your keyboard sends? Hold a modifier key and run `keycrypt status` in another window. The
`last modifiers` line shows what the keyboard sent and what the Pico passed on.
If you also remap keys in macOS (Modifier Keys) or Karabiner, that applies on top.

## 6. Use it

- **Encrypt mode:** type a line and press Enter. The line becomes ciphertext and is sent.
- **Decrypt mode:** type a ciphertext and press Enter. It turns into plaintext.
  If it can't be decrypted (wrong key, typo), your text is left as-is and Enter is sent normally.
- **Stealth:** type a line and press Enter. **Nothing** is sent while you type: the Pico keeps
  the text in its own memory, and on Enter types only the ciphertext, then Enter. A keylogger or
  spyware on the computer only ever sees ciphertext. You can't see what you type on screen, so:
  - **Backspace** deletes the last hidden character; **Esc** throws the whole hidden line away.
  - The Pico LED flickers while hidden text is waiting.
  - Shortcuts (Cmd/Ctrl + key), arrows, Tab etc. still go straight through.
  - Caps Lock on = normal keyboard here too (anything hidden is discarded).
  - No key auto-repeat, and lines over 512 characters are cut off (never leaked).
- **Password:** turns a passphrase you can remember into a long, strong password. Works like
  stealth (nothing sent while you type, Backspace/Esc work), but there is **no random number** in
  the output: the same text with the same key gives the **same output every time**. So type
  `my bank pass` on Enter and you always get the same ~60-100 character password to use.
  - Output has no `k2.` header (so it's a clean password). It only uses `A-Z a-z 0-9 + / =`.
  - The output depends on the key **and** the cipher: change either and every password changes.
    Keep a copy of your key (`keycrypt readkey`) or you lose these passwords.
  - Make the same password on the computer: `keycrypt encrypt --password "my bank pass"`
    (uses the same cipher flag: `--cipher cascade` if the device is on cascade).
  - Trade-off: someone who sees two outputs can tell if the texts were the same. That's the
    point for passwords, but use normal or stealth mode for messages.
- **Off:** a normal keyboard.

### Caps Lock = bypass

While **Caps Lock is on**, Enter works like a normal keyboard: nothing is encrypted or decrypted.
This beats both the switch and `keycrypt mode`. Turn Caps Lock off and the normal mode is back.
The Pico's LED stays solid while Caps Lock is on, and `keycrypt status` shows `[CAPS LOCK ON: bypassed]`.

### Decrypting on the computer

Received some ciphertext? Copy it, then:
```bash
keycrypt decrypt PASTE_CIPHERTEXT_HERE
```
Decrypt figures out the method by itself. For text with no header (password mode, or old
ciphertext) it tries every key on this computer and every method, and prints only the one whose
authentication tag checks out, so a wrong guess can never print junk. Decrypt mode on the Pico
does the same with its own key.

Or pipe in a whole file or chat log. Every ciphertext on each line is decrypted:
```bash
pbpaste | keycrypt decrypt
```

To make ciphertext on the computer:
```bash
keycrypt encrypt "some secret"
```

### All commands

| Command | What it does |
|---|---|
| `keycrypt flash` | install or update the firmware |
| `keycrypt setkey [--random\|--code\|--plain]` | set the key (default: passphrase, PBKDF2) |
| `keycrypt readkey` | show the Pico's key as `KC2-…` (press BOOTSEL to allow) |
| `keycrypt importkey KC2-… [--to-device]` | save a key or secret code you were given |
| `keycrypt usbdump` | debug: what the keyboard's media keys / knob send |
| `keycrypt cipher aes\|chacha\|cascade` | which encryption the device uses |
| `keycrypt status` | show mode, key, keyboard state, and what the USB port sees |
| `keycrypt mode encrypt\|decrypt\|stealth\|password\|off\|switch` | set the mode from the computer (`switch` = follow the switch again) |
| `keycrypt layout us\|uk` | the keyboard layout your computer uses |
| `keycrypt os mac\|windows\|linux` | your computer's OS |
| `keycrypt remap win-mac\|win-mac-ctrl\|off` | Windows keyboard on a Mac (see above) |
| `keycrypt clearkey` | wipe the key from the device |
| `keycrypt encrypt [--cipher …] [--password] TEXT` | encrypt on this computer with your current key (`--password` = password mode) |
| `keycrypt decrypt TEXT` | decrypt on this computer (or from stdin) |

Set `KEYCRYPT_PASSPHRASE` in the environment to skip the passphrase prompt (for scripts).

## Limits (read these)

- The Pico can only see what you **type**. If you use arrow keys, Tab, Esc, Delete, mouse
  edits, or shortcuts (Cmd/Ctrl + anything), or hold a key until it auto-repeats, it can't
  know what's in the box any more. That line is then sent **unencrypted** with a normal Enter.
  Nothing is wrongly deleted, but check before you send.
- Pasted text is invisible to the Pico, so decrypt mode only works on ciphertext you type.
  For pasted ciphertext, use `keycrypt decrypt`.
- Only `us` and `uk` computer layouts are supported for now (see `keycrypt layout`).
- Max 512 characters per line.
- Media keys and volume knobs (consumer-control keys) are passed through in every mode. Other extras
  (power/sleep keys, built-in mouse functions, vendor lighting software) are not. If your media keys
  don't work, press them and run `keycrypt usbdump` to see what the keyboard sends.

## Ciphertext format

**v2** (anything except `aes` with a PBKDF2 key): `k2.` + `base64( cipher[1] ‖ kdf[1] ‖ key id[16, not for PBKDF2] ‖ body )`

- `aes`: `nonce[12] ‖ AES-256-GCM(Ka) ‖ tag[16]`
- `chacha`: `nonce[12] ‖ ChaCha20-Poly1305(Kc) ‖ tag[16]`
- `cascade`: ChaCha20-Poly1305(Kc) wrapped around the whole `aes` body

The header is authenticated (AAD). `Ka = HMAC-SHA256(key, "keycrypt aes")`, `Kc = HMAC-SHA256(key, "keycrypt chacha")`.
kdf: 1 = scrypt(passphrase, id), 2 = random key, 3 = scrypt(HMAC-SHA256(secret code, passphrase), id); scrypt N=2^17, r=8, p=1.
Key strings: `KC2-` + base32( type `K`/`C` ‖ kdf ‖ id[16] ‖ key[32] or code[16] ‖ SHA-256 checksum[2] ).

**Password mode** (no header, deterministic): `base64( body )`, body as in v2 but with AAD `"keycrypt-pw"`
and synthetic nonces instead of random ones: `nonce = HMAC-SHA256(Kn, "a" ‖ plaintext)[:12]` for the aes layer,
`HMAC-SHA256(Kn, "c" ‖ data)[:12]` for the chacha layer (data = the aes body for cascade),
`Kn = HMAC-SHA256(key, "keycrypt siv")`. Different texts get different nonces, so nonces never repeat
for different messages.

**Legacy** (`aes` + PBKDF2 key, and everything before v1.6): `base64( nonce[12] ‖ AES-256-GCM ciphertext ‖ tag[16] )`,
key = `PBKDF2-HMAC-SHA256(passphrase, salt="keycrypt-v1", 50000 iterations, 32 bytes)`.

## Build from source

```bash
brew install arduino-cli
```
```bash
arduino-cli config add board_manager.additional_urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
```
```bash
arduino-cli core update-index && arduino-cli core install rp2040:rp2040@6.4.0 && arduino-cli lib install "Pico PIO USB@0.7.3"
```
```bash
./scripts/build-firmware.sh
```
```bash
keycrypt flash --file build/keycrypt.ino.uf2
```

Firmware: [`firmware/keycrypt/keycrypt.ino`](firmware/keycrypt/keycrypt.ino) · CLI: [`keycrypt_cli/main.py`](keycrypt_cli/main.py)

## Open source

Everything here is open source: the firmware ([`firmware/keycrypt/keycrypt.ino`](firmware/keycrypt/keycrypt.ino)),
the CLI, and the build script. The prebuilt `keycrypt.uf2` (in the CLI package and on the
[Releases](../../releases) page) is built from this repo with the steps above, and you can rebuild and reflash it yourself.

The firmware is built on these open-source projects:

| Project | Used for | License |
|---|---|---|
| [arduino-pico](https://github.com/earlephilhower/arduino-pico) 6.4.0 (incl. Pico SDK) | RP2040 core | LGPL-2.1 (Pico SDK: BSD-3) |
| [Adafruit TinyUSB](https://github.com/adafruit/Adafruit_TinyUSB_Arduino) (bundled with the core) | USB keyboard + host stack | MIT |
| [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) 0.7.3 | USB host on GP2/GP3 | MIT |
| [BearSSL](https://bearssl.org/) (bundled with the core) | AES-GCM, ChaCha20-Poly1305, SHA-256, HMAC | MIT |
| [pyserial](https://github.com/pyserial/pyserial), [cryptography](https://github.com/pyca/cryptography) | CLI | BSD / Apache-2.0 |

## License

KeyCrypt is [MIT](LICENSE).
