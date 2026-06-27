/*
 * WonderMedia WM8505 X.Org Video Driver
 *
 * DDX Lifecycle
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "xf86.h"
#include "xf86_OSproc.h"
#include "xf86Module.h"
#include "xf86Crtc.h"
#include "xf86cmap.h"
#include "mipointer.h"
#include "micmap.h"
#include "fb.h"

#ifdef XSERVER_PLATFORM_BUS
#include "xf86platformBus.h"
#endif

#include "wmt.h"

/* Driver State */

static Bool WMTPreInit(ScrnInfoPtr pScrn, int flags);
static Bool WMTScreenInit(ScreenPtr pScreen, int argc, char **argv);
static Bool WMTCloseScreen(ScreenPtr pScreen);
static Bool WMTCreateScreenResources(ScreenPtr pScreen);
static Bool WMTEnterVT(ScrnInfoPtr pScrn);
static void WMTLeaveVT(ScrnInfoPtr pScrn);
static Bool WMTSwitchMode(ScrnInfoPtr pScrn, DisplayModePtr mode);
static void WMTAdjustFrame(ScrnInfoPtr pScrn, int x, int y);
static ModeStatus WMTValidMode(ScrnInfoPtr pScrn, DisplayModePtr mode,
			       Bool verbose, int flags);
static void WMTFreeScreen(ScrnInfoPtr pScrn);

typedef enum {
	OPTION_ACCEL,
	OPTION_TEARFREE,
} WMTOpts;

static const OptionInfoRec WMTOptions[] = {
	{ OPTION_ACCEL,		"Accel",	OPTV_BOOLEAN, {0}, FALSE },
	{ OPTION_TEARFREE,	"TearFree",	OPTV_BOOLEAN, {0}, FALSE },
	{ -1,				NULL,		OPTV_NONE,    {0}, FALSE },
};

/* Helpers */

static Bool
WMTGetRec(ScrnInfoPtr pScrn)
{
	if (pScrn->driverPrivate)
		return TRUE;
	pScrn->driverPrivate = xnfcalloc(1, sizeof(WMTRec));
	if (!pScrn->driverPrivate)
		return FALSE;
	WMTPTR(pScrn)->fd = -1;
	return TRUE;
}

static void
WMTFreeRec(ScrnInfoPtr pScrn)
{
	WMTPtr wmt = WMTPTR(pScrn);

	if (!wmt)
		return;
	if (wmt->fd >= 0 && wmt->fd_owned)
		close(wmt->fd);
	free(wmt->kmsdev);
	free(wmt->Options);
	free(wmt);
	pScrn->driverPrivate = NULL;
}

static Bool
WMTOpenDRM(ScrnInfoPtr pScrn)
{
	WMTPtr wmt = WMTPTR(pScrn);
	EntityInfoPtr pEnt = wmt->pEnt;
	const char *path = NULL;
	int fd = -1;

#ifdef XSERVER_PLATFORM_BUS
	if (pEnt->location.type == BUS_PLATFORM) {
		struct xf86_platform_device *plat = pEnt->location.id.plat;

#ifdef XF86_PDEV_SERVER_FD
		if (plat->flags & XF86_PDEV_SERVER_FD) {
			fd = xf86_platform_device_odev_attributes(plat)->fd;
			wmt->fd_owned = FALSE;
		} else
#endif
		{
			path = xf86_platform_device_odev_attributes(plat)->path;
		}
	}
#endif

	if (fd < 0) {
		if (!path && pEnt->device)
			path = xf86FindOptionValue(pEnt->device->options, "kmsdev");
		if (!path)
			path = "/dev/dri/card0";

		fd = open(path, O_RDWR | O_CLOEXEC, 0);
		if (fd < 0) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "Failed to open DRM device %s: %s\n",
				   path, strerror(errno));
			return FALSE;
		}
		wmt->fd_owned = TRUE;
	}

	wmt->fd = fd;
	wmt->kmsdev = path ? strdup(path) : NULL;
	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Using DRM device %s (fd %d)\n",
		   path ? path : "(server fd)", fd);
	return TRUE;
}

/* PreInit */

static Bool
WMTPreInit(ScrnInfoPtr pScrn, int flags)
{
	WMTPtr wmt;
	rgb defaultWeight = { 0, 0, 0 };
	Gamma zeros = { 0.0, 0.0, 0.0 };

	if (pScrn->numEntities != 1)
		return FALSE;
	if (flags & PROBE_DETECT)
		return FALSE;

	if (!WMTGetRec(pScrn))
		return FALSE;
	wmt = WMTPTR(pScrn);
	wmt->pEnt = xf86GetEntityInfo(pScrn->entityList[0]);

	pScrn->monitor = pScrn->confScreen->monitor;
	pScrn->progClock = TRUE;
	pScrn->rgbBits = 8;

	if (!WMTOpenDRM(pScrn))
		return FALSE;

	if (!xf86SetDepthBpp(pScrn, WMT_DEPTH, WMT_DEPTH, WMT_BPP,
			     Support32bppFb | SupportConvert24to32 |
			     PreferConvert24to32))
		return FALSE;
	if (pScrn->depth != WMT_DEPTH) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Only depth %d is supported (requested %d)\n",
			   WMT_DEPTH, pScrn->depth);
		return FALSE;
	}
	xf86PrintDepthBpp(pScrn);

	xf86CollectOptions(pScrn, NULL);
	wmt->Options = malloc(sizeof(WMTOptions));
	if (!wmt->Options)
		return FALSE;
	memcpy(wmt->Options, WMTOptions, sizeof(WMTOptions));
	xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, wmt->Options);

	wmt->accel = xf86ReturnOptValBool(wmt->Options, OPTION_ACCEL, TRUE);
	wmt->tearfree = xf86ReturnOptValBool(wmt->Options, OPTION_TEARFREE, TRUE);

	if (!xf86SetWeight(pScrn, defaultWeight, defaultWeight))
		return FALSE;
	if (!xf86SetDefaultVisual(pScrn, -1))
		return FALSE;
	if (!xf86SetGamma(pScrn, zeros))
		return FALSE;

	if (!WMTKMSPreInit(pScrn))
		return FALSE;

	if (!pScrn->modes) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "No valid modes found\n");
		return FALSE;
	}
	pScrn->currentMode = pScrn->modes;
	xf86SetDpi(pScrn, 0, 0);

	if (!xf86LoadSubModule(pScrn, "fb"))
		return FALSE;
	if (wmt->accel && !xf86LoadSubModule(pScrn, "exa")) {
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "Could not load EXA module; disabling acceleration\n");
		wmt->accel = FALSE;
	}

	return TRUE;
}

/* ScreenInit */

static Bool
WMTScreenInit(ScreenPtr pScreen, int argc, char **argv)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WMTPtr wmt = WMTPTR(pScrn);
	int w = pScrn->virtualX, h = pScrn->virtualY;
	VisualPtr visual;

	pScrn->pScreen = pScreen;

	if (wmt->fd_owned && drmSetMaster(wmt->fd))
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "drmSetMaster failed: %s\n", strerror(errno));

	if (!wmt->accel)
		wmt->tearfree = FALSE;

	/* Displayed scanout buffer (the sole buffer when TearFree is off) */
	wmt->scanout[0] = wmt_bo_new(wmt->fd, w, h, TRUE);
	if (!wmt->scanout[0]) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Failed to allocate scanout buffer\n");
		return FALSE;
	}
	wmt->current = 0;
	pScrn->displayWidth = wmt->scanout[0]->pitch / WMT_BYTES_PP;

	if (wmt->tearfree) {
		wmt->scanout[1] = wmt_bo_new(wmt->fd, w, h, TRUE);
		wmt->screen_bo = wmt_bo_new(wmt->fd, w, h, FALSE);
		if (!wmt->scanout[1] || !wmt->screen_bo) {
			xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
				   "TearFree buffer allocation failed; disabling it\n");
			if (wmt->scanout[1])
				wmt_bo_destroy(wmt->fd, wmt->scanout[1]);
			if (wmt->screen_bo)
				wmt_bo_destroy(wmt->fd, wmt->screen_bo);
			wmt->scanout[1] = wmt->screen_bo = NULL;
			wmt->tearfree = FALSE;
		}
	}
	if (!wmt->screen_bo)
		wmt->screen_bo = wmt->scanout[0];

	miClearVisualTypes();
	if (!miSetVisualTypes(pScrn->depth, miGetDefaultVisualMask(pScrn->depth),
			      pScrn->rgbBits, pScrn->defaultVisual))
		return FALSE;
	if (!miSetPixmapDepths())
		return FALSE;

	/* Pass NULL so EXA manages the front pixmap allocation in MIXED mode */
	if (!fbScreenInit(pScreen, NULL, pScrn->virtualX, pScrn->virtualY,
			  pScrn->xDpi, pScrn->yDpi, pScrn->displayWidth,
			  pScrn->bitsPerPixel))
		return FALSE;

	visual = pScreen->visuals + pScreen->numVisuals;
	while (--visual >= pScreen->visuals) {
		if ((visual->class | DynamicClass) == DirectColor) {
			visual->offsetRed = pScrn->offset.red;
			visual->offsetGreen = pScrn->offset.green;
			visual->offsetBlue = pScrn->offset.blue;
			visual->redMask = pScrn->mask.red;
			visual->greenMask = pScrn->mask.green;
			visual->blueMask = pScrn->mask.blue;
		}
	}

	fbPictureInit(pScreen, NULL, 0);

	xf86SetBlackWhitePixels(pScreen);
	xf86SetBackingStore(pScreen);
	xf86SetSilkenMouse(pScreen);

	if (wmt->accel && !WMTExaInit(pScreen)) {
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "EXA initialisation failed; running unaccelerated\n");
		wmt->accel = FALSE;
	}

	miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

	wmt->CreateScreenResources = pScreen->CreateScreenResources;
	pScreen->CreateScreenResources = WMTCreateScreenResources;

	if (!WMTKMSScreenInit(pScreen))
		return FALSE;

	if (!miCreateDefColormap(pScreen))
		return FALSE;
	if (!xf86HandleColormaps(pScreen, 256, 8, NULL, NULL,
				 CMAP_PALETTED_TRUECOLOR |
				 CMAP_RELOAD_ON_MODE_SWITCH))
		return FALSE;

	xf86DPMSInit(pScreen, xf86DPMSSet, 0);

	pScrn->vtSema = TRUE;

	wmt->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = WMTCloseScreen;

	if (serverGeneration == 1)
		xf86ShowUnusedOptions(pScrn->scrnIndex, pScrn->options);

	return TRUE;
}

static Bool
WMTCreateScreenResources(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WMTPtr wmt = WMTPTR(pScrn);
	Bool ret;

	pScreen->CreateScreenResources = wmt->CreateScreenResources;
	ret = pScreen->CreateScreenResources(pScreen);
	pScreen->CreateScreenResources = WMTCreateScreenResources;
	if (!ret)
		return FALSE;

	/* Force front buffer migration to GPU copy for immediate acceleration */
	if (wmt->accel && wmt->exa)
		exaMoveInPixmap(pScreen->GetScreenPixmap(pScreen));

	if (!xf86SetDesiredModes(pScrn))
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "Failed to set the initial mode\n");

	WMTFlipInit(pScreen);

	return TRUE;
}

static Bool
WMTCloseScreen(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	WMTPtr wmt = WMTPTR(pScrn);
	Bool ret;

	/* Tear down page flips before closing screen/master */
	WMTFlipFini(pScreen);

	if (wmt->fd_owned)
		drmDropMaster(wmt->fd);

	/* Call wrapped CloseScreen chain */
	pScreen->CreateScreenResources = wmt->CreateScreenResources;
	pScreen->CloseScreen = wmt->CloseScreen;
	ret = (*pScreen->CloseScreen)(pScreen);

	if (wmt->exa)
		WMTExaCloseScreen(pScreen);

	if (wmt->screen_bo && wmt->screen_bo != wmt->scanout[0])
		wmt_bo_destroy(wmt->fd, wmt->screen_bo);
	if (wmt->scanout[0])
		wmt_bo_destroy(wmt->fd, wmt->scanout[0]);
	if (wmt->scanout[1])
		wmt_bo_destroy(wmt->fd, wmt->scanout[1]);
	wmt->screen_bo = wmt->scanout[0] = wmt->scanout[1] = NULL;

	pScrn->vtSema = FALSE;
	return ret;
}

/* VT / Mode */

static Bool
WMTEnterVT(ScrnInfoPtr pScrn)
{
	pScrn->vtSema = TRUE;
	return WMTKMSEnterVT(pScrn);
}

static void
WMTLeaveVT(ScrnInfoPtr pScrn)
{
	WMTFlipDrain(WMTPTR(pScrn));
	WMTKMSLeaveVT(pScrn);
	pScrn->vtSema = FALSE;
}

static Bool
WMTSwitchMode(ScrnInfoPtr pScrn, DisplayModePtr mode)
{
	return xf86SetSingleMode(pScrn, mode, RR_Rotate_0);
}

static void
WMTAdjustFrame(ScrnInfoPtr pScrn, int x, int y)
{
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86OutputPtr output;
	xf86CrtcPtr crtc;

	if (config->compat_output < 0)
		return;
	output = config->output[config->compat_output];
	crtc = output->crtc;
	if (crtc && crtc->enabled)
		crtc->funcs->set_mode_major(crtc, &crtc->mode, crtc->rotation, x, y);
}

static ModeStatus
WMTValidMode(ScrnInfoPtr pScrn, DisplayModePtr mode, Bool verbose, int flags)
{
	if (mode->HDisplay > WMT_GE_MAX_DIM || mode->VDisplay > WMT_GE_MAX_DIM)
		return MODE_BAD;
	return MODE_OK;
}

static void
WMTFreeScreen(ScrnInfoPtr pScrn)
{
	WMTFreeRec(pScrn);
}

/* Driver Glue */

static void
WMTIdentify(int flags)
{
	xf86Msg(X_INFO, "wmt: driver for the WonderMedia WM8505 (GE 2D)\n");
}

static const OptionInfoRec *
WMTAvailableOptions(int chipid, int busid)
{
	return WMTOptions;
}

static Bool
WMTDriverFunc(ScrnInfoPtr pScrn, xorgDriverFuncOp op, void *data)
{
	xorgHWFlags *flag;

	switch (op) {
	case GET_REQUIRED_HW_INTERFACES:
		flag = (xorgHWFlags *)data;
		*flag = HW_SKIP_CONSOLE;
		return TRUE;
#ifdef SUPPORTS_SERVER_FDS
	case SUPPORTS_SERVER_FDS:
		return TRUE;
#endif
	default:
		return FALSE;
	}
}

#ifdef XSERVER_PLATFORM_BUS
static Bool
WMTPlatformProbe(DriverPtr driver, int entity_num, int flags,
		 struct xf86_platform_device *dev, intptr_t match_data)
{
	ScrnInfoPtr pScrn;

	if (flags & PLATFORM_PROBE_GPU_SCREEN)
		return FALSE;

	pScrn = xf86AllocateScreen(driver, 0);
	if (!pScrn)
		return FALSE;

	xf86AddEntityToScreen(pScrn, entity_num);

	pScrn->driverVersion = 1;
	pScrn->driverName = "wmt";
	pScrn->name = "WMT";
	pScrn->Probe = NULL;
	pScrn->PreInit = WMTPreInit;
	pScrn->ScreenInit = WMTScreenInit;
	pScrn->SwitchMode = WMTSwitchMode;
	pScrn->AdjustFrame = WMTAdjustFrame;
	pScrn->EnterVT = WMTEnterVT;
	pScrn->LeaveVT = WMTLeaveVT;
	pScrn->FreeScreen = WMTFreeScreen;
	pScrn->ValidMode = WMTValidMode;

	return TRUE;
}
#endif

_X_EXPORT DriverRec WMT = {
	1,
	"wmt",
	WMTIdentify,
	NULL, /* Probe (legacy PCI/ISA path unused) */
	WMTAvailableOptions,
	NULL,
	0,
	WMTDriverFunc,
	NULL, /* supported_devices */
	NULL, /* PciProbe */
#ifdef XSERVER_PLATFORM_BUS
	WMTPlatformProbe,
#endif
};

/* Module Entry */

static MODULESETUPPROTO(WMTSetup);

static XF86ModuleVersionInfo WMTVersRec = {
	"wmt",
	MODULEVENDORSTRING,
	MODINFOSTRING1,
	MODINFOSTRING2,
	XORG_VERSION_CURRENT,
	WMT_VERSION_MAJOR, WMT_VERSION_MINOR, WMT_VERSION_PATCH,
	ABI_CLASS_VIDEODRV,
	ABI_VIDEODRV_VERSION,
	MOD_CLASS_VIDEODRV,
	{ 0, 0, 0, 0 },
};

_X_EXPORT XF86ModuleData wmtModuleData = { &WMTVersRec, WMTSetup, NULL };

static pointer
WMTSetup(pointer module, pointer opts, int *errmaj, int *errmin)
{
	static Bool setupDone = FALSE;

	if (!setupDone) {
		setupDone = TRUE;
		xf86AddDriver(&WMT, module, HaveDriverFuncs);
		return (pointer)1;
	}

	if (errmaj)
		*errmaj = LDR_ONCEONLY;
	return NULL;
}
