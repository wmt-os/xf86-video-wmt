/*
 * WonderMedia WM8505 X.Org video driver.
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

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "wmt_drm_uapi.h"

#define WMT_DRIVER_NAME	"wmt"
#define WMT_VERSION_MAJOR 0
#define WMT_VERSION_MINOR 1
#define WMT_VERSION_PATCH 0
#define WMT_VERSION_CURRENT \
	((WMT_VERSION_MAJOR << 20) | (WMT_VERSION_MINOR << 10) | WMT_VERSION_PATCH)

/* The GE and scanout always operate at 32 bpp / depth 24. */
#define WMT_BPP		32
#define WMT_DEPTH	24

/* A GEM dumb buffer object. */
typedef struct wmt_bo {
	uint32_t	handle;		/* GEM handle on the DRM fd            */
	uint32_t	pitch;		/* row stride in bytes                 */
	uint64_t	size;		/* total allocation size in bytes      */
	int		width;
	int		height;
	uint32_t	fb_id;		/* drmModeAddFB id, or 0 if none       */
	void	       *map;		/* mmap'd CPU pointer, or NULL         */
	int		map_refcnt;	/* nested PrepareAccess count          */
} WMTBO;

/* Per-pixmap driver private (EXA_HANDLES_PIXMAPS). */
typedef struct {
	WMTBO	       *bo;		/* GE-addressable buffer, or NULL...   */
	void	       *sysmem;		/* ...software backing when not a BO   */
	int		pitch;		/* row stride in bytes                 */
	int		width;
	int		height;
	int		bpp;
} WMTPixmapPriv;

typedef struct {
	int		fd;		/* DRM master fd                       */
	Bool		fd_owned;	/* did we open it ourselves?           */
	char	       *kmsdev;		/* device node path                    */
	EntityInfoPtr	pEnt;
	OptionInfoPtr	Options;

	Bool		accel;		/* GE acceleration enabled             */
	Bool		tearfree;	/* TearFree page-flipping enabled      */
	Bool		ge_overlap_ok;	/* GE self-handles overlapping blits   */

	/* Scanout buffers: [0] is always the displayed front buffer; [1]   */
	/* exists only when TearFree is active.                             */
	WMTBO	       *scanout[2];
	int		nscanout;
	int		current;	/* index of the displayed buffer       */

	/* EXA */
	ExaDriverPtr	exa;
	WMTPixmapPriv  *root_priv;	/* screen pixmap private, for teardown */
	struct wmt_ge_op *batch;	/* op accumulation buffer              */
	unsigned	batch_count;
	unsigned	batch_max;

	/* State captured by the current PrepareSolid/PrepareCopy.          */
	WMTBO	       *op_dst_bo;
	uint32_t	op_dst_pitch;
	uint32_t	op_rop;
	uint32_t	op_fg;
	WMTBO	       *op_src_bo;
	uint32_t	op_src_pitch;

	/* TearFree damage tracking */
	DamagePtr	damage;
	Bool		flip_pending;

	/* Wrapped screen entry points */
	CloseScreenProcPtr		CloseScreen;
	CreateScreenResourcesProcPtr	CreateScreenResources;
	ScreenBlockHandlerProcPtr	BlockHandler;
} WMTRec, *WMTPtr;

#define WMTPTR(scrn)		((WMTPtr)((scrn)->driverPrivate))
#define WMT_PIXMAP_PRIV(pPix)	((WMTPixmapPriv *)exaGetPixmapDriverPrivate(pPix))

/* wmt_bo.c -- GEM dumb buffer helpers */
WMTBO	*wmt_bo_create(int fd, int width, int height);
void	 wmt_bo_destroy(int fd, WMTBO *bo);
void	*wmt_bo_map(int fd, WMTBO *bo);
Bool	 wmt_bo_add_fb(int fd, WMTBO *bo);

/* wmt_kms.c -- KMS / RandR-1.2 backend */
Bool	 WMTKMSPreInit(ScrnInfoPtr pScrn);
Bool	 WMTKMSScreenInit(ScreenPtr pScreen);
void	 WMTKMSCloseScreen(ScreenPtr pScreen);
Bool	 WMTKMSEnterVT(ScrnInfoPtr pScrn);
void	 WMTKMSLeaveVT(ScrnInfoPtr pScrn);

/* wmt_exa.c -- EXA acceleration over the GE batch IOCTL */
Bool	 WMTExaInit(ScreenPtr pScreen);
void	 WMTExaCloseScreen(ScreenPtr pScreen);
void	 wmt_ge_flush(WMTPtr wmt);		/* submit any queued ops    */

#endif /* WMT_H */
