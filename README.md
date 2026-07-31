# xf86-video-wmt

An X.org video driver for the **WonderMedia WM8505** display controller and its 2D Graphics Engine (GE), specifically targeting the early-2010s ARM netbooks built around this SoC.

It sits on top of the in-kernel `wmt-drm` DRM/KMS driver. X11 solid fills and copies are EXA-accelerated on the GE through the kernel's asynchronous job ring.

## Capabilities

* KMS modesetting.
* EXA-accelerated 2D solid fills and copies.
* Pixmaps backed by GEM dumb buffers.
* Optional TearFree page-flipping.
* Backlight control via RandR.

The GE is a ROP3 fill/blit engine with no per-pixel alpha. The driver uses its copy and XOR ops. Render compositing is handled by the X server in software; the resulting blits to the screen are GE-accelerated.

## Building

### Standard autotools

Requires: `xserver-xorg-dev libdrm-dev xutils-dev automake libtool pkgconf`

```sh
./autogen.sh
make
sudo make install
```

### Cross-build deb package

Requires: `mmdebstrap qemu-user-binfmt uidmap` (Build dependencies are automatically resolved inside the chroot.)

```sh
./build-deb.sh
```

## License

MIT/X11 - see [COPYING](COPYING).
