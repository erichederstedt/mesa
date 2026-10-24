#ifndef ZGFX_H
#define ZGFX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

void zgfx_hello_world(void);

typedef struct zgfx_device zgfx_device;
typedef struct zgfx_backbuffer zgfx_backbuffer;
typedef struct zgfx_command zgfx_command;
typedef struct zgfx_shader zgfx_shader;
typedef struct zgfx_swapchain zgfx_swapchain;
typedef enum zgfx_color_format {
    ZGFX_COLOR_UNKOWN,
    ZGFX_COLOR_RGBA8_UNORM,
    ZGFX_COLOR_RGBA32_FLOAT,
    ZGFX_COLOR_BGRA8_UNORM,
} zgfx_color_format;
typedef enum zgfx_shader_type {
    ZGFX_SHADER_UNKOWN,
    ZGFX_SHADER_COMPUTE,
    ZGFX_SHADER_VERTEX,
    ZGFX_SHADER_PIXEL,
} zgfx_shader_type;
typedef struct zgfx_viewport {
    float    x;
    float    y;
    float    width;
    float    height;
    float    minDepth;
    float    maxDepth;
} zgfx_viewport;

zgfx_device *device_create(void);
void device_destroy(zgfx_device *dev);

uint8_t format_is_block_compressed(zgfx_color_format format);
uint64_t format_bit_size(zgfx_color_format format);
uint64_t format_compute_row_pitch_size(zgfx_color_format format, uint64_t width);
uint64_t format_compute_mip_size(zgfx_color_format format, uint64_t width, uint64_t height);

void *galloc(zgfx_device *dev, uint64_t size);
void gfree(zgfx_device *dev, void *ptr);
void *galloca(zgfx_device *dev, uint64_t size, uint64_t align);                                                                       // aligned
void *galloct(zgfx_device *dev, uint64_t width, uint64_t height, zgfx_color_format format, uint64_t mip_count, uint64_t array_count); // texture

zgfx_command *command_begin(zgfx_device *dev);
void command_reset(zgfx_command *cmd);
void command_destroy(zgfx_command *cmd);
void command_nop(zgfx_command *cmd);
void command_clear(zgfx_command *cmd, void *backbuffer, uint32_t color /*0xAABBGGRR*/, uint64_t size);
void command_set_compute_shader(zgfx_command *cmd, zgfx_shader* shader);
void command_dispatch(zgfx_command *cmd, int x, int y, int z);
void command_set_vertex_shader(zgfx_command *cmd, zgfx_shader* shader);
void command_set_pixel_shader(zgfx_command *cmd, zgfx_shader* shader);
void command_draw(zgfx_command *cmd, int vertexCount);
void command_set_viewport(zgfx_command *cmd, zgfx_viewport viewport); // just single view port for now
void command_set_rendertarget(zgfx_command *cmd, void* rendertarget, uint64_t width, uint64_t height, zgfx_color_format format);

void queue_submit(zgfx_device *dev, zgfx_command *cmd);

zgfx_shader *shader_create(zgfx_device *dev, uint8_t *spriv_bytes, uint64_t spriv_size, zgfx_shader_type shader_type, char *entry);
void shader_destroy(zgfx_shader *shader);

typedef struct zgfx_swapchain_desc {
    struct {
        void *connection;
        uint32_t window;
    } xcb;
    // Could add wayland/win32/etc later.
} zgfx_swapchain_desc;
typedef enum zgfx_swapchain_result {
    ZGFX_SWAPCHAIN_READY = 0,
    ZGFX_SWAPCHAIN_NOT_READY = 1,
    ZGFX_SWAPCHAIN_ERROR = 2,
} zgfx_swapchain_result;
typedef struct zgfx_swapchain_info {
    uint32_t width, height, stride;
    zgfx_color_format format;
} zgfx_swapchain_info;
zgfx_swapchain *swapchain_create(zgfx_device *dev, zgfx_swapchain_desc desc);
zgfx_swapchain_info swapchain_get_info(zgfx_swapchain *chain);
zgfx_swapchain_result swapchain_acquire(zgfx_swapchain *swapchain, void **frame);
int swapchain_present(zgfx_swapchain *swapchain, const void *frame);
void swapchain_destroy(zgfx_swapchain *swapchain);

typedef struct {
    void  *ptr;    /* address of the caller's argument */
    size_t size;   /* sizeof the argument */
} Args;

#define NARGS(...)  NARGS_(__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1)
#define NARGS_(_1, _2, _3, _4, _5, _6, _7, _8, N, ...) N

#define CAT(a, b)  CAT_(a, b)
#define CAT_(a, b) a##b

#define MAP_1(m, x)      m(x)
#define MAP_2(m, x, ...) m(x), MAP_1(m, __VA_ARGS__)
#define MAP_3(m, x, ...) m(x), MAP_2(m, __VA_ARGS__)
#define MAP_4(m, x, ...) m(x), MAP_3(m, __VA_ARGS__)
#define MAP_5(m, x, ...) m(x), MAP_4(m, __VA_ARGS__)
#define MAP_6(m, x, ...) m(x), MAP_5(m, __VA_ARGS__)
#define MAP_7(m, x, ...) m(x), MAP_6(m, __VA_ARGS__)
#define MAP_8(m, x, ...) m(x), MAP_7(m, __VA_ARGS__)

#define MAP(m, ...) CAT(MAP_, NARGS(__VA_ARGS__))(m, __VA_ARGS__)

#define LVAL_ARG(x) { (void *)&(x), sizeof(x) }

void command_set_compute_shader_args_impl(zgfx_command *cmd, zgfx_shader* shader, Args *args, size_t count);
#define command_set_compute_shader_args(cmd, shader, ...)    \
    command_set_compute_shader_args_impl((cmd), (shader),    \
                  (Args[]){ MAP(LVAL_ARG, __VA_ARGS__) },   \
                  NARGS(__VA_ARGS__))

void command_set_vertex_shader_args_impl(zgfx_command *cmd, zgfx_shader* shader, Args *args, size_t count);
#define command_set_vertex_shader_args(cmd, shader, ...)    \
    command_set_vertex_shader_args_impl((cmd), (shader),    \
                  (Args[]){ MAP(LVAL_ARG, __VA_ARGS__) },   \
                  NARGS(__VA_ARGS__))

void command_set_pixel_shader_args_impl(zgfx_command *cmd, zgfx_shader* shader, Args *args, size_t count);
#define command_set_pixel_shader_args(cmd, shader, ...)    \
    command_set_pixel_shader_args_impl((cmd), (shader),    \
                  (Args[]){ MAP(LVAL_ARG, __VA_ARGS__) },   \
                  NARGS(__VA_ARGS__))

void command_set_compute_shader_arg_impl(zgfx_command *cmd, zgfx_shader* shader, uint32_t index, const void *value, size_t size);
#define command_set_compute_shader_arg(cmd, shader, index, x) \
    command_set_compute_shader_arg_impl((cmd), (shader), (index), &(x), sizeof(x))

void command_set_vertex_shader_arg_impl(zgfx_command *cmd, zgfx_shader* shader, uint32_t index, const void *value, size_t size);
#define command_set_vertex_shader_arg(cmd, shader, index, x) \
    command_set_vertex_shader_arg_impl((cmd), (shader), (index), &(x), sizeof(x))

void command_set_pixel_shader_arg_impl(zgfx_command *cmd, zgfx_shader* shader, uint32_t index, const void *value, size_t size);
#define command_set_pixel_shader_arg(cmd, shader, index, x) \
    command_set_pixel_shader_arg_impl((cmd), (shader), (index), &(x), sizeof(x))

#ifdef __cplusplus
}
#endif

#endif
