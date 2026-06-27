/*
 * WonderMedia WM8505 X.Org Video Driver
 *
 * EXA Acceleration
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "wmt.h"

static inline void
wmt_wc_barrier(void)
{
	__sync_synchronize();
}

static int
wmt_solid_rop(int alu)
{
	switch (alu) {
	case GXcopy:	return WMT_GE_ROP_PAT_COPY;
	case GXxor:		return WMT_GE_ROP_PAT_XOR;
	default:		return -1;
	}
}

static int
wmt_copy_rop(int alu)
{
	switch (alu) {
	case GXcopy:	return WMT_GE_ROP_SRC_COPY;
	case GXxor:		return WMT_GE_ROP_SRC_XOR;
	default:		return -1;
	}
}

/* Submit queued operations */
void
wmt_ge_flush(WMTPtr wmt)
{
	struct wmt_ge_submit req;

	if (wmt->batch_count == 0)
		return;

	wmt_wc_barrier();

	memset(&req, 0, sizeof(req));
	req.ops = (uint64_t)(uintptr_t)wmt->batch;
	req.num_ops = wmt->batch_count;

	if (drmIoctl(wmt->fd, DRM_IOCTL_WMT_GE_SUBMIT, &req) != 0) {
		struct wmt_ge_op *o = &wmt->batch[0];

		xf86DrvMsgVerb(0, X_WARNING, 0,
			       "GE submit of %u ops failed: %s "
			       "[op0 t=%u rop=%#x dst=%u pitch=%u @%u,%u %ux%u "
			       "src=%u pitch=%u @%u,%u]\n",
			       wmt->batch_count, strerror(errno),
			       o->type, o->rop, o->dest_handle, o->dest_pitch,
			       o->dest_x, o->dest_y, o->width, o->height,
			       o->src_handle, o->src_pitch, o->src_x, o->src_y);
	} else {
		wmt->last_submit_seqno = req.out_seqno;

		/* Stamp seqno on BOs to track read/write operations */
		if (wmt->batch_dst_bo)
			wmt->batch_dst_bo->last_seqno = req.out_seqno;
		if (wmt->batch_src_bo)
			wmt->batch_src_bo->last_seqno = req.out_seqno;
	}

	wmt->batch_count = 0;
	wmt->batch_dst_bo = NULL;
	wmt->batch_src_bo = NULL;
}

/* Sync GE work touching bo */
void
wmt_ge_sync(WMTPtr wmt, WMTBO *bo)
{
	struct wmt_ge_wait w;

	wmt_ge_flush(wmt);

	if (!bo || !bo->last_seqno || wmt_ge_passed(bo->last_synced, bo->last_seqno))
		return;

	memset(&w, 0, sizeof(w));
	w.seqno = bo->last_seqno;
	if (drmIoctl(wmt->fd, DRM_IOCTL_WMT_GE_WAIT, &w) != 0)
		xf86DrvMsgVerb(0, X_WARNING, 0, "GE wait for seqno %u failed: %s\n",
			       w.seqno, strerror(errno));

	bo->last_synced = bo->last_seqno;
}

/* Global GE sync */
static void
wmt_ge_drain_global(WMTPtr wmt)
{
	struct wmt_ge_wait w;

	wmt_ge_flush(wmt);

	if (!wmt->last_submit_seqno ||
	    wmt_ge_passed(wmt->last_synced_seqno, wmt->last_submit_seqno))
		return;

	memset(&w, 0, sizeof(w));
	w.seqno = wmt->last_submit_seqno;
	if (drmIoctl(wmt->fd, DRM_IOCTL_WMT_GE_WAIT, &w) != 0)
		xf86DrvMsgVerb(0, X_WARNING, 0, "GE wait for seqno %u failed: %s\n",
			       w.seqno, strerror(errno));
	wmt->last_synced_seqno = wmt->last_submit_seqno;
}

static struct wmt_ge_op *
wmt_ge_next(WMTPtr wmt, WMTBO *dst, WMTBO *src)
{
	if (wmt->batch_count &&
	    (dst != wmt->batch_dst_bo ||
	     (src && wmt->batch_src_bo && src != wmt->batch_src_bo)))
		wmt_ge_flush(wmt);
	if (wmt->batch_count >= wmt->batch_max)
		wmt_ge_flush(wmt);

	wmt->batch_dst_bo = dst;
	if (src)
		wmt->batch_src_bo = src;
	return &wmt->batch[wmt->batch_count++];
}

/* Clip coordinates to buffer bounds; EXA can request boxes that overrun */
static Bool
wmt_clip_to_bo(WMTBO *bo, int x, int y, int *w, int *h)
{
	if (x < 0 || y < 0 || x >= bo->width || y >= bo->height)
		return FALSE;
	if (x + *w > bo->width)
		*w = bo->width - x;
	if (y + *h > bo->height)
		*h = bo->height - y;
	return *w > 0 && *h > 0;
}

/* Queue a straight source-copy blit between two buffers (same coordinates) */
void
wmt_ge_blit(WMTPtr wmt, WMTBO *src, WMTBO *dst, int x, int y, int w, int h)
{
	struct wmt_ge_op *op;

	if (!wmt_clip_to_bo(dst, x, y, &w, &h) || !wmt_clip_to_bo(src, x, y, &w, &h))
		return;

	op = wmt_ge_next(wmt, dst, src);

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

/* Solid */

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
	int w = x2 - x1, h = y2 - y1;
	struct wmt_ge_op *op;

	if (!wmt_clip_to_bo(wmt->op_dst_bo, x1, y1, &w, &h))
		return;

	op = wmt_ge_next(wmt, wmt->op_dst_bo, NULL);
	memset(op, 0, sizeof(*op));
	op->type = WMT_GE_OP_FILL;
	op->rop = wmt->op_rop;
	op->dest_handle = wmt->op_dst_bo->handle;
	op->dest_pitch = wmt->op_dst_pitch;
	op->dest_x = x1;
	op->dest_y = y1;
	op->width = w;
	op->height = h;
	op->color = wmt->op_fg;
}

/* Copy */

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
	struct wmt_ge_op *op;

	if (!wmt_clip_to_bo(wmt->op_dst_bo, dstX, dstY, &w, &h) ||
	    !wmt_clip_to_bo(wmt->op_src_bo, srcX, srcY, &w, &h))
		return;

	op = wmt_ge_next(wmt, wmt->op_dst_bo, wmt->op_src_bo);
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
WMTDoneOp(PixmapPtr pPix)
{
	/* No-op: GE ops are lazily batched to save ioctls */
}

static void
WMTWaitMarker(ScreenPtr pScreen, int marker)
{
	wmt_ge_drain_global(WMTPTR(xf86ScreenToScrn(pScreen)));
}

/* Pixmap Management */

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

	/* Bind root pixmap to screen shadow/scanout */
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

	if (priv->bo && priv->bo != wmt->scanout[0] && priv->bo != wmt->scanout[1] &&
	    priv->bo != wmt->screen_bo) {
		wmt_ge_flush(wmt);
		wmt_bo_destroy(wmt->fd, priv->bo);
	}
	free(priv);
}

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
	pPix->devPrivate.ptr = priv->bo->map;
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
		return TRUE;

	map = wmt_bo_map(wmt->fd, priv->bo);
	if (!map)
		return FALSE;

	/* Sync with GE before CPU reads/writes */
	wmt_ge_sync(wmt, priv->bo);
	pPix->devPrivate.ptr = map;
	return TRUE;
}

static void
WMTFinishAccess(PixmapPtr pPix, int index)
{
	WMTPixmapPriv *priv = WMT_PIXMAP_PRIV(pPix);

	if (priv && priv->bo) {
		wmt_wc_barrier();
		pPix->devPrivate.ptr = NULL;
	}
}

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

	wmt_ge_sync(wmt, priv->bo);
	bpp = pDst->drawable.bitsPerPixel / 8;
	dst = (char *)priv->bo->map + y * priv->pitch + x * bpp;
	for (i = 0; i < h; i++)
		memcpy(dst + i * priv->pitch, src + i * src_pitch, (size_t)w * bpp);
	wmt_wc_barrier();
	return TRUE;
}

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

	wmt_ge_sync(wmt, priv->bo);
	bpp = pSrc->drawable.bitsPerPixel / 8;
	s = (char *)priv->bo->map + y * priv->pitch + x * bpp;
	for (i = 0; i < h; i++)
		memcpy(dst + i * dst_pitch, s + i * priv->pitch, (size_t)w * bpp);
	return TRUE;
}

/* Overlap Self-Test */

/* Test if the 2D engine arbitrates overlapping copies correctly */
static void
wmt_ge_overlap_selftest(ScrnInfoPtr pScrn)
{
	WMTPtr wmt = WMTPTR(pScrn);
	const int W = 64, H = 64;
	WMTBO *bo;
	uint32_t *p;
	struct wmt_ge_op op;
	struct wmt_ge_submit req;
	struct wmt_ge_wait w;
	int stride, x, y;
	Bool ok = FALSE;

	wmt->ge_overlap_ok = FALSE;

	bo = wmt_bo_create(wmt->fd, W, H);
	if (!bo)
		goto done;
	p = wmt_bo_map(wmt->fd, bo);
	if (!p)
		goto done;

	stride = bo->pitch / sizeof(*p);
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			p[y * stride + x] = (uint32_t)(y * W + x);	/* Unique per pixel */

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

	memset(&req, 0, sizeof(req));
	req.ops = (uint64_t)(uintptr_t)&op;
	req.num_ops = 1;

	wmt_wc_barrier();
	if (drmIoctl(wmt->fd, DRM_IOCTL_WMT_GE_SUBMIT, &req) == 0) {
		uint32_t got, want;

		memset(&w, 0, sizeof(w));
		w.seqno = req.out_seqno;
		if (drmIoctl(wmt->fd, DRM_IOCTL_WMT_GE_WAIT, &w) == 0) {
			got = p[(H - 1) * stride + (W - 1)];
			want = (uint32_t)((H - 2) * W + (W - 2));
			ok = (got == want);
		}
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

/* Init / Close */

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

	exa->flags = EXA_OFFSCREEN_PIXMAPS | EXA_HANDLES_PIXMAPS | EXA_MIXED_PIXMAPS;
	exa->maxX = WMT_GE_MAX_DIM;
	exa->maxY = WMT_GE_MAX_DIM;
	exa->maxPitchBytes = WMT_GE_MAX_DIM * WMT_BYTES_PP;
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

void
WMTExaCloseScreen(ScreenPtr pScreen)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pScreen));

	free(wmt->batch);
	wmt->batch = NULL;
	free(wmt->exa);
	wmt->exa = NULL;
}
