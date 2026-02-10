/*
 * rmpp-stabilizer — Stroke stabilization for reMarkable Paper Pro
 *
 * LD_PRELOAD library that intercepts pen input events and applies
 * configurable smoothing algorithms before xochitl processes them.
 *
 * Algorithms informed by studying Krita's open-source stabilizer
 * (GPL-2.0, KDE/krita). This is a clean-room reimplementation
 * adapted for the LD_PRELOAD/evdev interception model.
 *
 * Build: docker run --rm -v $(pwd):/build rmpp-toolchain
 * Usage: LD_PRELOAD=/home/root/libstabilizer.so /usr/bin/xochitl
 *
 * MIT License
 */

#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <dlfcn.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================
// Configuration
// ============================================================

static const char* CONFIG_PATH = "/home/root/.stabilizer.conf";
static const int MAX_HISTORY = 64;

enum Algorithm {
    ALG_MOVING_AVG,
    ALG_GAUSSIAN_AVG,   // Krita-style weighted smoothing
    ALG_STRING_PULL,     // Krita-style stabilizer / delay distance
    ALG_ONE_EURO,        // Casiez et al. 2012
    ALG_OFF
};

struct Config {
    Algorithm algorithm = ALG_STRING_PULL;
    double strength = 0.5;          // 0.0-1.0 master control
    bool pressure_smoothing = false;
    bool tilt_smoothing = false;

    // Algorithm-specific params (derived from strength)
    int moving_avg_window = 8;
    double gaussian_sigma = 30.0;    // distance-based sigma
    double string_length = 25.0;     // dead zone radius
    bool string_finish = true;       // complete line on lift
    double one_euro_mincutoff = 1.0;
    double one_euro_beta = 0.007;
    double one_euro_dcutoff = 1.0;
};

static Config g_config;

// ============================================================
// Point history (for Gaussian weighting)
// ============================================================

struct Point {
    double x = 0, y = 0;
    double pressure = 0;
    double tilt_x = 0, tilt_y = 0;
    double distance = 0;  // distance from previous point
};

struct FilterState {
    // History ring buffer
    Point history[MAX_HISTORY];
    int hist_count = 0;
    int hist_head = 0;  // newest entry index

    // String pull state
    double string_x = 0, string_y = 0;
    bool string_init = false;

    // 1€ filter state
    double oe_x = 0, oe_y = 0;
    double oe_dx = 0, oe_dy = 0;
    double oe_last_time = 0;
    bool oe_init = false;

    // Current raw values (accumulated between SYN_REPORTs)
    int raw_x = 0, raw_y = 0;
    int raw_pressure = 0;
    int raw_tilt_x = 0, raw_tilt_y = 0;
    bool has_x = false, has_y = false;

    // Previous output (for distance calc)
    double prev_x = 0, prev_y = 0;
    bool prev_init = false;
};

static FilterState g_state;
static int g_pen_fd = -1;
static bool g_active = false;

// ============================================================
// Config file reader
// ============================================================

// Derive algorithm params from strength value
static void derive_params() {
    double s = g_config.strength;
    g_config.moving_avg_window = 4 + (int)(s * 28);
    g_config.gaussian_sigma = 50.0 + s * 450.0;
    g_config.string_length = 100.0 + s * 900.0;
    g_config.one_euro_mincutoff = 1.5 - s * 1.3;
    g_config.one_euro_beta = 0.001 + s * 0.01;
}

static void load_config() {
    // Always derive params from defaults first
    derive_params();

    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f) {
        fprintf(stderr, "[stabilizer] No config file, using defaults: alg=%d strength=%.2f string_len=%.1f\n",
                g_config.algorithm, g_config.strength, g_config.string_length);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64], val[64];
        if (sscanf(line, "%63[^=]=%63s", key, val) == 2) {
            if (strcmp(key, "algorithm") == 0) {
                if (strcmp(val, "off") == 0) g_config.algorithm = ALG_OFF;
                else if (strcmp(val, "moving_avg") == 0) g_config.algorithm = ALG_MOVING_AVG;
                else if (strcmp(val, "gaussian") == 0) g_config.algorithm = ALG_GAUSSIAN_AVG;
                else if (strcmp(val, "string_pull") == 0) g_config.algorithm = ALG_STRING_PULL;
                else if (strcmp(val, "one_euro") == 0) g_config.algorithm = ALG_ONE_EURO;
            }
            else if (strcmp(key, "strength") == 0) {
                g_config.strength = atof(val);
                if (g_config.strength < 0) g_config.strength = 0;
                if (g_config.strength > 1) g_config.strength = 1;
            }
            else if (strcmp(key, "pressure_smoothing") == 0) {
                g_config.pressure_smoothing = (strcmp(val, "true") == 0);
            }
            else if (strcmp(key, "tilt_smoothing") == 0) {
                g_config.tilt_smoothing = (strcmp(val, "true") == 0);
            }
        }
    }
    fclose(f);
    // Re-derive params with any values from config
    derive_params();
    fprintf(stderr, "[stabilizer] Config: alg=%d strength=%.2f string_len=%.1f\n",
            g_config.algorithm, g_config.strength, g_config.string_length);
}

// ============================================================
// History management
// ============================================================

static void history_push(double x, double y, double pressure,
                         double tilt_x, double tilt_y) {
    FilterState& s = g_state;
    int idx = (s.hist_head + 1) % MAX_HISTORY;
    Point& p = s.history[idx];
    p.x = x; p.y = y;
    p.pressure = pressure;
    p.tilt_x = tilt_x; p.tilt_y = tilt_y;

    // Compute distance from previous point
    if (s.hist_count > 0) {
        Point& prev = s.history[s.hist_head];
        double dx = x - prev.x, dy = y - prev.y;
        p.distance = sqrt(dx*dx + dy*dy);
    } else {
        p.distance = 0;
    }

    s.hist_head = idx;
    if (s.hist_count < MAX_HISTORY) s.hist_count++;
}

static void history_clear() {
    g_state.hist_count = 0;
    g_state.hist_head = 0;
    g_state.string_init = false;
    g_state.oe_init = false;
    g_state.prev_init = false;
    g_state.has_x = false;
    g_state.has_y = false;
}

// ============================================================
// Algorithm: Gaussian-Weighted Average
// Inspired by Krita's weighted smoothing. Weight decays
// exponentially with cumulative distance traveled, using
// a Gaussian kernel. Recent nearby points contribute most.
// ============================================================

static void gaussian_smooth(double raw_x, double raw_y, double raw_p,
                            double& out_x, double& out_y, double& out_p) {
    FilterState& s = g_state;
    double sigma = g_config.gaussian_sigma;
    if (sigma <= 0 || s.hist_count < 2) {
        out_x = raw_x; out_y = raw_y; out_p = raw_p;
        return;
    }

    double sigma2 = sigma * sigma;
    double gauss_norm = 1.0 / (sqrt(2.0 * M_PI) * sigma);

    double sum_x = 0, sum_y = 0, sum_p = 0;
    double sum_w = 0;
    double cum_dist = 0;

    // Walk backward through history, accumulating distance
    int idx = s.hist_head;
    for (int i = 0; i < s.hist_count; i++) {
        Point& p = s.history[idx];
        cum_dist += p.distance;

        double w = gauss_norm * exp(-cum_dist * cum_dist / (2.0 * sigma2));

        // Early termination: if weight is negligible, stop
        if (i > 0 && sum_w > 0 && w / sum_w < 0.001) break;

        sum_x += w * p.x;
        sum_y += w * p.y;
        sum_p += w * p.pressure;
        sum_w += w;

        idx = (idx - 1 + MAX_HISTORY) % MAX_HISTORY;
    }

    if (sum_w > 0) {
        out_x = sum_x / sum_w;
        out_y = sum_y / sum_w;
        out_p = g_config.pressure_smoothing ? sum_p / sum_w : raw_p;
    } else {
        out_x = raw_x; out_y = raw_y; out_p = raw_p;
    }
}

// ============================================================
// Algorithm: String Pull / Delay Distance
// Inspired by Krita's stabilizer mode and Lazy Nezumi.
// Output point is connected to pen by a virtual string of
// fixed length. Pen "pulls" the output along behind it.
// Zero steady-state latency when pen is stationary.
// ============================================================

static void string_pull_filter(double raw_x, double raw_y,
                               double& out_x, double& out_y) {
    FilterState& s = g_state;
    double L = g_config.string_length;

    if (!s.string_init) {
        s.string_x = raw_x;
        s.string_y = raw_y;
        s.string_init = true;
    }

    double dx = raw_x - s.string_x;
    double dy = raw_y - s.string_y;
    double dist = sqrt(dx*dx + dy*dy);

    if (dist > L) {
        // Pull the string endpoint toward the pen
        double ratio = (dist - L) / dist;
        s.string_x += dx * ratio;
        s.string_y += dy * ratio;
    }
    // If within dead zone, output stays put (the magic)

    out_x = s.string_x;
    out_y = s.string_y;
}

// ============================================================
// Algorithm: 1€ Filter (Casiez et al. 2012)
// Speed-adaptive: heavy smoothing when slow (precise drawing),
// minimal smoothing when fast (responsive gestures).
// ============================================================

static double oe_alpha(double cutoff, double dt) {
    double tau = 1.0 / (2.0 * M_PI * cutoff);
    return 1.0 / (1.0 + tau / dt);
}

static double oe_lowpass(double x, double prev, double a) {
    return a * x + (1.0 - a) * prev;
}

static void one_euro_filter(double raw_x, double raw_y, double timestamp,
                            double& out_x, double& out_y) {
    FilterState& s = g_state;
    Config& c = g_config;

    if (!s.oe_init) {
        s.oe_x = raw_x; s.oe_y = raw_y;
        s.oe_dx = 0; s.oe_dy = 0;
        s.oe_last_time = timestamp;
        s.oe_init = true;
        out_x = raw_x; out_y = raw_y;
        return;
    }

    double dt = timestamp - s.oe_last_time;
    if (dt <= 0) dt = 0.002; // ~500Hz fallback
    s.oe_last_time = timestamp;

    // Estimate speed via derivative
    double ad = oe_alpha(c.one_euro_dcutoff, dt);
    s.oe_dx = oe_lowpass((raw_x - s.oe_x) / dt, s.oe_dx, ad);
    s.oe_dy = oe_lowpass((raw_y - s.oe_y) / dt, s.oe_dy, ad);
    double speed = sqrt(s.oe_dx*s.oe_dx + s.oe_dy*s.oe_dy);

    // Adaptive cutoff: higher speed → higher cutoff → less smoothing
    double cutoff = c.one_euro_mincutoff + c.one_euro_beta * speed;
    double a = oe_alpha(cutoff, dt);

    s.oe_x = oe_lowpass(raw_x, s.oe_x, a);
    s.oe_y = oe_lowpass(raw_y, s.oe_y, a);

    out_x = s.oe_x;
    out_y = s.oe_y;
}

// ============================================================
// Algorithm: Simple Moving Average
// ============================================================

static void moving_avg_filter(double raw_x, double raw_y,
                              double& out_x, double& out_y) {
    FilterState& s = g_state;
    int window = g_config.moving_avg_window;
    if (window < 1) window = 1;

    double sx = 0, sy = 0;
    int n = (s.hist_count < window) ? s.hist_count : window;
    if (n == 0) { out_x = raw_x; out_y = raw_y; return; }

    int idx = s.hist_head;
    for (int i = 0; i < n; i++) {
        sx += s.history[idx].x;
        sy += s.history[idx].y;
        idx = (idx - 1 + MAX_HISTORY) % MAX_HISTORY;
    }
    out_x = sx / n;
    out_y = sy / n;
}

// ============================================================
// Master filter dispatch
// ============================================================

static void apply_filter(double raw_x, double raw_y, double raw_p,
                         double timestamp,
                         double& out_x, double& out_y, double& out_p) {
    out_p = raw_p; // default: pass through

    switch (g_config.algorithm) {
        case ALG_MOVING_AVG:
            moving_avg_filter(raw_x, raw_y, out_x, out_y);
            break;
        case ALG_GAUSSIAN_AVG:
            gaussian_smooth(raw_x, raw_y, raw_p, out_x, out_y, out_p);
            break;
        case ALG_STRING_PULL:
            string_pull_filter(raw_x, raw_y, out_x, out_y);
            break;
        case ALG_ONE_EURO:
            one_euro_filter(raw_x, raw_y, timestamp, out_x, out_y);
            break;
        case ALG_OFF:
        default:
            out_x = raw_x; out_y = raw_y;
            break;
    }
}

// ============================================================
// LD_PRELOAD hooks
// ============================================================

typedef int (*open_func_t)(const char*, int, ...);
typedef ssize_t (*read_func_t)(int, void*, size_t);

static open_func_t real_open = nullptr;
static read_func_t real_read = nullptr;

static void init_hooks() {
    if (!real_open)
        real_open = (open_func_t)dlsym(RTLD_NEXT, "open");
    if (!real_read)
        real_read = (read_func_t)dlsym(RTLD_NEXT, "read");
}

static bool is_pen_device(const char* path) {
    if (!path) return false;
    // RMPP: "Elan marker input" = /dev/input/event2
    return (strcmp(path, "/dev/input/event2") == 0);
}

extern "C" int open(const char* pathname, int flags, ...) {
    init_hooks();

    int fd;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode_t mode = va_arg(args, mode_t);
        va_end(args);
        fd = real_open(pathname, flags, mode);
    } else {
        fd = real_open(pathname, flags);
    }

    if (fd >= 0 && is_pen_device(pathname)) {
        g_pen_fd = fd;
        g_active = true;
        load_config();
        fprintf(stderr, "[stabilizer] Intercepting: %s (fd=%d) alg=%d\n",
                pathname, fd, g_config.algorithm);
    }

    return fd;
}

extern "C" ssize_t read(int fd, void* buf, size_t count) {
    init_hooks();
    ssize_t ret = real_read(fd, buf, count);

    if (ret <= 0 || fd != g_pen_fd || !g_active || g_config.algorithm == ALG_OFF)
        return ret;

    size_t ev_size = sizeof(struct input_event);
    size_t num_events = ret / ev_size;
    struct input_event* events = (struct input_event*)buf;

    // First pass: accumulate raw values
    for (size_t i = 0; i < num_events; i++) {
        struct input_event& ev = events[i];

        if (ev.type == EV_ABS) {
            switch (ev.code) {
                case ABS_X: g_state.raw_x = ev.value; g_state.has_x = true; break;
                case ABS_Y: g_state.raw_y = ev.value; g_state.has_y = true; break;
                case ABS_PRESSURE: g_state.raw_pressure = ev.value; break;
                case ABS_TILT_X: g_state.raw_tilt_x = ev.value; break;
                case ABS_TILT_Y: g_state.raw_tilt_y = ev.value; break;
            }
        }

        // On SYN_REPORT: apply filter and write back
        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            // Only filter if we have position data
            if (g_state.has_x || g_state.has_y) {
                double rx = g_state.raw_x, ry = g_state.raw_y;
                double rp = g_state.raw_pressure;
                double ts = ev.time.tv_sec + ev.time.tv_usec / 1e6;

                // Push raw point into history
                history_push(rx, ry, rp,
                            g_state.raw_tilt_x, g_state.raw_tilt_y);

                // Apply filter
                double fx, fy, fp;
                apply_filter(rx, ry, rp, ts, fx, fy, fp);

                // Debug: log every 50th event to show filtering is working
                static int debug_counter = 0;
                if (debug_counter++ % 50 == 0) {
                    fprintf(stderr, "[stab] raw=(%d,%d) filtered=(%.0f,%.0f) delta=(%.1f,%.1f)\n",
                            g_state.raw_x, g_state.raw_y, fx, fy,
                            fx - rx, fy - ry);
                }

                // Write filtered values back into event buffer
                for (size_t j = 0; j <= i; j++) {
                    size_t k = i - j;
                    if (events[k].type == EV_ABS) {
                        if (events[k].code == ABS_X)
                            events[k].value = (int)(fx + 0.5);
                        else if (events[k].code == ABS_Y)
                            events[k].value = (int)(fy + 0.5);
                        else if (events[k].code == ABS_PRESSURE
                                 && g_config.pressure_smoothing)
                            events[k].value = (int)(fp + 0.5);
                    }
                }
            }
            g_state.has_x = false;
            g_state.has_y = false;
        }

        // Pen lift detection: RMPP has no BTN_TOUCH
        // Reset on pressure < threshold or BTN_TOOL_PEN release
        if (ev.type == EV_ABS && ev.code == ABS_PRESSURE
            && ev.value < 50) {
            history_clear();
        }
        if (ev.type == EV_KEY && ev.code == BTN_TOOL_PEN
            && ev.value == 0) {
            history_clear();
        }
    }

    return ret;
}
