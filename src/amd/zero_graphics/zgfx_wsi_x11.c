#include "zgfx_private.h"
#include "zgfx_wsi_x11.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include "util/macros.h"

struct zgfx_x11_image {
   void *pixels;
   xcb_pixmap_t pixmap;
   uint32_t serial;
   bool busy, acquired;
};

struct zgfx_swapchain {
   zgfx_device *dev;
   xcb_connection_t *connection;
   xcb_window_t window;
   xcb_present_event_t event_id;
   xcb_special_event_t *events;
   struct zgfx_x11_image images[2];
   zgfx_color_format format;
   uint16_t width, height, image_width, image_height;
   uint32_t stride, serial;
   uint64_t last_msc;
   uint8_t depth;
   bool use_modifiers, pending, lost;
};

static bool x11_check(xcb_connection_t *connection, xcb_void_cookie_t cookie, const char *operation)
{
   xcb_generic_error_t *error = xcb_request_check(connection, cookie);
   if (error) {
      fprintf(stderr, "zgfx X11: %s failed (X11 error %u)\n", operation, error->error_code);
      free(error);
      return false;
   }
   return !xcb_connection_has_error(connection);
}

static void x11_images_destroy(zgfx_x11_swapchain *chain)
{
   for (unsigned i = 0; i < ARRAY_SIZE(chain->images); i++) {
      struct zgfx_x11_image *image = &chain->images[i];
      if (image->pixmap)
         xcb_free_pixmap(chain->connection, image->pixmap);
      gfree(chain->dev, image->pixels);
      *image = (struct zgfx_x11_image){0};
   }
   chain->image_width = chain->image_height = 0;
}

static bool x11_images_create(zgfx_x11_swapchain *chain)
{
   x11_images_destroy(chain);
   if (!chain->width || !chain->height || chain->width > 16384 || chain->height > 16384)
      return false;
   chain->stride = align((uint32_t)chain->width * 4, 256);
   if (!chain->use_modifiers && chain->stride > UINT16_MAX) {
      fprintf(stderr, "zgfx X11: row stride exceeds DRI3 1.0's limit\n");
      return false;
   }
   for (unsigned i = 0; i < ARRAY_SIZE(chain->images); i++) {
      struct zgfx_x11_image *image = &chain->images[i];
      image->pixels = galloct(chain->dev, chain->width, chain->height, chain->format, 1, 1);
      uint64_t index = find_buffer(chain->dev, image->pixels);
      assert(index != UINT64_MAX);
      assert(image->pixels == chain->dev->buffers_raw_ptr[index]);
      chain->dev->buffers_external[index] = true;
      struct amdgpu_bo_info info;
      uint32_t fd;
      if (amdgpu_bo_query_info(chain->dev->buffers_bo_handle[index], &info) ||
          info.alloc_size > UINT32_MAX ||
          amdgpu_bo_export(chain->dev->buffers_bo_handle[index], amdgpu_bo_handle_type_dma_buf_fd, &fd)) {
         fprintf(stderr, "zgfx X11: failed to export presentation buffer\n");
         goto fail;
      }
      xcb_pixmap_t pixmap = xcb_generate_id(chain->connection);
      xcb_void_cookie_t cookie;
      if (chain->use_modifiers) {
         int32_t dma_fd = fd;
         cookie = xcb_dri3_pixmap_from_buffers_checked(chain->connection, pixmap, chain->window,
                     1, chain->width, chain->height, chain->stride, 0, 0, 0, 0, 0, 0, 0,
                     chain->depth, 32, DRM_FORMAT_MOD_LINEAR, &dma_fd);
      } else {
         cookie = xcb_dri3_pixmap_from_buffer_checked(chain->connection, pixmap, chain->window,
                     info.alloc_size, chain->width, chain->height, chain->stride, chain->depth, 32, fd);
      }
      if (!x11_check(chain->connection, cookie, "import pixmap"))
         goto fail;
      image->pixmap = pixmap;
   }
   chain->image_width = chain->width;
   chain->image_height = chain->height;
   return true;
fail:
   x11_images_destroy(chain);
   return false;
}

static void x11_poll(zgfx_x11_swapchain *chain)
{
   xcb_generic_event_t *event;
   while ((event = xcb_poll_for_special_event(chain->connection, chain->events))) {
      xcb_present_generic_event_t *present = (void *)event;
      switch (present->evtype) {
      case XCB_PRESENT_CONFIGURE_NOTIFY: {
         xcb_present_configure_notify_event_t *configure = (void *)event;
         chain->width = configure->width;
         chain->height = configure->height;
         if (configure->pixmap_flags & 1)
            chain->lost = true;
         break;
      }
      case XCB_PRESENT_IDLE_NOTIFY: {
         xcb_present_idle_notify_event_t *idle = (void *)event;
         for (unsigned i = 0; i < ARRAY_SIZE(chain->images); i++) {
            if (chain->images[i].pixmap == idle->pixmap && chain->images[i].serial == idle->serial)
               chain->images[i].busy = false;
         }
         break;
      }
      case XCB_PRESENT_COMPLETE_NOTIFY: {
         xcb_present_complete_notify_event_t *complete = (void *)event;
         if (complete->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP && complete->serial == chain->serial) {
            chain->pending = false;
            chain->last_msc = complete->msc;
         }
         break;
      }
      }
      free(event);
   }
   chain->lost |= xcb_connection_has_error(chain->connection) != 0;
}

PUBLIC zgfx_x11_swapchain *zgfx_x11_swapchain_create(zgfx_device *dev, xcb_connection_t *connection,
                                                     xcb_window_t window)
{
   if (!dev || !connection || xcb_connection_has_error(connection))
      return NULL;
   zgfx_x11_swapchain *chain = calloc(1, sizeof(*chain));
   if (!chain)
      return NULL;
   chain->dev = dev;
   chain->connection = connection;
   chain->window = window;

   const xcb_query_extension_reply_t *dri3 = xcb_get_extension_data(connection, &xcb_dri3_id);
   const xcb_query_extension_reply_t *present = xcb_get_extension_data(connection, &xcb_present_id);
   if (!dri3 || !dri3->present || !present || !present->present) {
      fprintf(stderr, "zgfx X11: DRI3 and Present are required\n");
      goto fail;
   }
   xcb_dri3_query_version_cookie_t dc = xcb_dri3_query_version(connection, 1, 4);
   xcb_present_query_version_cookie_t pc = xcb_present_query_version(connection, 1, 4);
   xcb_dri3_query_version_reply_t *dv = xcb_dri3_query_version_reply(connection, dc, NULL);
   xcb_present_query_version_reply_t *pv = xcb_present_query_version_reply(connection, pc, NULL);
   bool supported = dv && pv && dv->major_version >= 1 && pv->major_version >= 1;
   if (supported) {
      chain->use_modifiers = (dv->major_version > 1 || dv->minor_version >= 2) &&
                             (pv->major_version > 1 || pv->minor_version >= 2);
      printf("DRI3 %u.%u, Present %u.%u\n", dv->major_version, dv->minor_version,
             pv->major_version, pv->minor_version);
   }
   free(dv);
   free(pv);
   if (!supported)
      goto fail;

   xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, window), NULL);
   xcb_get_window_attributes_reply_t *attributes =
      xcb_get_window_attributes_reply(connection, xcb_get_window_attributes(connection, window), NULL);
   if (!geometry || !attributes) {
      free(geometry);
      free(attributes);
      goto fail;
   }
   xcb_window_t root = geometry->root;
   chain->width = geometry->width;
   chain->height = geometry->height;
   chain->depth = geometry->depth;
   xcb_visualid_t visual_id = attributes->visual;
   free(geometry);
   free(attributes);
   const xcb_setup_t *setup = xcb_get_setup(connection);
   xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
   while (screens.rem && screens.data->root != root)
      xcb_screen_next(&screens);
   const xcb_visualtype_t *visual = NULL;
   if (screens.rem) {
      for (xcb_depth_iterator_t depths = xcb_screen_allowed_depths_iterator(screens.data); depths.rem; xcb_depth_next(&depths)) {
         for (xcb_visualtype_iterator_t visuals = xcb_depth_visuals_iterator(depths.data); visuals.rem; xcb_visualtype_next(&visuals)) {
            if (visuals.data->visual_id == visual_id)
               visual = visuals.data;
         }
      }
   }
   unsigned bpp = 0;
   for (xcb_format_iterator_t formats = xcb_setup_pixmap_formats_iterator(setup); formats.rem; xcb_format_next(&formats)) {
      if (formats.data->depth == chain->depth)
         bpp = formats.data->bits_per_pixel;
   }
   if (!visual || visual->_class != XCB_VISUAL_CLASS_TRUE_COLOR || bpp != 32 ||
       chain->depth != 24 || setup->image_byte_order != XCB_IMAGE_ORDER_LSB_FIRST || visual->green_mask != 0xff00) {
      fprintf(stderr, "zgfx X11: a 24-bit TrueColor visual with 32-bit storage is required\n");
      goto fail;
   }
   if (visual->red_mask == 0xff0000 && visual->blue_mask == 0xff)
      chain->format = ZGFX_COLOR_BGRA8_UNORM;
   else if (visual->red_mask == 0xff && visual->blue_mask == 0xff0000)
      chain->format = ZGFX_COLOR_RGBA8_UNORM;
   else
      goto fail;

   xcb_dri3_open_reply_t *opened = xcb_dri3_open_reply(connection, xcb_dri3_open(connection, root, 0), NULL);
   if (!opened)
      goto fail;
   int *fds = xcb_dri3_open_reply_fds(connection, opened);
   drmDevicePtr x_gpu = NULL, zgfx_gpu = NULL;
   bool same_gpu = opened->nfd == 1 && !drmGetDevice2(fds[0], 0, &x_gpu) &&
                   !drmGetDevice2(dev->fd, 0, &zgfx_gpu) && drmDevicesEqual(x_gpu, zgfx_gpu);
   for (unsigned i = 0; i < opened->nfd; i++)
      close(fds[i]);
   free(opened);
   drmFreeDevice(&x_gpu);
   drmFreeDevice(&zgfx_gpu);
   if (!same_gpu) {
      fprintf(stderr, "zgfx X11: X and zgfx must use the same GPU\n");
      goto fail;
   }
   if (chain->use_modifiers) {
      xcb_dri3_get_supported_modifiers_reply_t *modifiers = xcb_dri3_get_supported_modifiers_reply(
         connection, xcb_dri3_get_supported_modifiers(connection, window, chain->depth, bpp), NULL);
      bool linear = false;
      if (modifiers) {
         uint64_t *window_modifiers = xcb_dri3_get_supported_modifiers_window_modifiers(modifiers);
         uint64_t *screen_modifiers = xcb_dri3_get_supported_modifiers_screen_modifiers(modifiers);
         for (uint32_t i = 0; i < modifiers->num_window_modifiers; i++)
            linear |= window_modifiers[i] == DRM_FORMAT_MOD_LINEAR;
         for (uint32_t i = 0; i < modifiers->num_screen_modifiers; i++)
            linear |= screen_modifiers[i] == DRM_FORMAT_MOD_LINEAR;
      }
      free(modifiers);
      chain->use_modifiers = linear;
   }
   xcb_present_query_capabilities_reply_t *caps =
      xcb_present_query_capabilities_reply(connection, xcb_present_query_capabilities(connection, window), NULL);
   if (!caps)
      goto fail;
   printf("Present capabilities: 0x%08x; %s import\n", caps->capabilities,
          chain->use_modifiers ? "linear modifier" : "PixmapFromBuffer");
   free(caps);

   chain->event_id = xcb_generate_id(connection);
   chain->events = xcb_register_for_special_xge(connection, &xcb_present_id, chain->event_id, NULL);
   if (!chain->events || !x11_check(connection, xcb_present_select_input_checked(connection, chain->event_id, window,
         XCB_PRESENT_EVENT_MASK_CONFIGURE_NOTIFY | XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY |
         XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY), "select Present events"))
      goto fail;
   if (!x11_images_create(chain))
      goto fail;
   return chain;
fail:
   zgfx_x11_swapchain_destroy(chain);
   return NULL;
}

PUBLIC zgfx_x11_result zgfx_x11_swapchain_acquire(zgfx_x11_swapchain *chain, zgfx_x11_frame *frame)
{
   if (!chain || !frame)
      return ZGFX_X11_ERROR;
   x11_poll(chain);
   if (chain->lost)
      return ZGFX_X11_ERROR;
   for (unsigned i = 0; i < ARRAY_SIZE(chain->images); i++) {
      if (chain->images[i].acquired)
         return ZGFX_X11_NOT_READY;
   }
   if (chain->pending || !chain->width || !chain->height)
      return ZGFX_X11_NOT_READY;
   if (chain->image_width != chain->width || chain->image_height != chain->height) {
      if (!x11_images_create(chain)) {
         chain->lost = true;
         return ZGFX_X11_ERROR;
      }
   }
   for (unsigned i = 0; i < ARRAY_SIZE(chain->images); i++) {
      struct zgfx_x11_image *image = &chain->images[i];
      if (image->busy)
         continue;
      image->acquired = true;
      chain->dev->buffers_external[find_buffer(chain->dev, image->pixels)] = false;
      *frame = (zgfx_x11_frame){image->pixels, chain->width, chain->height, chain->stride, chain->format, i};
      return ZGFX_X11_READY;
   }
   return ZGFX_X11_NOT_READY;
}

PUBLIC zgfx_x11_result zgfx_x11_swapchain_present(zgfx_x11_swapchain *chain, const zgfx_x11_frame *frame)
{
   if (!chain || !frame || chain->lost || frame->image_index >= ARRAY_SIZE(chain->images))
      return ZGFX_X11_ERROR;
   struct zgfx_x11_image *image = &chain->images[frame->image_index];
   if (!image->acquired || image->pixels != frame->pixels || chain->pending)
      return ZGFX_X11_ERROR;
   uint32_t serial = ++chain->serial;
   if (!x11_check(chain->connection, xcb_present_pixmap_checked(chain->connection, chain->window,
         image->pixmap, serial, 0, 0, 0, 0, XCB_NONE, XCB_NONE, XCB_NONE, 0,
         chain->last_msc ? chain->last_msc + 1 : 0, 0, 0, 0, NULL), "present pixmap")) {
      chain->lost = true;
      return ZGFX_X11_ERROR;
   }
   image->acquired = false;
   image->busy = true;
   image->serial = serial;
   chain->pending = true;
   chain->dev->buffers_external[find_buffer(chain->dev, image->pixels)] = true;
   return ZGFX_X11_READY;
}

PUBLIC void zgfx_x11_swapchain_destroy(zgfx_x11_swapchain *chain)
{
   if (!chain)
      return;
   if (chain->events) {
      xcb_void_cookie_t cookie = xcb_present_select_input_checked(chain->connection, chain->event_id, chain->window, 0);
      free(xcb_request_check(chain->connection, cookie));
      xcb_unregister_for_special_event(chain->connection, chain->events);
   }
   x11_images_destroy(chain);
   xcb_flush(chain->connection);
   free(chain);
}

PUBLIC zgfx_swapchain *swapchain_create(zgfx_device *dev, zgfx_swapchain_desc desc)
{
   if (!desc.xcb.window)
      return NULL;
   return zgfx_x11_swapchain_create(dev, desc.xcb.connection, desc.xcb.window);
}

PUBLIC zgfx_swapchain_result swapchain_get_info(zgfx_swapchain *chain, zgfx_swapchain_info *info)
{
   if (!info)
      return ZGFX_SWAPCHAIN_ERROR;
   *info = (zgfx_swapchain_info){0};
   if (!chain || chain->lost || xcb_connection_has_error(chain->connection))
      return ZGFX_SWAPCHAIN_ERROR;
   for (unsigned i = 0; i < ARRAY_SIZE(chain->images); i++) {
      if (chain->images[i].acquired) {
         *info = (zgfx_swapchain_info){chain->image_width, chain->image_height, chain->stride, chain->format};
         return ZGFX_SWAPCHAIN_READY;
      }
   }
   return ZGFX_SWAPCHAIN_NOT_READY;
}

PUBLIC zgfx_swapchain_result swapchain_acquire(zgfx_swapchain *chain, void **frame)
{
   if (!frame)
      return ZGFX_SWAPCHAIN_ERROR;
   *frame = NULL;
   zgfx_x11_frame acquired;
   switch (zgfx_x11_swapchain_acquire(chain, &acquired)) {
   case ZGFX_X11_READY:
      *frame = acquired.pixels;
      return ZGFX_SWAPCHAIN_READY;
   case ZGFX_X11_NOT_READY:
      return ZGFX_SWAPCHAIN_NOT_READY;
   default:
      return ZGFX_SWAPCHAIN_ERROR;
   }
}

PUBLIC int swapchain_present(zgfx_swapchain *chain, const void *frame)
{
   if (!chain || !frame)
      return -1;
   for (unsigned i = 0; i < ARRAY_SIZE(chain->images); i++) {
      if (chain->images[i].pixels == frame && chain->images[i].acquired) {
         zgfx_x11_frame acquired = {.pixels = chain->images[i].pixels, .image_index = i};
         return zgfx_x11_swapchain_present(chain, &acquired) == ZGFX_X11_READY ?
                   0 : -1;
      }
   }
   return -1;
}

PUBLIC void swapchain_destroy(zgfx_swapchain *chain)
{
   zgfx_x11_swapchain_destroy(chain);
}
