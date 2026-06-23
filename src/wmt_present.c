/*
 * WonderMedia WM8505 X.Org video driver -- TearFree page-flipping.
 *
 * The root window renders into an off-screen shadow buffer (screen_bo).  Once
 * per frame, in the screen BlockHandler, the regions that changed are GE-blitted
 * into the back scanout buffer and a vsync'd page flip presents it; the engine
 * never writes to the buffer being scanned out, so the display never tears.
 *
 * Two scanout buffers alternate.  Because each buffer is only refreshed every
 * other flip, a present must also replay the *previous* frame's damage (which
 * landed in the other buffer) -- flip_region carries that forward.
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

/* Page-flip completion: the buffer we flipped to is now the displayed one. */
static void
wmt_flip_complete(int fd, unsigned int seq, unsigned int sec, unsigned int usec,
		  void *data)
{
	WMTPtr wmt = data;

	if (!wmt->flip_pending)		/* a stale/late event must not desync buffers */
		return;
	wmt->current ^= 1;
	wmt->flip_pending = FALSE;
}

/* DRM fd became readable: dispatch the queued flip-completion event(s). */
static void
wmt_drm_notify(int fd, int xevents, void *data)
{
	drmEventContext ctx = {
		.version = 2,
		.page_flip_handler = wmt_flip_complete,
	};

	drmHandleEvent(fd, &ctx);
}

/* Block until an outstanding flip reports completion (or clearly never will),
 * so a late event can never land in freed driver state. */
void
WMTFlipDrain(WMTPtr wmt)
{
	int guard = 100;

	while (wmt->flip_pending && guard-- > 0) {
		struct pollfd pfd = { .fd = wmt->fd, .events = POLLIN };

		if (poll(&pfd, 1, 50) <= 0)
			break;
		wmt_drm_notify(wmt->fd, 0, wmt);
	}
	wmt->flip_pending = FALSE;
}

/* Copy the changed regions of the shadow into the back buffer and flip to it. */
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

	/* The back buffer also missed the previous frame's damage; replay both. */
	RegionNull(&copy);
	RegionUnion(&copy, damage, &wmt->flip_region);
	RegionIntersect(&copy, &copy, &clip);

	box = RegionRects(&copy);
	n = RegionNumRects(&copy);
	for (i = 0; i < n; i++)
		wmt_ge_blit(wmt, wmt->screen_bo, wmt->scanout[back], box[i].x1,
			    box[i].y1, box[i].x2 - box[i].x1, box[i].y2 - box[i].y1);
	wmt_ge_flush(wmt);

	if (drmModePageFlip(wmt->fd, wmt->crtc_id, wmt->scanout[back]->fb_id,
			    DRM_MODE_PAGE_FLIP_EVENT, wmt) == 0) {
		wmt->flip_pending = TRUE;
		/* The old front becomes the back and will need this frame's damage. */
		RegionCopy(&wmt->flip_region, damage);
	} else {
		/* Cannot flip (blanked): update the displayed buffer in place with
		 * just this frame's damage -- it already holds everything older.  The
		 * blit is submitted asynchronously (the kernel pins the live scanout BO
		 * until the job retires); a later CPU read is re-fenced by wmt_ge_sync. */
		RegionIntersect(&copy, damage, &clip);
		box = RegionRects(&copy);
		n = RegionNumRects(&copy);
		for (i = 0; i < n; i++)
			wmt_ge_blit(wmt, wmt->screen_bo, wmt->scanout[wmt->current],
				    box[i].x1, box[i].y1, box[i].x2 - box[i].x1,
				    box[i].y2 - box[i].y1);
		wmt_ge_flush(wmt);
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

	/* Present the frame if TearFree has something to show and we own the
	 * display; otherwise just submit this iteration's queued GE ops.  Either
	 * way the per-primitive flush is gone: a drawing burst costs one batch
	 * submission per frame. */
	if (pScrn->vtSema && wmt->tearfree && wmt->damage && !wmt->flip_pending &&
	    wmt->mode_h > 0 && RegionNotEmpty(DamageRegion(wmt->damage)))
		wmt_present(wmt);
	else
		wmt_ge_flush(wmt);
}

/* The screen damage was destroyed (by us, or with the screen pixmap). */
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

	if (!wmt->exa)			/* no GE: nothing to batch or present */
		return;

	if (wmt->tearfree) {
		/* All drawing to any window lands in the screen pixmap on this
		 * non-composited screen, so its damage captures the whole display. */
		RegionNull(&wmt->flip_region);
		wmt->damage = DamageCreate(NULL, wmt_damage_destroyed,
					   DamageReportNone, TRUE, pScreen, wmt);
		if (!wmt->damage ||
		    !SetNotifyFd(wmt->fd, wmt_drm_notify, X_NOTIFY_READ, wmt)) {
			xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
				   "TearFree setup failed; running without it\n");
			if (wmt->damage)
				DamageDestroy(wmt->damage);
			wmt->tearfree = FALSE;
		} else {
			DamageRegister(&pScreen->GetScreenPixmap(pScreen)->drawable,
				       wmt->damage);
		}
	}

	/* Wrap the block handler in every accelerated mode: it coalesces each
	 * frame's GE ops into a single submission (and presents, under TearFree). */
	wmt->BlockHandler = pScreen->BlockHandler;
	pScreen->BlockHandler = WMTBlockHandler;
}

void
WMTFlipFini(ScreenPtr pScreen)
{
	WMTPtr wmt = WMTPTR(xf86ScreenToScrn(pScreen));

	if (!wmt->exa)
		return;

	wmt_ge_flush(wmt);	/* don't strand queued ops when the batch is freed */

	if (wmt->tearfree) {
		WMTFlipDrain(wmt);
		RemoveNotifyFd(wmt->fd);
		if (wmt->damage)
			DamageDestroy(wmt->damage);	/* nulls wmt->damage via callback */
		RegionUninit(&wmt->flip_region);
	}
	if (wmt->BlockHandler) {
		pScreen->BlockHandler = wmt->BlockHandler;
		wmt->BlockHandler = NULL;
	}
}
