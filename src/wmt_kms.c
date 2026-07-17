/*
 * WonderMedia WM8505 X.Org Video Driver
 *
 * KMS / RandR Backend
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xf86.h"
#include "xf86Crtc.h"

#include <X11/Xatom.h>
#include <X11/extensions/dpmsconst.h>

#include "wmt.h"

/* Private Records */

typedef struct {
	WMTPtr		wmt;
	uint32_t	crtc_id;
} WMTCrtcPriv;

typedef struct {
	WMTPtr		wmt;
	uint32_t	output_id;
	drmModeConnectorPtr	conn;
	char		bl_path[128];	/* sysfs brightness file */
	INT32		bl_max;
} WMTOutputPriv;

/* Mode Conversion */

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

/* CRTC Functions */

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
		if (output_count > 0)
			wmt->dpms_off = FALSE;	/* a real mode-set re-lights the panel */
	}

	free(output_ids);
	return ret;
}

static void
wmt_dpms_set(xf86CrtcPtr crtc, int mode)
{
	WMTPtr wmt;
	Bool off = (mode != DPMSModeOn);

	if (!crtc)
		return;
	wmt = WMTPTR(crtc->scrn);
	if (off == wmt->dpms_off)
		return;

	if (off) {
		WMTCrtcPriv *cp = crtc->driver_private;

		/* Settle outstanding flips before disabling CRTC */
		if (wmt->tearfree)
			WMTFlipDrain(wmt);
		if (drmModeSetCrtc(wmt->fd, cp->crtc_id, 0, 0, 0, NULL, 0, NULL) == 0)
			wmt->dpms_off = TRUE;
	} else {
		wmt_crtc_set_mode_major(crtc, &crtc->mode, crtc->rotation,
					crtc->x, crtc->y);
	}
}

static void
wmt_crtc_dpms(xf86CrtcPtr crtc, int mode)
{
	wmt_dpms_set(crtc, mode);
}

static void
wmt_crtc_gamma_set(xf86CrtcPtr crtc, CARD16 *red, CARD16 *green, CARD16 *blue,
		   int size)
{
	WMTCrtcPriv *cp = crtc->driver_private;

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

/* Backlight */

static Atom wmt_backlight_atom;

static int
wmt_backlight_read(const char *path)
{
	char buf[16];
	int fd, n;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	return atoi(buf);
}

static Bool
wmt_backlight_write(const char *path, INT32 level)
{
	char buf[16];
	int fd, n;
	Bool ok;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return FALSE;
	n = snprintf(buf, sizeof(buf), "%d", level);
	ok = write(fd, buf, n) == n;
	close(fd);
	return ok;
}

static void
wmt_backlight_probe(WMTOutputPriv *op)
{
	DIR *dir = opendir("/sys/class/backlight");
	struct dirent *ent;
	char path[sizeof(op->bl_path)];
	int max;

	if (!dir)
		return;
	while (!op->bl_path[0] && (ent = readdir(dir))) {
		if (ent->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path),
			 "/sys/class/backlight/%s/max_brightness", ent->d_name);
		max = wmt_backlight_read(path);
		if (max < 1)
			continue;
		snprintf(op->bl_path, sizeof(op->bl_path),
			 "/sys/class/backlight/%s/brightness", ent->d_name);
		op->bl_max = max;
	}
	closedir(dir);
}

/* Output Functions */

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
wmt_output_create_resources(xf86OutputPtr output)
{
	WMTOutputPriv *op = output->driver_private;
	INT32 range[2] = { 0, 0 };
	INT32 level;

	if (!op->bl_path[0])
		wmt_backlight_probe(op);
	if (!op->bl_path[0])
		return;

	wmt_backlight_atom = MakeAtom("Backlight", 9, TRUE);
	level = wmt_backlight_read(op->bl_path);
	if (level < 0)
		return;

	range[1] = op->bl_max;
	RRConfigureOutputProperty(output->randr_output, wmt_backlight_atom,
				  FALSE, TRUE, FALSE, 2, range);
	RRChangeOutputProperty(output->randr_output, wmt_backlight_atom,
			       XA_INTEGER, 32, PropModeReplace, 1, &level,
			       FALSE, FALSE);
}

static void
wmt_output_dpms(xf86OutputPtr output, int mode)
{
	wmt_dpms_set(output->crtc, mode);
}

static Bool
wmt_output_set_property(xf86OutputPtr output, Atom property,
			RRPropertyValuePtr value)
{
	WMTOutputPriv *op = output->driver_private;
	INT32 level;

	if (!op->bl_path[0] || property != wmt_backlight_atom)
		return TRUE;	/* not ours; let the server store it */

	if (value->type != XA_INTEGER || value->format != 32 ||
	    value->size != 1)
		return FALSE;
	level = *(INT32 *)value->data;
	if (level < 0 || level > op->bl_max)
		return FALSE;

	return wmt_backlight_write(op->bl_path, level);
}

static Bool
wmt_output_get_property(xf86OutputPtr output, Atom property)
{
	WMTOutputPriv *op = output->driver_private;
	INT32 level;

	if (!op->bl_path[0] || property != wmt_backlight_atom)
		return FALSE;

	/* The console and hotkeys write sysfs behind us; re-sync */
	level = wmt_backlight_read(op->bl_path);
	if (level < 0)
		return FALSE;

	RRChangeOutputProperty(output->randr_output, wmt_backlight_atom,
			       XA_INTEGER, 32, PropModeReplace, 1, &level,
			       FALSE, FALSE);
	return TRUE;
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
	.create_resources = wmt_output_create_resources,
	.dpms = wmt_output_dpms,
	.detect = wmt_output_detect,
	.mode_valid = wmt_output_mode_valid,
	.get_modes = wmt_output_get_modes,
	.set_property = wmt_output_set_property,
	.get_property = wmt_output_get_property,
	.destroy = wmt_output_destroy,
};

/* Resize */

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

	wmt_ge_flush(wmt);
	if (wmt->tearfree)
		WMTFlipDrain(wmt);

	/* Allocate scanout and shadow buffers */
	s0 = wmt_bo_new(wmt->fd, width, height, TRUE);
	if (wmt->tearfree && s0) {
		s1 = wmt_bo_new(wmt->fd, width, height, TRUE);
		shadow = wmt_bo_new(wmt->fd, width, height, FALSE);
	} else {
		shadow = s0;
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
	if (wmt->tearfree)
		RegionEmpty(&wmt->flip_region);
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

	if (old[2] && old[2] != old[0])
		wmt_bo_destroy(wmt->fd, old[2]);
	if (old[0]) wmt_bo_destroy(wmt->fd, old[0]);
	if (old[1]) wmt_bo_destroy(wmt->fd, old[1]);
	return TRUE;
}

static const xf86CrtcConfigFuncsRec wmt_xf86crtc_config_funcs = {
	.resize = wmt_xf86crtc_resize,
};

/* PreInit */

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

		if (!wmt->crtc_id)
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

	xf86InitialConfiguration(pScrn, FALSE);
	return TRUE;
}

/* ScreenInit */

Bool
WMTKMSScreenInit(ScreenPtr pScreen)
{
	return xf86CrtcScreenInit(pScreen);
}

/* VT Switch */

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
