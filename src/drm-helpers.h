#include <obs/graphics/graphics.h>

#include <drm/drm_fourcc.h>
#include <stdint.h>

const char *getDrmFourccName(uint32_t drm_fourcc);
enum gs_color_format drmFourccToGs(uint32_t drm_fourcc);
