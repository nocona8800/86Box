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
 * The monitor raster is not resized by CRTC programming.  Standard CGA has
 * 912 dot clocks and 262 scanlines per frame; the 80-dot HSYNC interval is not
 * visible, leaving an 832x262 raster window.  Area 5150 deliberately moves and
 * resizes the active display inside this fixed window.
 */
#define PRECISE_TOTAL_DOTS            912
#define PRECISE_RASTER_WIDTH          832
#define PRECISE_RASTER_HEIGHT         262
#define PRECISE_VISIBLE_WIDTH_HIRES   832
#define PRECISE_VISIBLE_WIDTH_LORES   752
#define PRECISE_ACTIVE_X_HIRES        112
#define PRECISE_ACTIVE_X_LORES        32
#define PRECISE_ACTIVE_Y              38
#define PRECISE_ACTIVE_WIDTH          640
#define PRECISE_ACTIVE_HEIGHT         200
#define PRECISE_MONITOR_VSYNC_MIN_Y   127
#define PRECISE_CARD_VSYNC_LINES      3

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

static int
cga_precise_mode_is_graphics(uint8_t mode)
{
    /* The undocumented GRAPHICS+80-column combination remains on the text
     * data path.  This matches the CGA glue logic used by Area 5150. */
    return (mode & CGA_MODE_FLAG_GRAPHICS) && !(mode & CGA_MODE_FLAG_HIGHRES);
}

static void
cga_precise_request_clock(cga_t *cga)
{
    const uint8_t want_high = !!(cga->precise_mode & CGA_MODE_FLAG_HIGHRES);

    if (want_high != cga->precise_clock_high)
        cga->precise_clock_pending = 1;
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
    if (!cga->precise_clock_pending || (cga->precise_master_phase & 0x0f))
        return;

    cga->precise_clock_high = !!(cga->precise_mode & CGA_MODE_FLAG_HIGHRES);
    cga->precise_char_time = cga->precise_clock_high ? CGACONST : (CGACONST << 1);
    cga->precise_clock_pending = 0;
}

void
cga_precise_mode_write(cga_t *cga, uint8_t mode)
{
    const int old_graphics = cga_precise_mode_is_graphics(cga->precise_mode);
    const int new_graphics = cga_precise_mode_is_graphics(mode);

    /* The text/graphics mux is latched at horizontal sync.  Applying it at the
     * CPU OUT callback caused Tetra3d's graphics window to switch partway
     * through the wrong physical line.  Other bits, including -VIDEO and
     * palette interpretation, remain live. */
    if (old_graphics != new_graphics) {
        cga->precise_pending_mode = mode;
        cga->precise_mode_pending = 1;
        return;
    }

    /* A later write supersedes any deferred transition that has not yet
     * reached HSYNC.  Without this, a text->graphics->text sequence inside
     * one line can resurrect the stale graphics request at the next sync. */
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

/*
 * Beam X is measured from the falling edge of HSYNC.  Standard 80-column
 * timing leaves fourteen 8-dot character clocks before HCC wraps to zero,
 * so display enable begins at X=112.  Standard 40-column timing leaves only
 * two 16-dot character clocks, so display enable begins at X=32.
 *
 * These are fixed monitor apertures for the two CGA dot-clock modes.  They are
 * deliberately not recomputed from live R0/R2/R3 values: raster tricks must be
 * allowed to move through the viewport instead of being recentered by it.
 */
static int
cga_precise_active_x_for_clock(int high_clock)
{
    return high_clock ? PRECISE_ACTIVE_X_HIRES : PRECISE_ACTIVE_X_LORES;
}

static int
cga_precise_visible_width(const cga_t *cga)
{
    return cga->precise_clock_high ? PRECISE_VISIBLE_WIDTH_HIRES : PRECISE_VISIBLE_WIDTH_LORES;
}

static int
cga_precise_frame_geometry_plausible(const cga_t *cga)
{
    const int char_pixels = cga_precise_char_pixels(cga);
    const int total_dots = ((int) cga->crtc[MC6845_R0_HTOTAL] + 1) * char_pixels;
    const int total_lines = (((int) cga->crtc[MC6845_R4_VTOTAL] + 1) *
                             ((int) cga->crtc[MC6845_R9_MAX_RASTER] + 1)) +
                            (int) cga->crtc[MC6845_R5_VTOTAL_ADJUST];
    const int displayed_dots = (int) cga->crtc[MC6845_R1_HDISPLAYED] * char_pixels;
    const int displayed_lines = (int) cga->crtc[MC6845_R6_VDISPLAYED] *
                                ((int) cga->crtc[MC6845_R9_MAX_RASTER] + 1);

    /* A real monitor lock is acquired only from a full-size CGA-like frame.
     * Area 5150's one-character auxiliary frames therefore cannot become the
     * physical monitor frame boundary, no matter how long their raw VSYNC pin
     * happens to remain asserted. */
    return (total_dots >= 880) && (total_dots <= 960) &&
           (total_lines >= 240) && (total_lines <= 280) &&
           (displayed_dots >= 600) && (displayed_dots <= 768) &&
           (displayed_lines >= 190) && (displayed_lines <= 208);
}

static int
cga_precise_hblank_width(const cga_t *cga)
{
    unsigned chars = cga->crtc[MC6845_R3_SYNC_WIDTH] & 0x0f;

    if (!chars)
        chars = 16;
    return (int) chars * cga_precise_char_pixels(cga);
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
cga_precise_process_physical_line(cga_t *cga, int physical_y, int drawn_width)
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

    /* A monitor has a fixed raster.  If HSYNC arrives early, the remainder of
     * the line is border rather than stale pixels from the previous frame. */
    if (drawn_width < width)
        cga_precise_fill_cell(cga, drawn_width, physical_y, width - drawn_width,
                              cga_precise_border(cga));

    if (cga->composite)
        Composite_Process(cga->precise_mode, border, (width + 3) >> 2, buffer32->line[y]);
    else
        video_process_8(width, y);

    if (cga->double_type == PRECISE_DOUBLE_SIMPLE) {
        if ((y + 1) < buffer_height) {
            if (cga->composite)
                Composite_Process(cga->precise_mode, border, (width + 3) >> 2, buffer32->line[y + 1]);
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
cga_precise_reset_frame_bounds(cga_t *cga, int keep_horizontal_phase)
{
    const int saved_x = cga->precise_beam_x;

    if (!keep_horizontal_phase)
        cga->precise_beam_x = 0;
    cga->precise_beam_y = 0;
    cga->precise_max_x = 0;
    cga->precise_max_y = 0;
    cga->precise_active_min_x = PRECISE_MAX_X;
    cga->precise_active_min_y = PRECISE_MAX_Y;
    cga->precise_active_max_x = 0;
    cga->precise_active_max_y = 0;
    cga->precise_frame_hires = !!(cga->precise_mode & CGA_MODE_FLAG_HIGHRES);

    /* A qualified VSYNC can begin partway through a physical scanline.  Keep
     * horizontal phase and blank the prefix of the new frame instead of
     * snapping the beam back to X=0. */
    if (keep_horizontal_phase && !cga->precise_monitor_hblank && (saved_x > 0))
        cga_precise_fill_cell(cga, 0, 0, saved_x, cga_precise_border(cga));
}

static void
cga_precise_present_frame(cga_t *cga, int keep_horizontal_phase)
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
        cga_precise_reset_frame_bounds(cga, keep_horizontal_phase);
        return;
    }

    if ((cga->precise_beam_x > 0) && !cga->precise_monitor_hblank)
        cga_precise_process_line(cga);

    /* Complete every unwritten monitor line with border.  Internal CRTC frames
     * and malformed sync pulses must never expose stale pixels from the prior
     * physical monitor frame. */
    for (int y = cga->precise_max_y; y < raster_height; y++)
        cga_precise_process_physical_line(cga, y, 0);

    if (enable_overscan) {
        src_x = 0;
        src_y = 0;
        /* Keep the host canvas fixed even when the dot clock changes inside a
         * frame.  Low-clock lines simply contain more right-hand border. */
        width = raster_width;
        height = raster_height;
    } else {
        /* The monitor aperture is fixed in signal coordinates.  It must not
         * follow display-enable bounds or the currently active internal CRTC
         * frame; doing so turns vertical motion into scaling and lets tiny
         * auxiliary frames recenter the host image. */
        src_x = cga_precise_active_x_for_clock(cga->precise_frame_hires);
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
    else if (width > (buffer_width - src_x))
        width = buffer_width - src_x;
    if (src_y >= buffer_height)
        height = 0;
    else if (height > (buffer_height - src_y))
        height = buffer_height - src_y;

    if ((width > 0) && (height > 0) && cga->precise_frame_valid) {
        if ((width != xsize) || (height != ysize) || video_force_resize_get()) {
            xsize = width;
            ysize = height;
            set_screen_size(xsize, ysize);
            video_force_resize_set(0);
        }

        video_wait_for_buffer();
        if (cga->double_type > PRECISE_DOUBLE_SIMPLE)
            cga_blit_memtoscreen(src_x, src_y, width, height, cga->double_type);
        else
            video_blit_memtoscreen(src_x, src_y, width, height);

        frames++;
        video_res_x = width;
        video_res_y = height;
        video_bpp = (cga->precise_mode & CGA_MODE_FLAG_GRAPHICS) ?
                    ((cga->precise_mode & CGA_MODE_FLAG_HIGHRES_GRAPHICS) ? 1 : 2) : 0;
    }

    cga->precise_frame_valid = 1;
    cga->cgablink++;
    cga->oddeven ^= 1;
    cga_precise_reset_frame_bounds(cga, keep_horizontal_phase);
}

static void
cga_precise_start_monitor_hblank(cga_t *cga, int from_hsync)
{
    int target;

    if (cga->precise_monitor_hblank)
        return;

    cga_precise_process_line(cga);
    cga->precise_monitor_hblank = 1;
    cga->precise_hblank_pixels = 0;

    if (from_hsync)
        target = cga_precise_hblank_width(cga);
    else
        target = PRECISE_TOTAL_DOTS - cga_precise_visible_width(cga);

    if (target < cga_precise_char_pixels(cga))
        target = cga_precise_char_pixels(cga);
    if (target > PRECISE_RASTER_WIDTH)
        target = PRECISE_RASTER_WIDTH;
    cga->precise_hblank_target = target;
}

static void
cga_precise_finish_monitor_line(cga_t *cga)
{
    const int acquire_lock = cga->precise_sync_pending &&
                             !cga->precise_monitor_locked &&
                             cga_precise_frame_geometry_plausible(cga);

    cga->precise_monitor_hblank = 0;
    cga->precise_hblank_pixels = 0;
    cga->precise_hblank_target = 0;
    cga->precise_beam_x = 0;
    cga->precise_beam_y++;
    cga->displine = cga->precise_beam_y;

    if (cga->precise_card_vsync) {
        if (cga->precise_card_vsync_lines < 255)
            cga->precise_card_vsync_lines++;
        if (cga->precise_card_vsync_lines >= PRECISE_CARD_VSYNC_LINES) {
            cga->precise_card_vsync = 0;
            cga->precise_card_vsync_lines = 0;
        }
    }

    /* Acquire monitor phase once, from a complete CGA-like frame, one physical
     * line after the CRTC VSYNC edge.  Once locked, the monitor free-runs at
     * 262 lines and ignores later internal CRTC frames.  CPU/video clocks are
     * derived from the same crystal, so continuously snapping to raw VSYNC is
     * both unnecessary and destructive. */
    if (acquire_lock) {
        cga->precise_monitor_locked = 1;
        cga->precise_sync_pending = 0;
        cga_precise_present_frame(cga, 0);
        return;
    }

    /* A monitor eventually flies back even if the CRTC fails to provide a
     * usable VSYNC.  This is also the normal frame boundary after lock. */
    if (cga->precise_beam_y >= PRECISE_RASTER_HEIGHT)
        cga_precise_present_frame(cga, 0);
}

void
cga_precise_init(cga_t *cga)
{
    mc6845_core_init(&cga->precise_crtc, cga->crtc);
    cga->precise_enabled = 1;
    cga->precise_mode = cga->cgamode;
    cga->precise_pending_mode = cga->cgamode;
    cga->precise_mode_pending = 0;
    cga->precise_clock_high = !!(cga->cgamode & CGA_MODE_FLAG_HIGHRES);
    cga->precise_clock_pending = 0;
    cga->precise_master_phase = 0;
    cga->precise_char_time = cga->precise_clock_high ? CGACONST : (CGACONST << 1);
    cga->precise_beam_x = 0;
    cga->precise_prev_hsync = 0;
    cga->precise_prev_vsync = 0;
    cga->precise_frame_valid = 0;
    cga->precise_monitor_hblank = 0;
    cga->precise_monitor_vsync = 0;
    cga->precise_monitor_locked = 0;
    cga->precise_sync_pending = 0;
    cga->precise_card_vsync = 0;
    cga->precise_card_vsync_lines = 0;
    cga->precise_hblank_pixels = 0;
    cga->precise_hblank_target = 0;
    cga->precise_vsync_dots = 0;
    cga_precise_reset_frame_bounds(cga, 0);
}

void
cga_poll_precise(void *priv)
{
    cga_t *cga = (cga_t *) priv;
    mc6845_outputs_t before;
    mc6845_outputs_t after;
    uint64_t char_time;
    int cell_width;
    int visible_limit;
    int was_hblank;

    cga_precise_apply_clock_if_due(cga);
    char_time = cga->precise_char_time;
    cell_width = cga_precise_char_pixels(cga);
    visible_limit = cga_precise_visible_width(cga);
    was_hblank = cga->precise_monitor_hblank;

    if (!char_time)
        char_time = cga->precise_clock_high ? CGACONST : (CGACONST << 1);
    timer_advance_u64(&cga->timer, char_time);

    mc6845_core_get_outputs(&cga->precise_crtc, &before);

    if (before.de)
        cga->cgastat &= ~1;
    else
        cga->cgastat |= 1;
    if (before.vsync)
        cga->cgastat |= 8;
    else
        cga->cgastat &= ~8;

    if (was_hblank) {
        if (cga->precise_hblank_pixels <= PRECISE_MAX_X - cell_width)
            cga->precise_hblank_pixels += cell_width;
    } else {
        const int beam_limit = cga_precise_raster_width();

        if ((beam_limit > 0) && (cga->precise_beam_x < beam_limit)) {
            cga_precise_render_cell(cga, &before);
            if (cell_width >= (beam_limit - cga->precise_beam_x))
                cga->precise_beam_x = beam_limit;
            else
                cga->precise_beam_x += cell_width;
        }
    }

    mc6845_core_tick(&cga->precise_crtc);
    mc6845_core_get_outputs(&cga->precise_crtc, &after);

    /* The CGA card gates the 6845's sixteen-line VSYNC to roughly three
     * physical scanlines.  More importantly, only the first plausible full
     * frame is allowed to acquire monitor phase; later raw VSYNC edges are not
     * host-frame boundaries. */
    if (!before.vsync && after.vsync) {
        cga->precise_card_vsync = 1;
        cga->precise_card_vsync_lines = 0;
        cga->precise_monitor_vsync = 1;
        if (!cga->precise_monitor_locked &&
            cga_precise_frame_geometry_plausible(cga))
            cga->precise_sync_pending = 1;
    } else if (before.vsync && !after.vsync) {
        cga->precise_monitor_vsync = 0;
        if (!cga->precise_monitor_locked)
            cga->precise_sync_pending = 0;
    }

    /* Keep legacy debugger-visible fields coherent. */
    cga->scanline = after.ra;
    cga->vc = after.vcc;
    cga->memaddr = after.ma;
    cga->memaddr_backup = cga->precise_crtc.vma_row;
    cga->cgadispon = after.de;
    cga->cursorvisible = after.cursor;

    /* Raw CRTC HSYNC starts monitor flyback, but monitor flyback has its own
     * duration and fallback.  Missing or malformed HSYNC can no longer freeze
     * the physical scanline counter. */
    if (!before.hsync && after.hsync) {
        cga_precise_apply_pending_mode(cga);
        cga_precise_start_monitor_hblank(cga, 1);
    }
    else if (!cga->precise_monitor_hblank &&
             (cga->precise_beam_x >= visible_limit))
        cga_precise_start_monitor_hblank(cga, 0);

    if (was_hblank && cga->precise_monitor_hblank &&
        (cga->precise_hblank_pixels >= cga->precise_hblank_target))
        cga_precise_finish_monitor_line(cga);

    cga->precise_master_phase = (cga->precise_master_phase + (uint32_t) cell_width) & 0x0f;
    cga->precise_prev_hsync = after.hsync;
    cga->precise_prev_vsync = after.vsync;
}
