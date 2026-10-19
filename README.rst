`ZGFX Mesa <https://mesa3d.org>`
======================================================

This is a custom version of mesa with a new experimental graphics api called Zero Graphics

Build lib + demo:
.. code-block:: sh

  $ meson setup build-zgfx
  $ meson compile -C build-zgfx zgfx_demo

Run demo
.. code-block:: sh

  $ ./build-zgfx/src/amd/zero_graphics/zgfx_demo