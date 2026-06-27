/*
 * WonderMedia WM8505 X.Org Video Driver
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#ifndef WMT_H
#define WMT_H

#include <stdint.h>

#include "xf86.h"
#include "xf86Crtc.h"
#include "exa.h"
#include "damage.h"
#include "regionstr.h"

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "wmt_drm_uapi.h"

#define WMT_DRIVER_NAME		"wmt"
#define WMT_VERSION_MAJOR	1
#define WMT_VERSION_MINOR	0
#define WMT_VERSION_PATCH	0

#define WMT_BPP				32
#define WMT_DEPTH			24
#define WMT_BYTES_PP		(WMT_BPP / 8)	/* Bytes per pixel */

/* GEM dumb buffer */
typedef struct wmt_bo {
	uint32_t			handle;				/* GEM handle */
	uint32_t			pitch;				/* Row stride in bytes */
	uint64_t			size;				/* Allocation size in bytes */
	int					width;
	int					height;
	uint32_t			fb_id;				/* KMS framebuffer ID, 0 if none */
	void				*map;				/* CPU mapping pointer, NULL if unmapped */
	uint32_t			last_seqno;			/* Seqno of last GE batch to touch buffer */
	uint32_t			last_synced;		/* Highest seqno already synced */
} WMTBO;

/* Pixmap private */
typedef struct {
	WMTBO				*bo;				/* GE buffer object */
	int					pitch;				/* Row stride in bytes */
} WMTPixmapPriv;

typedef struct {
	int					fd;					/* DRM master fd */
	Bool				fd_owned;			/* True if opened by driver */
	char				*kmsdev;			/* DRM device path */
	EntityInfoPtr		pEnt;
	OptionInfoPtr		Options;

	Bool				accel;				/* 2D acceleration enabled */
	Bool				tearfree;			/* TearFree page-flipping enabled */
	Bool				ge_overlap_ok;		/* True if hardware supports overlapping copies */

	WMTBO				*scanout[2];		/* Front/back scanout buffers */
	WMTBO				*screen_bo;			/* Root pixmap target (shadow or scanout) */
	int					current;			/* Displayed scanout index */
	uint32_t			crtc_id;			/* CRTC ID for page flips */
	int					mode_w, mode_h;		/* Screen dimensions */
	Bool				dpms_off;			/* Screen blanked by DPMS */

	/* EXA */
	ExaDriverPtr		exa;
	Bool				screen_bound;		/* Root pixmap bound to screen_bo */
	struct wmt_ge_op	*batch;				/* Op accumulation buffer */
	unsigned			batch_count;
	unsigned			batch_max;
	WMTBO				*batch_dst_bo;		/* Destination buffer of queued batch */
	WMTBO				*batch_src_bo;		/* Source buffer of queued batch */
	uint32_t			last_submit_seqno;	/* Seqno of last submitted batch */
	uint32_t			last_synced_seqno;	/* Highest seqno already synced */

	/* Render state */
	WMTBO				*op_dst_bo;
	uint32_t			op_dst_pitch;
	uint32_t			op_rop;
	uint32_t			op_fg;
	WMTBO				*op_src_bo;
	uint32_t			op_src_pitch;

	/* Page flips */
	DamagePtr			damage;				/* Damage region tracking */
	RegionRec			flip_region;		/* Damage owed to the alternate buffer */
	Bool				flip_pending;		/* Page flip event outstanding */

	/* Wrapped functions */
	CloseScreenProcPtr				CloseScreen;
	CreateScreenResourcesProcPtr	CreateScreenResources;
	ScreenBlockHandlerProcPtr		BlockHandler;
} WMTRec, *WMTPtr;

#define WMTPTR(scrn)			((WMTPtr)((scrn)->driverPrivate))
#define WMT_PIXMAP_PRIV(pPix)	((WMTPixmapPriv *)exaGetPixmapDriverPrivate(pPix))

static inline Bool
wmt_ge_passed(uint32_t done, uint32_t target)
{
	return (int32_t)(done - target) >= 0;
}

/* wmt_bo.c */
WMTBO	*wmt_bo_create(int fd, int width, int height);
WMTBO	*wmt_bo_new(int fd, int width, int height, Bool scanout);
void	 wmt_bo_destroy(int fd, WMTBO *bo);
void	*wmt_bo_map(int fd, WMTBO *bo);
Bool	 wmt_bo_add_fb(int fd, WMTBO *bo);

/* wmt_kms.c */
Bool	 WMTKMSPreInit(ScrnInfoPtr pScrn);
Bool	 WMTKMSScreenInit(ScreenPtr pScreen);
Bool	 WMTKMSEnterVT(ScrnInfoPtr pScrn);
void	 WMTKMSLeaveVT(ScrnInfoPtr pScrn);

/* wmt_exa.c */
Bool	 WMTExaInit(ScreenPtr pScreen);
void	 WMTExaCloseScreen(ScreenPtr pScreen);
void	 wmt_ge_flush(WMTPtr wmt);
void	 wmt_ge_sync(WMTPtr wmt, WMTBO *bo);
void	 wmt_ge_blit(WMTPtr wmt, WMTBO *src, WMTBO *dst,
		     int x, int y, int w, int h);

/* wmt_present.c */
void	 WMTFlipInit(ScreenPtr pScreen);
void	 WMTFlipFini(ScreenPtr pScreen);
void	 WMTFlipDrain(WMTPtr wmt);

#endif /* WMT_H */
