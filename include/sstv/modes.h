#ifndef SSTV_MODES_H
#define SSTV_MODES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSTV_SAMPLE_RATE 12800.0

typedef enum {
    SSTV_ENC_GBR,
    SSTV_ENC_RGB,
    SSTV_ENC_YUV_ROBOT,
    SSTV_ENC_YUV_ROBOT422,
    SSTV_ENC_YUV_PD,
    SSTV_ENC_BW
} sstv_encoding_t;

typedef struct {
    const char     *name;
    const char     *short_name;
    double          sync_time;
    double          porch_time;
    double          separator_time;
    double          pixel_time;
    double          line_time;
    int             width;
    int             transmitted_lines;
    int             line_height;
    sstv_encoding_t encoding;
    double          chroma_marker_time;
    double          chroma_porch_time;
    int             vis_code;
} sstv_mode_t;

size_t             sstv_mode_count(void);
const sstv_mode_t *sstv_mode_at(size_t index);
const sstv_mode_t *sstv_mode_find(const char *short_name);
const sstv_mode_t *sstv_mode_from_vis(int vis_code);
int                sstv_mode_height(const sstv_mode_t *mode);
const char        *sstv_encoding_name(sstv_encoding_t encoding);

#ifdef __cplusplus
}
#endif

#endif
