/*
 * WonderMedia WM8505 X.Org video driver -- GEM dumb buffer helpers.
 *
 * The 2D engine addresses surfaces by GEM handle (no byte offset), so every
 * accelerated surface is its own physically-contiguous dumb (CMA) buffer.
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "wmt.h"

/* Allocate a 32-bpp dumb buffer of the given pixel dimensions. */
WMTBO *
wmt_bo_create(int fd, int width, int height)
{
	struct drm_mode_create_dumb arg;
	WMTBO *bo;

	bo = calloc(1, sizeof(*bo));
	if (!bo)
		return NULL;

	memset(&arg, 0, sizeof(arg));
	arg.width = width;
	arg.height = height;
	arg.bpp = WMT_BPP;

	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &arg)) {
		free(bo);
		return NULL;
	}

	bo->handle = arg.handle;
	bo->pitch = arg.pitch;
	bo->size = arg.size;
	bo->width = width;
	bo->height = height;
	return bo;
}

/* Lazily mmap the buffer for CPU access; returns the cached mapping. */
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

/* Wrap the buffer in a KMS framebuffer object so it can be scanned out. */
Bool
wmt_bo_add_fb(int fd, WMTBO *bo)
{
	if (bo->fb_id)
		return TRUE;

	if (drmModeAddFB(fd, bo->width, bo->height, WMT_DEPTH, WMT_BPP,
			 bo->pitch, bo->handle, &bo->fb_id)) {
		bo->fb_id = 0;
		return FALSE;
	}
	return TRUE;
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
