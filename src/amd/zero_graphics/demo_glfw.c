#include "zgfx.h"

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <X11/Xlib-xcb.h>
#include <stdio.h>

#include "shaders/demo_triangle.h"

static void glfw_error(int code, const char *description)
{
   fprintf(stderr, "GLFW error %d: %s\n", code, description);
}

int main(void)
{
   glfwSetErrorCallback(glfw_error);
   glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
   if (!glfwInit())
      return 1;

   int result = 1;
   zgfx_device *dev = NULL;
   zgfx_swapchain *swapchain = NULL;
   zgfx_shader *vs = NULL, *ps = NULL;
   zgfx_command *cmd = NULL;

   glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
   GLFWwindow *window = glfwCreateWindow(640, 480, "zgfx GLFW demo", NULL, NULL);
   if (!window)
      goto cleanup;

   Display *display = glfwGetX11Display();
   if (!display)
      goto cleanup;
   xcb_connection_t *connection = XGetXCBConnection(display);
   if (!connection || xcb_connection_has_error(connection)) {
      fprintf(stderr, "Failed to get GLFW's XCB connection\n");
      goto cleanup;
   }

   dev = device_create();
   if (!dev)
      goto cleanup;
   swapchain = swapchain_create(dev, (zgfx_swapchain_desc){
      .xcb = {.connection = connection, .window = glfwGetX11Window(window)},
   });
   if (!swapchain)
      goto cleanup;

   vs = shader_create(dev, demo_triangle_spv, demo_triangle_spv_len, ZGFX_SHADER_VERTEX, "vertexMain");
   ps = shader_create(dev, demo_triangle_spv, demo_triangle_spv_len, ZGFX_SHADER_PIXEL, "fragmentMain");
   if (!vs || !ps)
      goto cleanup;

   typedef struct Vertex {
      float position[3];
      float normal[3];
      float uv[2];
   } Vertex;
   Vertex *vertices = galloc(dev, 3 * sizeof(Vertex));
   vertices[0] = (Vertex){.position = {-0.5f, -0.5f, 0.0f}};
   vertices[1] = (Vertex){.position = { 0.5f, -0.5f, 0.0f}};
   vertices[2] = (Vertex){.position = { 0.0f,  0.5f, 0.0f}};
   float *color = galloc(dev, 4 * sizeof(float));
   color[0] = color[3] = 1.0f;
   color[1] = color[2] = 0.0f;
   cmd = command_begin(dev);
   fflush(stdout);

   result = 0;
   while (!glfwWindowShouldClose(window)) {
      glfwPollEvents();
      if (glfwWindowShouldClose(window) || glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
         break;
      int width, height;
      glfwGetFramebufferSize(window, &width, &height);
      if (width == 0 || height == 0 || glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
         glfwWaitEventsTimeout(0.1);
         continue;
      }

      void *frame = NULL;
      zgfx_swapchain_result acquired = swapchain_acquire(swapchain, &frame);
      if (acquired == ZGFX_SWAPCHAIN_NOT_READY) {
         glfwWaitEventsTimeout(0.01);
         continue;
      }
      if (acquired == ZGFX_SWAPCHAIN_ERROR) {
         result = 1;
         break;
      }
      zgfx_swapchain_info info = swapchain_get_info(swapchain);

      command_reset(cmd);
      command_clear(cmd, frame, 0xff202020, 0);
      command_set_rendertarget(cmd, frame, info.width, info.height, info.format);
      command_set_viewport(cmd, (zgfx_viewport){
         .width = info.width, .height = info.height, .maxDepth = 1,
      });
      command_set_vertex_shader(cmd, vs);
      command_set_vertex_shader_args(cmd, vs, vertices);
      command_set_pixel_shader(cmd, ps);
      command_set_pixel_shader_args(cmd, ps, color);
      command_draw(cmd, 3);
      queue_submit(dev, cmd);
      if (swapchain_present(swapchain, frame) != 0) {
         result = 1;
         break;
      }
   }

cleanup:
   command_destroy(cmd);
   shader_destroy(vs);
   shader_destroy(ps);
   swapchain_destroy(swapchain);
   device_destroy(dev);
   if (window)
      glfwDestroyWindow(window);
   glfwTerminate();
   return result;
}
