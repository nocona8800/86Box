#ifndef VIDEO_CGA_H
#define VIDEO_CGA_H

#include <86box/vid_mc6845.h>

/* Mode flags written through 3D8h. */
typedef enum cga_mode_flags_e {
    CGA_MODE_FLAG_HIGHRES          = 1 << 0,
    CGA_MODE_FLAG_GRAPHICS         = 1 << 1,
    CGA_MODE_FLAG_BW               = 1 << 2,
    CGA_MODE_FLAG_VIDEO_ENABLE     = 1 << 3,
    CGA_MODE_FLAG_HIGHRES_GRAPHICS = 1 << 4,
    CGA_MODE_FLAG_BLINK            = 1 << 5
} cga_mode_flags;

/* Motorola MC6845 CRTC registers. */
typedef enum cga_crtc_registers_e {
    CGA_CRTC_HTOTAL = 0x0,
    CGA_CRTC_HDISP = 0x1,
    CGA_CRTC_HSYNC_POS = 0x2,
    CGA_CRTC_HSYNC_WIDTH = 0x3,
    CGA_CRTC_VTOTAL = 0x4,
    CGA_CRTC_VTOTAL_ADJUST = 0x5,
    CGA_CRTC_VDISP = 0x6,
    CGA_CRTC_VSYNC = 0x7,
    CGA_CRTC_INTERLACE = 0x8,
    CGA_CRTC_MAX_SCANLINE_ADDR = 0x9,
    CGA_CRTC_CURSOR_START = 0xA,
    CGA_CRTC_CURSOR_END = 0xB,
    CGA_CRTC_START_ADDR_HIGH = 0xC,
    CGA_CRTC_START_ADDR_LOW = 0xD,
    CGA_CRTC_CURSOR_ADDR_HIGH = 0xE,
    CGA_CRTC_CURSOR_ADDR_LOW = 0xF,
    CGA_CRTC_LIGHT_PEN_ADDR_HIGH = 0x10,
    CGA_CRTC_LIGHT_PEN_ADDR_LOW = 0x11
} cga_crtc_registers;

/* CGA I/O registers. */
typedef enum cga_registers_e {
    CGA_REGISTER_CRTC_INDEX = 0x3D4,
    CGA_REGISTER_CRTC_DATA = 0x3D5,
    CGA_REGISTER_MODE_CONTROL = 0x3D8,
    CGA_REGISTER_COLOR_SELECT = 0x3D9,
    CGA_REGISTER_STATUS = 0x3DA,
    CGA_REGISTER_CLEAR_LIGHT_PEN_LATCH = 0x3DB,
    CGA_REGISTER_SET_LIGHT_PEN_LATCH = 0x3DC
} cga_registers;

#define CGA_NUM_CRTC_REGS 32

typedef struct cga_t {
    mem_mapping_t mapping;

    int     crtcreg;
    uint8_t crtc[CGA_NUM_CRTC_REGS];

    uint8_t cgastat;
    uint8_t cgamode;
    uint8_t cgacol;
    uint8_t lp_strobe;

    int fontbase;
    int linepos;
    int displine;
    int scanline;
    int vc;
    int cgadispon;
    int cursorvisible;
    int cursoron;
    int cgablink;
    int vsynctime;
    int vadj;
    uint16_t memaddr;
    uint16_t memaddr_backup;
    int oddeven;

    uint64_t dispontime;
    uint64_t dispofftime;
    pc_timer_t timer;

    int firstline;
    int lastline;
    int drawcursor;
    int fullchange;

    uint8_t *vram;
    uint8_t charbuffer[256];

    int revision;
    int composite;
    int snow_enabled;
    int rgb_type;
    int double_type;

    /* Character-clock MC6845 and legacy physical-monitor state. */
    mc6845_core_t precise_crtc;
    uint64_t precise_char_time;
    int precise_beam_x;
    int precise_beam_y;
    int precise_max_x;
    int precise_max_y;
    int precise_active_min_x;
    int precise_active_min_y;
    int precise_active_max_x;
    int precise_active_max_y;
    uint8_t precise_enabled;
    uint8_t precise_prev_hsync;
    uint8_t precise_prev_vsync;
    uint8_t precise_frame_valid;
    int precise_hblank_pixels;
    int precise_hblank_target;
    uint32_t precise_vsync_dots;
    uint8_t precise_monitor_hblank;
    uint8_t precise_monitor_vsync;
    uint8_t precise_frame_hires;
    uint8_t precise_mode;
    uint8_t precise_pending_mode;
    uint8_t precise_mode_pending;
    uint8_t precise_clock_high;
    uint8_t precise_clock_pending;
    uint8_t precise_monitor_locked;
    int precise_hsync_candidate_x;
    uint8_t precise_hsync_candidate_count;
    uint32_t precise_master_phase;
    uint8_t precise_sync_pending;
    uint8_t precise_card_vsync;

    /* Flat raw-dot line staging for the monitor sync-PLL CGA signal path. */
    uint8_t *precise_signal_color;
    uint8_t *precise_signal_flags;
    uint32_t *precise_signal_xrgb;
    uint32_t precise_signal_count;
    uint32_t precise_signal_capacity;
    /* IBM CGA monitor-sync shaper.  The MC6845 raw HS/VS outputs are not
     * the signals delivered to the display connector: HS is delayed by two
     * LCLKs and limited to four LCLKs, while monitor VS is aligned to shaped
     * HS and lasts three physical lines.  CRTC VS independently blanks video
     * for its full sixteen internal scanlines. */
    uint8_t precise_signal_raw_hsync;
    uint8_t precise_signal_raw_vsync;
    uint8_t precise_signal_hsync;
    uint8_t precise_signal_vsync_gate;
    uint8_t precise_signal_vsync_armed;
    uint8_t precise_signal_vsync_lines;
    uint16_t precise_signal_hsync_delay;
    uint16_t precise_signal_hsync_width;
    uint64_t precise_signal_present_dots;
} cga_t;

extern void cga_precise_init(cga_t *cga);
extern void cga_poll_precise(void *priv);
extern void cga_precise_mode_write(cga_t *cga, uint8_t mode);
extern void cga_precise_signal_flush(cga_t *cga);
extern void cga_init(cga_t *cga);
extern void cga_out(uint16_t addr, uint8_t val, void *priv);
extern uint8_t cga_in(uint16_t addr, void *priv);
extern void cga_write(uint32_t addr, uint8_t val, void *priv);
extern uint8_t cga_read(uint32_t addr, void *priv);
extern void cga_recalctimings(cga_t *cga);
extern void cga_interpolate_init(void);
extern void cga_blit_memtoscreen(int x, int y, int w, int h, int double_type);
extern void cga_do_blit(int vid_xsize, int firstline, int lastline,
                        int double_type);
extern void cga_poll(void *priv);

#endif /* VIDEO_CGA_H */
