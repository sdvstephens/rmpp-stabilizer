# rmpp-stabilizer

Stroke stabilization for the reMarkable Paper Pro. Brings GoodNotes/Noteful-style line smoothing to the rMPP's native drawing experience.

## What It Does

Intercepts pen input events before xochitl processes them and applies configurable smoothing algorithms. Your lines come out cleaner without changing how the tablet feels to use.

**Before / After:**
> Screenshots coming once v0.1 is tested on device.

## Algorithms

- **Moving Average** — Simple N-sample window smoothing. Removes jitter.
- **String Pull** — Virtual "string" between pen and output. Produces naturally smooth curves with minimal latency. This is what makes apps like GoodNotes feel magic.
- **1€ Filter** — Speed-adaptive smoothing (Casiez et al. 2012). Heavy smoothing for slow/precise strokes, minimal smoothing for fast gestures.

## Requirements

- reMarkable Paper Pro with Developer Mode enabled
- Firmware 3.22.x (tested on 3.22.4.2)
- SSH access via USB (`ssh root@10.11.99.1`)

## Technical Details

- **Pen device:** "Elan marker input" at `/dev/input/event2` (SPI-connected)
- **xochitl:** Dynamically linked against Qt6 (6.8.2) — LD_PRELOAD confirmed working
- **Pen axes:** ABS_X, ABS_Y, ABS_PRESSURE (24), ABS_DISTANCE (25), ABS_TILT_X (26), ABS_TILT_Y (27)
- **Sample rate:** ~500Hz (~2ms between events)
- **No BTN_TOUCH:** Pen lift detected via pressure threshold

## Installation

```bash
# From your computer (macOS/Linux):
git clone https://github.com/sdvstephens/rmpp-stabilizer.git
cd rmpp-stabilizer
./scripts/install.sh
```

The install script will:
1. Copy the precompiled library to your device
2. Configure xochitl to load it on startup
3. Restart xochitl

## Configuration

Edit `/home/root/.stabilizer.conf` on the device:

```ini
algorithm=string_pull
strength=0.5
pressure_smoothing=true
tilt_smoothing=false
```

Changes take effect on next xochitl restart.

## Uninstall

```bash
./scripts/uninstall.sh
```

## How It Works

Uses `LD_PRELOAD` to intercept pen input device reads before xochitl sees them. The smoothing filter runs inline with negligible overhead (<1ms typical). No modification to xochitl or system files required.

Inspired by [recept](https://github.com/funkey/recept) (RM2 line smoothing) and [Krita's stabilizer](https://github.com/KDE/krita) algorithms.

## Status

🚧 **In Development** — Phase 2 (cross-compilation & device testing)

## Credits

- [recept](https://github.com/funkey/recept) — RM2 line smoothing via LD_PRELOAD
- [Krita](https://github.com/KDE/krita) — Open source stabilizer algorithm reference
- 1€ Filter — [Casiez et al. 2012](https://cristal.univ-lille.fr/~casiez/1euro/)
- [xovi](https://github.com/asivery/xovi) framework by @asivery

## License

MIT
