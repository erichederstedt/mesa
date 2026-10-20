# ZGFX Mesa <<https://mesa3d.org>>
This is a custom version of mesa with a new experimental graphics api called Zero Graphics

Build lib + demo:

```sh
$ meson setup build-zgfx -Dzero-graphics=true -Dallow-fallback-for=libdrm -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms=x11 -Dglx=disabled -Degl=disabled -Dgbm=disabled
$ meson compile -C build-zgfx zgfx_demo
```

Run demo
```sh
$ ./build-zgfx/src/amd/zero_graphics/zgfx_demo
```