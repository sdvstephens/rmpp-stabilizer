# Architecture

## System Overview

```
┌──────────────────────────────────────────────────────────────┐
│                    reMarkable Paper Pro                       │
│                    Firmware 3.22.4.2                          │
│                                                              │
│  ┌─────────────┐     ┌──────────────────┐     ┌──────────┐  │
│  │ Elan marker │────▶│  libstabilizer   │────▶│ xochitl  │  │
│  │   input     │     │  (LD_PRELOAD)    │     │ (Qt6.8)  │  │
│  │ /dev/input/ │     │                  │     │          │  │
│  │  event2     │     │  ┌────────────┐  │     └──────────┘  │
│  └─────────────┘     │  │ Algorithms │  │                    │
│   SPI / Elan         │  │            │  │                    │
│   USI 2.0 pen        │  │ • MovAvg   │  │                    │
│                      │  │ • StringPl │  │                    │
│                      │  │ • 1€ Filtr │  │                    │
│                      │  └────────────┘  │                    │
│                      └──────────────────┘                    │
│                              ▲                               │
│                              │                               │
│                      ~/.stabilizer.conf                      │
└──────────────────────────────────────────────────────────────┘
```

## Device Details (from Phase 1 Recon)

### Input Devices
| Device | Path | Description |
|--------|------|-------------|
| Power key | /dev/input/event0 | 30370000.snvs:snvs-powerkey |
| Hall sensors | /dev/input/event1 | GPIO hall effect sensors |
| **Pen** | **/dev/input/event2** | **Elan marker input (SPI)** |
| Touch | /dev/input/event3 | Elan touch input (capacitive) |

### Pen Axes
| Axis | Code | Observed Range | Notes |
|------|------|----------------|-------|
| ABS_X | 0 | ~6600–7800+ | Position X |
| ABS_Y | 1 | ~11200–12500+ | Position Y |
| ABS_PRESSURE | 24 | 642–2483 | Never 0 during contact |
| ABS_DISTANCE | 25 | 18776–19550 | Pen-to-screen distance |
| ABS_TILT_X | 26 | 259–997 | Pen angle X |
| ABS_TILT_Y | 27 | -1386 to -467 | Pen angle Y |

### Key Characteristics
- **~500Hz sample rate** (~2ms between events)
- **No BTN_TOUCH events** — pen lift detected via pressure threshold
- **Partial events** — sometimes only X or Y sent when other unchanged
- **Hover events** — tilt/pressure/distance continue after stroke ends

### xochitl
- Path: `/usr/bin/xochitl`
- **Dynamically linked** against Qt6 (6.8.2)
- Shared libs at `/usr/lib/libQt6*.so.6.8.2`
- LD_PRELOAD confirmed viable

## Interception Method

xochitl reads pen events from `/dev/input/event2` using standard POSIX
`read()` syscalls. By injecting `libstabilizer.so` via `LD_PRELOAD`, we
intercept these reads and modify the event data in-place before returning
it to xochitl. From xochitl's perspective, the pen simply moves more
smoothly.

### Event Flow

1. Pen touches screen → Elan digitizer generates input_event structs
2. Kernel writes events to `/dev/input/event2`
3. xochitl calls `read(fd, buf, count)`
4. **libstabilizer intercepts** → applies smoothing to X/Y values
5. Modified events returned to xochitl
6. xochitl renders smoothed stroke

### Why LD_PRELOAD?

- Zero modification to xochitl binary or system files
- Trivial to install/uninstall (systemd env var)
- Works across firmware versions (xochitl is dynamically linked)
- Negligible performance overhead (filter runs in-process)

## Algorithms

### 1. Moving Average
Window of N samples. Output = mean of window.
Latency: N/2 samples. Simple but adds perceptible lag at large N.

### 2. String Pull (Lazy Nezumi / Krita Stabilizer style)
Virtual string of length L between pen tip and output point.
Output follows pen but can never be more than L units away.
Zero steady-state latency — output converges when stationary.
Produces naturally smooth curves during motion.

### 3. 1€ Filter (Casiez et al. 2012)
Speed-adaptive low-pass filter. Slow movements get heavy smoothing,
fast movements get minimal smoothing.
Parameters: min_cutoff (smoothing at rest), beta (speed sensitivity).

## Pen Lift Detection

The Elan digitizer does NOT send BTN_TOUCH events. Pen lift is detected
by watching for ABS_PRESSURE dropping below a threshold (currently 100).
All filter state resets on lift to prevent smoothing artifacts carrying
over between strokes.

## Configuration

Config file: `/home/root/.stabilizer.conf`

```ini
algorithm=string_pull    # moving_avg | string_pull | one_euro | off
strength=0.5             # 0.0-1.0, maps to algorithm-specific params
pressure_smoothing=false # smooth pressure axis
tilt_smoothing=false     # smooth tilt axes
```

## rmHacks Integration (Planned)

The rmHacks `.qmd` patch system can inject UI elements into xochitl's
settings pages. Plan: add a toggle in rmHacks settings that writes to
`~/.stabilizer.conf`. The LD_PRELOAD library watches this file for
changes. Two decoupled systems communicating via config file.
