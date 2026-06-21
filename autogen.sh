#! /bin/sh
#  Copyright (C) 2026 Logan Russell <me@lrussell.net>

srcdir=`dirname "$0"`
test -z "$srcdir" && srcdir=.

ORIGDIR=`pwd`
cd "$srcdir"

autoreconf -v --install || exit 1
cd "$ORIGDIR" || exit $?

test -n "$NOCONFIGURE" || exec "$srcdir/configure" "$@"
