# Graph Report - pico-desktop-encryptor  (2026-11-19)

## Corpus Check
- 5 files · ~4,121 words
- Verdict: corpus is large enough that graph structure adds value.
- Unclassified: 4 file(s) not represented in the graph (top: (none) 2, .ino 1, .uf2 1)

## Summary
- 75 nodes · 158 edges · 12 communities (9 shown, 3 thin omitted)
- Extraction: 91% EXTRACTED · 9% INFERRED · 0% AMBIGUOUS · INFERRED: 15 edges (avg confidence: 0.85)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `d2a4a648`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- cmd_encrypt
- main
- main.py
- pico-desktop-encryptor (KeyCrypt)
- cmd_setkey
- cmd_importkey
- 5. Set your key
- 6. Use it
- 2. Wiring
- build-firmware.sh
- pico-desktop-encryptor

## God Nodes (most connected - your core abstractions)
1. `main()` - 16 edges
2. `talk()` - 15 edges
3. `pico-desktop-encryptor (KeyCrypt)` - 12 edges
4. `cmd_setkey()` - 11 edges
5. `cmd_encrypt()` - 8 edges
6. `cmd_importkey()` - 8 edges
7. `ask_pass()` - 7 edges
8. `Keys` - 7 edges
9. `cmd_flash()` - 7 edges
10. `scrypt_key()` - 6 edges

## Surprising Connections (you probably didn't know these)
- `main()` --indirect_call--> `cmd_decrypt()`  [INFERRED]
  pico-desktop-encryptor/keycrypt_cli/main.py → pico-desktop-encryptor/keycrypt_cli/main.py  _Bridges community 0 → community 1_
- `main()` --indirect_call--> `cmd_setkey()`  [INFERRED]
  pico-desktop-encryptor/keycrypt_cli/main.py → pico-desktop-encryptor/keycrypt_cli/main.py  _Bridges community 4 → community 1_
- `main()` --indirect_call--> `cmd_importkey()`  [INFERRED]
  pico-desktop-encryptor/keycrypt_cli/main.py → pico-desktop-encryptor/keycrypt_cli/main.py  _Bridges community 5 → community 1_
- `main()` --indirect_call--> `cmd_flash()`  [INFERRED]
  pico-desktop-encryptor/keycrypt_cli/main.py → pico-desktop-encryptor/keycrypt_cli/main.py  _Bridges community 2 → community 1_
- `cmd_importkey()` --calls--> `scrypt_key()`  [EXTRACTED]
  pico-desktop-encryptor/keycrypt_cli/main.py → pico-desktop-encryptor/keycrypt_cli/main.py  _Bridges community 0 → community 5_

## Import Cycles
- None detected.

## Communities (12 total, 3 thin omitted)

### Community 0 - "cmd_encrypt"
Cohesion: 0.21
Nodes (9): cmd_decrypt(), cmd_encrypt(), inputs(), Keys, pbkdf2_key(), scrypt_key(), seal(), subkeys() (+1 more)

### Community 1 - "main"
Cohesion: 0.31
Nodes (10): cmd_cipher(), cmd_clearkey(), cmd_layout(), cmd_mode(), cmd_os(), cmd_remap(), cmd_status(), cmd_usbdump() (+2 more)

### Community 2 - "main.py"
Cohesion: 0.33
Nodes (3): cmd_flash(), find_bootdrive(), find_port()

### Community 3 - "pico-desktop-encryptor (KeyCrypt)"
Cohesion: 0.20
Nodes (9): 1. What you need, 3. Install the `keycrypt` command, 4. Flash the Pico, Build from source, Ciphertext format, License, Limits (read these), Open source (+1 more)

### Community 4 - "cmd_setkey"
Cohesion: 0.48
Nodes (6): cmd_readkey(), cmd_setkey(), export_string(), load_keyring(), remember(), save_keyring()

### Community 5 - "cmd_importkey"
Cohesion: 0.53
Nodes (4): ask_pass(), cmd_importkey(), parse_export(), send_key()

### Community 6 - "5. Set your key"
Cohesion: 0.40
Nodes (5): 5. Set your key, Reading the key back, Stronger encryption, Tell it your computer's layout and OS, Windows keyboard on a Mac

### Community 7 - "6. Use it"
Cohesion: 0.50
Nodes (4): 6. Use it, All commands, Caps Lock = bypass, Decrypting on the computer

### Community 8 - "2. Wiring"
Cohesion: 0.67
Nodes (3): 2. Wiring, Keyboard socket (USB-A female), Mode switch

## Knowledge Gaps
- **1 isolated node(s):** `pico-desktop-encryptor`
  These have ≤1 connection - possible missing edges or undocumented components. (Counts symbols only; 25 node(s) total have ≤1 connection when file, concept and rationale nodes are included.)
- **3 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `pico-desktop-encryptor (KeyCrypt)` connect `pico-desktop-encryptor (KeyCrypt)` to `2. Wiring`, `5. Set your key`, `6. Use it`?**
  _High betweenness centrality (0.434) - this node is a cross-community bridge._
- **Are the 14 inferred relationships involving `main()` (e.g. with `cmd_cipher()` and `cmd_clearkey()`) actually correct?**
  _`main()` has 14 INFERRED edges - model-reasoned connections that need verification._
- **What connects `pico-desktop-encryptor` to the rest of the system?**
  _1 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Why does `5. Set your key` connect `5. Set your key` to `pico-desktop-encryptor (KeyCrypt)`?**
  _High betweenness centrality (0.100) - this node is a cross-community bridge._
- **Why does `6. Use it` connect `6. Use it` to `pico-desktop-encryptor (KeyCrypt)`?**
  _High betweenness centrality (0.076) - this node is a cross-community bridge._