#include "zgfx.h"
#include <stdio.h>

#include "shaders/demo_compute.h"
#include "shaders/demo_triangle.h"

typedef struct float2 {
   float x, y;
} float2;
typedef union float3 {
   struct {
      float2 xy;
   };
   struct
   {
      float x, y, z;
   };
} float3;
typedef union float4 {
   struct {
      float2 xy;
   };
   struct {
      float3 xyz;
   };
   struct
   {
      float x, y, z, w;
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

   #if 1
   typedef struct __attribute__((aligned(16))) Vertex
   {
      float3 position;
      float3 normal;
      float2 uv;
   } Vertex;
   Vertex* vertices = galloc(dev, sizeof(Vertex) * 3);
   // Fill in vert data
   float4* triColor = galloc(dev, sizeof(float4));
   // Pick triangle color
   zgfx_shader* vs_shader = shader_create(dev, demo_triangle_spv, demo_triangle_spv_len, ZGFX_SHADER_VERTEX, "vertexMain");
   zgfx_shader* ps_shader = shader_create(dev, demo_triangle_spv, demo_triangle_spv_len, ZGFX_SHADER_PIXEL, "fragmentMain");
   void *backbuffer = galloct(dev, 1024, 1024, ZGFX_COLOR_RGBA8_UNORM, 1, 1);
   zgfx_command *cmd = command_begin(dev);
   command_clear(cmd, backbuffer, 0x00FF0000, 0);
   command_set_vertex_shader(cmd, vs_shader);
   command_set_vertex_shader_args(cmd, vs_shader, vertices);
   command_set_pixel_shader(cmd, ps_shader);
   command_set_pixel_shader_args(cmd, ps_shader, triColor);
   queue_submit(dev, cmd);
   #endif

   return 0;
}
