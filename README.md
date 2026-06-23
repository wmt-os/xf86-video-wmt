# xf86-video-wmt

An X.Org DDX (2D acceleration) driver for the **WonderMedia WM8505** display
controller and its 2D Graphics Engine (GE), targeting WMT OS on the device's
ARMv5 netbook.

It drives the panel through the in-kernel `wmt-drm` DRM/KMS driver and
accelerates X11 solid fills and copies on the GE via the EXA architecture and
the kernel's asynchronous GE job ring (`DRM_IOCTL_WMT_GE_SUBMIT` /
`DRM_IOCTL_WMT_GE_WAIT`).

## Capabilities

* KMS modesetting on the single GOVRH CRTC / DPI panel output.
* EXA-accelerated `PrepareSolid`/`PrepareCopy` mapped onto GE fills and blits.
* Pixmaps backed by GEM dumb (CMA) buffers, with graceful software fallback
  when contiguous memory is unavailable.
* Optional TearFree double-buffered, vblank-synchronised page flipping.

The GE is a 32-bpp fill/copy/XOR engine with no alpha blending, so Render
compositing (e.g. anti-aliased glyphs) is handled by the X server in software;
the resulting blits to the screen are GE-accelerated.

## Building

Standard autotools:

```sh
./autogen.sh
make
sudo make install
```

Build-depends: `xserver-xorg-dev`, `libdrm-dev`, X.Org util-macros.

## License

MIT/X11 — see [COPYING](COPYING).
