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
# v2:     "k2." base64(cipher kdf [key id 16] | body), see firmware for the layout
# kdf: 0 PBKDF2, 1 scrypt(passphrase), 2 random key, 3 scrypt(passphrase + secret code)
CIPHERS = ["aes", "chacha", "cascade"]
KDF_NAME = ["PBKDF2", "scrypt passphrase", "random key", "passphrase + secret code"]
SCRYPT = dict(n=2 ** 17, r=8, p=1, maxmem=256 * 1024 * 1024, dklen=32)
KEYRING = os.path.join(os.path.expanduser("~"), ".config", "keycrypt", "keyring.json")


def pbkdf2_key(pw: str) -> bytes:
    return hashlib.pbkdf2_hmac("sha256", pw.encode(), SALT, ITERS, 32)


def scrypt_key(pw: str, salt: bytes, code: bytes = None) -> bytes:
    import hmac
    secret = hmac.new(code, pw.encode(), "sha256").digest() if code else pw.encode()
    return hashlib.scrypt(secret, salt=salt, **SCRYPT)


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


# ---------------------------------------------------------------- keyring + shareable key strings
# Stored in ~/.config/keycrypt/keyring.json (only you can read it):
#   keys:  key id -> [kdf, full key]        (from setkey --random, readkey, importkey)
#   codes: key id -> secret code            (passphrase + secret code keys; passphrase is never stored)
# Shareable string: KC2-<base32 of type | id | secret | checksum>, type K = full key, C = secret code
def load_keyring():
    try:
        import json
        with open(KEYRING) as f:
            return json.load(f)
    except FileNotFoundError:
        return {"keys": {}, "codes": {}, "current": None}


def save_keyring(kr):
    import json
    os.makedirs(os.path.dirname(KEYRING), exist_ok=True)
    fd = os.open(KEYRING, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(kr, f, indent=1)
    os.chmod(KEYRING, 0o600)


def remember(kid: bytes, kdf: int = None, key: bytes = None, code: bytes = None):
    kr = load_keyring()
    if key is not None:
        kr["keys"][kid.hex()] = [kdf, key.hex()]
    if code is not None:
        kr["codes"][kid.hex()] = code.hex()
    kr["current"] = kid.hex()
    save_keyring(kr)


def export_string(kind: str, kdf: int, kid: bytes, secret: bytes) -> str:
    raw = kind.encode() + bytes([kdf]) + kid + secret
    raw += hashlib.sha256(raw).digest()[:2]
    b = base64.b32encode(raw).decode().rstrip("=")
    return "KC2-" + "-".join(b[i:i + 5] for i in range(0, len(b), 5))


def parse_export(s: str):
    b = s.strip().upper().replace(" ", "")
    if not b.startswith("KC2-"):
        sys.exit("not a KeyCrypt key string (should start with KC2-)")
    b = b[4:].replace("-", "")
    raw = base64.b32decode(b + "=" * (-len(b) % 8))
    if hashlib.sha256(raw[:-2]).digest()[:2] != raw[-2:]:
        sys.exit("key string has a typo (checksum wrong)")
    return raw[:1].decode(), raw[1], raw[2:18], raw[18:-2]


# ---------------------------------------------------------------- encrypt / decrypt
def seal(pt: bytes, key: bytes, cipher: str, kdf: int, kid: bytes = b"") -> str:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
    if cipher == "aes" and kdf == 0:
        n = os.urandom(12)
        return base64.b64encode(n + AESGCM(key).encrypt(n, pt, None)).decode()
    hdr = bytes([CIPHERS.index(cipher), kdf]) + (kid if kdf else b"")
    ka, kc = subkeys(key)
    if cipher in ("aes", "cascade"):
        n = os.urandom(12)
        pt = n + AESGCM(ka).encrypt(n, pt, hdr)
    if cipher in ("chacha", "cascade"):
        n = os.urandom(12)
        pt = n + ChaCha20Poly1305(kc).encrypt(n, pt, hdr)
    return "k2." + base64.b64encode(hdr + pt).decode()


class Keys:
    """Finds the key for a ciphertext: keyring first, passphrase only when needed (asked once)."""
    def __init__(self):
        self.kr, self.pw, self.cache = load_keyring(), None, {}

    def passphrase(self):
        if self.pw is None:
            self.pw = ask_pass()
        return self.pw

    def get(self, kdf: int, kid: bytes) -> bytes:
        k = (kdf, kid)
        if k in self.cache:
            return self.cache[k]
        saved = self.kr["keys"].get(kid.hex()) if kdf else None
        if saved:
            key = bytes.fromhex(saved[1])
        elif kdf == 0:
            key = pbkdf2_key(self.passphrase())
        elif kdf == 1:
            key = scrypt_key(self.passphrase(), kid)
        elif kdf == 3 and kid.hex() in self.kr["codes"]:
            key = scrypt_key(self.passphrase(), kid, bytes.fromhex(self.kr["codes"][kid.hex()]))
        else:
            raise LookupError("this key isn't on this computer: get its KC2- string and run keycrypt importkey")
        self.cache[k] = key
        return key


def unseal(word: str, keys: Keys) -> str:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
    if not word.startswith("k2."):
        raw = base64.b64decode(word, validate=True)
        return AESGCM(keys.get(0, b"")).decrypt(raw[:12], raw[12:], None).decode()
    raw = base64.b64decode(word[3:], validate=True)
    cipher, kdf = CIPHERS[raw[0]], raw[1]
    if kdf > 3:
        raise ValueError("unknown key type")
    h = 18 if kdf else 2
    hdr, body = raw[:h], raw[h:]
    ka, kc = subkeys(keys.get(kdf, hdr[2:]))
    if cipher in ("chacha", "cascade"):
        body = ChaCha20Poly1305(kc).decrypt(body[:12], body[12:], hdr)
    if cipher in ("aes", "cascade"):
        body = AESGCM(ka).decrypt(body[:12], body[12:], hdr)
    return body.decode()


def cmd_decrypt(a):
    items = inputs(a.text)
    keys = Keys()
    for ct in items:
        for word in ct.split():  # allow several ciphertexts on one line
            try:
                print(unseal(word, keys))
            except LookupError as e:
                print(f"[cannot decrypt: {e}]")
            except Exception:
                print("[cannot decrypt: wrong key or damaged text]")


def cmd_encrypt(a):
    items = inputs(a.text)
    kr = load_keyring()
    cur = kr.get("current")
    if a.passphrase_only or not cur:
        key, kdf, kid = pbkdf2_key(ask_pass()), 0, b""
    else:
        kid = bytes.fromhex(cur)
        if cur in kr["keys"]:
            kdf, key = kr["keys"][cur][0], bytes.fromhex(kr["keys"][cur][1])
        elif cur in kr["codes"]:
            kdf, key = 3, scrypt_key(ask_pass(), kid, bytes.fromhex(kr["codes"][cur]))
        else:
            kdf, key = 1, scrypt_key(ask_pass(), kid)
    for pt in items:
        print(seal(pt.encode(), key, a.cipher, kdf, kid))


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


def talk(cmd, wait=1.0, until=None):
    """Send a command. With `until`, read lines until one starts with a marker (or `wait` seconds pass)."""
    import serial
    with serial.Serial(find_port(), 115200, timeout=0.5) as p:
        p.reset_input_buffer()
        p.write((cmd + "\r").encode())
        if not until:
            time.sleep(wait)
            return p.read(4096).decode(errors="replace").strip()
        out, end = [], time.time() + wait
        while time.time() < end:
            line = p.readline().decode(errors="replace").strip()
            if not line:
                continue
            out.append(line)
            if line.startswith("press"):
                print(line)
            if line.startswith(until):
                break
        return "\n".join(out)


def send_key(key: bytes, kid: bytes, kdf: int):
    reply = talk(f"rawkey {key.hex()} {kid.hex()} {kdf}", 3, until=("key saved", "usage"))
    if "key saved" not in reply:
        sys.exit("device did not accept the key:\n" + reply)
    print("key saved on the Pico")


def cmd_setkey(a):
    if not (a.random or a.plain or a.code):
        pw = ask_pass(confirm=True)
        print("saving key on device (takes a few seconds)...")
        print(talk("key " + pw, 8))
        kr = load_keyring(); kr["current"] = None; save_keyring(kr)
        return
    kid = os.urandom(16)
    if a.random:
        key = os.urandom(32)
        send_key(key, kid, 2)
        remember(kid, 2, key=key)
        print("\nYour random key (also saved on this computer):\n\n  " + export_string("K", 2, kid, key))
        print("\nGive it to whoever decrypts your messages; they run:  keycrypt importkey <key>")
        print("Anyone with this string can read your messages. Share it in person, not over chat.")
        return
    pw = ask_pass(confirm=True)
    if a.plain:
        print("making key with scrypt...")
        send_key(scrypt_key(pw, kid), kid, 1)
        remember(kid)  # current key = passphrase-only, nothing secret stored
        return
    # --code
    code = os.urandom(16)
    print("making key from passphrase + a new random secret code (scrypt)...")
    send_key(scrypt_key(pw, kid, code), kid, 3)
    remember(kid, code=code)
    print("\nYour secret code (saved on this computer, the passphrase is not):\n\n  " + export_string("C", 3, kid, code))
    print("\nTo decrypt elsewhere you need BOTH this code and the passphrase:  keycrypt importkey <code>")


def cmd_readkey(a):
    reply = talk("readkey", 17, until=("KEY ", "cancelled", "no key"))
    line = next((l for l in reply.splitlines() if l.startswith("KEY ")), None)
    if not line:
        sys.exit(reply.splitlines()[-1] if reply else "no reply from device")
    _, kdf, kid, key = line.split()
    kdf, kid, key = int(kdf), bytes.fromhex(kid), bytes.fromhex(key)
    if kdf == 0:
        print("This key was made on the Pico with PBKDF2 (old style); your passphrase is the key.")
        print("Make a new one with:  keycrypt setkey --random")
        return
    remember(kid, kdf, key=key)
    print(f"key type: {KDF_NAME[kdf]}  (saved on this computer)\n\n  " + export_string("K", kdf, kid, key))
    print("\nAnyone with this string can read your messages. Share it in person, not over chat.")


def cmd_importkey(a):
    kind, kdf, kid, secret = parse_export(" ".join(a.key))
    if kind == "K":
        remember(kid, kdf, key=secret)
        print(f"key imported ({KDF_NAME[kdf]})")
        if a.to_device:
            send_key(secret, kid, kdf)
    elif kind == "C":
        remember(kid, code=secret)
        print("secret code imported; you'll be asked for the passphrase when decrypting")
        if a.to_device:
            send_key(scrypt_key(ask_pass(), kid, secret), kid, 3)
    else:
        sys.exit("unknown key string type")


def cmd_usbdump(a):
    print(talk("usbdump"))


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
    p = sub.add_parser("setkey", help="set the key on the device (default: passphrase, PBKDF2 on the Pico)")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--code", action="store_true", help="passphrase + random secret code (blocks dictionary attacks)")
    g.add_argument("--random", action="store_true", help="truly random 256-bit key, no passphrase (strongest)")
    g.add_argument("--plain", action="store_true", help="passphrase only (scrypt), no secret code")
    g.add_argument("--pbkdf2", action="store_true", help="passphrase, PBKDF2 on the Pico (the default)")
    p.set_defaults(f=cmd_setkey)
    sub.add_parser("readkey", help="show the device's key as a KC2- string (press BOOTSEL on the Pico to allow)"
                   ).set_defaults(f=cmd_readkey)
    p = sub.add_parser("importkey", help="save a KC2- key or secret code from someone on this computer")
    p.add_argument("key", nargs="+")
    p.add_argument("--to-device", action="store_true", help="also put the key on the connected Pico")
    p.set_defaults(f=cmd_importkey)
    sub.add_parser("usbdump", help="debug: what the keyboard's extra USB interfaces send (media keys, knob)"
                   ).set_defaults(f=cmd_usbdump)
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
    p = sub.add_parser("encrypt", help="encrypt text on this computer with your current saved key (args or stdin)")
    p.add_argument("--cipher", choices=CIPHERS, default="aes")
    p.add_argument("--passphrase-only", action="store_true", help="use the old PBKDF2 passphrase key instead")
    p.add_argument("text", nargs="*")
    p.set_defaults(f=cmd_encrypt)
    p = sub.add_parser("flash", help="install the KeyCrypt firmware on a Pico")
    p.add_argument("--file", help="use this .uf2 instead of the bundled one")
    p.set_defaults(f=cmd_flash)
    a = ap.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()
