#ifndef VID_MC6845_H
#define VID_MC6845_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Character-clock Motorola MC6845 core for 86Box.
 *
 * The register file may be owned by the caller.  Direct writes to that shared
 * file are detected at the beginning of the next character clock, while
 * mc6845_core_select_write() applies the same hardware side effects
 * immediately.
 */

enum {
    MC6845_R0_HTOTAL = 0,
    MC6845_R1_HDISPLAYED,
    MC6845_R2_HSYNC_POS,
    MC6845_R3_SYNC_WIDTH,
    MC6845_R4_VTOTAL,
    MC6845_R5_VTOTAL_ADJUST,
    MC6845_R6_VDISPLAYED,
    MC6845_R7_VSYNC_POS,
    MC6845_R8_INTERLACE,
    MC6845_R9_MAX_RASTER,
    MC6845_R10_CURSOR_START,
    MC6845_R11_CURSOR_END,
    MC6845_R12_START_H,
    MC6845_R13_START_L,
    MC6845_R14_CURSOR_H,
    MC6845_R15_CURSOR_L,
    MC6845_R16_LIGHTPEN_H,
    MC6845_R17_LIGHTPEN_L,
    MC6845_REG_COUNT
};

typedef enum mc6845_mode_t {
    MC6845_MODE_NORMAL = 0,
    MC6845_MODE_VTOTAL_ADJUST,
    MC6845_MODE_INTERLACE_HALF_LINE
} mc6845_mode_t;

typedef struct mc6845_outputs_t {
    uint16_t ma;
    uint8_t  ra;
    uint8_t  hcc;
    uint8_t  vcc;
    bool     de;
    bool     hsync;
    bool     vsync;
    bool     cursor;
    bool     hborder;
    bool     vborder;
} mc6845_outputs_t;

typedef struct mc6845_core_t {
    uint8_t *reg;
    uint8_t  owned_reg[MC6845_REG_COUNT];

    /* Last values observed in a caller-owned register file. */
    uint8_t observed_reg[MC6845_REG_COUNT];
    bool    observed_reg_valid;

    uint64_t ticks;
    uint64_t frames;

    mc6845_mode_t mode;

    uint16_t start_address;
    uint16_t start_address_latch;
    uint16_t cursor_address;
    uint16_t lightpen_position;

    uint8_t hcc;
    uint8_t vlc;
    uint8_t vlc_interlace;
    uint8_t vcc;
    uint8_t hsync_counter;
    uint8_t vsync_counter;
    uint8_t vta_counter;

    uint16_t vma;
    uint16_t vma_row;

    bool last_row;
    bool previous_last_line;
    bool last_line;
    bool last_line_mgmt;

    bool interlace_sync;
    bool interlace_video;
    bool frame_odd;

    bool horizontal_de;
    bool vertical_de;
    bool in_hsync;
    bool in_vsync;
    bool in_last_vblank_line;

    bool cursor_enabled;
    bool cursor_active;
    bool cursor_blink_state;
    uint8_t cursor_start_line;
    uint8_t cursor_blink_mode;
} mc6845_core_t;

void mc6845_core_init(mc6845_core_t *core, uint8_t *external_register_file);
void mc6845_core_reset(mc6845_core_t *core);

void    mc6845_core_select_write(mc6845_core_t *core, uint8_t index, uint8_t value);
uint8_t mc6845_core_select_read(const mc6845_core_t *core, uint8_t index);
void    mc6845_core_latch_lightpen(mc6845_core_t *core);

void mc6845_core_get_outputs(const mc6845_core_t *core, mc6845_outputs_t *out);
void mc6845_core_tick(mc6845_core_t *core);

#ifdef __cplusplus
}
#endif

#endif
