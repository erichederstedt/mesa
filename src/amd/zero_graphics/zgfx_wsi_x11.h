#ifndef ZGFX_WSI_X11_H
#define ZGFX_WSI_X11_H

#include "zgfx.h"
#include <xcb/xcb.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef zgfx_swapchain zgfx_x11_swapchain;

typedef struct zgfx_x11_frame {
   void *pixels;
   uint32_t width, height, stride;
   zgfx_color_format format;
   uint32_t image_index;
} zgfx_x11_frame;

typedef enum zgfx_x11_result {
   ZGFX_X11_ERROR = -1,
   ZGFX_X11_NOT_READY = 0,
   ZGFX_X11_READY = 1,
} zgfx_x11_result;

zgfx_x11_swapchain *zgfx_x11_swapchain_create(zgfx_device *dev, xcb_connection_t *connection, xcb_window_t window);
zgfx_x11_result zgfx_x11_swapchain_acquire(zgfx_x11_swapchain *swapchain, zgfx_x11_frame *frame);
zgfx_x11_result zgfx_x11_swapchain_present(zgfx_x11_swapchain *swapchain, const zgfx_x11_frame *frame);
void zgfx_x11_swapchain_destroy(zgfx_x11_swapchain *swapchain);

#ifdef __cplusplus
}
#endif

#endif
