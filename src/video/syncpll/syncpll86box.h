#ifndef SYNCPLL86BOX_H
#define SYNCPLL86BOX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum syncpll_signal_flags_t {
    SYNCPLL_SIGNAL_HSYNC = 1u << 0,
    SYNCPLL_SIGNAL_VSYNC = 1u << 1
} syncpll_signal_flags_t;

typedef struct syncpll_stats_t {
    uint64_t submitted_samples;
    uint64_t completed_scans;
    uint64_t rejected_hsync_edges;
    uint64_t rejected_vsync_edges;
    double horizontal_hz;
    double vertical_hz;
    double horizontal_lock;
    double vertical_lock;
    int live_active;
    int frame_ready;
    int presentation_w;
    int presentation_h;
} syncpll_stats_t;

/* Global lifetime, called from video.c. */
void syncpll_global_init(void);
void syncpll_global_shutdown(void);

/* Called by the normal 86Box blit thread. For live CGA this copies the most
   recent PLL-scanned raster into the ordinary target buffer. Other adapters
   pass through unchanged. */
void syncpll_process_blit(int x, int y, int w, int h, int monitor_index);

/* Continuous CGA video plus independent electrical sync levels. The source
   supplies no monitor frame boundary. H/V oscillator wrap is the only scan
   completion event. */
void syncpll_cga_submit_line(int monitor_index,
                             const uint32_t *xrgb,
                             const uint8_t *flags,
                             uint32_t dot_count,
                             uint64_t dot_clock_millihz);
void syncpll_cga_stream_reset(int monitor_index);

void syncpll_set_enabled(int enabled);
int syncpll_get_enabled(void);
int syncpll_get_stats(int monitor_index, syncpll_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* SYNCPLL86BOX_H */
