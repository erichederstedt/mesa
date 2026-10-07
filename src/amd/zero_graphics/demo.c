#include "zgfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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
   /* Open the connection to the X server */
   xcb_connection_t *connection = xcb_connect(NULL, NULL);

   /* Get the first screen */
   const xcb_setup_t *setup = xcb_get_setup(connection);
   xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
   xcb_screen_t *screen = iter.data;

   /* Create the window */
   xcb_window_t window = xcb_generate_id (connection);
   xcb_create_window_checked(connection, screen->root_depth, window, screen->root, 0, 0, 150, 150, 10, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, NULL);
   const char title[] = "zgfx demo";
   xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, sizeof(title) - 1, title);

   /* Map the window on the screen */
   xcb_map_window(connection, window);

   /* Make sure commands are sent before we pause so that the window gets shown */
   xcb_flush(connection);

   pause(); /* hold client until Ctrl-C */

   xcb_disconnect(connection);
   #endif

   return 0;
}
