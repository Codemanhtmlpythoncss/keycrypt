"""keycrypt - command line tool for the KeyCrypt Pico keyboard."""
import argparse
import base64
import getpass
import glob
import hashlib
import os
import shutil
import sys
import time
from importlib import resources

SALT, ITERS = b"keycrypt-v1", 50000


# ---------------------------------------------------------------- crypto
# legacy: base64(nonce|AES-GCM ct|tag), key = PBKDF2 with fixed salt
# v2:     "k2." base64(cipher kdf [salt16] | body), see firmware for the layout
CIPHERS = ["aes", "chacha", "cascade"]
SCRYPT = dict(n=2 ** 17, r=8, p=1, maxmem=256 * 1024 * 1024, dklen=32)


def pbkdf2_key(pw: str) -> bytes:
    return hashlib.pbkdf2_hmac("sha256", pw.encode(), SALT, ITERS, 32)


def scrypt_key(pw: str, salt: bytes) -> bytes:
    return hashlib.scrypt(pw.encode(), salt=salt, **SCRYPT)


def subkeys(key: bytes):
    import hmac
    return (hmac.new(key, b"keycrypt aes", "sha256").digest(),
            hmac.new(key, b"keycrypt chacha", "sha256").digest())


def ask_pass(confirm=False):
    pw = os.environ.get("KEYCRYPT_PASSPHRASE") or getpass.getpass("passphrase: ")
    if confirm and "KEYCRYPT_PASSPHRASE" not in os.environ and pw != getpass.getpass("again: "):
        sys.exit("passphrases differ")
    if not pw:
        sys.exit("empty passphrase")
    return pw


def inputs(args_text):
    if args_text:
        return [" ".join(args_text)]
    if sys.stdin.isatty():
        print("paste text, then Ctrl-D:", file=sys.stderr)
    return [l.strip() for l in sys.stdin.read().splitlines() if l.strip()]


def seal(pt: bytes, key: bytes, cipher: str, kdf: int, salt: bytes = b"") -> str:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
    if cipher == "aes" and kdf == 0:
        n = os.urandom(12)
        return base64.b64encode(n + AESGCM(key).encrypt(n, pt, None)).decode()
    hdr = bytes([CIPHERS.index(cipher), kdf]) + (salt if kdf == 1 else b"")
    ka, kc = subkeys(key)
    if cipher in ("aes", "cascade"):
        n = os.urandom(12)
        pt = n + AESGCM(ka).encrypt(n, pt, hdr)
    if cipher in ("chacha", "cascade"):
        n = os.urandom(12)
        pt = n + ChaCha20Poly1305(kc).encrypt(n, pt, hdr)
    return "k2." + base64.b64encode(hdr + pt).decode()


def unseal(word: str, pw: str, cache: dict) -> str:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
    if "pbkdf2" not in cache:
        cache["pbkdf2"] = pbkdf2_key(pw)
    if not word.startswith("k2."):
        raw = base64.b64decode(word, validate=True)
        return AESGCM(cache["pbkdf2"]).decrypt(raw[:12], raw[12:], None).decode()
    raw = base64.b64decode(word[3:], validate=True)
    cipher, kdf = CIPHERS[raw[0]], raw[1]
    if kdf == 1:
        hdr, body = raw[:18], raw[18:]
        if hdr[2:] not in cache:
            cache[hdr[2:]] = scrypt_key(pw, hdr[2:])
        key = cache[hdr[2:]]
    elif kdf == 0:
        hdr, body, key = raw[:2], raw[2:], cache["pbkdf2"]
    else:
        raise ValueError("unknown key type")
    ka, kc = subkeys(key)
    if cipher in ("chacha", "cascade"):
        body = ChaCha20Poly1305(kc).decrypt(body[:12], body[12:], hdr)
    if cipher in ("aes", "cascade"):
        body = AESGCM(ka).decrypt(body[:12], body[12:], hdr)
    return body.decode()


def cmd_decrypt(a):
    items = inputs(a.text)
    pw, cache = ask_pass(), {}
    for ct in items:
        for word in ct.split():  # allow several ciphertexts on one line
            try:
                print(unseal(word, pw, cache))
            except Exception:
                print("[cannot decrypt: wrong key or damaged text]")


def cmd_encrypt(a):
    items = inputs(a.text)
    pw = ask_pass()
    if a.kdf == "scrypt":
        salt = os.urandom(16)
        key, kdf = scrypt_key(pw, salt), 1
    else:
        salt, key, kdf = b"", pbkdf2_key(pw), 0
    for pt in items:
        print(seal(pt.encode(), key, a.cipher, kdf, salt))


# ---------------------------------------------------------------- device
def find_port():
    pats = ["/dev/cu.usbmodem*", "/dev/ttyACM*"]
    ports = [p for pat in pats for p in glob.glob(pat)]
    if not ports and os.name == "nt":
        from serial.tools import list_ports
        ports = [p.device for p in list_ports.comports() if p.vid == 0x2E8A]
    if not ports:
        sys.exit("KeyCrypt not found. Is the Pico plugged in?")
    return ports[0]


def talk(cmd, wait=1.0):
    import serial
    with serial.Serial(find_port(), 115200, timeout=0.5) as p:
        p.reset_input_buffer()
        p.write((cmd + "\r").encode())
        time.sleep(wait)
        return p.read(4096).decode(errors="replace").strip()


def cmd_setkey(a):
    pw = ask_pass(confirm=True)
    if a.kdf == "pbkdf2":
        print("saving key on device (takes a few seconds)...")
        print(talk("key " + pw, 8))
        return
    print("making key with scrypt (strong, takes a second)...")
    salt = os.urandom(16)
    key = scrypt_key(pw, salt)
    print(talk(f"rawkey {key.hex()} {salt.hex()}", 1.5))


def cmd_cipher(a):
    print(talk("cipher " + a.cipher))


def cmd_status(a):
    print(talk("status"))


def cmd_mode(a):
    print(talk("mode " + a.mode))


def cmd_layout(a):
    print(talk("layout " + a.layout))


def cmd_os(a):
    print(talk("os " + a.os))


def cmd_remap(a):
    print(talk("remap " + a.remap))


def cmd_clearkey(a):
    print(talk("clearkey"))


def find_bootdrive():
    cands = ["/Volumes/RPI-RP2"] + glob.glob("/media/*/RPI-RP2") + glob.glob("/run/media/*/RPI-RP2")
    if os.name == "nt":
        cands += [f"{d}:\\" for d in "DEFGHIJKLMNOPQRSTUVWXYZ"]
    for c in cands:
        if os.path.exists(os.path.join(c, "INFO_UF2.TXT")):
            return c
    return None


def cmd_flash(a):
    drive = find_bootdrive()
    if not drive:
        # ask a running KeyCrypt to reboot into the bootloader (1200 baud touch)
        try:
            import serial
            serial.Serial(find_port(), 1200).close()
        except SystemExit:
            sys.exit("No Pico found. Hold BOOTSEL while plugging it in, then try again.")
        except Exception:
            pass
        for _ in range(20):
            time.sleep(0.5)
            drive = find_bootdrive()
            if drive:
                break
    if not drive:
        sys.exit("Pico drive (RPI-RP2) not found. Hold BOOTSEL while plugging it in, then try again.")
    src = a.file or str(resources.files("keycrypt_cli") / "firmware" / "keycrypt.uf2")
    print(f"flashing {os.path.basename(src)} -> {drive}")
    with open(src, "rb") as f, open(os.path.join(drive, "keycrypt.uf2"), "wb") as g:
        shutil.copyfileobj(f, g)
    print("done. The Pico restarts as KeyCrypt.")


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(prog="keycrypt", description="Control the KeyCrypt Pico keyboard.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("setkey", help="set the passphrase on the device")
    p.add_argument("--kdf", choices=["scrypt", "pbkdf2"], default="scrypt",
                   help="scrypt (default, strong, made on this computer) or pbkdf2 (old, made on the Pico)")
    p.set_defaults(f=cmd_setkey)
    p = sub.add_parser("cipher", help="encryption the device uses: aes, chacha, cascade (aes inside chacha)")
    p.add_argument("cipher", choices=CIPHERS)
    p.set_defaults(f=cmd_cipher)
    sub.add_parser("status", help="show mode, key and keyboard state").set_defaults(f=cmd_status)
    p = sub.add_parser("mode", help="set the mode from here (overrides the switch until 'mode switch')")
    p.add_argument("mode", choices=["switch", "off", "encrypt", "decrypt", "stealth"])
    p.set_defaults(f=cmd_mode)
    p = sub.add_parser("layout", help="keyboard layout the computer uses (us or uk)")
    p.add_argument("layout", choices=["us", "uk"])
    p.set_defaults(f=cmd_layout)
    p = sub.add_parser("os", help="operating system of the computer (mac, windows, linux)")
    p.add_argument("os", choices=["mac", "windows", "linux"])
    p.set_defaults(f=cmd_os)
    p = sub.add_parser("remap", help="Windows keyboard on a Mac. win-mac: Alt<->Win swap; "
                                     "win-mac-ctrl: Ctrl=Cmd, Win=Option, Alt=Ctrl; off")
    p.add_argument("remap", choices=["off", "win-mac", "win-mac-ctrl"])
    p.set_defaults(f=cmd_remap)
    sub.add_parser("clearkey", help="erase the key from the device").set_defaults(f=cmd_clearkey)
    p = sub.add_parser("decrypt", help="decrypt text (args or stdin)")
    p.add_argument("text", nargs="*")
    p.set_defaults(f=cmd_decrypt)
    p = sub.add_parser("encrypt", help="encrypt text on this computer (args or stdin)")
    p.add_argument("--cipher", choices=CIPHERS, default="aes")
    p.add_argument("--kdf", choices=["pbkdf2", "scrypt"], default="pbkdf2",
                   help="scrypt output can only be decrypted with the CLI (new random salt)")
    p.add_argument("text", nargs="*")
    p.set_defaults(f=cmd_encrypt)
    p = sub.add_parser("flash", help="install the KeyCrypt firmware on a Pico")
    p.add_argument("--file", help="use this .uf2 instead of the bundled one")
    p.set_defaults(f=cmd_flash)
    a = ap.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()
