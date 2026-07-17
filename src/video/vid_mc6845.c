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

static void
mc6845_refresh_programmed_values(mc6845_core_t *core)
{
    core->start_address = (uint16_t) ((((uint16_t) core->reg[MC6845_R12_START_H]) << 8) |
                                      core->reg[MC6845_R13_START_L]) & MC6845_MA_MASK;
    core->cursor_address = (uint16_t) ((((uint16_t) core->reg[MC6845_R14_CURSOR_H]) << 8) |
                                       core->reg[MC6845_R15_CURSOR_L]) & MC6845_MA_MASK;

    core->cursor_start_line = core->reg[MC6845_R10_CURSOR_START] & 0x1f;
    core->cursor_blink_mode = (core->reg[MC6845_R10_CURSOR_START] >> 5) & 3;
    core->cursor_enabled = core->cursor_blink_mode != 1;

    core->interlace_sync  = (core->reg[MC6845_R8_INTERLACE] & 1) != 0;
    core->interlace_video = (core->reg[MC6845_R8_INTERLACE] & 3) == 3;
}

static uint8_t
mc6845_ra(const mc6845_core_t *core)
{
    if (core->interlace_video)
        return (uint8_t) (((core->vlc << 1) | (core->frame_odd ? 1 : 0)) & 0x1f);
    return core->vlc & 0x1f;
}

static unsigned
mc6845_hsync_width(const mc6845_core_t *core)
{
    unsigned width = core->reg[MC6845_R3_SYNC_WIDTH] & MC6845_HSYNC_MASK;
    return width ? width : 16;
}

static void
mc6845_update_de(mc6845_core_t *core)
{
    const uint8_t r1 = core->reg[MC6845_R1_HDISPLAYED];
    const uint8_t r6 = core->reg[MC6845_R6_VDISPLAYED];

    /* Equality counters produce a zero-width display when R1/R6 are zero. */
    core->horizontal_de = (r1 != 0) && (core->hcc < r1);
    core->vertical_de = (core->mode == MC6845_MODE_NORMAL) &&
                        (r6 != 0) && (core->vcc < r6);
}

static void
mc6845_start_hsync_if_due(mc6845_core_t *core)
{
    if (!core->in_hsync && (core->hcc == core->reg[MC6845_R2_HSYNC_POS])) {
        core->in_hsync = true;
        core->hsync_counter = (uint8_t) mc6845_hsync_width(core);
    }
}

static void
mc6845_start_vsync_if_due(mc6845_core_t *core)
{
    if (!core->in_vsync && (core->mode == MC6845_MODE_NORMAL) &&
        (core->vlc == 0) && (core->vcc == core->reg[MC6845_R7_VSYNC_POS])) {
        core->in_vsync = true;
        core->vsync_counter = 0;
    }
}

static void
mc6845_update_cursor_blink(mc6845_core_t *core)
{
    switch (core->cursor_blink_mode) {
        case 2:
            if ((core->frames & 7) == 0)
                core->cursor_blink_state = !core->cursor_blink_state;
            break;
        case 3:
            if ((core->frames & 15) == 0)
                core->cursor_blink_state = !core->cursor_blink_state;
            break;
        default:
            core->cursor_blink_state = true;
            break;
    }
}

static void
mc6845_start_frame(mc6845_core_t *core)
{
    mc6845_refresh_programmed_values(core);

    core->frames++;
    if (core->interlace_sync)
        core->frame_odd = !core->frame_odd;

    core->mode = MC6845_MODE_NORMAL;
    core->last_row = false;
    core->previous_last_line = false;
    core->last_line = false;
    core->last_line_mgmt = false;
    core->vta_counter = 0;
    core->vcc = 0;
    core->vlc = 0;
    core->vlc_interlace = 0;

    core->start_address_latch = core->start_address;
    core->vma = core->start_address_latch;
    core->vma_row = core->vma;

    core->in_vsync = false;
    core->vsync_counter = 0;
    core->in_last_vblank_line = false;

    mc6845_update_cursor_blink(core);
    mc6845_start_vsync_if_due(core);
    mc6845_update_de(core);
}

static bool
mc6845_cursor_scanline_visible(const mc6845_core_t *core)
{
    const uint8_t ra = mc6845_ra(core);
    const uint8_t start = core->reg[MC6845_R10_CURSOR_START] & 0x1f;
    const uint8_t end = core->reg[MC6845_R11_CURSOR_END] & 0x1f;

    if (start <= end)
        return (ra >= start) && (ra <= end);
    return (ra >= start) || (ra <= end);
}

static bool
mc6845_cursor(const mc6845_core_t *core)
{
    bool enabled = core->cursor_enabled && mc6845_cursor_scanline_visible(core) &&
                   ((core->vma & MC6845_MA_MASK) == core->cursor_address);

    if ((core->cursor_blink_mode == 2) || (core->cursor_blink_mode == 3))
        enabled = enabled && core->cursor_blink_state;
    return enabled;
}

static void
mc6845_advance_vertical(mc6845_core_t *core)
{
    const uint8_t r4 = core->reg[MC6845_R4_VTOTAL];
    const uint8_t r5 = core->reg[MC6845_R5_VTOTAL_ADJUST];
    const uint8_t r9 = core->reg[MC6845_R9_MAX_RASTER];
    const bool last_raster = mc6845_ra(core) == r9;

    /* The 6845's VSYNC width is counted in internal scanlines, not HSYNC
     * pulses.  Area 5150 deliberately creates internal lines with no HSYNC. */
    if (core->in_vsync) {
        core->vsync_counter++;
        if (core->vsync_counter >= MC6845_VSYNC_LINES) {
            core->vsync_counter = 0;
            core->in_vsync = false;
        }
    }

    if (core->mode == MC6845_MODE_VTOTAL_ADJUST) {
        core->vta_counter++;
        core->vlc = (core->vlc + 1) & 0x1f;
        if (core->vta_counter >= r5)
            mc6845_start_frame(core);
        else
            mc6845_update_de(core);
        return;
    }

    if (!last_raster) {
        core->vlc = (core->vlc + 1) & 0x1f;
        core->vlc_interlace = (core->vlc_interlace + 1) & 0x0f;
        mc6845_update_de(core);
        return;
    }

    core->vlc = 0;
    core->vlc_interlace = 0;

    if (core->vcc == r4) {
        if (r5 != 0) {
            core->mode = MC6845_MODE_VTOTAL_ADJUST;
            core->vta_counter = 0;
            mc6845_update_de(core);
        } else {
            mc6845_start_frame(core);
        }
        return;
    }

    core->vcc = (core->vcc + 1) & 0x7f;
    mc6845_start_vsync_if_due(core);
    mc6845_update_de(core);
}

void
mc6845_core_init(mc6845_core_t *core, uint8_t *external_register_file)
{
    memset(core, 0, sizeof(*core));
    core->reg = external_register_file ? external_register_file : core->owned_reg;
    core->cursor_blink_state = true;
    mc6845_refresh_programmed_values(core);
    core->start_address_latch = core->start_address;
    core->vma = core->start_address_latch;
    core->vma_row = core->vma;
    mc6845_update_de(core);
}

void
mc6845_core_reset(mc6845_core_t *core)
{
    uint8_t *reg = core->reg;
    uint8_t owned[MC6845_REG_COUNT];

    if (reg == core->owned_reg)
        memcpy(owned, core->owned_reg, sizeof(owned));

    memset(core, 0, sizeof(*core));
    core->reg = reg == core->owned_reg ? core->owned_reg : reg;
    if (core->reg == core->owned_reg)
        memcpy(core->owned_reg, owned, sizeof(owned));

    core->cursor_blink_state = true;
    mc6845_refresh_programmed_values(core);
    core->start_address_latch = core->start_address;
    core->vma = core->start_address_latch;
    core->vma_row = core->vma;
    mc6845_update_de(core);
}

void
mc6845_core_select_write(mc6845_core_t *core, uint8_t index, uint8_t value)
{
    if (index >= MC6845_REG_COUNT || index == MC6845_R16_LIGHTPEN_H ||
        index == MC6845_R17_LIGHTPEN_L)
        return;

    core->reg[index] = mc6845_masked_value(index, value);
    mc6845_refresh_programmed_values(core);
    mc6845_update_de(core);
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
    const uint8_t r0 = core->reg[MC6845_R0_HTOTAL];
    const uint8_t r1 = core->reg[MC6845_R1_HDISPLAYED];
    const uint8_t r9 = core->reg[MC6845_R9_MAX_RASTER];
    const uint8_t current_hcc = core->hcc;
    const bool line_end = current_hcc == r0;
    const bool last_raster = mc6845_ra(core) == r9;
    uint16_t next_vma = (core->vma + 1) & MC6845_MA_MASK;
    uint8_t next_hcc = line_end ? 0 : (uint8_t) (current_hcc + 1);

    mc6845_refresh_programmed_values(core);

    /* Consume one character of an existing HSync pulse. */
    if (core->in_hsync) {
        if (core->hsync_counter > 1)
            core->hsync_counter--;
        else {
            core->hsync_counter = 0;
            core->in_hsync = false;
        }
    }

    /* Capture the address immediately after the displayed part of the final
     * raster line.  That becomes the base address of the next character row. */
    if (last_raster && ((uint16_t) current_hcc + 1u == (uint16_t) r1))
        core->vma_row = next_vma;

    core->hcc = next_hcc;
    core->vma = next_vma;

    if (line_end) {
        core->vma = core->vma_row;
        mc6845_advance_vertical(core);
    }

    mc6845_update_de(core);
    mc6845_start_hsync_if_due(core);
    core->ticks++;
}
