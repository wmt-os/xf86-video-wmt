/*
 * WonderMedia WM8505 X.Org Video Driver
 *
 * GEM Dumb Buffer Helpers
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "wmt.h"

WMTBO *
wmt_bo_create(int fd, int width, int height, uint32_t format)
{
	struct drm_mode_create_dumb arg;
	WMTBO *bo;

	bo = calloc(1, sizeof(*bo));
	if (!bo)
		return NULL;

	memset(&arg, 0, sizeof(arg));
	arg.width = width;
	arg.height = height;
	arg.bpp = format == WMT_FORMAT ? WMT_BPP : WMT_SCANOUT_BPP;

	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &arg)) {
		free(bo);
		return NULL;
	}

	bo->handle = arg.handle;
	bo->format = format;
	bo->pitch = arg.pitch;
	bo->size = arg.size;
	bo->width = width;
	bo->height = height;
	return bo;
}

static Bool
wmt_bo_add_fb(int fd, WMTBO *bo)
{
	uint32_t handles[4] = { bo->handle };
	uint32_t pitches[4] = { bo->pitch };
	uint32_t offsets[4] = { 0 };

	if (bo->fb_id)
		return TRUE;

	if (drmModeAddFB2(fd, bo->width, bo->height, bo->format, handles, pitches,
			  offsets, &bo->fb_id, 0)) {
		bo->fb_id = 0;
		return FALSE;
	}
	return TRUE;
}

WMTBO *
wmt_bo_new(int fd, int width, int height, Bool scanout)
{
	WMTBO *bo = wmt_bo_create(fd, width, height,
				  scanout ? WMT_SCANOUT_FORMAT : WMT_FORMAT);

	if (!bo)
		return NULL;
	if (!wmt_bo_map(fd, bo) || (scanout && !wmt_bo_add_fb(fd, bo))) {
		wmt_bo_destroy(fd, bo);
		return NULL;
	}
	memset(bo->map, 0, bo->size);
	return bo;
}

void *
wmt_bo_map(int fd, WMTBO *bo)
{
	struct drm_mode_map_dumb arg;
	void *map;

	if (bo->map)
		return bo->map;

	memset(&arg, 0, sizeof(arg));
	arg.handle = bo->handle;
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &arg))
		return NULL;

	map = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, arg.offset);
	if (map == MAP_FAILED)
		return NULL;

	bo->map = map;
	return map;
}

void
wmt_bo_destroy(int fd, WMTBO *bo)
{
	struct drm_mode_destroy_dumb arg;

	if (!bo)
		return;

	if (bo->fb_id)
		drmModeRmFB(fd, bo->fb_id);
	if (bo->map)
		munmap(bo->map, bo->size);

	memset(&arg, 0, sizeof(arg));
	arg.handle = bo->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &arg);

	free(bo);
}
