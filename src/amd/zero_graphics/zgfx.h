#ifndef ZGFX_H
#define ZGFX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void zgfx_hello_world(void);

typedef struct zgfx_device zgfx_device;
typedef struct zgfx_backbuffer zgfx_backbuffer;
typedef struct zgfx_command zgfx_command;
typedef enum zgfx_color_format {
    ZGFX_COLOR_UNKOWN,
    ZGFX_COLOR_RGBA8_UNORM,
    ZGFX_COLOR_RGBA32_FLOAT,
} zgfx_color_format;

zgfx_device *device_create(void);

uint8_t format_is_block_compressed(zgfx_color_format format);
uint64_t format_bit_size(zgfx_color_format format);
uint64_t format_compute_row_pitch_size(zgfx_color_format format, uint64_t width);
uint64_t format_compute_mip_size(zgfx_color_format format, uint64_t width, uint64_t height);

void *galloc(zgfx_device *dev, uint64_t size);
void *galloca(zgfx_device *dev, uint64_t size, uint64_t align);                                                                       // aligned
void *galloct(zgfx_device *dev, uint64_t width, uint64_t height, zgfx_color_format format, uint64_t mip_count, uint64_t array_count); // texture

zgfx_command *command_begin(zgfx_device *dev);
void command_nop(zgfx_command *cmd);
void command_clear(zgfx_command *cmd, void *backbuffer, uint32_t color, uint64_t size);

void queue_submit(zgfx_device *dev, zgfx_command *cmd);

#ifdef __cplusplus
}
#endif

#endif
