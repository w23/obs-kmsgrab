#pragma once

#include <stdint.h>

/* This defines an interface between obs-drmsend and obs. */

#define OBS_DRMSEND_MAX_FRAMEBUFFERS 16
#define OBS_DRMSEND_TAG 0x0b500002u

typedef struct {
	uint32_t fb_id;
	int width, height;
	uint32_t fourcc;
	int planes;

	int fd_indexes[4];
	uint32_t pitches[4];
	uint32_t offsets[4];
	uint64_t modifiers[4];

	/* fds are delivered OOB using control msg */
} drmsend_framebuffer_t;

typedef struct {
	unsigned tag;
	int num_framebuffers;
	int num_fds;
	drmsend_framebuffer_t framebuffers[OBS_DRMSEND_MAX_FRAMEBUFFERS];
} drmsend_response_t;
