/*
 * Userspace mirror of the WonderMedia WM8505 wmt-drm GE command interface.
 *
 * Kept byte-compatible with the kernel's include/uapi/drm/wmt_drm.h (driver
 * major 2: asynchronous job ring + cached BOs + cache-sync).
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#ifndef WMT_DRM_UAPI_H
#define WMT_DRM_UAPI_H

#include <stdint.h>
#include <drm.h>

/* Limits enforced by the kernel */
#define WMT_GE_MAX_DIM		2048	/* max width/height of an op or surface */
#define WMT_GE_MAX_OPS		8192	/* max ops per submit / rects per sync */

/* wmt_ge_op.type */
#define WMT_GE_OP_FILL		0x1
#define WMT_GE_OP_BLIT		0x2

/* wmt_ge_op.rop: 8-bit ROP3 (0 selects the op's default) */
#define WMT_GE_ROP_PAT_XOR	0x5a	/* P ^ D */
#define WMT_GE_ROP_SRC_XOR	0x66	/* S ^ D */
#define WMT_GE_ROP_SRC_COPY	0xcc	/* S */
#define WMT_GE_ROP_PAT_COPY	0xf0	/* P (pattern / solid) */

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

/* Queue a batch of ops against one dst BO (+ at most one src); returns a job seqno, never blocks. */
struct wmt_ge_submit {
	uint64_t ops;		/* pointer to struct wmt_ge_op[num_ops] */
	uint32_t num_ops;
	uint32_t flags;		/* must be 0 */
	uint32_t out_seqno;	/* OUT: this job's completion seqno (valid only on success) */
	uint32_t pad;		/* must be 0 */
};

/* Block until the GE completes seqno; seqno 0 = block until a ring slot frees (backpressure). */
struct wmt_ge_wait {
	uint32_t seqno;
	uint32_t timeout_us;	/* 0 selects the default */
};

/* wmt_gem_create.flags */
#define WMT_GEM_CACHED		0x1	/* CPU-cached (composite target); else write-combine */

/* Allocate a GE buffer. */
struct wmt_gem_create {
	uint32_t size;
	uint32_t flags;
	uint32_t handle;	/* OUT */
	uint32_t pad;		/* must be 0 */
};

/* wmt_gem_sync.flags: exactly one direction. */
#define WMT_SYNC_FOR_GE		0x1	/* hand CPU-composited rows to the GE */
#define WMT_SYNC_FOR_CPU	0x2	/* hand GE-written rows back to the CPU */

/* Damage rectangle; rows [y, y + height) are synced, x/width ignored. */
struct wmt_rect {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

/* Cache-maintain a cached BO's damaged rows across a CPU<->GE handoff. */
struct wmt_gem_sync {
	uint64_t rects;		/* pointer to struct wmt_rect[num_rects] */
	uint32_t handle;
	uint32_t flags;
	uint32_t pitch;
	uint32_t num_rects;
	uint32_t wait_seqno;	/* FOR_CPU: wait this GE seqno before invalidating; 0 = skip */
	uint32_t pad;		/* must be 0 */
};

#define DRM_WMT_GE_SUBMIT	0x00
#define DRM_WMT_GE_WAIT		0x01
#define DRM_WMT_GEM_CREATE	0x02
#define DRM_WMT_GEM_SYNC	0x03

#define DRM_IOCTL_WMT_GE_SUBMIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_WMT_GE_SUBMIT, struct wmt_ge_submit)
#define DRM_IOCTL_WMT_GE_WAIT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_WMT_GE_WAIT, struct wmt_ge_wait)
#define DRM_IOCTL_WMT_GEM_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_WMT_GEM_CREATE, struct wmt_gem_create)
#define DRM_IOCTL_WMT_GEM_SYNC \
	DRM_IOW(DRM_COMMAND_BASE + DRM_WMT_GEM_SYNC, struct wmt_gem_sync)

#endif /* WMT_DRM_UAPI_H */
