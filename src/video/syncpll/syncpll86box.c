#include "syncpll86box.h"

#include <86box/thread.h>
#include <86box/video.h>

#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef SYNCPLL_MONITORS
#define SYNCPLL_MONITORS MONITORS_NUM
#endif

#define SYNCPLL_CGA_DOT_CLOCK_HZ 14318180.0
#define SYNCPLL_CGA_H_TOTAL 912.0
#define SYNCPLL_CGA_V_TOTAL 262.0
#define SYNCPLL_CGA_H_CENTER_HZ (SYNCPLL_CGA_DOT_CLOCK_HZ / SYNCPLL_CGA_H_TOTAL)
#define SYNCPLL_CGA_V_CENTER_HZ (SYNCPLL_CGA_H_CENTER_HZ / SYNCPLL_CGA_V_TOTAL)
/* IBM CGA connector sync, after the adapter's one-shot shaping network.
 * Raw MC6845 HS begins at dot 720, but the external pulse begins two LCLKs
 * later at dot 752. Connector VS begins on that shaped-HS edge of line 224. */
#define SYNCPLL_CGA_HSYNC_DOT 752.0
#define SYNCPLL_HSYNC_PHASE (SYNCPLL_CGA_HSYNC_DOT / 912.0)
#define SYNCPLL_VSYNC_PHASE ((224.0 + SYNCPLL_CGA_HSYNC_DOT / 912.0) / 262.0)
#define SYNCPLL_BLACK 0xff000000u
#define SYNCPLL_Q32_SCALE 4294967296.0
#define SYNCPLL_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

typedef struct syncpll_monitor_state_t {
    mutex_t *mutex;

    /* Presentation geometry is authored only by the 86Box blit thread and
       applied by the CGA producer at a segment boundary. */
    int requested_x;
    int requested_y;
    int requested_w;
    int requested_h;
    atomic_uint_fast64_t requested_generation;
    uint64_t applied_generation;

    /* Logical monitor aperture. build is currently scanned; front is the last
       complete vertical-oscillator cycle. There is no phosphor or optical pass. */
    uint32_t *build;
    uint32_t *front;
    size_t build_capacity;
    size_t front_capacity;
    int raster_w;
    int raster_h;
    int front_w;
    int front_h;
    int front_valid;
    uint64_t front_generation;
    int frame_armed;

    /* Independent monitor oscillators and PI sync loops. */
    uint32_t h_phase_q32;
    uint32_t v_phase_q32;
    uint32_t h_step_q32;
    uint32_t v_step_q32;
    double h_osc_hz;
    double h_integrator_hz;
    double h_period_samples;
    double h_lock;
    double v_osc_hz;
    double v_integrator_hz;
    double v_period_samples;
    double v_lock;

    uint64_t sample_index;
    uint64_t last_hsync_rise;
    uint64_t hsync_assert_sample;
    uint64_t last_hsync_width;
    uint64_t last_vsync_rise;
    uint64_t vsync_assert_sample;
    uint64_t last_vsync_width;
    uint8_t signal_hsync;
    uint8_t signal_vsync;
    uint8_t seen_hsync;
    uint8_t seen_vsync;
    double sample_clock_hz;

    /* Stretch isolated skipped output samples rather than leaving pinholes
       when the oscillator phase lies between integer output coordinates. */
    int last_x;
    int last_y;
    int last_valid;
    uint32_t last_pixel;

    atomic_int live_active;
    atomic_uint_fast64_t submitted_samples;
    atomic_uint_fast64_t completed_scans;
    atomic_uint_fast64_t rejected_hsync_edges;
    atomic_uint_fast64_t rejected_vsync_edges;
} syncpll_monitor_state_t;

static syncpll_monitor_state_t monitor_state[SYNCPLL_MONITORS];
static atomic_int syncpll_enabled;
static atomic_int syncpll_started;

static double wrap_signed_half(double value)
{
    value -= floor(value + 0.5);
    return value;
}

static uint32_t phase_from_double(double value)
{
    value -= floor(value);
    return (uint32_t)(uint64_t)llrint(value * SYNCPLL_Q32_SCALE);
}

static double phase_to_double(uint32_t value)
{
    return (double)value / SYNCPLL_Q32_SCALE;
}

static uint32_t step_from_hz(double hz, double sample_clock_hz)
{
    double step;
    if (!(hz > 0.0) || !(sample_clock_hz > 0.0))
        return 1u;
    step = hz * SYNCPLL_Q32_SCALE / sample_clock_hz;
    if (step < 1.0)
        step = 1.0;
    if (step > 4294967295.0)
        step = 4294967295.0;
    return (uint32_t)llrint(step);
}

static void fill_black(uint32_t *pixels, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i)
        pixels[i] = SYNCPLL_BLACK;
}

static int ensure_pixels(uint32_t **pixels, size_t *capacity, size_t count)
{
    uint32_t *next;
    if (!pixels || !capacity || count == 0)
        return 0;
    if (count <= *capacity)
        return 1;
    if (count > SIZE_MAX / sizeof(uint32_t))
        return 0;
    next = (uint32_t *)realloc(*pixels, count * sizeof(uint32_t));
    if (!next)
        return 0;
    *pixels = next;
    *capacity = count;
    return 1;
}

static void reset_pll(syncpll_monitor_state_t *state)
{
    state->h_phase_q32 = 0u;
    state->v_phase_q32 = 0u;
    state->h_osc_hz = SYNCPLL_CGA_H_CENTER_HZ;
    state->v_osc_hz = SYNCPLL_CGA_V_CENTER_HZ;
    state->h_integrator_hz = 0.0;
    state->v_integrator_hz = 0.0;
    state->h_period_samples = 0.0;
    state->v_period_samples = 0.0;
    state->h_lock = 0.0;
    state->v_lock = 0.0;
    state->sample_index = 0u;
    state->last_hsync_rise = 0u;
    state->last_vsync_rise = 0u;
    state->hsync_assert_sample = 0u;
    state->vsync_assert_sample = 0u;
    state->last_hsync_width = 0u;
    state->last_vsync_width = 0u;
    state->signal_hsync = 0u;
    state->signal_vsync = 0u;
    state->seen_hsync = 0u;
    state->seen_vsync = 0u;
    state->sample_clock_hz = SYNCPLL_CGA_DOT_CLOCK_HZ;
    state->h_step_q32 = step_from_hz(state->h_osc_hz, state->sample_clock_hz);
    state->v_step_q32 = step_from_hz(state->v_osc_hz, state->sample_clock_hz);
    state->last_valid = 0;
}

static int configure_raster_locked(syncpll_monitor_state_t *state,
                                   int presentation_w, int presentation_h)
{
    const int overscan = presentation_w >= 800;
    const int raster_w = overscan ? 832 : 640;
    const int raster_h = overscan ? 262 : 200;
    const size_t count = (size_t)raster_w * (size_t)raster_h;

    (void)presentation_h;
    if (state->raster_w == raster_w && state->raster_h == raster_h
        && state->build && state->front) {
        state->applied_generation = atomic_load_explicit(
            &state->requested_generation, memory_order_acquire);
        return 1;
    }
    if (!ensure_pixels(&state->build, &state->build_capacity, count)
        || !ensure_pixels(&state->front, &state->front_capacity, count))
        return 0;
    state->raster_w = raster_w;
    state->raster_h = raster_h;
    state->front_w = raster_w;
    state->front_h = raster_h;
    state->front_valid = 0;
    state->frame_armed = 0;
    state->last_valid = 0;
    fill_black(state->build, count);
    fill_black(state->front, count);
    state->applied_generation = atomic_load_explicit(
        &state->requested_generation, memory_order_acquire);
    return 1;
}

static int apply_requested_geometry(syncpll_monitor_state_t *state)
{
    int ok;
    thread_wait_mutex(state->mutex);
    if (state->requested_w <= 0 || state->requested_h <= 0) {
        state->requested_x = 0;
        state->requested_y = 0;
        state->requested_w = 640;
        state->requested_h = 200;
        atomic_fetch_add_explicit(&state->requested_generation, 1u,
                                  memory_order_release);
    }
    ok = configure_raster_locked(state, state->requested_w,
                                 state->requested_h);
    thread_release_mutex(state->mutex);
    return ok;
}

static void update_h_step(syncpll_monitor_state_t *state)
{
    state->h_step_q32 = step_from_hz(state->h_osc_hz,
                                     state->sample_clock_hz);
}

static void update_v_step(syncpll_monitor_state_t *state)
{
    state->v_step_q32 = step_from_hz(state->v_osc_hz,
                                     state->sample_clock_hz);
}

static uint32_t phase_after_samples(uint32_t phase, uint32_t step,
                                    uint64_t samples)
{
    return phase + (uint32_t)((uint64_t)step * samples);
}

/*
 * Qualify a complete pulse before it is allowed to become the PLL's period
 * reference.  The previous implementation updated last_hsync_rise on every
 * rising edge, including rejected auxiliary CRTC pulses.  One phantom pulse
 * therefore poisoned both itself and the following real line sync.  Area 5150
 * deliberately creates such internal CRTC activity.
 */
static void horizontal_pulse_complete(syncpll_monitor_state_t *state)
{
    const uint64_t rise = state->hsync_assert_sample;
    const uint64_t width = state->sample_index - rise;
    const double width_s = state->sample_clock_hz > 0.0
                         ? (double)width / state->sample_clock_hz : 0.0;
    int width_valid = width_s >= 0.35e-6 && width_s <= 20.0e-6;

    /* Acquisition favours a genuine separator pulse over the one-character
     * pulses produced by auxiliary CRTC frames. Once locked, allow the normal
     * 80/40-column 2:1 width change but reject a sudden order-of-magnitude
     * runt pulse. */
    if (!state->seen_hsync)
        width_valid = width_valid && width_s >= 1.5e-6;
    else if (state->last_hsync_width != 0u)
        width_valid = width_valid &&
                      width * 4u >= state->last_hsync_width &&
                      width <= state->last_hsync_width * 4u;

    if (!width_valid) {
        state->h_lock *= 0.90;
        atomic_fetch_add_explicit(&state->rejected_hsync_edges, 1u,
                                  memory_order_relaxed);
        return;
    }

    if (!state->seen_hsync) {
        state->last_hsync_rise = rise;
        state->last_hsync_width = width;
        state->seen_hsync = 1u;
        state->h_phase_q32 = phase_after_samples(
            phase_from_double(SYNCPLL_HSYNC_PHASE), state->h_step_q32, width);
        state->last_valid = 0;
        state->h_lock = fmax(state->h_lock, 0.04);
        return;
    }

    {
        const uint64_t period_samples = rise - state->last_hsync_rise;
        const double measured_hz = period_samples != 0u
                                 ? state->sample_clock_hz
                                   / (double)period_samples : 0.0;
        const uint32_t expected_phase = phase_after_samples(
            phase_from_double(SYNCPLL_HSYNC_PHASE), state->h_step_q32, width);
        const double phase_error = wrap_signed_half(
            phase_to_double(expected_phase - state->h_phase_q32));
        const int valid = measured_hz >= 10000.0 && measured_hz <= 26000.0;

        if (valid) {
            double alpha;
            double filtered_hz;
            double residual;
            double quality;

            if (!(state->h_period_samples > 0.0))
                state->h_period_samples = (double)period_samples;
            else {
                alpha = state->h_lock < 0.35 ? 0.50 : 0.0625;
                state->h_period_samples +=
                    ((double)period_samples - state->h_period_samples) * alpha;
            }
            filtered_hz = state->sample_clock_hz / state->h_period_samples;
            filtered_hz = SYNCPLL_CLAMP(filtered_hz, 10000.0, 26000.0);
            residual = measured_hz - filtered_hz;
            state->h_osc_hz = filtered_hz;
            state->h_integrator_hz = filtered_hz - SYNCPLL_CGA_H_CENTER_HZ;
            update_h_step(state);

            /* Commit the corrected phase at the falling edge.  The pulse has
             * now been electrically qualified and the beam is still blanked. */
            state->h_phase_q32 = phase_after_samples(
                phase_from_double(SYNCPLL_HSYNC_PHASE), state->h_step_q32,
                width);
            state->last_hsync_rise = rise;
            state->last_hsync_width = width;
            state->last_valid = 0;

            quality = exp(-0.5 * (residual / 120.0)
                                 * (residual / 120.0));
            quality *= exp(-0.5 * (phase_error / 0.18)
                                 * (phase_error / 0.18));
            quality = fmax(quality, 0.72);
            state->h_lock += (quality - state->h_lock)
                           * (quality > state->h_lock ? 0.10 : 0.24);
        } else {
            /* Crucially, keep last_hsync_rise anchored to the last accepted
             * pulse.  The next genuine edge is then measured over one genuine
             * line instead of from this rejected auxiliary pulse. */
            state->h_integrator_hz *= 0.985;
            state->h_osc_hz += (SYNCPLL_CGA_H_CENTER_HZ - state->h_osc_hz)
                             * 0.02;
            state->h_lock *= 0.90;
            atomic_fetch_add_explicit(&state->rejected_hsync_edges, 1u,
                                      memory_order_relaxed);
            update_h_step(state);
        }
        state->h_lock = SYNCPLL_CLAMP(state->h_lock, 0.0, 1.0);
    }
}

static void horizontal_edge(syncpll_monitor_state_t *state, int asserted)
{
    if (asserted == !!state->signal_hsync)
        return;

    if (asserted)
        state->hsync_assert_sample = state->sample_index;
    else
        horizontal_pulse_complete(state);

    state->signal_hsync = (uint8_t)!!asserted;
}

static void vertical_pulse_complete(syncpll_monitor_state_t *state)
{
    const uint64_t rise = state->vsync_assert_sample;
    const uint64_t width = state->sample_index - rise;
    const double width_s = state->sample_clock_hz > 0.0
                         ? (double)width / state->sample_clock_hz : 0.0;
    int width_valid = width_s >= 20.0e-6 && width_s <= 5.0e-3;

    if (state->seen_vsync && state->last_vsync_width != 0u)
        width_valid = width_valid &&
                      width * 5u >= state->last_vsync_width &&
                      width <= state->last_vsync_width * 5u;

    if (!width_valid) {
        state->v_lock *= 0.70;
        atomic_fetch_add_explicit(&state->rejected_vsync_edges, 1u,
                                  memory_order_relaxed);
        return;
    }

    if (!state->seen_vsync) {
        state->last_vsync_rise = rise;
        state->last_vsync_width = width;
        state->seen_vsync = 1u;
        state->v_phase_q32 = phase_after_samples(
            phase_from_double(SYNCPLL_VSYNC_PHASE), state->v_step_q32, width);
        state->last_valid = 0;
        state->v_lock = fmax(state->v_lock, 0.04);
        return;
    }

    {
        const uint64_t period_samples = rise - state->last_vsync_rise;
        const double measured_hz = period_samples != 0u
                                 ? state->sample_clock_hz
                                   / (double)period_samples : 0.0;
        const uint32_t expected_phase = phase_after_samples(
            phase_from_double(SYNCPLL_VSYNC_PHASE), state->v_step_q32, width);
        const double phase_error = wrap_signed_half(
            phase_to_double(expected_phase - state->v_phase_q32));
        const int valid = measured_hz >= 35.0 && measured_hz <= 100.0;

        if (valid) {
            double alpha;
            double filtered_hz;
            double residual;
            double quality;

            if (!(state->v_period_samples > 0.0))
                state->v_period_samples = (double)period_samples;
            else {
                alpha = state->v_lock < 0.35 ? 0.50 : 0.125;
                state->v_period_samples +=
                    ((double)period_samples - state->v_period_samples) * alpha;
            }
            filtered_hz = state->sample_clock_hz / state->v_period_samples;
            filtered_hz = SYNCPLL_CLAMP(filtered_hz, 35.0, 100.0);
            residual = measured_hz - filtered_hz;
            state->v_osc_hz = filtered_hz;
            state->v_integrator_hz = filtered_hz - SYNCPLL_CGA_V_CENTER_HZ;
            update_v_step(state);
            state->v_phase_q32 = phase_after_samples(
                phase_from_double(SYNCPLL_VSYNC_PHASE), state->v_step_q32,
                width);
            state->last_vsync_rise = rise;
            state->last_vsync_width = width;
            state->last_valid = 0;

            quality = exp(-0.5 * (residual / 0.35)
                                 * (residual / 0.35));
            quality *= exp(-0.5 * (phase_error / 0.18)
                                 * (phase_error / 0.18));
            quality = fmax(quality, 0.72);
            state->v_lock += (quality - state->v_lock)
                           * (quality > state->v_lock ? 0.28 : 0.45);
        } else {
            state->v_integrator_hz *= 0.94;
            state->v_osc_hz += (SYNCPLL_CGA_V_CENTER_HZ - state->v_osc_hz)
                             * 0.08;
            state->v_lock *= 0.70;
            atomic_fetch_add_explicit(&state->rejected_vsync_edges, 1u,
                                      memory_order_relaxed);
            update_v_step(state);
        }
        state->v_lock = SYNCPLL_CLAMP(state->v_lock, 0.0, 1.0);
    }
}

static void vertical_edge(syncpll_monitor_state_t *state, int asserted)
{
    if (asserted == !!state->signal_vsync)
        return;

    if (asserted)
        state->vsync_assert_sample = state->sample_index;
    else
        vertical_pulse_complete(state);

    state->signal_vsync = (uint8_t)!!asserted;
}

static void decay_missing_sync(syncpll_monitor_state_t *state,
                               uint32_t sample_count)
{
    const double dt = state->sample_clock_hz > 0.0
                    ? (double)sample_count / state->sample_clock_hz : 0.0;
    const uint64_t h_timeout = (uint64_t)(state->sample_clock_hz
                               * 3.2 / SYNCPLL_CGA_H_CENTER_HZ);
    const uint64_t v_timeout = (uint64_t)(state->sample_clock_hz
                               * 1.7 / SYNCPLL_CGA_V_CENTER_HZ);

    if (!state->seen_hsync || state->sample_index - state->last_hsync_rise
                             > h_timeout) {
        const double a = dt > 0.0 ? 1.0 - exp(-dt / 0.080) : 0.0;
        if (state->seen_hsync
            && state->sample_index - state->last_hsync_rise > h_timeout) {
            state->seen_hsync = 0u;
            state->h_period_samples = 0.0;
        }
        state->h_osc_hz += (SYNCPLL_CGA_H_CENTER_HZ - state->h_osc_hz) * a;
        state->h_integrator_hz *= dt > 0.0 ? exp(-dt / 0.12) : 1.0;
        state->h_lock *= dt > 0.0 ? exp(-dt / 0.030) : 1.0;
        update_h_step(state);
    }
    if (!state->seen_vsync || state->sample_index - state->last_vsync_rise
                             > v_timeout) {
        const double a = dt > 0.0 ? 1.0 - exp(-dt / 0.180) : 0.0;
        if (state->seen_vsync
            && state->sample_index - state->last_vsync_rise > v_timeout) {
            state->seen_vsync = 0u;
            state->v_period_samples = 0.0;
        }
        state->v_osc_hz += (SYNCPLL_CGA_V_CENTER_HZ - state->v_osc_hz) * a;
        state->v_integrator_hz *= dt > 0.0 ? exp(-dt / 0.30) : 1.0;
        state->v_lock *= dt > 0.0 ? exp(-dt / 0.10) : 1.0;
        update_v_step(state);
    }
}

static void complete_monitor_scan(syncpll_monitor_state_t *state)
{
    const size_t count = (size_t)state->raster_w * (size_t)state->raster_h;

    state->last_valid = 0;
    if (!state->frame_armed) {
        fill_black(state->build, count);
        state->frame_armed = 1;
        return;
    }

    thread_wait_mutex(state->mutex);
    {
        uint32_t *tmp = state->front;
        size_t tmp_capacity = state->front_capacity;
        state->front = state->build;
        state->front_capacity = state->build_capacity;
        state->build = tmp;
        state->build_capacity = tmp_capacity;
        state->front_w = state->raster_w;
        state->front_h = state->raster_h;
        state->front_valid = 1;
        state->front_generation = state->applied_generation;
    }
    thread_release_mutex(state->mutex);
    fill_black(state->build, count);
    atomic_fetch_add_explicit(&state->completed_scans, 1u,
                              memory_order_relaxed);
}

static void store_raster_sample(syncpll_monitor_state_t *state,
                                int x, int y, uint32_t pixel)
{
    uint32_t *row;
    int fill_x;

    if ((unsigned)x >= (unsigned)state->raster_w
        || (unsigned)y >= (unsigned)state->raster_h) {
        state->last_valid = 0;
        return;
    }
    pixel |= 0xff000000u;
    row = state->build + (size_t)y * (size_t)state->raster_w;

    if (state->last_valid && y == state->last_y && x > state->last_x + 1
        && x - state->last_x <= 16) {
        for (fill_x = state->last_x + 1; fill_x < x; ++fill_x)
            row[fill_x] = state->last_pixel;
    }
    row[x] = pixel;
    state->last_x = x;
    state->last_y = y;
    state->last_pixel = pixel;
    state->last_valid = 1;
}

static void copy_scaled(uint32_t *const *dst_lines, int dst_x, int dst_y,
                        int dst_w, int dst_h, const uint32_t *src,
                        int src_w, int src_h)
{
    int y;
    if (dst_w == src_w && dst_h == src_h) {
        for (y = 0; y < dst_h; ++y)
            memcpy(dst_lines[dst_y + y] + dst_x,
                   src + (size_t)y * (size_t)src_w,
                   (size_t)src_w * sizeof(uint32_t));
        return;
    }

    for (y = 0; y < dst_h; ++y) {
        const int sy = (int)(((uint64_t)y * (uint64_t)src_h)
                           / (uint64_t)dst_h);
        uint32_t *dst = dst_lines[dst_y + y] + dst_x;
        int x;
        for (x = 0; x < dst_w; ++x) {
            const int sx = (int)(((uint64_t)x * (uint64_t)src_w)
                               / (uint64_t)dst_w);
            dst[x] = src[(size_t)sy * (size_t)src_w + (size_t)sx];
        }
    }
}

void syncpll_global_init(void)
{
    int expected = 0;
    int i;
    const char *env;

    if (!atomic_compare_exchange_strong(&syncpll_started, &expected, 1))
        return;
    memset(monitor_state, 0, sizeof(monitor_state));
    for (i = 0; i < SYNCPLL_MONITORS; ++i) {
        syncpll_monitor_state_t *state = &monitor_state[i];
        state->mutex = thread_create_mutex();
        state->requested_w = 640;
        state->requested_h = 200;
        atomic_init(&state->requested_generation, 1u);
        atomic_init(&state->live_active, 0);
        atomic_init(&state->submitted_samples, 0u);
        atomic_init(&state->completed_scans, 0u);
        atomic_init(&state->rejected_hsync_edges, 0u);
        atomic_init(&state->rejected_vsync_edges, 0u);
        reset_pll(state);
        if (state->mutex)
            apply_requested_geometry(state);
    }
    atomic_store(&syncpll_enabled, 1);
    env = getenv("86BOX_SYNC_PLL");
    if (env && (*env == '0' || *env == 'n' || *env == 'N'))
        atomic_store(&syncpll_enabled, 0);
}

void syncpll_global_shutdown(void)
{
    int i;
    if (!atomic_exchange(&syncpll_started, 0))
        return;
    for (i = 0; i < SYNCPLL_MONITORS; ++i) {
        syncpll_monitor_state_t *state = &monitor_state[i];
        free(state->build);
        free(state->front);
        if (state->mutex)
            thread_close_mutex(state->mutex);
        memset(state, 0, sizeof(*state));
    }
}

void syncpll_set_enabled(int enabled)
{
    atomic_store_explicit(&syncpll_enabled, !!enabled, memory_order_release);
}

int syncpll_get_enabled(void)
{
    return atomic_load_explicit(&syncpll_enabled, memory_order_acquire);
}

void syncpll_process_blit(int x, int y, int w, int h, int monitor_index)
{
    syncpll_monitor_state_t *state;
    bitmap_t *target;

    if (!syncpll_get_enabled() || monitor_index < 0
        || monitor_index >= SYNCPLL_MONITORS || x < 0 || y < 0
        || w < 64 || h < 64)
        return;
    state = &monitor_state[monitor_index];
    if (!state->mutex
        || !atomic_load_explicit(&state->live_active, memory_order_acquire))
        return;
    target = monitors[monitor_index].target_buffer;
    if (!target || x + w > target->w || y + h > target->h)
        return;

    thread_wait_mutex(state->mutex);
    if (state->requested_x != x || state->requested_y != y
        || state->requested_w != w || state->requested_h != h) {
        state->requested_x = x;
        state->requested_y = y;
        state->requested_w = w;
        state->requested_h = h;
        atomic_fetch_add_explicit(&state->requested_generation, 1u,
                                  memory_order_release);
        state->front_valid = 0;
    }

    if (atomic_load_explicit(&monitors[monitor_index].mon_dpms,
                             memory_order_relaxed)) {
        int row;
        for (row = 0; row < h; ++row)
            fill_black(target->line[y + row] + x, (size_t)w);
    } else if (state->front_valid && state->front
               && state->front_generation == atomic_load_explicit(
                    &state->requested_generation, memory_order_acquire)) {
        copy_scaled(target->line, x, y, w, h, state->front,
                    state->front_w, state->front_h);
    }
    thread_release_mutex(state->mutex);
}

void syncpll_cga_submit_line(int monitor_index,
                             const uint32_t *xrgb,
                             const uint8_t *flags,
                             uint32_t dot_count,
                             uint64_t dot_clock_millihz)
{
    syncpll_monitor_state_t *state;
    double sample_clock_hz;
    uint32_t aperture_x_start;
    uint32_t aperture_y_start;
    uint32_t aperture_x_span;
    uint32_t aperture_y_span;
    uint8_t previous_levels;
    uint32_t i;

    if (!syncpll_get_enabled() || monitor_index < 0
        || monitor_index >= SYNCPLL_MONITORS || !xrgb || !flags
        || dot_count == 0 || dot_clock_millihz == 0)
        return;
    state = &monitor_state[monitor_index];
    if (!state->mutex)
        return;
    atomic_store_explicit(&state->live_active, 1, memory_order_release);

    if (state->applied_generation != atomic_load_explicit(
            &state->requested_generation, memory_order_acquire)
        && !apply_requested_geometry(state))
        return;
    if (!state->build || state->raster_w <= 0 || state->raster_h <= 0)
        return;

    sample_clock_hz = (double)dot_clock_millihz * 0.001;
    if (fabs(sample_clock_hz - state->sample_clock_hz) > 0.5) {
        state->sample_clock_hz = sample_clock_hz;
        state->seen_hsync = 0u;
        state->seen_vsync = 0u;
        state->h_period_samples = 0.0;
        state->v_period_samples = 0.0;
        update_h_step(state);
        update_v_step(state);
    }

    if (state->raster_w >= 800) {
        aperture_x_start = phase_from_double(800.0 / 912.0);
        aperture_x_span = phase_from_double(832.0 / 912.0);
        aperture_y_start = phase_from_double(224.0 / 262.0);
        aperture_y_span = UINT32_MAX;
    } else {
        aperture_x_start = 0u;
        aperture_x_span = phase_from_double(640.0 / 912.0);
        aperture_y_start = 0u;
        aperture_y_span = phase_from_double(200.0 / 262.0);
    }

    previous_levels = (state->signal_hsync ? SYNCPLL_SIGNAL_HSYNC : 0u)
                    | (state->signal_vsync ? SYNCPLL_SIGNAL_VSYNC : 0u);

    for (i = 0; i < dot_count; ++i) {
        const uint8_t levels = flags[i]
                             & (SYNCPLL_SIGNAL_HSYNC | SYNCPLL_SIGNAL_VSYNC);
        int x = -1;
        int y = -1;

        if (levels != previous_levels) {
            if ((levels ^ previous_levels) & SYNCPLL_SIGNAL_HSYNC)
                horizontal_edge(state, !!(levels & SYNCPLL_SIGNAL_HSYNC));
            if ((levels ^ previous_levels) & SYNCPLL_SIGNAL_VSYNC)
                vertical_edge(state, !!(levels & SYNCPLL_SIGNAL_VSYNC));
            previous_levels = levels;
        }

        if (!levels) {
            const uint32_t rx = state->h_phase_q32 - aperture_x_start;
            const uint32_t ry = state->v_phase_q32 - aperture_y_start;
            if (rx < aperture_x_span)
                x = (int)(((uint64_t)rx * (uint64_t)state->raster_w)
                        / (uint64_t)aperture_x_span);
            if (aperture_y_span == UINT32_MAX || ry < aperture_y_span) {
                const uint64_t span = aperture_y_span == UINT32_MAX
                                    ? 0x100000000ULL
                                    : (uint64_t)aperture_y_span;
                y = (int)(((uint64_t)ry * (uint64_t)state->raster_h)
                        / span);
            }
            if (x >= 0 && y >= 0)
                store_raster_sample(state, x, y, xrgb[i]);
            else
                state->last_valid = 0;
        } else {
            state->last_valid = 0;
        }

        {
            const uint32_t old_v = state->v_phase_q32;
            state->h_phase_q32 += state->h_step_q32;
            state->v_phase_q32 += state->v_step_q32;
            state->sample_index++;
            if (state->v_phase_q32 < old_v)
                complete_monitor_scan(state);
        }
    }

    decay_missing_sync(state, dot_count);
    atomic_fetch_add_explicit(&state->submitted_samples, dot_count,
                              memory_order_relaxed);
}

void syncpll_cga_stream_reset(int monitor_index)
{
    syncpll_monitor_state_t *state;
    if (monitor_index < 0 || monitor_index >= SYNCPLL_MONITORS
        || !atomic_load_explicit(&syncpll_started, memory_order_acquire))
        return;
    state = &monitor_state[monitor_index];
    atomic_store_explicit(&state->live_active, 0, memory_order_release);
    if (!state->mutex)
        return;
    thread_wait_mutex(state->mutex);
    state->front_valid = 0;
    state->frame_armed = 0;
    state->last_valid = 0;
    if (state->build && state->raster_w > 0 && state->raster_h > 0)
        fill_black(state->build,
                   (size_t)state->raster_w * (size_t)state->raster_h);
    reset_pll(state);
    thread_release_mutex(state->mutex);
}

int syncpll_get_stats(int monitor_index, syncpll_stats_t *stats)
{
    syncpll_monitor_state_t *state;
    if (!stats || monitor_index < 0 || monitor_index >= SYNCPLL_MONITORS)
        return 0;
    state = &monitor_state[monitor_index];
    if (!state->mutex)
        return 0;
    memset(stats, 0, sizeof(*stats));
    stats->submitted_samples = atomic_load_explicit(
        &state->submitted_samples, memory_order_relaxed);
    stats->completed_scans = atomic_load_explicit(
        &state->completed_scans, memory_order_relaxed);
    stats->rejected_hsync_edges = atomic_load_explicit(
        &state->rejected_hsync_edges, memory_order_relaxed);
    stats->rejected_vsync_edges = atomic_load_explicit(
        &state->rejected_vsync_edges, memory_order_relaxed);
    stats->horizontal_hz = state->h_osc_hz;
    stats->vertical_hz = state->v_osc_hz;
    stats->horizontal_lock = state->h_lock;
    stats->vertical_lock = state->v_lock;
    stats->live_active = atomic_load_explicit(&state->live_active,
                                              memory_order_acquire);
    thread_wait_mutex(state->mutex);
    stats->frame_ready = state->front_valid;
    stats->presentation_w = state->requested_w;
    stats->presentation_h = state->requested_h;
    thread_release_mutex(state->mutex);
    return 1;
}
