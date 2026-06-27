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

#include <errno.h>
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
	int guard = 100;

	while (wmt->flip_pending && guard-- > 0) {
		struct pollfd pfd = { .fd = wmt->fd, .events = POLLIN };
		int r = poll(&pfd, 1, 50);

		if (r > 0)
			/* Consumes the event -> clears flip_pending + swaps current */
			wmt_drm_notify(wmt->fd, 0, wmt);
		else if (r < 0 && errno != EINTR)
			break;
	}

	if (wmt->flip_pending) {
		wmt->current ^= 1;
		wmt->flip_pending = FALSE;
	}
}

static void
wmt_present(WMTPtr wmt)
{
	RegionPtr damage = DamageRegion(wmt->damage);
	int back = wmt->current ^ 1;
	BoxRec bounds = { 0, 0, wmt->mode_w, wmt->mode_h };
	RegionRec clip, copy;
	BoxPtr box;
	int n, i;

	RegionInit(&clip, &bounds, 1);

	RegionNull(&copy);
	RegionUnion(&copy, damage, &wmt->flip_region);
	RegionIntersect(&copy, &copy, &clip);

	/* Copy damage region and schedule page flip */
	box = RegionRects(&copy);
	n = RegionNumRects(&copy);
	for (i = 0; i < n; i++)
		wmt_ge_blit(wmt, wmt->screen_bo, wmt->scanout[back], box[i].x1,
			    box[i].y1, box[i].x2 - box[i].x1, box[i].y2 - box[i].y1);
	wmt_ge_flush(wmt);

	if (drmModePageFlip(wmt->fd, wmt->crtc_id, wmt->scanout[back]->fb_id,
			    DRM_MODE_PAGE_FLIP_EVENT, wmt) == 0) {
		wmt->flip_pending = TRUE;
		RegionCopy(&wmt->flip_region, damage);
	} else {
		/* Flip failed (blanked): draw in-place and save damage */
		RegionIntersect(&copy, damage, &clip);
		box = RegionRects(&copy);
		n = RegionNumRects(&copy);
		for (i = 0; i < n; i++)
			wmt_ge_blit(wmt, wmt->screen_bo, wmt->scanout[wmt->current],
				    box[i].x1, box[i].y1, box[i].x2 - box[i].x1,
				    box[i].y2 - box[i].y1);
		wmt_ge_flush(wmt);
		RegionUnion(&wmt->flip_region, &wmt->flip_region, &copy);
	}

	RegionUninit(&clip);
	RegionUninit(&copy);
	DamageEmpty(wmt->damage);
}

static void
WMTBlockHandler(ScreenPtr pScreen, void *timeout)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WMTPtr wmt = WMTPTR(pScrn);

	pScreen->BlockHandler = wmt->BlockHandler;
	(*pScreen->BlockHandler)(pScreen, timeout);
	pScreen->BlockHandler = WMTBlockHandler;

	if (pScrn->vtSema && wmt->tearfree && wmt->damage && !wmt->flip_pending &&
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

void
WMTFlipInit(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WMTPtr wmt = WMTPTR(pScrn);

	if (!wmt->exa) /* No GE: nothing to batch or present */
		return;

	if (wmt->tearfree) {
		RegionNull(&wmt->flip_region);
		wmt->damage = DamageCreate(NULL, wmt_damage_destroyed,
					   DamageReportNone, TRUE, pScreen, wmt);
		if (!wmt->damage) {
			xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
				   "TearFree setup failed; running without it\n");
			wmt->tearfree = FALSE;
		} else {
			DamageRegister(&pScreen->GetScreenPixmap(pScreen)->drawable,
				       wmt->damage);

			if (!SetNotifyFd(wmt->fd, wmt_drm_notify, X_NOTIFY_READ, wmt)) {
				xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
					   "TearFree setup failed; running without it\n");
				DamageDestroy(wmt->damage);
				wmt->tearfree = FALSE;
			}
		}
	}

	wmt->BlockHandler = pScreen->BlockHandler;
	pScreen->BlockHandler = WMTBlockHandler;
}

void
WMTFlipFini(ScreenPtr pScreen)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pScreen));

	if (!wmt->exa)
		return;

	wmt_ge_flush(wmt);

	if (wmt->tearfree) {
		WMTFlipDrain(wmt);
		RemoveNotifyFd(wmt->fd);
		if (wmt->damage)
			DamageDestroy(wmt->damage);
		RegionUninit(&wmt->flip_region);
	}
	if (wmt->BlockHandler) {
		pScreen->BlockHandler = wmt->BlockHandler;
		wmt->BlockHandler = NULL;
	}
}
