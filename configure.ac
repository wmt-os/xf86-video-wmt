#  Copyright (C) 2026 Logan Russell <me@lrussell.net>

AC_PREREQ([2.60])
AC_INIT([xf86-video-wmt],
        [1.0.0],
        [me@lrussell.net],
        [xf86-video-wmt])

AC_CONFIG_SRCDIR([Makefile.am])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR(.)
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([foreign dist-bzip2])

# Require X.Org macros 1.8 or later for XORG_DEFAULT_OPTIONS
m4_ifndef([XORG_MACROS_VERSION],
          [m4_fatal([must install xorg-macros 1.8 or later before running autoconf/autogen])])
XORG_MACROS_VERSION(1.8)
XORG_DEFAULT_OPTIONS

AC_PROG_CC
AC_USE_SYSTEM_EXTENSIONS
AC_DISABLE_STATIC
LT_INIT

AH_TOP([#include "xorg-server.h"])

# Checks for extensions
XORG_DRIVER_CHECK_EXT(RANDR, randrproto)
XORG_DRIVER_CHECK_EXT(RENDER, renderproto)

# Checks for pkg-config packages
PKG_CHECK_MODULES(XORG, [xorg-server >= 21.1 xproto fontsproto $REQUIRED_MODULES])
PKG_CHECK_MODULES(LIBDRM, [libdrm >= 2.4.0])

# The driver module is installed into the X server's module directory
AC_MSG_CHECKING([for the X.Org module directory])
moduledir="`$PKG_CONFIG --variable=moduledir xorg-server`"
AC_MSG_RESULT([$moduledir])
AC_SUBST([moduledir])

# Where xorg.conf.d snippets live
AC_MSG_CHECKING([for the X.Org config directory])
configdir="`$PKG_CONFIG --variable=sysconfigdir xorg-server`"
AS_IF([test "x$configdir" = x], [configdir='${prefix}/share/X11/xorg.conf.d'])
AC_MSG_RESULT([$configdir])
AC_SUBST([configdir])

AC_CONFIG_FILES([
        Makefile
        src/Makefile
        man/Makefile
        conf/Makefile
])
AC_OUTPUT
