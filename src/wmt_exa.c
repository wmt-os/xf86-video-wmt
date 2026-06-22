/*
 * WonderMedia WM8505 X.Org video driver -- EXA acceleration.
 *
 * Maps EXA's solid-fill and copy primitives onto the kernel's GE command
 * batch IOCTL (DRM_IOCTL_WMT_GE_BATCH).
 *
 * The GE addresses surfaces by GEM handle, so each accelerated surface is its
 * own dumb buffer.  This uses the EXA_MIXED_PIXMAPS model: the server keeps a
 * cached system-memory copy of every pixmap (so Render compositing, which the
 * engine cannot do, runs correctly and quickly on cached memory) and EXA asks
 * the driver for a GE-addressable buffer only when a pixmap is actually
 * accelerated, migrating it in and out via UploadToScreen/DownloadFromScreen.
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
 * GE (and the scanout) before a DMA reads them. */
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
	case GXxor:	return WMT_GE_ROP_PAT_XOR;	/* P XOR D  */
	default:	return -1;
	}
}

static int
wmt_copy_rop(int alu)
{
	switch (alu) {
	case GXcopy:	return WMT_GE_ROP_SRC_COPY;	/* S        */
	case GXxor:	return WMT_GE_ROP_SRC_XOR;	/* S XOR D  */
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
		xf86DrvMsgVerb(0, X_WARNING, 1, "GE batch of %u ops failed\n",
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

/* Queue a straight source-copy blit between two buffers (same coordinates in
 * each).  Used by the TearFree flip path; the caller submits with wmt_ge_flush. */
void
wmt_ge_blit(WMTPtr wmt, WMTBO *src, WMTBO *dst, int x, int y, int w, int h)
{
	struct wmt_ge_op *op = wmt_ge_next(wmt);

	memset(op, 0, sizeof(*op));
	op->type = WMT_GE_OP_BLIT;
	op->rop = WMT_GE_ROP_SRC_COPY;
	op->dest_handle = dst->handle;
	op->dest_pitch = dst->pitch;
	op->dest_x = x;
	op->dest_y = y;
	op->width = w;
	op->height = h;
	op->src_handle = src->handle;
	op->src_pitch = src->pitch;
	op->src_x = x;
	op->src_y = y;
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
	wmt->op_rop = (uint32_t)rop;
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
	wmt->op_rop = (uint32_t)rop;
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

/*
 * Solid and Copy share one no-op completion.  Queued GE ops are submitted
 * lazily -- at WaitMarker, before CPU access, or once per frame in the block
 * handler -- so a burst of primitives costs a single ioctl, not one each.
 */
static void
WMTDoneOp(PixmapPtr pPix)
{
}

/* --------------------------------------------------------------- Sync */

static void
WMTWaitMarker(ScreenPtr pScreen, int marker)
{
	wmt_ge_flush(WMTPTR(xf86ScreenToScrn(pScreen)));
}

/* ------------------------------------------------------ Pixmap management */

/*
 * MIXED model: a driver pixmap is created only for GE-addressable surfaces
 * (32-bpp, within the engine's coordinate range).  Returning NULL leaves the
 * pixmap in system memory, where the server renders it directly.
 */
static void *
WMTCreatePixmap2(ScreenPtr pScreen, int width, int height, int depth,
		 int usage_hint, int bitsPerPixel, int *new_fb_pitch)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WMTPtr wmt = WMTPTR(pScrn);
	WMTPixmapPriv *priv;
	WMTBO *bo;

	if (!wmt->accel || bitsPerPixel != WMT_BPP ||
	    width <= 0 || height <= 0 ||
	    width > WMT_GE_MAX_DIM || height > WMT_GE_MAX_DIM)
		return NULL;

	priv = calloc(1, sizeof(*priv));
	if (!priv)
		return NULL;

	/*
	 * Bind the root pixmap's GPU copy to screen_bo.  Without TearFree that is
	 * the scanout itself, so screen drawing is GE-accelerated and shown with no
	 * extra copy; with TearFree it is an off-screen shadow the flip path
	 * presents.  Either way EXA migrates to a cached system copy for the
	 * operations the engine can't do (Render), then back here.
	 */
	if (!wmt->screen_bound &&
	    width == pScrn->virtualX && height == pScrn->virtualY) {
		priv->bo = wmt->screen_bo;
		priv->pitch = wmt->screen_bo->pitch;
		wmt->screen_bound = TRUE;
		*new_fb_pitch = priv->pitch;
		return priv;
	}

	bo = wmt_bo_create(wmt->fd, width, height);
	if (!bo || !wmt_bo_map(wmt->fd, bo)) {
		if (bo)
			wmt_bo_destroy(wmt->fd, bo);
		free(priv);
		return NULL;
	}
	priv->bo = bo;
	priv->pitch = bo->pitch;
	*new_fb_pitch = bo->pitch;
	return priv;
}

static void
WMTDestroyPixmap(ScreenPtr pScreen, void *driverPriv)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pScreen));
	WMTPixmapPriv *priv = driverPriv;

	if (!priv)
		return;

	/* The scanout and shadow buffers are owned by the driver, never a pixmap. */
	if (priv->bo && priv->bo != wmt->scanout[0] && priv->bo != wmt->scanout[1] &&
	    priv->bo != wmt->screen_bo) {
		/* Submit queued ops first; none may reference the freed handle. */
		wmt_ge_flush(wmt);
		wmt_bo_destroy(wmt->fd, priv->bo);
	}
	free(priv);
}

/* Only driver (BO-backed) pixmaps reach this hook in the MIXED model; expose
 * the buffer's mapping and stride to EXA as the GPU copy. */
static Bool
WMTModifyPixmapHeader(PixmapPtr pPix, int width, int height, int depth,
		      int bitsPerPixel, int devKind, void *pPixData)
{
	WMTPixmapPriv *priv = WMT_PIXMAP_PRIV(pPix);

	if (!priv || !priv->bo)
		return FALSE;

	if (width > 0)
		pPix->drawable.width = width;
	if (height > 0)
		pPix->drawable.height = height;
	if (depth > 0)
		pPix->drawable.depth = depth;
	if (bitsPerPixel > 0)
		pPix->drawable.bitsPerPixel = bitsPerPixel;
	pPix->drawable.serialNumber = NEXT_SERIAL_NUMBER;
	pPix->devKind = priv->pitch;
	pPix->devPrivate.ptr = priv->bo->map;	/* captured by EXA as fb_ptr */
	return TRUE;
}

/* MIXED's exaPixmapHasGpuCopy_mixed calls this unconditionally once a pixmap
 * has a driverPriv, so it must exist: a pixmap is GE-addressable iff it owns a
 * buffer object. */
static Bool
WMTPixmapIsOffscreen(PixmapPtr pPix)
{
	WMTPixmapPriv *priv = WMT_PIXMAP_PRIV(pPix);

	return priv && priv->bo != NULL;
}

/* Direct CPU access to a driver pixmap's buffer: ensure GE output has landed,
 * then hand back the (write-combined) mapping. */
static Bool
WMTPrepareAccess(PixmapPtr pPix, int index)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pPix->drawable.pScreen));
	WMTPixmapPriv *priv = WMT_PIXMAP_PRIV(pPix);
	void *map;

	if (!priv || !priv->bo)
		return TRUE;

	map = wmt_bo_map(wmt->fd, priv->bo);
	if (!map)
		return FALSE;

	wmt_ge_flush(wmt);
	pPix->devPrivate.ptr = map;
	return TRUE;
}

static void
WMTFinishAccess(PixmapPtr pPix, int index)
{
	WMTPixmapPriv *priv = WMT_PIXMAP_PRIV(pPix);

	if (priv && priv->bo) {
		wmt_wc_barrier();	/* flush CPU writes for the GE/scanout */
		pPix->devPrivate.ptr = NULL;
	}
}

/* Migrate a system-memory region into a driver pixmap's buffer. */
static Bool
WMTUploadToScreen(PixmapPtr pDst, int x, int y, int w, int h,
		  char *src, int src_pitch)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pDst->drawable.pScreen));
	WMTPixmapPriv *priv = WMT_PIXMAP_PRIV(pDst);
	int bpp, i;
	char *dst;

	if (!priv || !priv->bo || !priv->bo->map)
		return FALSE;

	wmt_ge_flush(wmt);	/* finish queued GE ops before the CPU writes */
	bpp = pDst->drawable.bitsPerPixel / 8;
	dst = (char *)priv->bo->map + y * priv->pitch + x * bpp;
	for (i = 0; i < h; i++)
		memcpy(dst + i * priv->pitch, src + i * src_pitch, (size_t)w * bpp);
	wmt_wc_barrier();
	return TRUE;
}

/* Migrate a driver pixmap's buffer back out to system memory. */
static Bool
WMTDownloadFromScreen(PixmapPtr pSrc, int x, int y, int w, int h,
		      char *dst, int dst_pitch)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pSrc->drawable.pScreen));
	WMTPixmapPriv *priv = WMT_PIXMAP_PRIV(pSrc);
	int bpp, i;
	char *s;

	if (!priv || !priv->bo || !priv->bo->map)
		return FALSE;

	wmt_ge_flush(wmt);
	bpp = pSrc->drawable.bitsPerPixel / 8;
	s = (char *)priv->bo->map + y * priv->pitch + x * bpp;
	for (i = 0; i < h; i++)
		memcpy(dst + i * dst_pitch, s + i * priv->pitch, (size_t)w * bpp);
	return TRUE;
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

	/* MIXED is selected by HANDLES|MIXED together; MIXED alone falls through
	 * to the classic model. */
	exa->flags = EXA_OFFSCREEN_PIXMAPS | EXA_HANDLES_PIXMAPS | EXA_MIXED_PIXMAPS;
	exa->maxX = WMT_GE_MAX_DIM;
	exa->maxY = WMT_GE_MAX_DIM;
	exa->maxPitchBytes = WMT_GE_MAX_DIM * 4;
	exa->pixmapOffsetAlign = 0;
	exa->pixmapPitchAlign = 4;

	exa->PrepareSolid = WMTPrepareSolid;
	exa->Solid = WMTSolid;
	exa->DoneSolid = WMTDoneOp;

	exa->PrepareCopy = WMTPrepareCopy;
	exa->Copy = WMTCopy;
	exa->DoneCopy = WMTDoneOp;

	exa->WaitMarker = WMTWaitMarker;

	exa->CreatePixmap2 = WMTCreatePixmap2;
	exa->DestroyPixmap = WMTDestroyPixmap;
	exa->ModifyPixmapHeader = WMTModifyPixmapHeader;
	exa->PixmapIsOffscreen = WMTPixmapIsOffscreen;
	exa->PrepareAccess = WMTPrepareAccess;
	exa->FinishAccess = WMTFinishAccess;
	exa->UploadToScreen = WMTUploadToScreen;
	exa->DownloadFromScreen = WMTDownloadFromScreen;

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
