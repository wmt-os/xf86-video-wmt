/*
 * WonderMedia WM8505 X.Org Video Driver
 *
 * Mirror of the WonderMedia WM8505 DRM/KMS Userspace ABI
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#ifndef WMT_DRM_UAPI_H
#define WMT_DRM_UAPI_H

#include <stdint.h>
#include <drm.h>

/* Limits */
#define WMT_GE_MAX_DIM		2048
#define WMT_GE_MAX_OPS		8192

/* OP Types */
#define WMT_GE_OP_FILL		0x1
#define WMT_GE_OP_BLIT		0x2

/* ROP Codes */
#define WMT_GE_ROP_PAT_XOR	0x5a
#define WMT_GE_ROP_SRC_XOR	0x66
#define WMT_GE_ROP_SRC_COPY	0xcc
#define WMT_GE_ROP_PAT_COPY	0xf0

/* GE Operation */
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

/* GE Submit Request */
struct wmt_ge_submit {
	uint64_t ops;
	uint32_t num_ops;
	uint32_t flags;
	uint32_t out_seqno;
	uint32_t pad;
};

/* GE Wait Request */
struct wmt_ge_wait {
	uint32_t seqno;
	uint32_t timeout_us;
};

#define DRM_WMT_GE_SUBMIT	0x00
#define DRM_WMT_GE_WAIT		0x01

#define DRM_IOCTL_WMT_GE_SUBMIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_WMT_GE_SUBMIT, struct wmt_ge_submit)
#define DRM_IOCTL_WMT_GE_WAIT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_WMT_GE_WAIT, struct wmt_ge_wait)

#endif /* WMT_DRM_UAPI_H */
