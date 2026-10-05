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
typedef struct zgfx_shader zgfx_shader;
typedef enum zgfx_color_format {
    ZGFX_COLOR_UNKOWN,
    ZGFX_COLOR_RGBA8_UNORM,
    ZGFX_COLOR_RGBA32_FLOAT,
} zgfx_color_format;
typedef enum zgfx_shader_type {
    ZGFX_SHADER_UNKOWN,
    ZGFX_SHADER_COMPUTE,
    ZGFX_SHADER_VERTEX,
    ZGFX_SHADER_PIXEL,
} zgfx_shader_type;

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
void command_set_compute_shader(zgfx_command *cmd, zgfx_shader* shader);
void command_set_compute_shader_args(zgfx_command *cmd, zgfx_shader* shader, void* data);
void command_dispatch(zgfx_command *cmd, int x, int y, int z);
void command_set_vertex_shader(zgfx_command *cmd, zgfx_shader* shader);
void command_set_vertex_shader_args(zgfx_command *cmd, zgfx_shader* shader, void* data);
void command_set_pixel_shader(zgfx_command *cmd, zgfx_shader* shader);
void command_set_pixel_shader_args(zgfx_command *cmd, zgfx_shader* shader, void* data);
void command_draw(zgfx_command *cmd, int vertexCount);

void queue_submit(zgfx_device *dev, zgfx_command *cmd);

zgfx_shader *shader_create(zgfx_device *dev, uint8_t* spriv_bytes, uint64_t spriv_size, zgfx_shader_type shader_type, char* entry);

#ifdef __cplusplus
}
#endif

#endif
