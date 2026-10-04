#include "zgfx.h"
#include <stdio.h>

int
main(void)
{
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
   zgfx_shader* shader = shader_create(dev, 0, 0, ZGFX_SHADER_COMPUTE, "main");
   #endif

   return 0;
}
