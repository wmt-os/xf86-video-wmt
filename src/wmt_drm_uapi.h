/*
 * Userspace mirror of the WonderMedia WM8505 wmt-drm GE command interface.
 *
 * Kept byte-compatible with the kernel's drivers/gpu/drm/wmt/wmt_drm.h.
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#ifndef WMT_DRM_UAPI_H
#define WMT_DRM_UAPI_H

#include <stdint.h>
#include <drm.h>

/* wmt_ge_op.type */
#define WMT_GE_OP_FILL		0x1
#define WMT_GE_OP_BLIT		0x2

/* GE raster operation codes (8-bit ROP) */
#define WMT_GE_ROP_PAT_XOR	0x5a	/* P XOR D              */
#define WMT_GE_ROP_SRC_XOR	0x66	/* S XOR D              */
#define WMT_GE_ROP_SRC_COPY	0xcc	/* S                    */
#define WMT_GE_ROP_PAT_COPY	0xf0	/* P (pattern/solid)    */

/* Hardware limits enforced by the kernel bounds check */
#define WMT_GE_MAX_DIM		2048
#define WMT_GE_MAX_OPS		8192

/* One 2D Graphics Engine operation. */
struct wmt_ge_op {
	uint32_t type;
	uint32_t rop;
	uint32_t dest_handle;
	uint32_t dest_pitch;
	uint32_t dest_x;
	uint32_t dest_y;
	uint32_t width;
	uint32_t height;
	uint32_t color;
	uint32_t src_handle;
	uint32_t src_pitch;
	uint32_t src_x;
	uint32_t src_y;
};

/* Request wrapper for a batch of GE operations. */
struct wmt_ge_batch_req {
	struct wmt_ge_op *ops;
	uint32_t num_ops;
};

#define DRM_WMT_GE_BATCH	0x0
#define DRM_IOCTL_WMT_GE_BATCH \
	DRM_IOW(DRM_COMMAND_BASE + DRM_WMT_GE_BATCH, struct wmt_ge_batch_req)

#endif /* WMT_DRM_UAPI_H */
