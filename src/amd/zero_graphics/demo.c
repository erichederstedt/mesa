#include "zgfx.h"
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <xcb/xcb.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

#include "shaders/demo_compute.h"
#include "shaders/demo_triangle.h"

typedef struct float2 {
   float x, y;
} float2;
typedef union float3 {
   struct
   {
      float x, y, z;
   };
   struct {
      float2 xy;
   };
} float3;
typedef union float4 {
   struct
   {
      float x, y, z, w;
   };
   struct {
      float3 xyz;
   };
   struct {
      float2 xy;
   };
} float4;

static void print_array(char* name, uint32_t* array, uint32_t count) {
   printf("%s: ", name);
   for (uint32_t i = 0; i < count; i++) {
      printf("%08x", array[i]);
      if (i != (count-1))
         printf(", ");
   }
   printf("\n");
}

int main(void) {
   zgfx_hello_world();

   zgfx_device* dev = device_create();
   if (!dev)
      return 1;

   #if 0
   zgfx_command *cmd = command_begin(dev);
   void *backbuffer = galloct(dev, 1024, 1024, ZGFX_COLOR_RGBA8_UNORM, 1, 1);
   command_nop(cmd);
   printf("col: %08x\n", ((uint32_t *)backbuffer)[128]);
   command_clear(cmd, backbuffer, 0x00FF0000, 0);
   queue_submit(dev, cmd);
   printf("col: %08x\n", ((uint32_t *)backbuffer)[128]);
   #endif

   #if 0
   zgfx_shader* shader = shader_create(dev, demo_compute_spv, demo_compute_spv_len, ZGFX_SHADER_COMPUTE, "main");
   zgfx_command *cmd = command_begin(dev);
   uint32_t* target = galloc(dev, sizeof(uint32_t) * 32);
   print_array("target", target, 32);
   command_set_compute_shader(cmd, shader);
   command_set_compute_shader_args(cmd, shader, target);
   command_dispatch(cmd, 32, 1, 1);
   queue_submit(dev, cmd);
   print_array("target", target, 32);
   #endif

   #if 0
   typedef struct __attribute__((aligned(16))) Vertex
   {
      float3 position;
      float3 normal;
      float2 uv;
   } Vertex;
   Vertex* vertices = galloc(dev, sizeof(Vertex) * 3);
   vertices[0].position = (float3){.x = -0.5f, .y = -0.5f, .z = 0.0f};
   vertices[1].position = (float3){.x = 0.5f, .y = -0.5f, .z = 0.0f};
   vertices[2].position = (float3){.x = 0.0f, .y = 0.5f, .z = 0.0f};
   float4* triColor = galloc(dev, sizeof(float4));
   triColor->x = 1.0;
   triColor->y = 0.0;
   triColor->z = 0.0;
   triColor->w = 1.0;
   zgfx_shader* vs_shader = shader_create(dev, demo_triangle_spv, demo_triangle_spv_len, ZGFX_SHADER_VERTEX, "vertexMain");
   zgfx_shader* ps_shader = shader_create(dev, demo_triangle_spv, demo_triangle_spv_len, ZGFX_SHADER_PIXEL, "fragmentMain");
   void *backbuffer = galloct(dev, 1024, 1024, ZGFX_COLOR_RGBA8_UNORM, 1, 1);
   zgfx_command *cmd = command_begin(dev);
   command_clear(cmd, backbuffer, 0x00000000, 0);
   command_set_rendertarget(cmd, backbuffer, 1024, 1024, ZGFX_COLOR_RGBA8_UNORM);
   command_set_viewport(cmd, (zgfx_viewport){
      .x = 0, .y = 0,
      .width = 1024, .height = 1024,
      .minDepth = 0, .maxDepth = 1,
   });
   command_set_vertex_shader(cmd, vs_shader);
   command_set_vertex_shader_args(cmd, vs_shader, vertices);
   command_set_pixel_shader(cmd, ps_shader);
   command_set_pixel_shader_args(cmd, ps_shader, triColor);
   command_draw(cmd, 3);
   queue_submit(dev, cmd);
   stbi_write_bmp("pos_output.bmp", 1024, 1024, 4, backbuffer);
   #endif

   #if 1
   int result = 1;
   int screen_number = 0;
   xcb_connection_t *connection = xcb_connect(NULL, &screen_number);
   xcb_window_t window = XCB_NONE;
   bool window_alive = false;
   zgfx_swapchain *swapchain = NULL;
   zgfx_shader *vs_shader = NULL, *ps_shader = NULL;
   zgfx_command *cmd = NULL;
   if (xcb_connection_has_error(connection)) {
      fprintf(stderr, "Failed to connect to the X server\n");
      goto cleanup;
   }
   xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
   for (int i = 0; i < screen_number && screens.rem; i++)
      xcb_screen_next(&screens);
   if (!screens.rem)
      goto cleanup;
   xcb_screen_t *screen = screens.data;
   window = xcb_generate_id(connection);
   uint32_t window_values[] = {screen->black_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_EXPOSURE};
   xcb_generic_error_t *error = xcb_request_check(connection,
      xcb_create_window_checked(connection, screen->root_depth, window, screen->root,
         0, 0, 640, 480, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
         XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, window_values));
   if (error) {
      fprintf(stderr, "Failed to create the window: X11 error %u\n", error->error_code);
      free(error);
      goto cleanup;
   }
   window_alive = true;
   const char title[] = "zgfx demo";
   xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME,
                       XCB_ATOM_STRING, 8, sizeof(title) - 1, title);
   xcb_intern_atom_cookie_t protocols_cookie = xcb_intern_atom(connection, 0, 12, "WM_PROTOCOLS");
   xcb_intern_atom_cookie_t delete_cookie = xcb_intern_atom(connection, 0, 16, "WM_DELETE_WINDOW");
   xcb_intern_atom_reply_t *protocols = xcb_intern_atom_reply(connection, protocols_cookie, NULL);
   xcb_intern_atom_reply_t *delete_window = xcb_intern_atom_reply(connection, delete_cookie, NULL);
   if (!protocols || !delete_window) {
      free(protocols);
      free(delete_window);
      goto cleanup;
   }
   xcb_atom_t wm_protocols = protocols->atom, wm_delete = delete_window->atom;
   free(protocols);
   free(delete_window);
   xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, wm_protocols, XCB_ATOM_ATOM, 32, 1, &wm_delete);

   swapchain = swapchain_create(dev, (zgfx_swapchain_desc){.xcb = {.connection = connection, .window = window}});
   if (!swapchain)
      goto cleanup;
   vs_shader = shader_create(dev, demo_triangle_spv, demo_triangle_spv_len, ZGFX_SHADER_VERTEX, "vertexMain");
   ps_shader = shader_create(dev, demo_triangle_spv, demo_triangle_spv_len, ZGFX_SHADER_PIXEL, "fragmentMain");
   if (!vs_shader || !ps_shader)
      goto cleanup;
   typedef struct Vertex {
      float3 position;
      float3 normal;
      float2 uv;
   } Vertex;
   Vertex *vertices = galloc(dev, sizeof(Vertex) * 3);
   vertices[0] = (Vertex){.position = {.x = -0.5f, .y = -0.5f, .z = 0.0f}};
   vertices[1] = (Vertex){.position = {.x =  0.5f, .y = -0.5f, .z = 0.0f}};
   vertices[2] = (Vertex){.position = {.x =  0.0f, .y =  0.5f, .z = 0.0f}};
   float4 *triColor = galloc(dev, sizeof(float4));
   *triColor = (float4){.x = 1.0f, .y = 0.0f, .z = 0.0f, .w = 1.0f};
   cmd = command_begin(dev);
   error = xcb_request_check(connection, xcb_map_window_checked(connection, window));
   if (error) {
      free(error);
      goto cleanup;
   }
   xcb_flush(connection);
   fflush(stdout);

   bool running = true, mapped = false;
   result = 0;
   while (running) {
      xcb_generic_event_t *event;
      while ((event = xcb_poll_for_event(connection))) {
         switch (event->response_type & 0x7f) {
         case XCB_CLIENT_MESSAGE: {
            xcb_client_message_event_t *message = (void *)event;
            if (message->window == window && message->type == wm_protocols &&
                message->format == 32 && message->data.data32[0] == wm_delete)
               running = false;
            break;
         }
         case XCB_DESTROY_NOTIFY:
            if (((xcb_destroy_notify_event_t *)event)->window == window) {
               window_alive = false;
               running = false;
            }
            break;
         case XCB_MAP_NOTIFY:
            if (((xcb_map_notify_event_t *)event)->window == window)
               mapped = true;
            break;
         case XCB_UNMAP_NOTIFY:
            if (((xcb_unmap_notify_event_t *)event)->window == window)
               mapped = false;
            break;
         case 0:
            fprintf(stderr, "X11 error %u\n", ((xcb_generic_error_t *)event)->error_code);
            result = 1;
            running = false;
            break;
         }
         free(event);
      }
      if (!running)
         break;
      if (xcb_connection_has_error(connection)) {
         result = 1;
         break;
      }
      if (mapped) {
         zgfx_texture *frame = NULL;
         zgfx_swapchain_result acquired = swapchain_acquire(swapchain, &frame);
         if (acquired == ZGFX_SWAPCHAIN_ERROR) {
            result = 1;
            break;
         }
         if (acquired == ZGFX_SWAPCHAIN_READY) {
            zgfx_swapchain_info info;
            if (swapchain_get_info(swapchain, &info) != ZGFX_SWAPCHAIN_READY) {
               result = 1;
               break;
            }
            command_reset(cmd);
            command_clear(cmd, frame, 0xff202020, 0);
            command_set_rendertarget(cmd, frame, info.width, info.height, info.format);
            command_set_viewport(cmd, (zgfx_viewport){
               .width = info.width, .height = info.height, .minDepth = 0, .maxDepth = 1,
            });
            command_set_vertex_shader(cmd, vs_shader);
            command_set_vertex_shader_args(cmd, vs_shader, vertices);
            command_set_pixel_shader(cmd, ps_shader);
            command_set_pixel_shader_args(cmd, ps_shader, triColor);
            command_draw(cmd, 3);
            queue_submit(dev, cmd);
            if (swapchain_present(swapchain, frame) != 0) {
               result = 1;
               break;
            }
            continue;
         }
      }
      struct pollfd poll_fd = {.fd = xcb_get_file_descriptor(connection), .events = POLLIN};
      poll(&poll_fd, 1, 100);
   }

cleanup:
   command_destroy(cmd);
   shader_destroy(vs_shader);
   shader_destroy(ps_shader);
   swapchain_destroy(swapchain);
   if (window_alive)
      xcb_destroy_window(connection, window);
   xcb_disconnect(connection);
   device_destroy(dev);
   return result;
   #endif

   device_destroy(dev);
   return 0;
}
