# KeyCrypt

A Raspberry Pi Pico that sits between your USB keyboard and your computer.
Keys pass straight through as you type. When you press **Enter**, the Pico
erases the line you just typed and re-types it **encrypted** (AES-256-GCM, shown as
base64), then presses Enter. A 2-position switch picks the mode:
**off**, **encrypt**, or **decrypt**.

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

| Switch 1 | Switch 2 | Mode | Pico LED |
|---|---|---|---|
| off | off | **Off** (normal keyboard) | dark |
| **on** | off | **Encrypt** | solid |
| off | **on** | **Decrypt** | blinking |
| on | on | Off | dark |

The LED stays dark until a key is set (step 5).

**No switch? Or want to override it?** Set the mode from the computer. It's saved on the device and overrides the switch:
```bash
keycrypt mode encrypt
```
Use `keycrypt mode decrypt` or `keycrypt mode off` the same way. To hand control back to the switch:
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
pipx install git+https://github.com/Codemanhtmlpythoncss/keycrypt
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
pipx install git+https://github.com/Codemanhtmlpythoncss/keycrypt
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

## 5. Set your passphrase

```bash
keycrypt setkey
```
Type the passphrase twice. It takes a few seconds. The key is derived on the device
(PBKDF2-SHA256, 50 000 rounds) and stored in the Pico's flash, so it survives unplugging.
You need the **same passphrase** to decrypt.

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
- **Off:** a normal keyboard.

### Caps Lock = bypass

While **Caps Lock is on**, Enter works like a normal keyboard: nothing is encrypted or decrypted.
This beats both the switch and `keycrypt mode`. Turn Caps Lock off and the normal mode is back.
The Pico's LED goes dark while Caps Lock is on, and `keycrypt status` shows `[CAPS LOCK ON: bypassed]`.

### Decrypting on the computer

Received some ciphertext? Copy it, then:
```bash
keycrypt decrypt PASTE_CIPHERTEXT_HERE
```
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
| `keycrypt setkey` | set the passphrase on the device |
| `keycrypt status` | show mode, key, keyboard state, and what the USB port sees |
| `keycrypt mode encrypt\|decrypt\|off\|switch` | set the mode from the computer (`switch` = follow the switch again) |
| `keycrypt layout us\|uk` | the keyboard layout your computer uses |
| `keycrypt os mac\|windows\|linux` | your computer's OS |
| `keycrypt remap win-mac\|win-mac-ctrl\|off` | Windows keyboard on a Mac (see above) |
| `keycrypt clearkey` | wipe the key from the device |
| `keycrypt encrypt TEXT` | encrypt on this computer |
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
- Only the main keyboard interface is passed through. Media keys and volume knobs on fancy keyboards won't work.

## Ciphertext format

`base64( nonce[12] ‖ AES-256-GCM ciphertext ‖ tag[16] )`
Key = `PBKDF2-HMAC-SHA256(passphrase, salt="keycrypt-v1", 50000 iterations, 32 bytes)`.

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
| [BearSSL](https://bearssl.org/) (bundled with the core) | AES-GCM, SHA-256, HMAC | MIT |
| [pyserial](https://github.com/pyserial/pyserial), [cryptography](https://github.com/pyca/cryptography) | CLI | BSD / Apache-2.0 |

## License

KeyCrypt is [MIT](LICENSE).
