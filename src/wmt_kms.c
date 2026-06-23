/*
 * WonderMedia WM8505 X.Org video driver -- KMS / RandR-1.2 backend.
 *
 * A deliberately small single-CRTC, single-output modesetting backend built on
 * libdrm and the xf86Crtc (RandR-1.2) infrastructure.  The cursor is rendered
 * in software (the display pipe exposes a single plane) and rotation is left to
 * the server, so the CRTC needs only mode-set, DPMS and gamma hooks.
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "xf86.h"
#include "xf86Crtc.h"

#include "wmt.h"

/* -------------------------------------------------------- private records */

typedef struct {
	WMTPtr		wmt;
	uint32_t	crtc_id;	/* kernel CRTC object id */
	int		dpms_mode;
} WMTCrtcPriv;

typedef struct {
	WMTPtr			wmt;
	uint32_t		output_id;	/* kernel connector id */
	drmModeConnectorPtr	conn;
} WMTOutputPriv;

/* ----------------------------------------------------- mode conversion */

static void
wmt_kmode_from_mode(drmModeModeInfo *kmode, const DisplayModeRec *mode)
{
	memset(kmode, 0, sizeof(*kmode));
	kmode->clock = mode->Clock;
	kmode->hdisplay = mode->HDisplay;
	kmode->hsync_start = mode->HSyncStart;
	kmode->hsync_end = mode->HSyncEnd;
	kmode->htotal = mode->HTotal;
	kmode->hskew = mode->HSkew;
	kmode->vdisplay = mode->VDisplay;
	kmode->vsync_start = mode->VSyncStart;
	kmode->vsync_end = mode->VSyncEnd;
	kmode->vtotal = mode->VTotal;
	kmode->vscan = mode->VScan;
	kmode->flags = mode->Flags;
	if (mode->name)
		strncpy(kmode->name, mode->name, DRM_DISPLAY_MODE_LEN - 1);
}

static void
wmt_mode_from_kmode(ScrnInfoPtr pScrn, drmModeModeInfo *kmode, DisplayModePtr mode)
{
	memset(mode, 0, sizeof(*mode));
	mode->status = MODE_OK;
	mode->Clock = kmode->clock;
	mode->HDisplay = kmode->hdisplay;
	mode->HSyncStart = kmode->hsync_start;
	mode->HSyncEnd = kmode->hsync_end;
	mode->HTotal = kmode->htotal;
	mode->HSkew = kmode->hskew;
	mode->VDisplay = kmode->vdisplay;
	mode->VSyncStart = kmode->vsync_start;
	mode->VSyncEnd = kmode->vsync_end;
	mode->VTotal = kmode->vtotal;
	mode->VScan = kmode->vscan;
	mode->Flags = kmode->flags;
	mode->name = strdup(kmode->name);
	if (kmode->type & DRM_MODE_TYPE_DRIVER)
		mode->type = M_T_DRIVER;
	if (kmode->type & DRM_MODE_TYPE_PREFERRED)
		mode->type |= M_T_PREFERRED;
	xf86SetModeCrtc(mode, pScrn->adjustFlags);
}

/* ------------------------------------------------------------- CRTC funcs */

static Bool
wmt_crtc_set_mode_major(xf86CrtcPtr crtc, DisplayModePtr mode,
			Rotation rotation, int x, int y)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	WMTPtr wmt = WMTPTR(pScrn);
	WMTCrtcPriv *cp = crtc->driver_private;
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	WMTBO *bo = wmt->scanout[wmt->current];
	drmModeModeInfo kmode;
	uint32_t *output_ids;
	int output_count = 0;
	int i;
	Bool ret = TRUE;

	if (!bo || !bo->fb_id)
		return FALSE;

	output_ids = calloc(config->num_output, sizeof(*output_ids));
	if (!output_ids)
		return FALSE;

	for (i = 0; i < config->num_output; i++) {
		xf86OutputPtr output = config->output[i];
		WMTOutputPriv *op;

		if (output->crtc != crtc)
			continue;
		op = output->driver_private;
		output_ids[output_count++] = op->output_id;
	}

	wmt_kmode_from_mode(&kmode, mode);

	if (drmModeSetCrtc(wmt->fd, cp->crtc_id, bo->fb_id, x, y,
			   output_ids, output_count, &kmode)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "drmModeSetCrtc failed: %s\n", strerror(errno));
		ret = FALSE;
	} else {
		crtc->mode = *mode;
		crtc->x = x;
		crtc->y = y;
		crtc->rotation = rotation;
		wmt->mode_w = mode->HDisplay;
		wmt->mode_h = mode->VDisplay;
	}

	free(output_ids);
	return ret;
}

static void
wmt_crtc_dpms(xf86CrtcPtr crtc, int mode)
{
	WMTCrtcPriv *cp = crtc->driver_private;

	cp->dpms_mode = mode;
}

static void
wmt_crtc_gamma_set(xf86CrtcPtr crtc, CARD16 *red, CARD16 *green, CARD16 *blue,
		   int size)
{
	WMTCrtcPriv *cp = crtc->driver_private;

	/* The display pipe may not implement a gamma LUT; ignore failure. */
	drmModeCrtcSetGamma(cp->wmt->fd, cp->crtc_id, size, red, green, blue);
}

static void
wmt_crtc_destroy(xf86CrtcPtr crtc)
{
	free(crtc->driver_private);
}

static const xf86CrtcFuncsRec wmt_crtc_funcs = {
	.dpms = wmt_crtc_dpms,
	.set_mode_major = wmt_crtc_set_mode_major,
	.gamma_set = wmt_crtc_gamma_set,
	.destroy = wmt_crtc_destroy,
};

/* ----------------------------------------------------------- output funcs */

static xf86OutputStatus
wmt_output_detect(xf86OutputPtr output)
{
	WMTOutputPriv *op = output->driver_private;
	WMTPtr wmt = op->wmt;
	drmModeConnectorPtr conn;

	conn = drmModeGetConnector(wmt->fd, op->output_id);
	if (conn) {
		if (op->conn)
			drmModeFreeConnector(op->conn);
		op->conn = conn;
		if (conn->connection == DRM_MODE_DISCONNECTED)
			return XF86OutputStatusDisconnected;
	}
	/* A fixed internal panel is always considered present. */
	return XF86OutputStatusConnected;
}

static int
wmt_output_mode_valid(xf86OutputPtr output, DisplayModePtr mode)
{
	if (mode->HDisplay > WMT_GE_MAX_DIM || mode->VDisplay > WMT_GE_MAX_DIM)
		return MODE_BAD;
	return MODE_OK;
}

static DisplayModePtr
wmt_output_get_modes(xf86OutputPtr output)
{
	WMTOutputPriv *op = output->driver_private;
	WMTPtr wmt = op->wmt;
	drmModeConnectorPtr conn = op->conn;
	DisplayModePtr modes = NULL;
	int i;

	if (!conn)
		conn = op->conn = drmModeGetConnector(wmt->fd, op->output_id);
	if (!conn)
		return NULL;

	output->mm_width = conn->mmWidth;
	output->mm_height = conn->mmHeight;

	for (i = 0; i < conn->count_modes; i++) {
		DisplayModePtr mode = xnfalloc(sizeof(DisplayModeRec));

		wmt_mode_from_kmode(output->scrn, &conn->modes[i], mode);
		modes = xf86ModesAdd(modes, mode);
	}

	/* Fall back to a sane default if the panel reports nothing. */
	if (!modes) {
		DisplayModePtr mode = xf86CVTMode(800, 480, 60.0, FALSE, FALSE);

		if (mode) {
			mode->type = M_T_DRIVER | M_T_PREFERRED;
			modes = xf86ModesAdd(modes, mode);
		}
	}

	return modes;
}

static void
wmt_output_dpms(xf86OutputPtr output, int mode)
{
}

static void
wmt_output_destroy(xf86OutputPtr output)
{
	WMTOutputPriv *op = output->driver_private;

	if (op) {
		if (op->conn)
			drmModeFreeConnector(op->conn);
		free(op);
	}
	output->driver_private = NULL;
}

static const xf86OutputFuncsRec wmt_output_funcs = {
	.dpms = wmt_output_dpms,
	.detect = wmt_output_detect,
	.mode_valid = wmt_output_mode_valid,
	.get_modes = wmt_output_get_modes,
	.destroy = wmt_output_destroy,
};

/* --------------------------------------------------------- config / resize */

static Bool
wmt_xf86crtc_resize(ScrnInfoPtr pScrn, int width, int height)
{
	WMTPtr wmt = WMTPTR(pScrn);
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	ScreenPtr pScreen = pScrn->pScreen;
	WMTBO *old[3] = { wmt->scanout[0], wmt->scanout[1], wmt->screen_bo };
	WMTBO *s0, *s1 = NULL, *shadow;
	int i;

	if (width == pScrn->virtualX && height == pScrn->virtualY)
		return TRUE;

	/* Submit any queued GE ops before their target buffers are freed below. */
	wmt_ge_flush(wmt);
	if (wmt->tearfree)
		WMTFlipDrain(wmt);

	/* Allocate the full buffer set up front so a failure changes nothing. */
	s0 = wmt_bo_new(wmt->fd, width, height, TRUE);
	if (wmt->tearfree && s0) {
		s1 = wmt_bo_new(wmt->fd, width, height, TRUE);
		shadow = wmt_bo_new(wmt->fd, width, height, FALSE);
	} else {
		shadow = s0;	/* render straight into the scanout */
	}
	if (!s0 || (wmt->tearfree && (!s1 || !shadow))) {
		if (s0) wmt_bo_destroy(wmt->fd, s0);
		if (s1) wmt_bo_destroy(wmt->fd, s1);
		if (shadow && shadow != s0) wmt_bo_destroy(wmt->fd, shadow);
		return FALSE;
	}

	wmt->scanout[0] = s0;
	wmt->scanout[1] = s1;
	wmt->screen_bo = shadow;
	wmt->current = 0;
	pScrn->virtualX = width;
	pScrn->virtualY = height;
	pScrn->displayWidth = s0->pitch / WMT_BYTES_PP;

	if (pScreen) {
		PixmapPtr root = pScreen->GetScreenPixmap(pScreen);
		WMTPixmapPriv *priv = wmt->exa ? WMT_PIXMAP_PRIV(root) : NULL;

		if (priv) {
			priv->bo = shadow;
			priv->pitch = shadow->pitch;
		}
		pScreen->ModifyPixmapHeader(root, width, height, -1, -1,
					    shadow->pitch, priv ? NULL : shadow->map);
	}

	for (i = 0; i < config->num_crtc; i++) {
		xf86CrtcPtr crtc = config->crtc[i];

		if (crtc->enabled)
			wmt_crtc_set_mode_major(crtc, &crtc->mode, crtc->rotation,
						crtc->x, crtc->y);
	}

	/* Release the previous buffers only once the new ones are scanning out. */
	if (old[2] && old[2] != old[0])
		wmt_bo_destroy(wmt->fd, old[2]);
	if (old[0]) wmt_bo_destroy(wmt->fd, old[0]);
	if (old[1]) wmt_bo_destroy(wmt->fd, old[1]);
	return TRUE;
}

static const xf86CrtcConfigFuncsRec wmt_xf86crtc_config_funcs = {
	.resize = wmt_xf86crtc_resize,
};

/* ---------------------------------------------------------------- PreInit */

Bool
WMTKMSPreInit(ScrnInfoPtr pScrn)
{
	WMTPtr wmt = WMTPTR(pScrn);
	drmModeResPtr res;
	int i;

	xf86CrtcConfigInit(pScrn, &wmt_xf86crtc_config_funcs);

	res = drmModeGetResources(wmt->fd);
	if (!res) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "drmModeGetResources failed: %s\n", strerror(errno));
		return FALSE;
	}

	xf86CrtcSetSizeRange(pScrn, 8, 8,
			     res->max_width ? res->max_width : WMT_GE_MAX_DIM,
			     res->max_height ? res->max_height : WMT_GE_MAX_DIM);

	for (i = 0; i < res->count_crtcs; i++) {
		xf86CrtcPtr crtc = xf86CrtcCreate(pScrn, &wmt_crtc_funcs);
		WMTCrtcPriv *cp;

		if (!crtc)
			continue;
		cp = xnfcalloc(1, sizeof(*cp));
		cp->wmt = wmt;
		cp->crtc_id = res->crtcs[i];
		crtc->driver_private = cp;

		if (!wmt->crtc_id)	/* the single display pipe, for page flips */
			wmt->crtc_id = res->crtcs[i];
	}

	for (i = 0; i < res->count_connectors; i++) {
		drmModeConnectorPtr conn = drmModeGetConnector(wmt->fd, res->connectors[i]);
		const char *tname;
		char name[32];
		xf86OutputPtr output;
		WMTOutputPriv *op;

		if (!conn)
			continue;

		tname = drmModeGetConnectorTypeName(conn->connector_type);
		snprintf(name, sizeof(name), "%s-%d",
			 tname ? tname : "Output", conn->connector_type_id);

		output = xf86OutputCreate(pScrn, &wmt_output_funcs, name);
		if (!output) {
			drmModeFreeConnector(conn);
			continue;
		}
		op = xnfcalloc(1, sizeof(*op));
		op->wmt = wmt;
		op->output_id = res->connectors[i];
		op->conn = conn;
		output->driver_private = op;
		output->possible_crtcs = (1 << res->count_crtcs) - 1;
		output->possible_clones = 0;
		output->interlaceAllowed = FALSE;
		output->doubleScanAllowed = FALSE;
	}

	drmModeFreeResources(res);

	/* Fixed internal panel: the screen never needs to grow past the mode. */
	xf86InitialConfiguration(pScrn, FALSE);
	return TRUE;
}

/* ------------------------------------------------------------- ScreenInit */

Bool
WMTKMSScreenInit(ScreenPtr pScreen)
{
	return xf86CrtcScreenInit(pScreen);
}

/* --------------------------------------------------------------- VT switch */

Bool
WMTKMSEnterVT(ScrnInfoPtr pScrn)
{
	WMTPtr wmt = WMTPTR(pScrn);

	if (wmt->fd_owned && drmSetMaster(wmt->fd)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "drmSetMaster failed: %s\n", strerror(errno));
		return FALSE;
	}
	return xf86SetDesiredModes(pScrn);
}

void
WMTKMSLeaveVT(ScrnInfoPtr pScrn)
{
	WMTPtr wmt = WMTPTR(pScrn);

	if (wmt->fd_owned)
		drmDropMaster(wmt->fd);
}
