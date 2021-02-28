# linux-kmsgrab plugin for OBS

## Introduction

This plugin is a proof-of-concept libdrm-based screen capture for OBS. It uses DMA-BUF to import CRTC framebuffer directly into EGL texture in OBS as a source. This bypasses expensive double GPU->RAM RAM->GPU framebuffer copy that is invoked by anything X11-XSHM-based.

It is Linux-Only, as DMA-BUF is a Linux-only thing. Other platforms might have similar functionality, but I'm totally not an expert.

It is almost completely agnostic of any windowing system you might have: it works reasonably well with both X11 and Wayland, and theoretically could work even with bare KMS terminals.

However, on Wayland I'd recommend using something like https://hg.sr.ht/~scoopta/wlrobs instead -- it also uses DMA-BUF, but supposedly does this in a less hacky way.

## Building

It requires latest master OBS, as EGL support is very fresh and has not yet been released. You'll need to compile and *install* master OBS yourself. Make sure that installation prefix is fed into `cmake` invocation too, as it needs access to latest OBS headers from master and won't work with any older released version.

Generally it works like this:
```
# Clone and cd
mkdir build && cd build
export CMAKE_PREFIX_PATH=<master-obs-prefix>
cmake .. -GNinja -DCMAKE_INSTALL_PREFIX="$CMAKE_PREFIX_PATH"
ninja
ninja install
```

`linux-kmsgrab-send` binary needs `CAP_SYS_ADMIN` in order to be able to grab any screen contents using libdrm. There's no way around that unfortunately, as the ability to grab any output from any user has serious security implications.
```
sudo setcap cap_sys_admin+ep "$CMAKE_PREFIX_PATH/lib64/obs-plugins/linux-kmsgrab-send"
```

## Known issues
- there's no way to specify grabbing device (in cause you have more than one GPU), it will just use the first available
- no sync whatsoever, known to rarily cause weird capture glitches (dirty regions missing for a few seconds)
- no resolution/framebuffer following -- may break if output resolution changes
- may conflict with some x11 compositors and wayland impls
- currently just having this `linux-kmsgrab-send` binary lying around with caps set will make it possible for anyone having local user on your machine to grab any of your screens. Decide for yourself whether that's a concerning threat model for your situation.
