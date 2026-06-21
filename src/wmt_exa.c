/*
 * WonderMedia WM8505 X.Org video driver -- EXA acceleration.
 *
 * Maps EXA's solid-fill and copy primitives onto the kernel's GE command
 * batch IOCTL (DRM_IOCTL_WMT_GE_BATCH).  Every accelerated pixmap is its own
 * GEM dumb buffer (the GE addresses surfaces by handle), so this uses the
 * EXA_HANDLES_PIXMAPS model and falls back to ordinary system-memory pixmaps
 * when a contiguous buffer cannot be obtained.
 *
 * The engine is a 32-bpp fill/copy/XOR unit with no alpha blending, so Render
 * compositing is left to the X server; the resulting blits to screen are
 * accelerated here.
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include "wmt.h"

/* Write-combined buffer drain: ensure CPU writes to a BO are visible to the
 * GE before we kick off a DMA that reads them. */
static inline void
wmt_wc_barrier(void)
{
	__sync_synchronize();
}

/*
 * Map an X11 GX raster op to a GE 8-bit ROP code.  The GE blends either the
 * pattern (solid fills) or the source (copies) against the destination.
 * Returns -1 for ops we don't accelerate (EXA then falls back to software).
 */
static int
wmt_solid_rop(int alu)
{
	switch (alu) {
	case GXcopy:	return WMT_GE_ROP_PAT_COPY;	/* P        */
	case GXxor:	return WMT_GE_ROP_XOR;		/* P XOR D  */
	default:	return -1;
	}
}

static int
wmt_copy_rop(int alu)
{
	switch (alu) {
	case GXcopy:	return WMT_GE_ROP_SRC_COPY;	/* S        */
	case GXxor:	return 0x66;			/* S XOR D  */
	default:	return -1;
	}
}

/* Submit any queued GE operations.  The IOCTL is synchronous: on return the
 * engine is idle, so no separate fence is required. */
void
wmt_ge_flush(WMTPtr wmt)
{
	struct wmt_ge_batch_req req;

	if (wmt->batch_count == 0)
		return;

	wmt_wc_barrier();

	req.ops = wmt->batch;
	req.num_ops = wmt->batch_count;

	if (drmCommandWrite(wmt->fd, DRM_WMT_GE_BATCH, &req, sizeof(req)) != 0)
		xf86DrvMsgVerb(0, X_WARNING, 3, "wmt: GE batch of %u ops failed\n",
			       wmt->batch_count);

	wmt->batch_count = 0;
}

/* Reserve the next op slot, flushing first if the batch is full. */
static struct wmt_ge_op *
wmt_ge_next(WMTPtr wmt)
{
	if (wmt->batch_count >= wmt->batch_max)
		wmt_ge_flush(wmt);
	return &wmt->batch[wmt->batch_count++];
}

/* ------------------------------------------------------------------ Solid */

static Bool
WMTPrepareSolid(PixmapPtr pPix, int alu, Pixel planemask, Pixel fg)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pPix->drawable.pScreen));
	WMTPixmapPriv *priv = WMT_PIXMAP_PRIV(pPix);
	int rop;

	if (!priv || !priv->bo)
		return FALSE;
	if (!EXA_PM_IS_SOLID(&pPix->drawable, planemask))
		return FALSE;

	rop = wmt_solid_rop(alu);
	if (rop < 0)
		return FALSE;

	wmt->op_dst_bo = priv->bo;
	wmt->op_dst_pitch = exaGetPixmapPitch(pPix);
	wmt->op_rop = (rop == WMT_GE_ROP_PAT_COPY) ? 0 : (uint32_t)rop;
	wmt->op_fg = fg;
	return TRUE;
}

static void
WMTSolid(PixmapPtr pPix, int x1, int y1, int x2, int y2)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pPix->drawable.pScreen));
	struct wmt_ge_op *op = wmt_ge_next(wmt);

	memset(op, 0, sizeof(*op));
	op->type = WMT_GE_OP_FILL;
	op->rop = wmt->op_rop;
	op->dest_handle = wmt->op_dst_bo->handle;
	op->dest_pitch = wmt->op_dst_pitch;
	op->dest_x = x1;
	op->dest_y = y1;
	op->width = x2 - x1;
	op->height = y2 - y1;
	op->color = wmt->op_fg;
}

static void
WMTDoneSolid(PixmapPtr pPix)
{
	wmt_ge_flush(WMTPTR(xf86ScreenToScrn(pPix->drawable.pScreen)));
}

/* ------------------------------------------------------------------- Copy */

static Bool
WMTPrepareCopy(PixmapPtr pSrc, PixmapPtr pDst, int dx, int dy,
	       int alu, Pixel planemask)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pDst->drawable.pScreen));
	WMTPixmapPriv *s = WMT_PIXMAP_PRIV(pSrc);
	WMTPixmapPriv *d = WMT_PIXMAP_PRIV(pDst);
	int rop;

	if (!s || !s->bo || !d || !d->bo)
		return FALSE;
	if (!EXA_PM_IS_SOLID(&pDst->drawable, planemask))
		return FALSE;

	rop = wmt_copy_rop(alu);
	if (rop < 0)
		return FALSE;

	/* If the engine doesn't self-arbitrate overlapping blits, refuse the
	 * reverse-direction self-copies that would otherwise corrupt. */
	if (!wmt->ge_overlap_ok && s->bo == d->bo && (dx < 0 || dy < 0))
		return FALSE;

	wmt->op_src_bo = s->bo;
	wmt->op_src_pitch = exaGetPixmapPitch(pSrc);
	wmt->op_dst_bo = d->bo;
	wmt->op_dst_pitch = exaGetPixmapPitch(pDst);
	wmt->op_rop = (rop == WMT_GE_ROP_SRC_COPY) ? 0 : (uint32_t)rop;
	return TRUE;
}

static void
WMTCopy(PixmapPtr pDst, int srcX, int srcY, int dstX, int dstY, int w, int h)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pDst->drawable.pScreen));
	struct wmt_ge_op *op = wmt_ge_next(wmt);

	memset(op, 0, sizeof(*op));
	op->type = WMT_GE_OP_BLIT;
	op->rop = wmt->op_rop;
	op->dest_handle = wmt->op_dst_bo->handle;
	op->dest_pitch = wmt->op_dst_pitch;
	op->dest_x = dstX;
	op->dest_y = dstY;
	op->width = w;
	op->height = h;
	op->src_handle = wmt->op_src_bo->handle;
	op->src_pitch = wmt->op_src_pitch;
	op->src_x = srcX;
	op->src_y = srcY;
}

static void
WMTDoneCopy(PixmapPtr pDst)
{
	wmt_ge_flush(WMTPTR(xf86ScreenToScrn(pDst->drawable.pScreen)));
}

/* --------------------------------------------------------------- Sync */

static void
WMTWaitMarker(ScreenPtr pScreen, int marker)
{
	wmt_ge_flush(WMTPTR(xf86ScreenToScrn(pScreen)));
}

/* ------------------------------------------------------ Pixmap management */

static int
wmt_sw_pitch(int width, int bpp)
{
	return (((width * bpp + 7) / 8) + 3) & ~3;
}

static void *
WMTCreatePixmap2(ScreenPtr pScreen, int width, int height, int depth,
		 int usage_hint, int bitsPerPixel, int *new_fb_pitch)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pScreen));
	WMTPixmapPriv *priv;

	priv = calloc(1, sizeof(*priv));
	if (!priv)
		return NULL;

	priv->width = width;
	priv->height = height;
	priv->bpp = bitsPerPixel;

	/* Deferred (headless) pixmap: keep the private, allocate later. */
	if (width == 0 || height == 0) {
		*new_fb_pitch = 0;
		return priv;
	}

	/* Accelerable surfaces are 32-bpp and within the GE's coordinate
	 * limits.  Anything else (or a CMA shortfall) becomes a software
	 * pixmap that the X server renders directly. */
	if (wmt->accel && bitsPerPixel == WMT_BPP &&
	    width <= WMT_GE_MAX_DIM && height <= WMT_GE_MAX_DIM) {
		WMTBO *bo = wmt_bo_create(wmt->fd, width, height);

		if (bo) {
			priv->bo = bo;
			priv->pitch = bo->pitch;
			*new_fb_pitch = bo->pitch;
			return priv;
		}
	}

	priv->pitch = wmt_sw_pitch(width, bitsPerPixel);
	priv->sysmem = malloc((size_t)priv->pitch * height);
	if (!priv->sysmem) {
		free(priv);
		return NULL;
	}
	*new_fb_pitch = priv->pitch;
	return priv;
}

static void
WMTDestroyPixmap(ScreenPtr pScreen, void *driverPriv)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pScreen));
	WMTPixmapPriv *priv = driverPriv;

	if (!priv)
		return;

	/* The scanout buffers are owned by the KMS layer, never by a pixmap. */
	if (priv->bo && priv->bo != wmt->scanout[0] && priv->bo != wmt->scanout[1])
		wmt_bo_destroy(wmt->fd, priv->bo);
	free(priv->sysmem);
	free(priv);
}

static Bool
WMTModifyPixmapHeader(PixmapPtr pPix, int width, int height, int depth,
		      int bitsPerPixel, int devKind, void *pPixData)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pPix->drawable.pScreen));
	WMTPixmapPriv *priv = WMT_PIXMAP_PRIV(pPix);

	if (width > 0)
		pPix->drawable.width = width;
	if (height > 0)
		pPix->drawable.height = height;
	if (depth > 0)
		pPix->drawable.depth = depth;
	if (bitsPerPixel > 0)
		pPix->drawable.bitsPerPixel = bitsPerPixel;
	if (devKind > 0)
		pPix->devKind = devKind;
	pPix->drawable.serialNumber = NEXT_SERIAL_NUMBER;

	if (!priv)
		return TRUE;

	if (width > 0)
		priv->width = width;
	if (height > 0)
		priv->height = height;
	if (bitsPerPixel > 0)
		priv->bpp = bitsPerPixel;

	if (pPixData) {
		/* Server is wrapping its own memory: demote to software. */
		if (priv->bo && priv->bo != wmt->scanout[0] &&
		    priv->bo != wmt->scanout[1])
			wmt_bo_destroy(wmt->fd, priv->bo);
		free(priv->sysmem);	/* release any driver-owned buffer */
		priv->bo = NULL;
		priv->sysmem = NULL;	/* new backing is externally owned */
		if (devKind > 0)
			priv->pitch = devKind;
		pPix->devPrivate.ptr = pPixData;
	} else if (priv->bo) {
		pPix->devKind = priv->pitch ? priv->pitch : (int)priv->bo->pitch;
		pPix->devPrivate.ptr = NULL;	/* offscreen: mapped on demand */
	} else if (priv->sysmem) {
		pPix->devKind = priv->pitch;
		pPix->devPrivate.ptr = priv->sysmem;
	}
	return TRUE;
}

static Bool
WMTPixmapIsOffscreen(PixmapPtr pPix)
{
	WMTPixmapPriv *priv = WMT_PIXMAP_PRIV(pPix);

	return priv && priv->bo != NULL;
}

static Bool
WMTPrepareAccess(PixmapPtr pPix, int index)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pPix->drawable.pScreen));
	WMTPixmapPriv *priv = WMT_PIXMAP_PRIV(pPix);
	void *map;

	if (!priv || !priv->bo)
		return TRUE;		/* software pixmap: ptr already valid */

	map = wmt_bo_map(wmt->fd, priv->bo);
	if (!map)
		return FALSE;

	pPix->devPrivate.ptr = map;
	return TRUE;
}

static void
WMTFinishAccess(PixmapPtr pPix, int index)
{
	WMTPixmapPriv *priv = WMT_PIXMAP_PRIV(pPix);

	/* Keep the mapping cached in the BO; just drop the per-access pointer
	 * for offscreen pixmaps (EXA also clears it). */
	if (priv && priv->bo)
		pPix->devPrivate.ptr = NULL;
}

/* ----------------------------------------------------- Overlap self-test */

/*
 * The GE op interface exposes no copy-direction control, so determine once at
 * start-up whether the engine arbitrates overlapping blits internally.  A
 * forward-only engine would corrupt a down-right self-copy; detect that by
 * copying a uniquely-valued block onto itself shifted by (1,1) and checking
 * the far corner (which a naive forward copy would smear).
 */
static void
wmt_ge_overlap_selftest(ScrnInfoPtr pScrn)
{
	WMTPtr wmt = WMTPTR(pScrn);
	const int W = 64, H = 64;
	WMTBO *bo;
	uint32_t *p;
	struct wmt_ge_op op;
	struct wmt_ge_batch_req req;
	int stride, x, y;
	Bool ok = FALSE;

	wmt->ge_overlap_ok = FALSE;

	bo = wmt_bo_create(wmt->fd, W, H);
	if (!bo)
		goto done;
	p = wmt_bo_map(wmt->fd, bo);
	if (!p)
		goto done;

	stride = bo->pitch / 4;
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			p[y * stride + x] = 0xff000000u | (uint32_t)(y * W + x);

	memset(&op, 0, sizeof(op));
	op.type = WMT_GE_OP_BLIT;
	op.rop = WMT_GE_ROP_SRC_COPY;
	op.dest_handle = op.src_handle = bo->handle;
	op.dest_pitch = op.src_pitch = bo->pitch;
	op.src_x = 0;
	op.src_y = 0;
	op.dest_x = 1;
	op.dest_y = 1;
	op.width = W - 1;
	op.height = H - 1;

	req.ops = &op;
	req.num_ops = 1;

	wmt_wc_barrier();
	if (drmCommandWrite(wmt->fd, DRM_WMT_GE_BATCH, &req, sizeof(req)) == 0) {
		uint32_t got = p[(H - 1) * stride + (W - 1)];
		uint32_t want = 0xff000000u | (uint32_t)((H - 2) * W + (W - 2));

		ok = (got == want);
	}

done:
	if (bo)
		wmt_bo_destroy(wmt->fd, bo);
	wmt->ge_overlap_ok = ok;
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "GE overlapping-blit self-test: %s\n",
		   ok ? "passed (hardware arbitrates overlap)"
		      : "failed (reverse self-copies fall back to software)");
}

/* ----------------------------------------------------------- Init / fini */

Bool
WMTExaInit(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WMTPtr wmt = WMTPTR(pScrn);
	ExaDriverPtr exa;

	exa = exaDriverAlloc();
	if (!exa)
		return FALSE;
	wmt->exa = exa;

	exa->exa_major = EXA_VERSION_MAJOR;
	exa->exa_minor = EXA_VERSION_MINOR;

	/* PREPARE_AUX: PrepareAccess ignores the index (one BO per pixmap), so we
	 * accept the AUX_* indices EXA uses for scratch pixmaps during fallbacks.
	 * Without this flag EXA FatalError()s on AUX access to a pinned pixmap. */
	exa->flags = EXA_OFFSCREEN_PIXMAPS | EXA_HANDLES_PIXMAPS |
		     EXA_SUPPORTS_PREPARE_AUX;
	exa->maxX = WMT_GE_MAX_DIM;
	exa->maxY = WMT_GE_MAX_DIM;
	exa->maxPitchBytes = WMT_GE_MAX_DIM * 4;
	exa->pixmapOffsetAlign = 0;
	exa->pixmapPitchAlign = 4;

	exa->PrepareSolid = WMTPrepareSolid;
	exa->Solid = WMTSolid;
	exa->DoneSolid = WMTDoneSolid;

	exa->PrepareCopy = WMTPrepareCopy;
	exa->Copy = WMTCopy;
	exa->DoneCopy = WMTDoneCopy;

	exa->WaitMarker = WMTWaitMarker;

	exa->CreatePixmap2 = WMTCreatePixmap2;
	exa->DestroyPixmap = WMTDestroyPixmap;
	exa->ModifyPixmapHeader = WMTModifyPixmapHeader;
	exa->PixmapIsOffscreen = WMTPixmapIsOffscreen;
	exa->PrepareAccess = WMTPrepareAccess;
	exa->FinishAccess = WMTFinishAccess;

	wmt->batch_max = 1024;
	wmt->batch = malloc(wmt->batch_max * sizeof(struct wmt_ge_op));
	if (!wmt->batch) {
		free(exa);
		wmt->exa = NULL;
		return FALSE;
	}
	wmt->batch_count = 0;

	if (!exaDriverInit(pScreen, exa)) {
		free(wmt->batch);
		wmt->batch = NULL;
		free(exa);
		wmt->exa = NULL;
		return FALSE;
	}

	wmt_ge_overlap_selftest(pScrn);
	return TRUE;
}

/*
 * Release the EXA driver record and batch buffer.  Must run *after* the
 * wrapped exaCloseScreen has executed (exaDriverInit installs that into the
 * CloseScreen chain and it reads the ExaDriverRec during teardown), so this is
 * called at the tail of WMTCloseScreen rather than before the chain.
 */
void
WMTExaCloseScreen(ScreenPtr pScreen)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pScreen));

	free(wmt->batch);
	wmt->batch = NULL;
	free(wmt->exa);
	wmt->exa = NULL;
}
