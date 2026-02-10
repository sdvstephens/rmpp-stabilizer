# Architecture

## System Overview

```
┌──────────────────────────────────────────────────────────────┐
│                    reMarkable Paper Pro                       │
│                                                              │
│  ┌─────────────┐     ┌──────────────────┐     ┌──────────┐  │
│  │  USI 2.0    │────▶│  libstabilizer   │────▶│ xochitl  │  │
│  │  Digitizer  │     │  (LD_PRELOAD)    │     │ (Qt GUI) │  │
│  │ /dev/input/ │     │                  │     │          │  │
│  │  eventN     │     │  ┌────────────┐  │     └──────────┘  │
│  └─────────────┘     │  │ Algorithms │  │                    │
│                      │  │            │  │                    │
│                      │  │ • MovAvg   │  │                    │
│                      │  │ • StringPl │  │                    │
│                      │  │ • 1€ Filtr │  │                    │
│                      │  └────────────┘  │                    │
│                      └──────────────────┘                    │
│                              ▲                               │
│                              │                               │
│                      ~/.stabilizer.conf                      │
└──────────────────────────────────────────────────────────────┘
```

## Interception Method

xochitl reads pen events from `/dev/input/eventN` using standard POSIX
`read()` syscalls. By injecting `libstabilizer.so` via `LD_PRELOAD`, we
intercept these reads and modify the event data in-place before returning
it to xochitl. From xochitl's perspective, the pen simply moves more
smoothly.

### Event Flow

1. Pen touches screen → USI 2.0 digitizer generates input_event structs
2. Kernel writes events to `/dev/input/eventN`
3. xochitl calls `read(fd, buf, count)`
4. **libstabilizer intercepts** → applies smoothing to X/Y/Pressure values
5. Modified events returned to xochitl
6. xochitl renders smoothed stroke

### Why LD_PRELOAD?

- Zero modification to xochitl binary or system files
- Trivial to install/uninstall (systemd env var)
- Works across firmware versions (unless xochitl becomes statically linked)
- Negligible performance overhead (filter runs in-process)

## Algorithms

### 1. Moving Average
Window of N samples. Output = mean of window.
Latency: N/2 samples. Simple but adds perceptible lag at large N.

### 2. String Pull (Lazy Nezumi-style)
Virtual string of length L between pen tip and output point.
Output follows pen but can never be more than L pixels away.
Zero steady-state latency — output converges to pen position when stationary.
Produces naturally smooth curves during motion.

### 3. 1€ Filter (Casiez et al. 2012)
Speed-adaptive low-pass filter. Slow movements get heavy smoothing (precise
drawing), fast movements get minimal smoothing (responsive gestures).
Parameters: min_cutoff (smoothing at rest), beta (speed sensitivity).

## Pen Lift Handling

All filter state is reset on BTN_TOUCH=0 (pen lift). This ensures:
- No "sticky" smoothing carrying over between strokes
- String pull snaps to pen on next touch-down
- Clean start for each new stroke
