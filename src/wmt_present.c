/*
 * WonderMedia WM8505 X.Org Video Driver
 *
 * TearFree Page-Flipping
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <poll.h>

#include "xf86.h"
#include "os.h"

#include "wmt.h"

static void
wmt_flip_complete(int fd, unsigned int seq, unsigned int sec, unsigned int usec,
		  void *data)
{
	WMTPtr wmt = data;

	/* Page flip completed; swap current buffer index */
	if (!wmt->flip_pending)
		return;
	wmt->current ^= 1;
	wmt->flip_pending = FALSE;
}

static void
wmt_drm_notify(int fd, int xevents, void *data)
{
	drmEventContext ctx = {
		.version = 2,
		.page_flip_handler = wmt_flip_complete,
	};

	drmHandleEvent(fd, &ctx);
}

void
WMTFlipDrain(WMTPtr wmt)
{
	struct pollfd pfd = { .fd = wmt->fd, .events = POLLIN };

	while (wmt->flip_pending && poll(&pfd, 1, 0) > 0)
		wmt_drm_notify(wmt->fd, 0, wmt);

	/* Unresolved: adopt kernel state; SetCrtc serializes behind the flip */
	if (wmt->flip_pending) {
		drmModeCrtcPtr crtc = drmModeGetCrtc(wmt->fd, wmt->crtc_id);

		if (crtc) {
			if (crtc->buffer_id == wmt->scanout[wmt->current ^ 1]->fb_id)
				wmt->current ^= 1;
			drmModeFreeCrtc(crtc);
		}
		wmt->flip_pending = FALSE;
	}
}

/* Expand a region of the shadow into a scanout buffer on the VDMA */
static void
wmt_convert_region(WMTPtr wmt, WMTBO *dst, RegionPtr region)
{
	BoxPtr box = RegionRects(region);
	int n = RegionNumRects(region), i;

	for (i = 0; i < n; i++)
		wmt_ge_convert(wmt, wmt->screen_bo, dst, box[i].x1, box[i].y1,
			       box[i].x2 - box[i].x1, box[i].y2 - box[i].y1);
	wmt_ge_flush(wmt);
}

static void
wmt_present(WMTPtr wmt)
{
	RegionPtr damage = DamageRegion(wmt->damage);
	int back = wmt->current ^ 1;
	BoxRec bounds = { 0, 0, wmt->mode_w, wmt->mode_h };
	RegionRec clip, copy;

	RegionInit(&clip, &bounds, 1);
	RegionNull(&copy);

	/* Convert current and prior damage, then flip to the back buffer */
	if (wmt->tearfree) {
		RegionUnion(&copy, damage, &wmt->flip_region);
		RegionIntersect(&copy, &copy, &clip);
		wmt_convert_region(wmt, wmt->scanout[back], &copy);

		if (drmModePageFlip(wmt->fd, wmt->crtc_id, wmt->scanout[back]->fb_id,
				    DRM_MODE_PAGE_FLIP_EVENT, wmt) == 0) {
			wmt->flip_pending = TRUE;
			RegionCopy(&wmt->flip_region, damage);
		}
	}

	/* Convert in place if not flipping */
	if (!wmt->flip_pending) {
		RegionIntersect(&copy, damage, &clip);
		wmt_convert_region(wmt, wmt->scanout[wmt->current], &copy);
		RegionEmpty(&wmt->flip_region);
	}

	RegionUninit(&clip);
	RegionUninit(&copy);
	DamageEmpty(wmt->damage);

	/* Without EXA there is no PrepareAccess to order CPU writes behind the VDMA */
	if (!wmt->exa)
		wmt_ge_sync(wmt, wmt->screen_bo);
}

static void
WMTBlockHandler(ScreenPtr pScreen, void *timeout)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WMTPtr wmt = WMTPTR(pScrn);

	pScreen->BlockHandler = wmt->BlockHandler;
	(*pScreen->BlockHandler)(pScreen, timeout);
	pScreen->BlockHandler = WMTBlockHandler;

	if (pScrn->vtSema && wmt->damage && !wmt->flip_pending &&
	    !wmt->dpms_off && wmt->mode_h > 0 && RegionNotEmpty(DamageRegion(wmt->damage)))
		wmt_present(wmt);
	else
		wmt_ge_flush(wmt);
}

static void
wmt_damage_destroyed(DamagePtr damage, void *closure)
{
	WMTPtr wmt = closure;

	wmt->damage = NULL;
}

Bool
WMTFlipInit(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WMTPtr wmt = WMTPTR(pScrn);

	/* Every frame reaches the panel through the shadow's damage */
	RegionNull(&wmt->flip_region);
	wmt->damage = DamageCreate(NULL, wmt_damage_destroyed, DamageReportNone, TRUE,
				   pScreen, wmt);
	if (!wmt->damage) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Damage setup failed\n");
		return FALSE;
	}
	DamageRegister(&pScreen->GetScreenPixmap(pScreen)->drawable, wmt->damage);

	if (!SetNotifyFd(wmt->fd, wmt_drm_notify, X_NOTIFY_READ, wmt)) {
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "Flip event setup failed; running without TearFree\n");
		wmt->tearfree = FALSE;
	}

	wmt->BlockHandler = pScreen->BlockHandler;
	pScreen->BlockHandler = WMTBlockHandler;
	return TRUE;
}

void
WMTFlipFini(ScreenPtr pScreen)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pScreen));

	wmt_ge_flush(wmt);
	WMTFlipDrain(wmt);
	RemoveNotifyFd(wmt->fd);
	if (wmt->damage)
		DamageDestroy(wmt->damage);
	RegionUninit(&wmt->flip_region);
	if (wmt->BlockHandler) {
		pScreen->BlockHandler = wmt->BlockHandler;
		wmt->BlockHandler = NULL;
	}
}
