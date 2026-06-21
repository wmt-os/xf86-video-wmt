#  Copyright (C) 2026 Logan Russell <me@lrussell.net>

drivermandir = $(DRIVER_MAN_DIR)

driverman_PRE = wmt.man
driverman_DATA = $(driverman_PRE:man=$(DRIVER_MAN_SUFFIX))

EXTRA_DIST = $(driverman_PRE)
CLEANFILES = $(driverman_DATA)

SUFFIXES = .$(DRIVER_MAN_SUFFIX) .man

# String replacements in MAN_SUBSTS come from xorg-macros.m4 via XORG_MANPAGE_SECTIONS
.man.$(DRIVER_MAN_SUFFIX):
	$(AM_V_GEN)$(SED) $(MAN_SUBSTS) < $< > $@
