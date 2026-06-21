#  Copyright (C) 2026 Logan Russell <me@lrussell.net>

SUBDIRS = src man conf

MAINTAINERCLEANFILES = ChangeLog INSTALL

EXTRA_DIST = README.md autogen.sh

.PHONY: ChangeLog INSTALL

INSTALL:
	$(INSTALL_CMD)

ChangeLog:
	$(CHANGELOG_CMD)

dist-hook: ChangeLog INSTALL
