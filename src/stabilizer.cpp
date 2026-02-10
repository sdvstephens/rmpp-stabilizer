/*
 * rmpp-stabilizer — Stroke stabilization for reMarkable Paper Pro
 *
 * LD_PRELOAD library that intercepts pen input events and applies
 * configurable smoothing algorithms before xochitl processes them.
 *
 * Build: make (requires aarch64-linux-gnu cross-compiler)
 * Usage: LD_PRELOAD=/home/root/libstabilizer.so /usr/bin/xochitl
 *
 * MIT License
 */

#include <cstring>
#include <cstdio>
#include <cmath>
#include <dlfcn.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

// --- Configuration ---
// TODO: Read from /home/root/.stabilizer.conf
static const int FILTER_WINDOW = 8;
static const char* PEN_DEVICE = nullptr; // Auto-detect from /proc/bus/input/devices

// --- Filter State ---
struct FilterState {
    // Moving average buffers
    int x_buf[32] = {};
    int y_buf[32] = {};
    int p_buf[32] = {};  // pressure
    int buf_idx = 0;
    int buf_count = 0;

    // String pull state
    double string_x = 0.0;
    double string_y = 0.0;
    bool string_initialized = false;
    double string_length = 20.0; // pixels

    // 1€ filter state
    double one_euro_x = 0.0;
    double one_euro_y = 0.0;
    double one_euro_dx = 0.0;
    double one_euro_dy = 0.0;
    double one_euro_last_time = 0.0;
    bool one_euro_initialized = false;
    double min_cutoff = 1.0;
    double beta = 0.007;
    double d_cutoff = 1.0;

    // Current raw values
    int raw_x = 0;
    int raw_y = 0;
    int raw_pressure = 0;
};

static FilterState g_state;
static int g_pen_fd = -1;
static bool g_active = false;

enum Algorithm {
    ALG_MOVING_AVG,
    ALG_STRING_PULL,
    ALG_ONE_EURO,
    ALG_OFF
};

static Algorithm g_algorithm = ALG_STRING_PULL;

// --- 1€ Filter Helper ---
static double one_euro_alpha(double cutoff, double dt) {
    double tau = 1.0 / (2.0 * M_PI * cutoff);
    return 1.0 / (1.0 + tau / dt);
}

static double low_pass(double x, double prev, double alpha) {
    return alpha * x + (1.0 - alpha) * prev;
}

// --- Moving Average Filter ---
static int moving_avg(int* buf, int& idx, int& count, int val, int window) {
    buf[idx] = val;
    idx = (idx + 1) % window;
    if (count < window) count++;
    long sum = 0;
    int n = (count < window) ? count : window;
    for (int i = 0; i < n; i++) sum += buf[i];
    return (int)(sum / n);
}

// --- String Pull Filter ---
static void string_pull(double raw_x, double raw_y,
                        double& out_x, double& out_y,
                        FilterState& s) {
    if (!s.string_initialized) {
        s.string_x = raw_x;
        s.string_y = raw_y;
        s.string_initialized = true;
    }
    double dx = raw_x - s.string_x;
    double dy = raw_y - s.string_y;
    double dist = sqrt(dx * dx + dy * dy);

    if (dist > s.string_length) {
        double ratio = (dist - s.string_length) / dist;
        s.string_x += dx * ratio;
        s.string_y += dy * ratio;
    }

    out_x = s.string_x;
    out_y = s.string_y;
}

// --- 1€ Filter ---
static void one_euro_filter(double raw_x, double raw_y, double timestamp,
                            double& out_x, double& out_y,
                            FilterState& s) {
    if (!s.one_euro_initialized) {
        s.one_euro_x = raw_x;
        s.one_euro_y = raw_y;
        s.one_euro_dx = 0;
        s.one_euro_dy = 0;
        s.one_euro_last_time = timestamp;
        s.one_euro_initialized = true;
        out_x = raw_x;
        out_y = raw_y;
        return;
    }

    double dt = timestamp - s.one_euro_last_time;
    if (dt <= 0) dt = 0.001;
    s.one_euro_last_time = timestamp;

    // Derivative (speed estimation)
    double a_d = one_euro_alpha(s.d_cutoff, dt);
    s.one_euro_dx = low_pass((raw_x - s.one_euro_x) / dt, s.one_euro_dx, a_d);
    s.one_euro_dy = low_pass((raw_y - s.one_euro_y) / dt, s.one_euro_dy, a_d);

    double speed = sqrt(s.one_euro_dx * s.one_euro_dx +
                        s.one_euro_dy * s.one_euro_dy);

    // Adaptive cutoff
    double cutoff = s.min_cutoff + s.beta * speed;
    double a = one_euro_alpha(cutoff, dt);

    s.one_euro_x = low_pass(raw_x, s.one_euro_x, a);
    s.one_euro_y = low_pass(raw_y, s.one_euro_y, a);

    out_x = s.one_euro_x;
    out_y = s.one_euro_y;
}

// --- LD_PRELOAD Hooks ---

// Original function pointers
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

// Detect pen input device
// TODO: Phase 1 recon will determine actual device path on RMPP
static bool is_pen_device(const char* path) {
    // RMPP USI 2.0 pen — exact path TBD after device recon
    // RM2 was /dev/input/event1 (Wacom I2C Digitizer)
    // RMPP likely /dev/input/event0 or event1 or event2
    if (!path) return false;
    // Placeholder: match any input event device for now
    // Will be narrowed after Phase 1
    return (strstr(path, "/dev/input/event") != nullptr);
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
        fprintf(stderr, "[stabilizer] Intercepting pen device: %s (fd=%d)\n",
                pathname, fd);
    }

    return fd;
}

extern "C" ssize_t read(int fd, void* buf, size_t count) {
    init_hooks();

    ssize_t ret = real_read(fd, buf, count);

    if (ret <= 0 || fd != g_pen_fd || !g_active)
        return ret;

    if (g_algorithm == ALG_OFF)
        return ret;

    // Process input events
    size_t ev_size = sizeof(struct input_event);
    size_t num_events = ret / ev_size;
    struct input_event* events = (struct input_event*)buf;

    for (size_t i = 0; i < num_events; i++) {
        struct input_event& ev = events[i];

        if (ev.type != EV_ABS) continue;

        if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
            g_state.raw_x = ev.value;
        } else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
            g_state.raw_y = ev.value;

            // Apply filter on Y (we now have both X and Y)
            double out_x, out_y;

            switch (g_algorithm) {
                case ALG_MOVING_AVG: {
                    // Simple independent axis averaging
                    // X was already buffered above — apply on sync
                    break;
                }
                case ALG_STRING_PULL: {
                    string_pull(g_state.raw_x, g_state.raw_y,
                               out_x, out_y, g_state);
                    // Write back filtered position
                    // Walk back to find the X event in this batch
                    for (size_t j = 0; j <= i; j++) {
                        if (events[i-j].type == EV_ABS &&
                            (events[i-j].code == ABS_X ||
                             events[i-j].code == ABS_MT_POSITION_X)) {
                            events[i-j].value = (int)out_x;
                            break;
                        }
                    }
                    ev.value = (int)out_y;
                    break;
                }
                case ALG_ONE_EURO: {
                    double ts = ev.time.tv_sec + ev.time.tv_usec / 1e6;
                    one_euro_filter(g_state.raw_x, g_state.raw_y, ts,
                                   out_x, out_y, g_state);
                    for (size_t j = 0; j <= i; j++) {
                        if (events[i-j].type == EV_ABS &&
                            (events[i-j].code == ABS_X ||
                             events[i-j].code == ABS_MT_POSITION_X)) {
                            events[i-j].value = (int)out_x;
                            break;
                        }
                    }
                    ev.value = (int)out_y;
                    break;
                }
                case ALG_OFF:
                    break;
            }
        } else if (ev.code == ABS_PRESSURE ||
                   ev.code == ABS_MT_PRESSURE) {
            g_state.raw_pressure = ev.value;
            // TODO: pressure smoothing
        }
    }

    // Reset string pull on pen lift (SYN_REPORT with no touch)
    for (size_t i = 0; i < num_events; i++) {
        if (events[i].type == EV_KEY && events[i].code == BTN_TOUCH &&
            events[i].value == 0) {
            // Pen lifted — reset filter state
            g_state.string_initialized = false;
            g_state.one_euro_initialized = false;
            g_state.buf_count = 0;
            g_state.buf_idx = 0;
        }
    }

    return ret;
}
