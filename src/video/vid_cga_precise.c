/*
 * Character-clock IBM CGA adapter path for 86Box.
 *
 * This file deliberately keeps physical monitor timing (HSYNC edges) separate
 * from MC6845 internal line/frame boundaries.  That distinction is required by
 * effects which execute more than one CRTC frame inside one monitor scanline.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <86box/86box.h>
#include "cpu.h"
#include <86box/timer.h>
#include <86box/pit.h>
#include <86box/mem.h>
#include <86box/video.h>
#include <86box/vid_cga.h>
#include <86box/vid_cga_comp.h>

#define PRECISE_DOUBLE_NONE               0
#define PRECISE_DOUBLE_SIMPLE             1
#define PRECISE_DOUBLE_INTERPOLATE_SRGB   2
#define PRECISE_DOUBLE_INTERPOLATE_LINEAR 3

#define PRECISE_VRAM_MASK 0x3fff
#define PRECISE_MAX_X     2048
#define PRECISE_MAX_Y     1024

/*
 * Keep the complete 912-dot signal period in the monitor raster and anchor X=0
 * to the accepted HSYNC rising edge.  With standard IBM CGA programming, both
 * 40- and 80-column modes then begin active video at X=192:
 *
 *   80-column: (R0 - R2 + 1) * 8  = (113 - 90 + 1) * 8  = 192
 *   40-column: (R0 - R2 + 1) * 16 = (56  - 45 + 1) * 16 = 192
 *
 * This removes the mode-dependent 32/112-dot crop origin which caused the
 * picture to wrap or expose an 80-pixel strip after transient clock changes.
 */
#define PRECISE_TOTAL_DOTS            912
#define PRECISE_RASTER_WIDTH          PRECISE_TOTAL_DOTS
#define PRECISE_RASTER_HEIGHT         262
#define PRECISE_ACTIVE_X              192
#define PRECISE_ACTIVE_Y              38
#define PRECISE_ACTIVE_WIDTH          640
#define PRECISE_ACTIVE_HEIGHT         200
#define PRECISE_MONITOR_VSYNC_MIN_Y   127
#define PRECISE_CARD_VSYNC_LINES      3
#define PRECISE_HSYNC_WINDOW_DIVISOR  8
#define PRECISE_HSYNC_RELOCK_LINES     4
#define PRECISE_HSYNC_RELOCK_MIN_TOL   16

static int
cga_precise_buffer_width(void)
{
    if (!buffer32 || (buffer32->w <= 0))
        return 0;
    return (buffer32->w < PRECISE_MAX_X) ? buffer32->w : PRECISE_MAX_X;
}

static int
cga_precise_buffer_height(void)
{
    if (!buffer32 || (buffer32->h <= 0))
        return 0;
    return (buffer32->h < PRECISE_MAX_Y) ? buffer32->h : PRECISE_MAX_Y;
}

static int
cga_precise_raster_width(void)
{
    const int width = cga_precise_buffer_width();
    return (width < PRECISE_RASTER_WIDTH) ? width : PRECISE_RASTER_WIDTH;
}

static int
cga_precise_raster_height(const cga_t *cga)
{
    int height = cga_precise_buffer_height();

    if (cga->double_type != PRECISE_DOUBLE_NONE)
        height >>= 1;
    return (height < PRECISE_RASTER_HEIGHT) ? height : PRECISE_RASTER_HEIGHT;
}

static void cga_precise_reset_hsync_candidate(cga_t *cga);

static int
cga_precise_mode_is_graphics(uint8_t mode)
{
    /* GRAPHICS+80-column remains on the text data path on IBM CGA. */
    return (mode & CGA_MODE_FLAG_GRAPHICS) &&
           !(mode & CGA_MODE_FLAG_HIGHRES);
}

static void
cga_precise_request_clock(cga_t *cga)
{
    const uint8_t want_high =
        !!(cga->precise_mode & CGA_MODE_FLAG_HIGHRES);

    if (want_high != cga->precise_clock_high) {
        cga->precise_clock_pending = 1;
        cga_precise_reset_hsync_candidate(cga);
    }
}

static void
cga_precise_apply_pending_mode(cga_t *cga)
{
    if (!cga->precise_mode_pending)
        return;

    cga->precise_mode = cga->precise_pending_mode;
    cga->precise_mode_pending = 0;
    cga_precise_request_clock(cga);
}

static void
cga_precise_apply_clock_if_due(cga_t *cga)
{
    if (!cga->precise_clock_pending ||
        (cga->precise_master_phase & 0x0f))
        return;

    cga->precise_clock_high =
        !!(cga->precise_mode & CGA_MODE_FLAG_HIGHRES);
    cga->precise_char_time = cga->precise_clock_high ?
                             CGACONST : (CGACONST << 1);
    cga->precise_clock_pending = 0;
}

void
cga_precise_mode_write(cga_t *cga, uint8_t mode)
{
    const uint8_t mux_mask =
        CGA_MODE_FLAG_HIGHRES | CGA_MODE_FLAG_GRAPHICS;
    const int old_graphics =
        cga_precise_mode_is_graphics(cga->precise_mode);
    const int new_graphics = cga_precise_mode_is_graphics(mode);

    /* The text/graphics mux is latched at HSYNC.  Non-mux bits remain live. */
    if (old_graphics != new_graphics) {
        cga->precise_pending_mode = mode;
        cga->precise_mode_pending = 1;
        cga->precise_mode = (cga->precise_mode & mux_mask) |
                            (mode & (uint8_t) ~mux_mask);
        return;
    }

    /* A later write supersedes a deferred transition which has not latched. */
    cga->precise_mode_pending = 0;
    cga->precise_mode = mode;
    cga->precise_pending_mode = mode;
    cga_precise_request_clock(cga);
}

static int
cga_precise_char_pixels(const cga_t *cga)
{
    return cga->precise_clock_high ? 8 : 16;
}

static int
cga_precise_frame_geometry_plausible(const cga_t *cga)
{
    const int char_pixels = cga_precise_char_pixels(cga);
    const int total_dots =
        ((int) cga->crtc[MC6845_R0_HTOTAL] + 1) * char_pixels;
    const int total_lines =
        (((int) cga->crtc[MC6845_R4_VTOTAL] + 1) *
         ((int) cga->crtc[MC6845_R9_MAX_RASTER] + 1)) +
        (int) cga->crtc[MC6845_R5_VTOTAL_ADJUST];
    const int displayed_dots =
        (int) cga->crtc[MC6845_R1_HDISPLAYED] * char_pixels;
    const int displayed_lines =
        (int) cga->crtc[MC6845_R6_VDISPLAYED] *
        ((int) cga->crtc[MC6845_R9_MAX_RASTER] + 1);

    return (total_dots >= 880) && (total_dots <= 960) &&
           (total_lines >= 240) && (total_lines <= 280) &&
           (displayed_dots >= 600) && (displayed_dots <= 768) &&
           (displayed_lines >= 190) && (displayed_lines <= 208);
}

static void
cga_precise_reset_hsync_candidate(cga_t *cga)
{
    cga->precise_hsync_candidate_x = -1;
    cga->precise_hsync_candidate_count = 0;
}

static int
cga_precise_hsync_phase_distance(int a, int b)
{
    int distance = a - b;

    if (distance < 0)
        distance = -distance;
    if (distance > (PRECISE_TOTAL_DOTS >> 1))
        distance = PRECISE_TOTAL_DOTS - distance;
    return distance;
}

static int
cga_precise_hsync_qualifies(cga_t *cga)
{
    int window;
    int earliest;
    int tolerance;
    const int active_end = PRECISE_ACTIVE_X + PRECISE_ACTIVE_WIDTH;

    /*
     * Never lock the monitor to one-character auxiliary CRTC frames.  The
     * geometry test is deliberately applied to both initial acquisition and
     * re-acquisition.
     */
    if (!cga_precise_frame_geometry_plausible(cga)) {
        cga_precise_reset_hsync_candidate(cga);
        return 0;
    }

    if (!cga->precise_monitor_locked) {
        cga_precise_reset_hsync_candidate(cga);
        return 1;
    }

    window = PRECISE_TOTAL_DOTS / PRECISE_HSYNC_WINDOW_DIVISOR;
    if (window < (cga_precise_char_pixels(cga) << 1))
        window = cga_precise_char_pixels(cga) << 1;

    earliest = PRECISE_TOTAL_DOTS - window;
    if (earliest < active_end)
        earliest = active_end;

    if ((cga->precise_beam_x >= earliest) &&
        (cga->precise_beam_x <= PRECISE_TOTAL_DOTS + window)) {
        cga_precise_reset_hsync_candidate(cga);
        return 1;
    }

    /*
     * A 40/80-column transition can move the raw HSYNC edge hundreds of dots
     * away from the monitor's old phase while retaining the correct 912-dot
     * period.  The old code rejected that edge forever, leaving a stable
     * half-screen wrap.  A real horizontal-hold loop instead re-acquires a
     * persistent off-phase pulse.  Require the same off-phase edge for several
     * complete CGA-like lines before moving the raster origin; brief demo
     * pulses therefore remain ignored.
     */
    tolerance = cga_precise_char_pixels(cga) << 1;
    if (tolerance < PRECISE_HSYNC_RELOCK_MIN_TOL)
        tolerance = PRECISE_HSYNC_RELOCK_MIN_TOL;

    if ((cga->precise_hsync_candidate_x >= 0) &&
        (cga_precise_hsync_phase_distance(cga->precise_beam_x,
                                          cga->precise_hsync_candidate_x) <= tolerance)) {
        if (cga->precise_hsync_candidate_count < 255)
            cga->precise_hsync_candidate_count++;
    } else {
        cga->precise_hsync_candidate_x = cga->precise_beam_x;
        cga->precise_hsync_candidate_count = 1;
    }

    if (cga->precise_hsync_candidate_count >= PRECISE_HSYNC_RELOCK_LINES) {
        cga_precise_reset_hsync_candidate(cga);
        return 1;
    }

    return 0;
}

static int
cga_precise_border(const cga_t *cga)
{
    const int highres_graphics = CGA_MODE_FLAG_HIGHRES_GRAPHICS | CGA_MODE_FLAG_GRAPHICS;
    return ((cga->precise_mode & highres_graphics) == highres_graphics) ? 0 : ((cga->cgacol & 15) + 16);
}

static int
cga_precise_output_y(const cga_t *cga, int physical_y)
{
    return (cga->double_type == PRECISE_DOUBLE_NONE) ? physical_y : (physical_y << 1);
}

static void
cga_precise_put(cga_t *cga, int x, int physical_y, int color)
{
    int y;

    const int buffer_width = cga_precise_raster_width();
    const int physical_height = cga_precise_raster_height(cga);
    const int buffer_height = cga_precise_buffer_height();

    if ((buffer_width <= 0) || (physical_height <= 0) || (buffer_height <= 0) ||
        (x < 0) || (x >= buffer_width) ||
        (physical_y < 0) || (physical_y >= physical_height))
        return;

    y = cga_precise_output_y(cga, physical_y);
    if ((y < 0) || (y >= buffer_height))
        return;

    buffer32->line[y][x] = color;

    if (cga->double_type == PRECISE_DOUBLE_SIMPLE) {
        if ((y + 1) < buffer_height)
            buffer32->line[y + 1][x] = color;
    } else if (cga->double_type > PRECISE_DOUBLE_SIMPLE) {
        /* Preserve the existing scanline/interpolation contract: the second
         * line begins black and cga_do_blit() performs interpolation later. */
        if ((y + 1) < buffer_height)
            buffer32->line[y + 1][x] = 0;
    }
}

static void
cga_precise_fill_cell(cga_t *cga, int x, int y, int width, int color)
{
    for (int i = 0; i < width; i++)
        cga_precise_put(cga, x + i, y, color);
}

static void
cga_precise_note_active(cga_t *cga, int x, int y, int width)
{
    const int buffer_width = cga_precise_raster_width();
    const int buffer_height = cga_precise_raster_height(cga);
    int right = x + width;

    if ((buffer_width <= 0) || (buffer_height <= 0) ||
        (x < 0) || (x >= buffer_width) || (y < 0) || (y >= buffer_height))
        return;
    if (right > buffer_width)
        right = buffer_width;

    if (cga->precise_active_min_y == PRECISE_MAX_Y)
        cga->precise_frame_hires = cga->precise_clock_high;

    if (x < cga->precise_active_min_x)
        cga->precise_active_min_x = x;
    if (right > cga->precise_active_max_x)
        cga->precise_active_max_x = right;
    if (y < cga->precise_active_min_y)
        cga->precise_active_min_y = y;
    if ((y + 1) > cga->precise_active_max_y)
        cga->precise_active_max_y = y + 1;
}

static void
cga_precise_render_cell(cga_t *cga, const mc6845_outputs_t *out)
{
    const int x = cga->precise_beam_x;
    const int y = cga->precise_beam_y;
    const int width = cga_precise_char_pixels(cga);
    const int border = cga_precise_border(cga);
    const uint16_t ma = out->ma & PRECISE_VRAM_MASK;
    const uint8_t ra = out->ra;
    uint8_t chr;
    uint8_t attr;
    uint16_t dat;
    int cols[4];

    if (!buffer32 || !cga->vram ||
        (x < 0) || (x >= cga_precise_raster_width()) ||
        (y < 0) || (y >= cga_precise_raster_height(cga)))
        return;

    /* Rejected/raw sync pulses still blank video, but they do not alter
     * monitor phase.  The three-line CGA-gated VSYNC is blank, not overscan. */
    if (out->hsync || cga->precise_card_vsync) {
        cga_precise_fill_cell(cga, x, y, width, 0);
        return;
    }

    if (!out->de) {
        cga_precise_fill_cell(cga, x, y, width, border);
        return;
    }

    cga_precise_note_active(cga, x, y, width);

    /* -VIDEO disables VRAM output, not display timing.  In text modes the
     * active area is character/attribute zero (black), while graphics modes
     * show their zero-data background.  Overscan remains Color Select. */
    if (!(cga->precise_mode & CGA_MODE_FLAG_VIDEO_ENABLE)) {
        int blank;

        if (!(cga->precise_mode & CGA_MODE_FLAG_GRAPHICS))
            blank = 16;
        else if (cga->precise_mode & CGA_MODE_FLAG_HIGHRES_GRAPHICS)
            blank = 0;
        else
            blank = (cga->cgacol & 15) | 16;
        cga_precise_fill_cell(cga, x, y, width, blank);
        return;
    }

    if (cga->precise_mode & CGA_MODE_FLAG_HIGHRES) {
        /* The CGA selects the 80-column text data path whenever the 80-column
         * clock bit is set, including otherwise nonsensical mixed mode words. */
        chr = cga->vram[(ma << 1) & PRECISE_VRAM_MASK];
        attr = cga->vram[((ma << 1) + 1) & PRECISE_VRAM_MASK];
        cols[1] = (attr & 15) + 16;
        if (cga->precise_mode & CGA_MODE_FLAG_BLINK) {
            cols[0] = ((attr >> 4) & 7) + 16;
            if ((cga->cgablink & 8) && (attr & 0x80) && !out->cursor)
                cols[1] = cols[0];
        } else {
            cols[0] = (attr >> 4) + 16;
        }
        for (int bit = 0; bit < 8; bit++) {
            int color = cols[(fontdat[chr + cga->fontbase][ra & 7] & (1 << (bit ^ 7))) ? 1 : 0];
            if (out->cursor)
                color ^= 15;
            cga_precise_put(cga, x + bit, y, color);
        }
        return;
    }

    if (!(cga->precise_mode & CGA_MODE_FLAG_GRAPHICS)) {
        /* 40-column text: each character dot is doubled. */
        chr = cga->vram[(ma << 1) & PRECISE_VRAM_MASK];
        attr = cga->vram[((ma << 1) + 1) & PRECISE_VRAM_MASK];
        cols[1] = (attr & 15) + 16;
        if (cga->precise_mode & CGA_MODE_FLAG_BLINK) {
            cols[0] = ((attr >> 4) & 7) + 16;
            if ((cga->cgablink & 8) && (attr & 0x80) && !out->cursor)
                cols[1] = cols[0];
        } else {
            cols[0] = (attr >> 4) + 16;
        }
        for (int bit = 0; bit < 8; bit++) {
            int color = cols[(fontdat[chr + cga->fontbase][ra & 7] & (1 << (bit ^ 7))) ? 1 : 0];
            if (out->cursor)
                color ^= 15;
            cga_precise_put(cga, x + (bit << 1), y, color);
            cga_precise_put(cga, x + (bit << 1) + 1, y, color);
        }
        return;
    }

    dat = (uint16_t) ((cga->vram[((ma << 1) & 0x1fff) + ((ra & 1) * 0x2000)] << 8) |
                      cga->vram[(((ma << 1) & 0x1fff) + ((ra & 1) * 0x2000) + 1) & PRECISE_VRAM_MASK]);

    if (cga->precise_mode & CGA_MODE_FLAG_HIGHRES_GRAPHICS) {
        /* 640x200 two-colour graphics: two bytes / sixteen pixels per CRTC
         * address at the 40-column character clock. */
        cols[0] = 0;
        cols[1] = (cga->cgacol & 15) + 16;
        for (int pix = 0; pix < 16; pix++) {
            cga_precise_put(cga, x + pix, y, cols[dat >> 15]);
            dat <<= 1;
        }
        return;
    }

    /* 320x200 four-colour graphics. */
    cols[0] = (cga->cgacol & 15) | 16;
    {
        const int base = (cga->cgacol & 16) ? 24 : 16;
        if (cga->precise_mode & CGA_MODE_FLAG_BW) {
            cols[1] = base | 3;
            cols[2] = base | 4;
            cols[3] = base | 7;
        } else if (cga->cgacol & 32) {
            cols[1] = base | 3;
            cols[2] = base | 5;
            cols[3] = base | 7;
        } else {
            cols[1] = base | 2;
            cols[2] = base | 4;
            cols[3] = base | 6;
        }
    }
    for (int pix = 0; pix < 8; pix++) {
        const int color = cols[dat >> 14];
        cga_precise_put(cga, x + (pix << 1), y, color);
        cga_precise_put(cga, x + (pix << 1) + 1, y, color);
        dat <<= 2;
    }
}

static void
cga_precise_process_physical_line(cga_t *cga, int physical_y,
                                  int drawn_width)
{
    const int width = cga_precise_raster_width();
    const int physical_height = cga_precise_raster_height(cga);
    const int y = cga_precise_output_y(cga, physical_y);
    const int border = cga_precise_border(cga) & 15;
    const int buffer_height = cga_precise_buffer_height();

    if ((width <= 0) || (physical_height <= 0) ||
        (physical_y < 0) || (physical_y >= physical_height) ||
        (y < 0) || (y >= buffer_height))
        return;

    if (drawn_width < 0)
        drawn_width = 0;
    if (drawn_width > width)
        drawn_width = width;

    /* Pixels which were never scanned are retrace blank, never fabricated
     * overscan.  This removes the random right-edge border on early sync. */
    if (drawn_width < width)
        cga_precise_fill_cell(cga, drawn_width, physical_y,
                              width - drawn_width, 0);

    if (cga->composite)
        Composite_Process(cga->precise_mode, border,
                          (width + 3) >> 2, buffer32->line[y]);
    else
        video_process_8(width, y);

    if (cga->double_type == PRECISE_DOUBLE_SIMPLE) {
        if ((y + 1) < buffer_height) {
            if (cga->composite)
                Composite_Process(cga->precise_mode, border,
                                  (width + 3) >> 2,
                                  buffer32->line[y + 1]);
            else
                video_process_8(width, y + 1);
        }
    } else if (cga->double_type > PRECISE_DOUBLE_SIMPLE) {
        if ((y + 1) < buffer_height)
            video_process_8(width, y + 1);
    }

    cga->precise_max_x = width;
    if ((physical_y + 1) > cga->precise_max_y)
        cga->precise_max_y = physical_y + 1;
}

static void
cga_precise_process_line(cga_t *cga)
{
    cga_precise_process_physical_line(cga, cga->precise_beam_y,
                                      cga->precise_beam_x);
}

static void
cga_precise_reset_frame_tracking(cga_t *cga)
{
    cga->precise_max_x = 0;
    cga->precise_max_y = 0;
    cga->precise_active_min_x = PRECISE_MAX_X;
    cga->precise_active_min_y = PRECISE_MAX_Y;
    cga->precise_active_max_x = 0;
    cga->precise_active_max_y = 0;
}

static void
cga_precise_present_frame(cga_t *cga)
{
    int src_x;
    int src_y;
    int width;
    int height;
    const int raster_width = cga_precise_raster_width();
    const int raster_height = cga_precise_raster_height(cga);
    const int buffer_width = cga_precise_buffer_width();
    const int buffer_height = cga_precise_buffer_height();

    if ((raster_width <= 0) || (raster_height <= 0) ||
        (buffer_width <= 0) || (buffer_height <= 0)) {
        cga->precise_frame_valid = 0;
        cga_precise_reset_frame_tracking(cga);
        return;
    }

    /* A qualified frame boundary is always committed at a physical line
     * boundary.  Complete only genuinely unscanned lines, using retrace black. */
    for (int line = cga->precise_max_y; line < raster_height; line++) {
        cga_precise_fill_cell(cga, 0, line, raster_width, 0);
        cga_precise_process_physical_line(cga, line, raster_width);
    }

    if (enable_overscan) {
        /*
         * Present a fixed 832-dot monitor aperture.  Keeping this independent
         * of the current character clock prevents a transient 40-column mode
         * from moving the host crop by 80 pixels.
         */
        src_x = PRECISE_TOTAL_DOTS - 832;
        src_y = 0;
        width = 832;
        height = raster_height;
    } else {
        src_x = PRECISE_ACTIVE_X;
        src_y = PRECISE_ACTIVE_Y;
        width = PRECISE_ACTIVE_WIDTH;
        height = PRECISE_ACTIVE_HEIGHT;

        if (src_x >= raster_width)
            width = 0;
        else if (width > raster_width - src_x)
            width = raster_width - src_x;
        if (src_y >= raster_height)
            height = 0;
        else if (height > raster_height - src_y)
            height = raster_height - src_y;
    }

    if (cga->double_type != PRECISE_DOUBLE_NONE) {
        src_y <<= 1;
        height <<= 1;
    }

    if (src_x < 0)
        src_x = 0;
    if (src_y < 0)
        src_y = 0;
    if (src_x >= buffer_width)
        width = 0;
    else if (width > buffer_width - src_x)
        width = buffer_width - src_x;
    if (src_y >= buffer_height)
        height = 0;
    else if (height > buffer_height - src_y)
        height = buffer_height - src_y;

    if ((width > 0) && (height > 0) && cga->precise_frame_valid) {
        if ((width != xsize) || (height != ysize) ||
            video_force_resize_get()) {
            xsize = width;
            ysize = height;
            set_screen_size(xsize, ysize);
            video_force_resize_set(0);
        }

        video_wait_for_buffer();
        if (cga->double_type > PRECISE_DOUBLE_SIMPLE)
            cga_blit_memtoscreen(src_x, src_y, width, height,
                                 cga->double_type);
        else
            video_blit_memtoscreen(src_x, src_y, width, height);

        frames++;
        video_res_x = width;
        video_res_y = height;
        video_bpp = (cga->precise_mode & CGA_MODE_FLAG_GRAPHICS) ?
                    ((cga->precise_mode &
                      CGA_MODE_FLAG_HIGHRES_GRAPHICS) ? 1 : 2) : 0;
    }

    cga->precise_frame_valid = 1;
    cga->cgablink++;
    cga->oddeven ^= 1;
    cga_precise_reset_frame_tracking(cga);
}

/* Perform vertical flyback at the accepted VSYNC phase.  Horizontal
 * deflection continues, so X is preserved instead of being snapped to zero. */
static void
cga_precise_vertical_flyback(cga_t *cga)
{
    const int raster_width = cga_precise_raster_width();
    const int raster_height = cga_precise_raster_height(cga);
    int phase = cga->precise_beam_x;
    int old_y = cga->precise_beam_y;

    if ((raster_width <= 0) || (raster_height <= 0))
        return;

    if (phase < 0)
        phase = 0;
    if (phase > raster_width)
        phase = raster_width;
    if (old_y < 0)
        old_y = 0;
    if (old_y >= raster_height)
        old_y = raster_height - 1;

    /*
     * If VSYNC begins between horizontal boundaries, commit the scanned prefix
     * of the old field.  If an accepted HSYNC edge has just reset X to zero,
     * there is no new line content to commit.
     */
    if (phase > 0)
        cga_precise_process_physical_line(cga, old_y, phase);

    cga_precise_present_frame(cga);

    cga->precise_beam_y = 0;
    cga->precise_beam_x = phase;
    cga->displine = 0;

    /* The new field begins at the same horizontal phase. */
    if (phase > 0)
        cga_precise_fill_cell(cga, 0, 0, phase, 0);
}

/*
 * Commit one physical monitor line.  Horizontal phase is anchored to the
 * accepted HSYNC rising edge, not to the falling edge or programmed pulse
 * width.  That gives both standard CGA dot-clock modes the same active origin.
 */
static void
cga_precise_finish_monitor_line(cga_t *cga)
{
    cga_precise_process_line(cga);

    cga->precise_beam_x = 0;
    cga->precise_beam_y++;
    cga->displine = cga->precise_beam_y;
    cga->precise_hblank_pixels = 0;
    cga->precise_hblank_target = 0;

    if (cga->precise_card_vsync) {
        if (cga->precise_card_vsync_lines < 255)
            cga->precise_card_vsync_lines++;
        if (cga->precise_card_vsync_lines >= PRECISE_CARD_VSYNC_LINES) {
            cga->precise_card_vsync = 0;
            cga->precise_card_vsync_lines = 0;
        }
    }

    if (cga->precise_beam_y >= PRECISE_RASTER_HEIGHT) {
        cga_precise_present_frame(cga);
        cga->precise_beam_y = 0;
        cga->displine = 0;
    }
}

void
cga_precise_init(cga_t *cga)
{
    mc6845_core_init(&cga->precise_crtc, cga->crtc);
    cga->precise_enabled = 1;
    cga->precise_mode = cga->cgamode;
    cga->precise_pending_mode = cga->cgamode;
    cga->precise_mode_pending = 0;
    cga->precise_clock_high =
        !!(cga->cgamode & CGA_MODE_FLAG_HIGHRES);
    cga->precise_clock_pending = 0;
    cga->precise_master_phase = 0;
    cga->precise_char_time = cga->precise_clock_high ?
                             CGACONST : (CGACONST << 1);
    cga->precise_beam_x = 0;
    cga->precise_beam_y = 0;
    cga->precise_prev_hsync = 0;
    cga->precise_prev_vsync = 0;
    cga->precise_frame_valid = 0;
    cga->precise_hblank_pixels = 0;
    cga->precise_hblank_target = 0;
    cga->precise_vsync_dots = 0;
    cga->precise_monitor_hblank = 0;
    cga->precise_monitor_vsync = 0;
    cga->precise_frame_hires = cga->precise_clock_high;
    cga->precise_monitor_locked = 0;
    cga_precise_reset_hsync_candidate(cga);
    cga->precise_sync_pending = 0;
    cga->precise_card_vsync = 0;
    cga->precise_card_vsync_lines = 0;
    cga_precise_reset_frame_tracking(cga);
}

void
cga_poll_precise(void *priv)
{
    cga_t *cga = (cga_t *) priv;
    mc6845_outputs_t before;
    mc6845_outputs_t after;
    uint64_t char_time;
    int cell_width;
    int line_finished = 0;

    cga_precise_apply_clock_if_due(cga);
    char_time = cga->precise_char_time;
    cell_width = cga_precise_char_pixels(cga);

    if (!char_time)
        char_time = cga->precise_clock_high ?
                    CGACONST : (CGACONST << 1);
    timer_advance_u64(&cga->timer, char_time);

    mc6845_core_get_outputs(&cga->precise_crtc, &before);

    if (before.de)
        cga->cgastat &= ~1;
    else
        cga->cgastat |= 1;
    if (cga->precise_card_vsync)
        cga->cgastat |= 8;
    else
        cga->cgastat &= ~8;

    /*
     * Draw every character clock in the complete 912-dot signal period.
     * HSYNC and the CGA-gated VSYNC are explicitly black in render_cell();
     * they still consume horizontal phase and therefore cannot compress or
     * offset the following active display.
     */
    {
        const int beam_limit = cga_precise_raster_width();

        if ((beam_limit > 0) && (cga->precise_beam_x < beam_limit)) {
            cga_precise_render_cell(cga, &before);
            if (cell_width >= beam_limit - cga->precise_beam_x)
                cga->precise_beam_x = beam_limit;
            else
                cga->precise_beam_x += cell_width;
        }
    }

    mc6845_core_tick(&cga->precise_crtc);
    mc6845_core_get_outputs(&cga->precise_crtc, &after);

    /*
     * Horizontal monitor phase is taken from the HSYNC leading edge.  Apply
     * the card's mode-mux latch at every raw edge, but reset the physical beam
     * only when the edge is in the monitor acceptance window.
     */
    if (!before.hsync && after.hsync) {
        cga_precise_apply_pending_mode(cga);

        if (cga_precise_hsync_qualifies(cga)) {
            cga->precise_monitor_locked = 1;
            cga->precise_monitor_hblank = 1;
            cga_precise_finish_monitor_line(cga);
            line_finished = 1;
        }
    } else if (before.hsync && !after.hsync) {
        cga->precise_monitor_hblank = 0;
    }

    /*
     * Missing or rejected HSYNC cannot stall the raster.  The oscillator
     * free-runs at 912 master dots.  Do this after edge handling so a valid
     * edge at the expected phase cannot finish the same line twice.
     */
    if (!line_finished && (cga->precise_beam_x >= PRECISE_TOTAL_DOTS)) {
        cga->precise_monitor_hblank = 0;
        cga_precise_finish_monitor_line(cga);
        line_finished = 1;
    }

    /*
     * Handle VSYNC after horizontal edge processing.  Coincident H/V edges
     * therefore commit the old scanline first and start the new field at X=0,
     * while a midline VSYNC preserves its genuine horizontal phase.
     */
    if (!before.vsync && after.vsync) {
        cga->precise_card_vsync = 1;
        cga->precise_card_vsync_lines = 0;
        cga->precise_monitor_vsync = 1;

        if ((cga->precise_beam_y >= PRECISE_MONITOR_VSYNC_MIN_Y) &&
            (cga->precise_monitor_locked ||
             cga_precise_frame_geometry_plausible(cga))) {
            cga->precise_monitor_locked = 1;
            cga->precise_sync_pending = 0;
            cga_precise_vertical_flyback(cga);
        }
    } else if (before.vsync && !after.vsync) {
        cga->precise_monitor_vsync = 0;
    }

    /* Keep legacy debugger-visible fields coherent. */
    cga->scanline = after.ra;
    cga->vc = after.vcc;
    cga->memaddr = after.ma;
    cga->memaddr_backup = cga->precise_crtc.vma_row;
    cga->cgadispon = after.de;
    cga->cursorvisible = after.cursor;

    cga->precise_master_phase =
        (cga->precise_master_phase + (uint32_t) cell_width) & 0x0f;
    cga->precise_prev_hsync = after.hsync;
    cga->precise_prev_vsync = after.vsync;
}
