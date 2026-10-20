# Find demo build instructions

> I have been working on a custom graphics api/driver in src/amd/zero_graphics for a while now but I have forgotten how to build it. I have some instructions in [README.rst](README.rst) but I am not sure if those are correct. I want to build the demo specifically.

<details><summary>2 previous messages</summary>

> I’ll check the README and the demo’s build configuration, then try the build.
>
> The README is missing `-Dzero-graphics=true`; that option defaults to off, so its setup command won’t create the demo target. I tried a configuration focused on Zero Graphics, but setup stopped because this checkout requires libdrm 2.4.133 or newer and your system has 2.4.124. The XCB DRI3 and Present development packages are also missing.
</details>

The README is missing **`-Dzero-graphics=true`**. Without it, Meson won’t create `zgfx_demo`.

From the repository root, use:

```sh
meson setup build-zgfx \
  -Dzero-graphics=true \
  -Dgallium-drivers= \
  -Dvulkan-drivers= \
  -Dplatforms=x11 \
  -Dglx=disabled \
  -Degl=disabled \
  -Dgbm=disabled

meson compile -C build-zgfx zgfx_demo
./build-zgfx/src/amd/zero_graphics/zgfx_demo
```

I tried setup, but **your installed dependencies currently block the build**:

- libdrm **≥ 2.4.133** required; installed version is **2.4.124**.
- `xcb-dri3` and `xcb-present` development packages are missing—on Debian, these are `libxcb-dri3-dev` and `libxcb-present-dev`.

Mesa can fetch and build its bundled libdrm dependency if you add `-Dallow-fallback-for=libdrm` to setup.

The demo uses checked-in shader headers, so building it doesn’t require `slangc`. Running it requires an X11 connection and an AMD GPU accessible at `/dev/dri/renderD128`. No source files were changed.

> Is there a way to change it so we only require 2.4.124? Or is there an actual reason we are using the newest thing possible

<details><summary>1 previous message</summary>

> I’ll check which newer libdrm APIs the Zero Graphics build uses and whether its requirement can be lowered independently.
</details>

There’s an actual dependency on newer libdrm in the shared AMD code, even though Zero Graphics itself might not need the new functionality.

For example, [ac_gpu_info.c](./src/amd/common/ac_gpu_info.c:1822) uses `amdgpu_sw_info_address_prt_wa_control_bit` to support a GPU memory-access workaround. Your installed 2.4.124 headers don’t define it. Zero Graphics builds this file and calls `ac_query_gpu_info()`, so simply lowering the Meson requirement would cause a compilation error.

Supporting 2.4.124 is possible, but would require compatibility changes and checking for other newer APIs. Disabling the workaround indiscriminately could cause incorrect behavior on affected GPUs.

The easier route is to build a newer libdrm locally using Meson’s existing fallback:

```sh
meson setup build-zgfx \
  -Dzero-graphics=true \
  -Dallow-fallback-for=libdrm \
  -Dgallium-drivers= -Dvulkan-drivers= \
  -Dplatforms=x11 -Dglx=disabled -Degl=disabled -Dgbm=disabled
```

That downloads and builds libdrm for this build without replacing your system installation. You’ll still need the missing XCB development packages.

> Alright, everything compiles and runs. The triangle that get's rendered is upside down which is what was happening before as well, this isn't anything new but it has always been incorrect. How would I go about fixing that. Do I insert a step right before present where I run a compute shader that flips the image or something?

<details><summary>1 previous message</summary>

> I’ll check the viewport transform and presentation path to see where the vertical orientation is set.
</details>

Fix this in the **viewport transform**. A compute pass would add unnecessary work over the whole image.

Your [viewport implementation](./src/amd/zero_graphics/zgfx.c:900) currently computes:

```text
screen_y = ndc_y * height/2 + y + height/2
```

With positive height, positive Y moves down the window. Your triangle’s tip has Y = +0.5, so it points down.

You already support negative viewport heights. In the [demo’s viewport setup](./src/amd/zero_graphics/demo.c:247), use:

```c
command_set_viewport(cmd, (zgfx_viewport){
   .x = 0,
   .y = (float)info.height,
   .width = (float)info.width,
   .height = -(float)info.height,
   .minDepth = 0,
   .maxDepth = 1,
});
```

Cast before negating because `info.height` may be unsigned. This maps NDC Y = +1 to the top and −1 to the bottom, with no extra rendering pass.

For your API design, decide whether positive viewport height should mean Y points up or down. If you want Y-up by default, implement that convention inside `command_set_viewport()` instead. A Y flip also reverses triangle winding, which matters when you enable face culling.

> Alright, I added two lines into command_set_viewport which fixes it. But this doen't feel right. What if someone actually means to set their y to something?

<details><summary>1 previous message</summary>

> I’ll look at those two lines and check how they affect viewports with a nonzero Y offset.
</details>

Yes—`viewport.y = viewport.height` discards the caller’s Y offset. To flip within their chosen rectangle, use:

```c
viewport.y += viewport.height;
viewport.height = -viewport.height;
```

For example, input `y = 100, height = 300` becomes `y = 400, height = -300`. Both cover rows 100–400, but the direction reverses.

If Y-up is your API’s default convention, a clearer implementation is to remove those two lines and change only the Y-scale register value to:

```c
fui(-viewport.height * 0.5f)
```

Keep the existing Y offset:

```c
fui(viewport.y + viewport.height * 0.5f)
```

That preserves the rectangle and its center while reversing the vertical mapping. Your bounds and guardband calculations can stay as they were.

> The demo is quite bloated by all the xcb code. Couldn't we just use xlib and use XGetXCBConnection etc

Yes. You can use **Xlib for window creation and events**, then pass `XGetXCBConnection(display)` and the Xlib window ID to your existing XCB swapchain implementation.

The demo would use:

- `XOpenDisplay`, `XCreateSimpleWindow`, and `XMapWindow` for setup.
- `XSetWMProtocols` for the close button.
- `XPending` / `XNextEvent` for events.
- `XGetXCBConnection` from `<X11/Xlib-xcb.h>` for the swapchain connection.

Keep **Xlib owning the event queue**—don’t call `XSetEventQueueOwner(..., XCBOwnsEventQueue)`—and remove the demo’s `xcb_poll_for_event` calls. Check whether your swapchain consumes events through XCB special event queues; those can coexist with Xlib event handling.

Call `XFlush(display)` before the swapchain starts issuing XCB requests against the new window. During cleanup, destroy the swapchain before `XCloseDisplay`; don’t separately call `xcb_disconnect` on the borrowed connection.

You’d add `x11` and `x11-xcb` dependencies to the demo. The driver can keep using XCB internally.

This would shorten window setup and error handling, though the event loop and swapchain lifecycle still need to exist.

> Can you check if what I've done is correct?
>
> Also changed how swapchain_get_info works. As it should not need an acquire check.

<details><summary>1 previous message</summary>

> I’ll review the Xlib changes and the new `swapchain_get_info` behavior, including event handling and resize cases.
</details>

The approach is sound, but there are a few issues:

- **Build failure: missing header.** [demo.c:295](./src/amd/zero_graphics/demo.c:295) calls `XGetXCBConnection` without including `<X11/Xlib-xcb.h>`. I ran the build and confirmed this fails.

- **Normal frame pacing exits the demo.** At [demo.c:333](./src/amd/zero_graphics/demo.c:333), `acquired != READY` includes `NOT_READY`, making the subsequent `continue` unreachable. Break only on `ZGFX_SWAPCHAIN_ERROR`; retry on `NOT_READY`. Add a wait to that retry path to avoid busy spinning.

- **Flush Xlib before creating the swapchain.** Add `XFlush(display)` after window setup and before `swapchain_create()`. Otherwise, XCB can query the window before Xlib has sent its creation request.

- **Check `XOpenDisplay()` for NULL** before using `display` at [demo.c:277](./src/amd/zero_graphics/demo.c:277). Currently, running without an accessible display crashes.

- **Stop after processing a close request.** Check `quit` immediately after the event loop. Also validate the client message’s window, `WM_PROTOCOLS` message type, and format before treating it as a close request.

Your `swapchain_get_info()` change is reasonable **as a getter for the currently allocated images**. It doesn’t need an acquired image. The resize detail matters, though: images are recreated during `swapchain_acquire()`, so calling the getter before acquire can return the previous dimensions. Keep querying it after successful acquire when configuring rendering, as your demo does now.

Removing error reporting also makes a valid, non-NULL swapchain a caller requirement, and the getter no longer indicates a lost connection. That’s fine if intentional.

The event ownership is correct: Xlib handles ordinary events, and your swapchain uses XCB’s separate Present event queue. No source files changed.

> I am trying to avoid bloat and non-critical error checks for brevitys sake. So I won't check the result of XOpenDisplay. Also it is normal to let the last frame play out so I won't be changing it to immediatly break out of the loop. Other than that can you see anything wrong with what I've done?

<details><summary>2 previous messages</summary>

> I’ll check the current code again, keeping those choices in mind.
>
> <details><summary>Ran a command</summary>
>
> - Ran `sed -n '270,400p' src/amd/zero_graphics/demo.c; sed -n '340,370p' src/amd/zero_graphics/zgfx_wsi_x11.c; head -12 src/amd/zero_graphics/demo.c; git diff -- src/amd/zero_graphics/meson.build`
>
> </details>
</details>

The header, flush, and acquire handling are fixed. Given your intended demo scope, I see one remaining practical issue:

**The `NOT_READY` path busy-spins.** While waiting for presentation completion, it repeatedly calls `XPending()` and `swapchain_acquire()`, potentially consuming a CPU core. A short delay there is a simple solution; an event-based wait would be more efficient but adds complexity.

Your `swapchain_get_info()` implementation is fine as a getter for allocated image dimensions. Calling it after successful acquire, as you do, ensures it reflects any resize.

Cleanup order and Xlib/XCB event ownership also look correct. Letting the current iteration finish after a close request is fine.

Model changed

gpt-6.1-sol -> gpt-6-astra

> Alright, so I am trying to implement setting more shader arguments. Specifically I am trying to implement setting a dynamic number of shader arguments via some macros.
>
> What is necessary to implement the new command_set_X_shader_args_impl functions?
>
> Would it be posssible to change command_set_graphics_shader_args to take in a argument index maybe? That way you can just iterate through and set the arguments and even have another user facing function that is just sets one argument but one specific index.

<details><summary>2 previous messages</summary>

> I’ll trace the new macros through argument storage and shader compilation to see what an indexed setter needs to update.
>
> <details><summary>Explored 4 files, 2 searches, ran a command</summary>
>
> - Ran `git diff -- src/amd/zero_graphics/zgfx.h src/amd/zero_graphics/zgfx.c; grep -nE 'shader_args|push_constant|user_data|args_impl' src/amd/zero_graphics/zgfx.c src/amd/zero_graphics/zgfx.h src/amd/zero_graphics/zgfx_private.h src/amd/zero_graphics/zgfx_shader.c`
>
> <details><summary>Explored 6 files, 2 searches</summary>
>
> - Read `./src/amd/zero_graphics/zgfx_shader.c`
> - Read `./src/amd/zero_graphics/zgfx_shader.c`
> - Read `./src/amd/zero_graphics/zgfx.c`
> - Read `./src/amd/zero_graphics/zgfx.c`
> - Read `./src/amd/zero_graphics/zgfx_private.h`
> - Searched for `argument_ptr|lower.*push|lower.*arg|zgfx_lower` in `zgfx_shader.c`
> - Read `./src/amd/zero_graphics/shaders/demo_compute.slang`
> - Searched for `shader_args` in `demo.c`
>
> </details>
>
> </details>
</details>

Yes—an indexed setter is a good foundation, and the bulk `_args_impl` functions can iterate over it. But **the shader compiler must also support multiple arguments**; changing the command emission alone won’t work.

Currently, your implementation assumes exactly one 64-bit argument:

- [zgfx_shader.c:331](./src/amd/zero_graphics/zgfx_shader.c:331) reserves two SGPRs for `argument_ptr`.
- Push-constant accesses beyond eight bytes are rejected.
- `shader_lower_arguments()` extracts all argument data from those same two registers.
- The command setters write one pointer into those registers.

Simply adding `index * 8` to the register address would overwrite other shader inputs, such as the vertex shader’s `base_vertex`.

For your proposed API, I’d implement it in these steps:

1. **Define the argument layout.** Each shader needs argument metadata: its byte offset, size, and register location. For mixed pointers, scalars, and vectors, `sizeof()` alone doesn’t tell you the shader’s alignment or padding. Obtain that layout from shader reflection or provide an explicit layout when creating the shader. Preserve argument indices even when an argument is unused.

2. **Reserve argument registers during compilation.** Allocate enough user SGPRs for the supported argument block, before adding built-in inputs. Update the eight-byte validation and the lowering pass to read the appropriate registers for each push-constant byte offset. Enforce the hardware’s user-SGPR budget, including built-ins.

3. **Have the indexed setter write argument values.** A suitable interface is:

   ```c
   void command_set_vertex_shader_arg(
      zgfx_command *cmd, zgfx_shader *shader,
      uint32_t index, const void *value, size_t size);
   ```

   Internally, look up the argument’s register location and emit its value there. Compute, vertex, and pixel setters can share the packing logic, selecting their respective register bases.

4. **Implement the bulk setters using that operation.** For each `args[i]`, pass index `i`, `args[i].ptr`, and `args[i].size`. Initially, calling the indexed setter in a loop is fine; later, you can emit adjacent register writes together.

There’s an important distinction in your macro: for `vertices`, `Args.ptr` is **`&vertices`**, not the GPU address stored in `vertices`. The implementation must read the pointer value from that CPU memory—typically using `memcpy`—and emit those bits. Casting `Args.ptr` to an integer, as the old setter does with `data`, would send the address of the local variable to the GPU.

For the smallest first implementation, you could support **only 64-bit pointer arguments**, with a matching shader layout of consecutive eight-byte slots. Supporting arbitrary values through `Args.size` requires the layout handling above.

Your macros are suitable for collecting arguments, with two limitations: they currently accept **1–8 arguments**, and each argument must be an addressable lvalue. Copy their values during the setter call; don’t retain the temporary array or its pointers.
