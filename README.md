mGBA-3DS: Pokemon FireRed Overlay
=================================

A custom [mGBA](https://mgba.io/) build for Nintendo 3DS with a real-time Pokemon party viewer on the bottom screen. Reads live game data directly from GBA EWRAM/ROM while FireRed is running, decrypts the Gen 3 data structures, and renders a detailed HUD with sprites, stats, moves, and more.

**Target ROM:** Pokemon FireRed US v1.0 (`BPRE`)

> For upstream mGBA documentation (features, platforms, dependencies, etc.), see [MGBA_README.md](MGBA_README.md).

Features
--------

- **Live party data** — reads and decrypts Gen 3 Pokemon structures from EWRAM in real-time
- **Front sprites** — LZ77-decompressed, 4bpp tile-decoded, palette-applied sprites rendered via Citro3D
- **Full stat display** — level, HP (color-coded), nature (highlighted), Atk/Def/SpA/SpD/Spe, IVs, EVs, EXP
- **Moveset viewer** — current 4 moves with PP counts
- **Learnset viewer** — upcoming level-up moves read from ROM, with scrolling
- **Status conditions** — SLP/PSN/BRN/FRZ/PAR/TOX displayed inline
- **Gym tracker** — next gym leader and current level cap shown in sidebar
- **Full party sidebar** — all 6 party members visible at a glance, with grayscale sprites for fainted mons
- **Party cycling** — ZR/ZL/Circle Pad/Touch to cycle through party members

Controls (Bottom Screen)
------------------------

| Input | Action |
|-------|--------|
| ZR / Circle Pad Right | Next party member |
| ZL / Circle Pad Left | Previous party member |
| Touch (right side, bottom) | Toggle Moves / Learnset view |
| Touch (elsewhere) | Next party member |
| Circle Pad Up/Down | Scroll learnset |

Prerequisites
-------------

You need the **devkitPro** toolchain with **devkitARM** (the 3DS cross-compiler).

### Windows

1. Download the [devkitPro installer](https://github.com/devkitPro/installer/releases) (`devkitProUpdater-Setup-X.X.X.exe`).
2. Run it and select **3DS Development** during component selection (this installs devkitARM, libctru, citro3d, etc.).
3. Default install path is `C:\devkitPro` — the build script auto-detects this.

### Linux / macOS

```bash
# Install the package manager
wget https://apt.devkitpro.org/install-devkitpro-pacman
chmod +x ./install-devkitpro-pacman
sudo ./install-devkitpro-pacman

# Install the 3DS toolchain
sudo dkp-pacman -S 3ds-dev
```

This installs to `/opt/devkitpro` by default. The build script auto-detects this as well.

### Python (optional, for serve.py)

If you want to use `serve.py` to wirelessly deploy `.cia` files to your 3DS via QR code:

```bash
pip install qrcode[pil]
```

Building
--------

```bash
./build.sh
```

That's it. The script handles everything: environment detection, CMake configuration, and compilation.

The output is `build/3ds/mgba.cia`.

### Options

```bash
./build.sh --clean          # Full rebuild (wipe build dir, re-run CMake)
./build.sh my-build-dir     # Custom build directory (default: build)
```

### What build.sh does

1. Auto-detects `DEVKITPRO` and `DEVKITARM` (checks `C:\devkitPro` on Windows, `/opt/devkitpro` on Unix)
2. Adds the toolchain to `PATH`
3. Runs CMake with the 3DS toolchain file (handles MSYS2 path quirks on Windows)
4. Runs `make -j$(nproc)` for a parallel build
5. Subsequent runs are incremental — only recompiles changed files

Deploying to 3DS
-----------------

### Option A: Build + serve in one step

```bash
./run.sh                    # Build, then serve .cia over HTTP
./run.sh --clean            # Clean build, then serve
./run.sh build 8084         # Custom build dir and port
```

This calls `build.sh` then `serve.py`. Your 3DS can scan the QR code (or open the URL) to download and install the `.cia` directly over WiFi using FBI.

### Option B: Serve an existing build

```bash
python serve.py build       # Serve from ./build/3ds/ on port 8080
python serve.py build 9000  # Custom port
```

`serve.py` detects your local IP, generates a QR code image (`build/3ds/qr.png`), prints the download URL, and starts an HTTP server. It auto-shuts down after the `.cia` is downloaded.

### Option C: Manual install

Copy `build/3ds/mgba.cia` to your 3DS SD card and install it with FBI.

Key Files
---------

```
src/platform/3ds/
  overlay.h        — Public API: overlayDraw() called from main.c
  overlay.c        — Party data decryption, stat calculation, UI layout & rendering
  sprite.h         — Public API: drawPokemonSprite(), drawRect(), spriteFree()
  sprite.c         — LZ77 decompressor, 4bpp tile decoder, palette handler,
                      Morton-order texture encoder, Citro3D texture management
  romprofile.h/c   — ROM profile system: configurable offsets/limits per ROM,
                      auto-detected by game code on first frame
  main.c           — _drawOverlay() hook that calls overlayDraw() each frame
  CMakeLists.txt   — Build config (overlay.c + sprite.c + romprofile.c)
  ctr-gpu.h/c      — Citro3D batch rendering (ctrActivateTexture, ctrAddRectEx)

build.sh           — Cross-platform build script (Windows + Unix)
serve.py           — HTTP server + QR code generator for wireless .cia install
run.sh             — Build + serve in one command
```

Architecture
------------

### Data Pipeline

```
GBA EWRAM (live)                    GBA ROM (static)
      |                                   |
      v                                   v
 Party slot (100 bytes)         Species/Move name tables
      |                         Sprite pointer table (0x2350AC)
      v                         Palette pointer table (0x23730C)
 XOR decrypt (PID ^ OTID)       Learnset pointer table
      |                                   |
      v                                   v
 4 substructures (GAEM order)   LZ77 decompress -> 4bpp detile
      |                         -> palette apply (RGB555->GPU_RGBA8)
      v                         -> Morton-order encode
 PokeSlot struct                -> C3D_Tex upload
      |                                   |
      v                                   v
 GUIFontPrintf (text)           ctrActivateTexture + ctrAddRectEx
      |                                   |
      +------- Citro3D batch pipeline ----+
                      |
                      v
              3DS bottom screen
```

### Gen 3 Decryption

Each 100-byte party Pokemon contains:
- **Header** (32 bytes): PID, OTID, nickname, language, flags
- **Encrypted data** (48 bytes): XOR'd with `PID ^ OTID`, split into 4 x 12-byte substructures
- **Party stats** (20 bytes): calculated stats, level, current HP (unencrypted)

Substructure order is determined by `PID % 24` (24 permutations of Growth/Attacks/EVs/Misc).

### Sprite Rendering

1. Read compressed sprite pointer from ROM table (`species * 8` byte entries)
2. LZ77 decompress (GBA BIOS type `0x10`: flag byte + back-references)
3. Decode 4bpp 8x8 tiles into 64x64 linear pixel indices
4. LZ77 decompress the palette (16 colors, RGB555)
5. Apply palette: RGB555 -> GPU_RGBA8 (`(R<<24)|(G<<16)|(B<<8)|A` for 3DS LE ARM)
6. Write pixels in Morton (Z-order) pattern into `C3D_Tex`
7. Flush CPU data cache, draw as textured quad via `ctrAddRectEx`

Textures are cached per species and only re-decoded on species change. Fainted Pokemon are rendered in grayscale.

ROM Offsets (FireRed US v1.0)
-----------------------------

### ROM Tables

| Offset | Description |
|--------|-------------|
| `0x2350AC` | Front sprite pointer table (8 bytes/entry) |
| `0x23730C` | Normal palette pointer table (8 bytes/entry) |
| `0x245EE0` | Species name table (11 bytes/entry, Gen3 encoded) |
| `0x247094` | Move name table (13 bytes/entry, Gen3 encoded) |

### EWRAM

| Offset (from WRAM base) | Description |
|--------------------------|-------------|
| `0x24029` | Party count |
| `0x24284` | Party data (6 x 100 bytes) |

Adding ROM Hack Support
------------------------

The `RomProfile` system ([romprofile.h/c](src/platform/3ds/romprofile.c)) externalizes all ROM table offsets and limits. Only a vanilla FireRed US v1.0 profile ships by default.

To add a ROM hack profile, add a new entry to `sProfiles[]` in `romprofile.c` with the correct offsets for your hack. Matching could be extended from game code to CRC32 for hacks that share the same game code as their base ROM.

---

This is a hobby project born out of curiosity about Pokemon internals, the GBA memory layout, and the mGBA codebase. I built it for my own use while playing FireRed on 3DS and have little intention of maintaining it beyond features I personally want. That said, feel free to fork it or use it as a reference for your own overlay work.
