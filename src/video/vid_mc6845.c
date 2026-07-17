#include <86box/vid_mc6845.h>

#include <string.h>

#define MC6845_HSYNC_MASK  0x0f
#define MC6845_VSYNC_LINES 16
#define MC6845_MA_MASK     0x3fff

static uint8_t
mc6845_masked_value(uint8_t index, uint8_t value)
{
    static const uint8_t mask[MC6845_REG_COUNT] = {
        0xff, 0xff, 0xff, 0xff,
        0x7f, 0x1f, 0x7f, 0x7f,
        0x03, 0x1f, 0x7f, 0x1f,
        0x3f, 0xff, 0x3f, 0xff,
        0x3f, 0xff
    };

    return index < MC6845_REG_COUNT ? value & mask[index] : value;
}

static uint8_t
mc6845_ra(const mc6845_core_t *core)
{
    if (core->interlace_video)
        return (uint8_t) (((core->vlc_interlace << 1) |
                           (core->frame_odd ? 1 : 0)) & 0x1f);
    return core->vlc & 0x1f;
}

static bool
mc6845_in_vta(const mc6845_core_t *core)
{
    return core->mode == MC6845_MODE_VTOTAL_ADJUST;
}

static void
mc6845_refresh_programmed_values(mc6845_core_t *core)
{
    core->start_address = (uint16_t) (((((uint16_t) core->reg[MC6845_R12_START_H]) << 8) |
                                       core->reg[MC6845_R13_START_L]) & MC6845_MA_MASK);
    core->cursor_address = (uint16_t) (((((uint16_t) core->reg[MC6845_R14_CURSOR_H]) << 8) |
                                        core->reg[MC6845_R15_CURSOR_L]) & MC6845_MA_MASK);

    core->cursor_start_line = core->reg[MC6845_R10_CURSOR_START] & 0x1f;
    core->cursor_blink_mode = (core->reg[MC6845_R10_CURSOR_START] >> 5) & 3;
    core->cursor_enabled = core->cursor_blink_mode != 1;

    core->interlace_sync  = (core->reg[MC6845_R8_INTERLACE] & 1) != 0;
    core->interlace_video = (core->reg[MC6845_R8_INTERLACE] & 3) == 3;
}

static void
mc6845_begin_hsync(mc6845_core_t *core)
{
    core->in_hsync = true;
    core->hsync_counter = core->reg[MC6845_R3_SYNC_WIDTH] & MC6845_HSYNC_MASK;
}

static void
mc6845_clock_cursor_blink(mc6845_core_t *core)
{
    if ((core->cursor_blink_mode == 2) && ((core->frames & 7) == 0))
        core->cursor_blink_state = !core->cursor_blink_state;
    else if ((core->cursor_blink_mode == 3) && ((core->frames & 15) == 0))
        core->cursor_blink_state = !core->cursor_blink_state;
}

static void
mc6845_begin_vsync(mc6845_core_t *core, bool clock_blink)
{
    if (core->in_vsync)
        return;

    core->in_vsync = true;
    core->in_last_vblank_line = false;
    core->vsync_counter = 0;

    if (clock_blink)
        mc6845_clock_cursor_blink(core);
}

/*
 * Motorola type-2 last-line management permits R4/R9 writes after C0=0 and
 * outside HSYNC to create a last-line coincidence in the current scanline.
 * Once latched, a later write cannot clear it.
 */
static void
mc6845_try_latch_last_line(mc6845_core_t *core)
{
    if (!core->last_line && core->last_line_mgmt && (core->hcc != 0) &&
        !core->in_hsync &&
        (core->vcc == core->reg[MC6845_R4_VTOTAL]) &&
        (core->vlc == core->reg[MC6845_R9_MAX_RASTER])) {
        core->last_row = true;
        core->last_line = true;
        core->last_line_mgmt = false;
        core->vta_counter = 0;
    }
}

/*
 * Observe a caller-owned register file.  This is important in 86Box because
 * the legacy I/O path writes cga->crtc[] directly instead of necessarily going
 * through mc6845_core_select_write().
 */
static void
mc6845_observe_registers(mc6845_core_t *core, bool apply_side_effects)
{
    uint8_t old_r1 = 0;
    uint8_t old_r2 = 0;
    uint8_t old_r4 = 0;
    uint8_t old_r6 = 0;
    uint8_t old_r7 = 0;
    uint8_t old_r8 = 0;
    uint8_t old_r9 = 0;
    uint8_t old_r10 = 0;
    bool had_old = core->observed_reg_valid;
    bool old_interlace_sync = core->interlace_sync;

    if (had_old) {
        old_r1  = core->observed_reg[MC6845_R1_HDISPLAYED];
        old_r2  = core->observed_reg[MC6845_R2_HSYNC_POS];
        old_r4  = core->observed_reg[MC6845_R4_VTOTAL];
        old_r6  = core->observed_reg[MC6845_R6_VDISPLAYED];
        old_r7  = core->observed_reg[MC6845_R7_VSYNC_POS];
        old_r8  = core->observed_reg[MC6845_R8_INTERLACE];
        old_r9  = core->observed_reg[MC6845_R9_MAX_RASTER];
        old_r10 = core->observed_reg[MC6845_R10_CURSOR_START];
    }

    /* Canonicalize writable registers even when the owner wrote them directly. */
    for (uint8_t i = 0; i < MC6845_R16_LIGHTPEN_H; i++)
        core->reg[i] = mc6845_masked_value(i, core->reg[i]);

    mc6845_refresh_programmed_values(core);

    if (had_old && apply_side_effects) {
        const bool r1_changed = old_r1 != core->reg[MC6845_R1_HDISPLAYED];
        const bool r2_changed = old_r2 != core->reg[MC6845_R2_HSYNC_POS];
        const bool r4_changed = old_r4 != core->reg[MC6845_R4_VTOTAL];
        const bool r6_changed = old_r6 != core->reg[MC6845_R6_VDISPLAYED];
        const bool r7_changed = old_r7 != core->reg[MC6845_R7_VSYNC_POS];
        const bool r8_changed = old_r8 != core->reg[MC6845_R8_INTERLACE];
        const bool r9_changed = old_r9 != core->reg[MC6845_R9_MAX_RASTER];
        const bool r10_changed = old_r10 != core->reg[MC6845_R10_CURSOR_START];

        if (r8_changed && old_interlace_sync && !core->interlace_sync)
            core->frame_odd = false;

        if (r4_changed || r9_changed)
            mc6845_try_latch_last_line(core);

        /* Coincidences at counter value zero must not wait for a wrap. */
        if (r1_changed && (core->hcc == 0))
            core->horizontal_de = core->reg[MC6845_R1_HDISPLAYED] != 0;

        if (r2_changed && (core->hcc == 0) &&
            (core->reg[MC6845_R2_HSYNC_POS] == 0) && !core->in_hsync)
            mc6845_begin_hsync(core);

        if (r6_changed && (core->vcc == 0) && (core->vlc == 0))
            core->vertical_de = core->reg[MC6845_R6_VDISPLAYED] != 0;

        if (r7_changed && (core->vcc == 0) && (core->vlc == 0) &&
            (core->reg[MC6845_R7_VSYNC_POS] == 0))
            mc6845_begin_vsync(core, true);

        if (r10_changed && !mc6845_in_vta(core) &&
            (mc6845_ra(core) == core->cursor_start_line))
            core->cursor_active = true;
    }

    memcpy(core->observed_reg, core->reg, sizeof(core->observed_reg));
    core->observed_reg_valid = true;
}

static bool
mc6845_cursor(const mc6845_core_t *core)
{
    bool enabled = core->cursor_enabled && core->cursor_active &&
                   ((core->vma & MC6845_MA_MASK) == core->cursor_address);

    if ((core->cursor_blink_mode == 2) || (core->cursor_blink_mode == 3))
        enabled = enabled && core->cursor_blink_state;

    return enabled;
}

static bool
mc6845_has_half_line(const mc6845_core_t *core)
{
    return core->interlace_sync && !core->frame_odd;
}

static void
mc6845_start_frame(mc6845_core_t *core)
{
    core->frames++;
    if (core->interlace_sync)
        core->frame_odd = !core->frame_odd;

    core->mode = MC6845_MODE_NORMAL;
    core->last_row = false;
    core->last_line = false;
    core->vta_counter = 0;
    core->vcc = 0;
    core->vlc = 0;
    core->vlc_interlace = 0;
    core->cursor_active = false;

    core->start_address_latch = core->start_address;
    core->vma = core->start_address_latch;
    core->vma_row = core->vma;

    /* A vertical restart must not reset the independent horizontal flip-flops. */
    core->vertical_de = core->reg[MC6845_R6_VDISPLAYED] != 0;
    core->in_vsync = false;
    core->in_last_vblank_line = false;
    core->vsync_counter = 0;
    if (core->reg[MC6845_R7_VSYNC_POS] == 0)
        mc6845_begin_vsync(core, true);

    if ((mc6845_ra(core) == core->cursor_start_line) && !mc6845_in_vta(core))
        core->cursor_active = true;
}

static void
mc6845_process_frame_end(mc6845_core_t *core)
{
    if (mc6845_has_half_line(core))
        core->mode = MC6845_MODE_INTERLACE_HALF_LINE;
    else
        mc6845_start_frame(core);
}

static void
mc6845_process_last_line(mc6845_core_t *core)
{
    switch (core->mode) {
        case MC6845_MODE_NORMAL:
            if (core->reg[MC6845_R5_VTOTAL_ADJUST] != 0) {
                core->mode = MC6845_MODE_VTOTAL_ADJUST;
                core->last_row = false;
                core->last_line = false;
            } else if (mc6845_has_half_line(core)) {
                core->mode = MC6845_MODE_INTERLACE_HALF_LINE;
            } else {
                mc6845_process_frame_end(core);
            }
            break;

        case MC6845_MODE_INTERLACE_HALF_LINE:
            mc6845_start_frame(core);
            break;

        case MC6845_MODE_VTOTAL_ADJUST:
            break;
    }
}

static void
mc6845_tick_vsync_line(mc6845_core_t *core)
{
    /* The fixed sixteen-line VSYNC counter is clocked by internal CRTC
     * scanlines, not by HSYNC pulses.  Raster programs may suppress HSYNC
     * while C0/R0 continues to generate internal lines. */
    if (!core->in_vsync)
        return;

    core->vsync_counter++;
    if (core->vsync_counter >= MC6845_VSYNC_LINES) {
        core->vsync_counter = 0;
        core->in_vsync = false;
        core->in_last_vblank_line = true;
    }
}

static void
mc6845_tick_vlc(mc6845_core_t *core, bool tick_interlace)
{
    core->vlc = (core->vlc + 1) & 0x1f;
    if (tick_interlace)
        core->vlc_interlace = (core->vlc_interlace + 1) & 0x0f;

    if ((mc6845_ra(core) == core->cursor_start_line) && !mc6845_in_vta(core))
        core->cursor_active = true;
}

/* Establish all state that is coincident with C0 == 0 for the new line. */
static void
mc6845_enter_line_start(mc6845_core_t *core)
{
    const bool first_line = (core->vcc == 0) && (core->vlc == 0);
    const bool c9_r9 = core->vlc == core->reg[MC6845_R9_MAX_RASTER];
    const bool c9_interlace_split = core->interlace_video &&
                                    (core->vlc == (core->reg[MC6845_R9_MAX_RASTER] >> 1));
    const bool terminal_match = (core->vcc == core->reg[MC6845_R4_VTOTAL]) && c9_r9;

    if (first_line)
        core->vma = core->start_address_latch;

    core->horizontal_de = true;

    /* R2=0 starts HSYNC at C0=0 and therefore suppresses last-line latching. */
    if (core->reg[MC6845_R2_HSYNC_POS] == 0)
        mc6845_begin_hsync(core);

    core->last_row = core->vcc == core->reg[MC6845_R4_VTOTAL];
    if (terminal_match && !core->previous_last_line && !core->in_hsync) {
        core->last_line = true;
        core->last_line_mgmt = false;
    } else {
        core->last_line = false;
        core->last_line_mgmt = !first_line;
    }

    if (core->last_row)
        core->vta_counter = 0;

    if ((mc6845_ra(core) == core->cursor_start_line) && !mc6845_in_vta(core))
        core->cursor_active = true;

    /* R1=0 is a real C0 coincidence: there is no displayed character. */
    if (core->reg[MC6845_R1_HDISPLAYED] == 0) {
        if (c9_r9 || c9_interlace_split)
            core->vma_row = core->vma;
        core->horizontal_de = false;
    }
}

static void
mc6845_prime_position(mc6845_core_t *core)
{
    core->horizontal_de = true;
    core->vertical_de = core->reg[MC6845_R6_VDISPLAYED] != 0;
    core->in_hsync = false;
    core->in_vsync = false;
    core->in_last_vblank_line = false;
    core->hsync_counter = 0;
    core->vsync_counter = 0;

    if (core->reg[MC6845_R7_VSYNC_POS] == 0)
        mc6845_begin_vsync(core, false);

    mc6845_enter_line_start(core);
}

void
mc6845_core_init(mc6845_core_t *core, uint8_t *external_register_file)
{
    memset(core, 0, sizeof(*core));
    core->reg = external_register_file ? external_register_file : core->owned_reg;
    core->cursor_blink_state = true;

    mc6845_observe_registers(core, false);
    core->start_address_latch = core->start_address;
    core->vma = core->start_address_latch;
    core->vma_row = core->vma;
    mc6845_prime_position(core);
}

void
mc6845_core_reset(mc6845_core_t *core)
{
    uint8_t *reg = core->reg;
    uint8_t owned[MC6845_REG_COUNT];
    bool owns_registers = reg == core->owned_reg;

    if (owns_registers)
        memcpy(owned, core->owned_reg, sizeof(owned));

    memset(core, 0, sizeof(*core));
    core->reg = owns_registers ? core->owned_reg : reg;
    if (owns_registers)
        memcpy(core->owned_reg, owned, sizeof(owned));

    core->cursor_blink_state = true;
    mc6845_observe_registers(core, false);
    core->start_address_latch = core->start_address;
    core->vma = core->start_address_latch;
    core->vma_row = core->vma;
    mc6845_prime_position(core);
}

void
mc6845_core_select_write(mc6845_core_t *core, uint8_t index, uint8_t value)
{
    if ((index >= MC6845_REG_COUNT) ||
        (index == MC6845_R16_LIGHTPEN_H) ||
        (index == MC6845_R17_LIGHTPEN_L))
        return;

    core->reg[index] = mc6845_masked_value(index, value);
    mc6845_observe_registers(core, true);
}

uint8_t
mc6845_core_select_read(const mc6845_core_t *core, uint8_t index)
{
    switch (index) {
        case MC6845_R14_CURSOR_H:
        case MC6845_R15_CURSOR_L:
        case MC6845_R16_LIGHTPEN_H:
        case MC6845_R17_LIGHTPEN_L:
            return core->reg[index];

        default:
            return 0xff;
    }
}

void
mc6845_core_latch_lightpen(mc6845_core_t *core)
{
    core->lightpen_position = core->vma & MC6845_MA_MASK;
    core->reg[MC6845_R16_LIGHTPEN_H] = (core->lightpen_position >> 8) & 0x3f;
    core->reg[MC6845_R17_LIGHTPEN_L] = core->lightpen_position & 0xff;
    core->observed_reg[MC6845_R16_LIGHTPEN_H] = core->reg[MC6845_R16_LIGHTPEN_H];
    core->observed_reg[MC6845_R17_LIGHTPEN_L] = core->reg[MC6845_R17_LIGHTPEN_L];
}

void
mc6845_core_get_outputs(const mc6845_core_t *core, mc6845_outputs_t *out)
{
    out->ma = core->vma & MC6845_MA_MASK;
    out->ra = mc6845_ra(core);
    out->hcc = core->hcc;
    out->vcc = core->vcc;
    out->de = core->horizontal_de && core->vertical_de;
    out->hsync = core->in_hsync;
    out->vsync = core->in_vsync;
    out->cursor = mc6845_cursor(core);
    out->hborder = !core->horizontal_de;
    out->vborder = !core->vertical_de;
}

void
mc6845_core_tick(mc6845_core_t *core)
{
    uint8_t r0;
    uint8_t r1;
    uint8_t r2;
    uint8_t r4;
    uint8_t r5;
    uint8_t r6;
    uint8_t r7;
    uint8_t r9;
    uint8_t r11;
    bool c0_r0;
    bool c0_r0_half;
    bool c9_r9;
    bool c9_interlace_split;
    bool c4_r4;

    /* Pick up direct writes before evaluating this character clock. */
    mc6845_observe_registers(core, true);

    r0 = core->reg[MC6845_R0_HTOTAL];
    r1 = core->reg[MC6845_R1_HDISPLAYED];
    r2 = core->reg[MC6845_R2_HSYNC_POS];
    r4 = core->reg[MC6845_R4_VTOTAL];
    r5 = core->reg[MC6845_R5_VTOTAL_ADJUST];
    r6 = core->reg[MC6845_R6_VDISPLAYED];
    r7 = core->reg[MC6845_R7_VSYNC_POS];
    r9 = core->reg[MC6845_R9_MAX_RASTER];
    r11 = core->reg[MC6845_R11_CURSOR_END] & 0x1f;

    c0_r0 = core->hcc == r0;
    c0_r0_half = core->hcc == (r0 >> 1);
    c9_r9 = core->vlc == r9;
    /* The interlaced-video row split is driven by progressive C9, not C9I. */
    c9_interlace_split = core->interlace_video && (core->vlc == (r9 >> 1));
    c4_r4 = core->vcc == r4;

    core->hcc = (uint8_t) (core->hcc + 1);
    core->vma = (core->vma + 1) & MC6845_MA_MASK;

    if (core->in_hsync) {
        /* A zero width wraps through 15 and therefore produces 16 characters. */
        core->hsync_counter = (core->hsync_counter - 1) & MC6845_HSYNC_MASK;
        if (core->hsync_counter == 0) {
            if (c4_r4 && c9_r9) {
                core->previous_last_line = true;
            } else {
                core->previous_last_line = false;
                core->last_line_mgmt = true;
            }

            core->in_hsync = false;
        }
    }

    if (!c0_r0) {
        if (core->hcc == r1) {
            if (c9_r9 || c9_interlace_split)
                core->vma_row = core->vma;
            core->horizontal_de = false;
        }

        if (core->hcc == r2)
            mc6845_begin_hsync(core);
    }

    if (c0_r0) {
        if (core->in_last_vblank_line)
            core->in_last_vblank_line = false;
        mc6845_tick_vsync_line(core);

        if ((mc6845_ra(core) == r11) && !mc6845_in_vta(core))
            core->cursor_active = false;

        core->hcc = 0;
        core->vma = core->vma_row;

        if (c9_r9) {
            core->vlc = 0;
            core->vlc_interlace = 0;
            core->vcc = (core->vcc + 1) & 0x7f;
            core->vma = core->vma_row;

            if (core->vcc == r7)
                mc6845_begin_vsync(core, true);

            if (core->last_line)
                mc6845_process_last_line(core);
        } else if (c9_interlace_split) {
            core->vlc_interlace = 0;
            mc6845_tick_vlc(core, false);
        } else {
            mc6845_tick_vlc(core, true);
        }

        if (core->vcc == r6)
            core->vertical_de = false;

        if (mc6845_in_vta(core)) {
            core->vta_counter++;
            if (core->vta_counter > r5)
                mc6845_process_frame_end(core);
        }

        mc6845_enter_line_start(core);
    } else if ((core->mode == MC6845_MODE_INTERLACE_HALF_LINE) && c0_r0_half) {
        /* Vertical field restart occurs without disturbing horizontal phase. */
        mc6845_process_last_line(core);
    }

    core->ticks++;
}
