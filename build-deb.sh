#!/bin/sh
# Build the xf86-video-wmt armel deb package
#
# Output: ./dist/*.deb (override: OUT=)
# Build-depends: mmdebstrap qemu-user-binfmt
#
# Copyright (C) 2026 Logan Russell <me@lrussell.net>

set -eu

SRC=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
OUT=${OUT:-$SRC/dist}

export LC_ALL=C.UTF-8
export LANG=C.UTF-8

tar=$(mktemp)
trap 'rm -f "$tar"' EXIT
tar -c -C "$SRC" --exclude=./.git --exclude=./dist -f "$tar" .

mkdir -p "$OUT"
mmdebstrap --variant=buildd --arch=armel \
	--customize-hook='mkdir "$1/src"' \
	--customize-hook="tar-in $tar /src" \
	--chrooted-customize-hook='cd /src &&
		apt-get -y --no-install-recommends build-dep ./ && dpkg-buildpackage -b -uc -us &&
		mkdir /out && mv /*.deb /out/' \
	--customize-hook="sync-out /out $OUT" \
	trixie /dev/null

ls -1 "$OUT"/*.deb
