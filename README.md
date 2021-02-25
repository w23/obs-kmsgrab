# linux-kmsgrab plugin for OBS

## Introduction

This plugin is a proof-of-concept libdrm-based screen capture for OBS. It uses DMA-BUF to import CRTC framebuffer directly into EGL texture in OBS as a source. This bypasses expensive double GPU->RAM RAM->GPU framebuffer copy that is invoked by anything X11-XSHM-based.

This plugins is almost completely agnostic of any windowing system you might have: it works reasonably well with both X11 and Wayland, and theoretically could work even with KMS terminals.

However, I'd recommend using something like https://hg.sr.ht/~scoopta/wlrobs instead -- it also uses dmabuf, but supposedly does this in a less hacky way.

This plugin is Linux-Only, as DMA-BUF is a Linux-only thing. Other platforms might have similar functionality, but I'm totally not an expert.

## Building

It requires latest master OBS, as EGL support is very fresh and has not yet been released. You'll need to compile and *install* master OBS yourself. Make sure that installation prefix is fed into `cmake` invocation too, as it needs access to latest OBS headers from master and won't work with any released version.

Generally it works like this:
```
export CMAKE_PREFIX_PATH=<obs-prefix>
cmake .. -GNinja -DCMAKE_INSTALL_PREFIX="$CMAKE_PREFIX_PATH"
ninja
ninja install
```

Note that install is slightly broken and won't install things properly, so some manual intervention is needed:
```
mv "$CMAKE_PREFIX_PATH"/lib64/obs-plugins/linux-kmsgrab.so "$CMAKE_PREFIX_PATH/lib/obs-plugins/"
cp obs-kmsgrab-send "$CMAKE_PREFIX_PATH/bin/"
sudo setcap cap_sys_admin+ep "$CMAKE_PREFIX_PATH/bin/obs-kmsgrab-send"
```

Yes, `obs-kmsgrab-send` does need `CAP_SYS_ADMIN` in order to be able to grab any screen contents. There's no way around that unfortunately.

## Known issues
- no device pick up -- just uses first available
- no sync whatsoever, known to rarly cause weird capture glitches (dirty regions missing for a few seconds)
- no resolution/framebuffer following -- may break if output resolution changes
- can conflict with some x11 compositors and wayland impls
