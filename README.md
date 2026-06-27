# xf86-video-wmt

An X.org video driver for the **WonderMedia WM8505** display controller and its 2D Graphics Engine (GE), specifically targeting the early-2010s ARM netbooks built around this SoC.

It drives the panel through the in-kernel `wmt-drm` DRM/KMS driver and accelerates X11 solid fills and copies on the GE via the EXA architecture and the kernel's asynchronous GE job ring.

## Capabilities

* KMS modesetting on the single GOVRH CRTC / DPI panel output.
* EXA-accelerated `PrepareSolid` and `PrepareCopy` mapped onto GE fills and blits.
* Pixmaps backed by GEM dumb (CMA) buffers, with graceful software fallback when contiguous memory is unavailable.
* Optional TearFree double-buffered, vblank-synchronised page flipping.

The GE is a 32-bpp fill/copy/XOR engine with no alpha blending. Render compositing is handled by the X server in software; the resulting blits to the screen are GE-accelerated.

## Building

Standard autotools:

```sh
./autogen.sh
make
sudo make install
```
Build-depends: `xserver-xorg-dev`, `libdrm-dev`, `xutils-dev`, `automake`, `libtool`, `pkgconf`.

## License

MIT/X11 - see [COPYING](COPYING).
