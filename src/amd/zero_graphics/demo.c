#include "zgfx.h"
#include <stdio.h>

#include "shaders/demo.h"

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

   #if 1
   zgfx_shader* shader = shader_create(dev, demo_spv, demo_spv_len, ZGFX_SHADER_COMPUTE, "main");
   zgfx_command *cmd = command_begin(dev);
   uint32_t* target = galloc(dev, sizeof(uint32_t) * 32);
   print_array("target", target, 32);
   command_set_compute_shader(cmd, shader);
   command_set_compute_shader_args(cmd, shader, target);
   command_dispatch(cmd, 32, 1, 1);
   queue_submit(dev, cmd);
   print_array("target", target, 32);
   #endif

   return 0;
}
