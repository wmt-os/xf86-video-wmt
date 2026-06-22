#  Copyright (C) 2026 Logan Russell <me@lrussell.net>

# _ladir passes a dummy rpath to libtool so the thing will actually link
# to a non-standard location.
wmt_drv_ladir = @moduledir@/drivers

AM_CFLAGS = @XORG_CFLAGS@ @LIBDRM_CFLAGS@ $(CWARNFLAGS)

wmt_drv_la_LTLIBRARIES = wmt_drv.la
wmt_drv_la_LDFLAGS = -module -avoid-version
wmt_drv_la_LIBADD = @LIBDRM_LIBS@

wmt_drv_la_SOURCES = \
	wmt.h \
	wmt_drm_uapi.h \
	wmt_driver.c \
	wmt_kms.c \
	wmt_bo.c \
	wmt_exa.c \
	wmt_present.c
