/*
 * Copyright © 2016 Canonical, Inc
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Cemil Azizoglu <cemil.azizoglu@canonical.com>
 */

#include <mir_toolkit/mir_presentation_chain.h>
#include <mir_toolkit/mir_buffer.h>
#include <mir_toolkit/rs/mir_render_surface.h>
#include <mir_toolkit/extensions/gbm_buffer.h>
#include <mir_toolkit/extensions/mesa_drm_auth.h>

#include <string.h>
#include <stdio.h>
#include <xf86drm.h>

#include "egl_dri2.h"
#include "loader.h"

#define MAX_BUFFERS (4)
#define NUM_DEFAULT_BUFFERS (3) /* Can be at most MAX_BUFFERS */

enum buffer_state
{
   buffer_state_none = 0,
   buffer_state_available,
   buffer_state_acquired,
   buffer_state_submitted,
};

typedef struct SwapChain
{
    MirRenderSurface*                     surface;
    MirPresentationChain*                 chain;
    MirPixelFormat                        format;
    uint32_t                              gbm_format;
    unsigned int                          buffer_count;
    MirBuffer*                            buffers[MAX_BUFFERS];
    enum buffer_state                     state[MAX_BUFFERS];
    uint32_t                              next_buffer_to_use;
    pthread_mutex_t                       lock;
    pthread_cond_t                        cv;
    struct MirExtensionGbmBufferV2 const* gbm_buffer_ext;
} SwapChain;

static uint32_t
mir_format_to_gbm_format(MirPixelFormat format)
{
    uint32_t gbm_pf;

    switch (format)
    {
    case mir_pixel_format_argb_8888:
        gbm_pf = GBM_FORMAT_ARGB8888;
        break;
    case mir_pixel_format_xrgb_8888:
        gbm_pf = GBM_FORMAT_XRGB8888;
        break;
    case mir_pixel_format_abgr_8888:
        gbm_pf = GBM_FORMAT_ABGR8888;
        break;
    case mir_pixel_format_xbgr_8888:
        gbm_pf = GBM_FORMAT_XBGR8888;
        break;
    case mir_pixel_format_rgb_565:
        gbm_pf = GBM_FORMAT_RGB565;
        break;
    default:
        gbm_pf = UINT32_MAX;
        break;
    }

    return gbm_pf;
}

static int
get_format_bpp(MirPixelFormat format)
{
	int bpp;

   switch (format) {
   case mir_pixel_format_argb_8888:
   case mir_pixel_format_xrgb_8888:
   case mir_pixel_format_abgr_8888:
   case mir_pixel_format_xbgr_8888:
      bpp = 4;
      break;
   case mir_pixel_format_rgb_565:
      bpp = 2;
      break;
   default:
      bpp = 0;
      break;
   }

   return bpp;
}

static struct gbm_bo *
create_gbm_bo_from_buffer(struct gbm_device* gbm_dev, int fd, int width, int height, uint32_t stride, uint32_t format)
{
   struct gbm_import_fd_data data;

   _eglLog(_EGL_INFO, "importing fd=%d", fd);
   data.fd = fd;
   data.width = width;
   data.height = height;
   data.format = format;
   data.stride = stride;

   return gbm_bo_import(gbm_dev, GBM_BO_IMPORT_FD, &data, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
}

static void
clear_cached_buffers(struct dri2_egl_surface *dri2_surf)
{
   size_t i;
   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (dri2_surf->color_buffers[i].bo != NULL)
         gbm_bo_destroy(dri2_surf->color_buffers[i].bo);
      dri2_surf->color_buffers[i].bo = NULL;
      dri2_surf->color_buffers[i].fd = -1;
      dri2_surf->color_buffers[i].age = 0;
   }
}

static ssize_t
find_cached_buffer_with_fd(struct dri2_egl_surface *dri2_surf, int fd)
{
   ssize_t i;

   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (dri2_surf->color_buffers[i].fd == fd)
         return i;
   }

   return -1;
}

static void
cache_buffer(struct dri2_egl_surface *dri2_surf, size_t slot,
                         int fd, int width, int height, uint32_t stride)
{
   SwapChain* sc = (SwapChain *)dri2_surf->sc;
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   if (dri2_surf->color_buffers[slot].bo != NULL)
      gbm_bo_destroy(dri2_surf->color_buffers[slot].bo);

   dri2_surf->color_buffers[slot].bo = create_gbm_bo_from_buffer(
      &dri2_dpy->gbm_dri->base,
      fd, width, height, stride,
      sc->gbm_format);

   _eglLog(_EGL_INFO, " imported bo : %p format = %d (GBM_FORMAT_ARGB8888=%d)",
      (void*)dri2_surf->color_buffers[slot].bo, gbm_bo_get_format(dri2_surf->color_buffers[slot].bo), GBM_FORMAT_ARGB8888);

   dri2_surf->color_buffers[slot].fd = fd;
}

static size_t
find_best_cache_slot(struct dri2_egl_surface *dri2_surf)
{
   size_t i;
   size_t start_slot = 0;

   /*
    * If we have a back buffer, start searching after it to ensure
    * we don't reuse the slot too soon.
    */
   if (dri2_surf->back != NULL) {
      start_slot = dri2_surf->back - dri2_surf->color_buffers;
      start_slot = (start_slot + 1) % ARRAY_SIZE(dri2_surf->color_buffers);
   }

   /* Try to find an empty slot */
   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      size_t slot = (start_slot + i) % ARRAY_SIZE(dri2_surf->color_buffers);
      if (dri2_surf->color_buffers[slot].bo == NULL)
         return slot;
   }

   /* If we don't have an empty slot, use the start slot */
   return start_slot;
}

static void
update_cached_buffer_ages(struct dri2_egl_surface *dri2_surf, size_t used_slot)
{
   /*
    * If 3 (Mir surfaces are triple buffered at most) other buffers have been
    * used since a buffer was used, we probably won't need this buffer again.
    */
   static const int destruction_age = 3;
   size_t i;

   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (dri2_surf->color_buffers[i].bo != NULL) {
         if (i == used_slot) {
            dri2_surf->color_buffers[i].age = 0;
         }
         else {
            ++dri2_surf->color_buffers[i].age;
            if (dri2_surf->color_buffers[i].age == destruction_age) {
               gbm_bo_destroy(dri2_surf->color_buffers[i].bo);
               dri2_surf->color_buffers[i].bo = NULL;
               dri2_surf->color_buffers[i].fd = -1;
            }
         }
      }
   }
}

static int
dri2_rs_authenticate(_EGLDisplay *disp, uint32_t id)
{
   return 0;
}

static void
buffer_submit_callback(MirBuffer* buffer, void* context)
{
   SwapChain* sc = (SwapChain*) context;

//  _eglLog(_EGL_DEBUG, "Buffer %p returned from server", (void*) buffer);
  for (uint32_t i = 0; i < sc->buffer_count; i++)
     if (sc->buffers[i] == buffer)
     {
        pthread_mutex_lock(&sc->lock);
        assert(sc->state[i] == buffer_state_submitted);
        sc->state[i] = buffer_state_available;
        pthread_mutex_unlock(&sc->lock);
        pthread_cond_broadcast(&sc->cv);
     }
}

static EGLBoolean
mir_submit_buffer(struct dri2_egl_surface *dri2_surf)
{
   SwapChain* sc = (SwapChain *)dri2_surf->sc;

   for (uint32_t i = 0; i < sc->buffer_count; i++)
   {
      if (sc->state[i] == buffer_state_acquired)
      {
         int buffer_fd = sc->gbm_buffer_ext->fd(sc->buffers[i]);
         if (buffer_fd == dri2_surf->local_buffers[__DRI_BUFFER_BACK_LEFT]->fd)
         {
//            _eglLog(_EGL_DEBUG, "..submitting buffer %p", sc->buffers[i]);
            sc->state[i] = buffer_state_submitted;
            mir_presentation_chain_submit_buffer(sc->chain,
                                                 sc->buffers[i],
                                                 buffer_submit_callback,
                                                 sc);
            break;
         }
      }
   }

   return EGL_TRUE;
}

static EGLBoolean
mir_acquire_buffer(struct dri2_egl_display *dri2_dpy, struct dri2_egl_surface *dri2_surf)
{
   SwapChain* sc = (SwapChain *)dri2_surf->sc;
   MirBuffer* buffer = NULL;
   unsigned int buffer_width, buffer_height;
   bool found = false;
   ssize_t buf_slot = -1;

   if (!sc)
       return EGL_TRUE;

   uint32_t wrap = (sc->next_buffer_to_use-1)%(sc->buffer_count);
   do
   {
      uint32_t const next_buffer = sc->next_buffer_to_use;
      if (sc->state[next_buffer] == buffer_state_available)
      {
          int rs_width, rs_height;

          found = true;
          sc->state[next_buffer] = buffer_state_acquired;
          buffer = sc->buffers[next_buffer];
          buffer_width = mir_buffer_get_width(buffer);
          buffer_height = mir_buffer_get_height(buffer);

    //          _eglLog(_EGL_DEBUG, "..acquired buffer %p with fd = %d", sc->buffers[sc->next_buffer_to_use], mbp->fd[0]);
          mir_render_surface_get_size(sc->surface, &rs_width, &rs_height);
          if ((rs_width != buffer_width) || (rs_height != buffer_height))
          {
              // release the old buffer
              mir_buffer_release(buffer);
              // .. replace it with a new buffer
              sc->buffers[next_buffer] =
                 sc->gbm_buffer_ext->allocate_buffer_gbm_sync(
                    dri2_dpy->mir_conn,
                    rs_width, rs_height,
                    sc->gbm_format,
                    GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);

              buffer = sc->buffers[next_buffer];
              buffer_width = rs_width;
              buffer_height = rs_height;
           }
      }
      else
      {
         if (next_buffer == wrap)
         {
            pthread_mutex_lock(&sc->lock);
            pthread_cond_wait(&sc->cv, &sc->lock);
            pthread_mutex_unlock(&sc->lock);
         }
      }

      sc->next_buffer_to_use = (next_buffer+1)%(sc->buffer_count);
   }
   while(!found);

   if (buffer_width && buffer_height) {
      dri2_surf->base.Width = buffer_width;
      dri2_surf->base.Height = buffer_height;
   }

   int buffer_fd = sc->gbm_buffer_ext->fd(buffer);
   assert(buffer_fd > 0);
   unsigned int buffer_age = sc->gbm_buffer_ext->age(buffer);
   uint32_t buffer_stride = sc->gbm_buffer_ext->stride(buffer);
   buf_slot = find_cached_buffer_with_fd(dri2_surf, buffer_fd);

   if (buf_slot != -1) {
      /*
       * If we get a new buffer with an fd of a previously cached buffer,
       * replace the old buffer in the cache...
       */
      if (buffer_age == 0)
         cache_buffer(dri2_surf, buf_slot, buffer_fd, buffer_width, buffer_height, buffer_stride);
      /* ... otherwise just reuse the existing cached buffer */
   }
   else {
      /* We got a new buffer with an fd that's not in the cache, so add it */
      buf_slot = find_best_cache_slot(dri2_surf);
      cache_buffer(dri2_surf, buf_slot, buffer_fd, buffer_width, buffer_height, buffer_stride);
   }

   update_cached_buffer_ages(dri2_surf, buf_slot);

   dri2_surf->back = &dri2_surf->color_buffers[buf_slot];
   dri2_surf->back->buffer_age = buffer_age;
   dri2_surf->local_buffers[__DRI_BUFFER_BACK_LEFT]->name = 0;
   dri2_surf->local_buffers[__DRI_BUFFER_BACK_LEFT]->fd = buffer_fd;
   dri2_surf->local_buffers[__DRI_BUFFER_BACK_LEFT]->pitch = buffer_stride;

   return EGL_TRUE;
}

static _EGLSurface *
dri2_rs_create_window_surface(_EGLDriver *drv, _EGLDisplay *disp,
                               _EGLConfig *conf, EGLNativeWindowType window,
                               const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;
   const __DRIconfig *config;
   MirRenderSurface* surface = window;
   SwapChain *sc = calloc(1, sizeof(SwapChain));
   int width = 0, height = 0;
   uint32_t num_buffers = 0;

   if (!mir_render_surface_is_valid(surface))
   {
      _eglError(EGL_BAD_NATIVE_WINDOW, "dri2_rs_create_window_surface: surface is bad");
      return NULL;
   }

   sc->surface = surface;
   sc->format = dri2_conf->base.NativeVisualID;
   _eglLog(_EGL_INFO, "Mir pixel format requested : %d", sc->format);
   sc->gbm_format = mir_format_to_gbm_format(sc->format);

   if (sc->gbm_format == UINT32_MAX)
   {
      _eglError(EGL_BAD_NATIVE_WINDOW, "dri2_rs_create_window_surface: bad format");
      return NULL;
   }

   mir_render_surface_get_size(surface, &width, &height);
   _eglLog(_EGL_INFO, "render surface of size : %dx%d", width, height);

   sc->chain = mir_render_surface_get_presentation_chain(surface);
   if (!mir_presentation_chain_is_valid(sc->chain))
   {
      _eglError(EGL_BAD_NATIVE_WINDOW, "dri2_rs_create_window_surface: pc is bad");
      return NULL;
   }

   pthread_mutex_init(&sc->lock, NULL);
   pthread_cond_init(&sc->cv, NULL);

   char* str_num = getenv("MIR_EGL_CLIENT_BUFFERS");
   if (str_num)
      num_buffers = atoi(str_num);
   if ((num_buffers < 2) || (num_buffers > MAX_BUFFERS))
      num_buffers = NUM_DEFAULT_BUFFERS;

   _eglLog(_EGL_INFO, "Allocating %d buffers", num_buffers);
   sc->gbm_buffer_ext = mir_extension_gbm_buffer_v2(dri2_dpy->mir_conn);
   assert(sc->gbm_buffer_ext);
   assert(sc->gbm_buffer_ext->allocate_buffer_gbm_sync);
   assert(sc->gbm_buffer_ext->fd);
   assert(sc->gbm_buffer_ext->stride);
   assert(sc->gbm_buffer_ext->age);

   for (unsigned int i = 0; i < num_buffers; i++)
   {
       sc->buffers[i] =
          sc->gbm_buffer_ext->allocate_buffer_gbm_sync(dri2_dpy->mir_conn,
                                                       width, height,
                                                       sc->gbm_format,
                                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
       assert(sc->state[i] == buffer_state_none);
       sc->state[i] = buffer_state_available;
       sc->buffer_count++;
   }

   _eglLog(_EGL_INFO, "Presentation chain : %p", sc->chain);
   _eglLog(_EGL_INFO, "\tcontains %d buffers", sc->buffer_count);
   for (uint32_t i=0; i<sc->buffer_count; i++) {
      _eglLog(_EGL_INFO, "Buffer #%d %dx%d: %p",
                         i,
                         mir_buffer_get_width(sc->buffers[i]),
                         mir_buffer_get_height(sc->buffers[i]),
                         sc->buffers[i]);
   }

   dri2_surf = calloc(1, sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "dri2_rs_create_window_surface");
      return NULL;
   }

   if (!_eglInitSurface(&dri2_surf->base, disp, EGL_WINDOW_BIT, conf, attrib_list, window))
      goto cleanup_surf;

   dri2_surf->sc = sc;

   dri2_surf->base.Width = width;
   dri2_surf->base.Height = height;

   dri2_surf->local_buffers[__DRI_BUFFER_FRONT_LEFT] =
      calloc(sizeof(*dri2_surf->local_buffers[0]), 1);
   dri2_surf->local_buffers[__DRI_BUFFER_BACK_LEFT] =
      calloc(sizeof(*dri2_surf->local_buffers[0]), 1);

   dri2_surf->local_buffers[__DRI_BUFFER_BACK_LEFT]->attachment =
      __DRI_BUFFER_BACK_LEFT;

   dri2_surf->local_buffers[__DRI_BUFFER_BACK_LEFT]->cpp = get_format_bpp(sc->format);

   clear_cached_buffers(dri2_surf);

   if(!mir_acquire_buffer(dri2_dpy, dri2_surf))
      goto cleanup_surf;

   config = dri2_get_dri_config(dri2_conf, EGL_WINDOW_BIT,
                                dri2_surf->base.GLColorspace);

   if (dri2_dpy->gbm_dri) {
      struct gbm_dri_surface *surf = malloc(sizeof *surf);

      dri2_surf->gbm_surf = surf;
      surf->base.gbm = &dri2_dpy->gbm_dri->base;
      surf->base.width = dri2_surf->base.Width;
      surf->base.height = dri2_surf->base.Height;
      surf->base.format = sc->gbm_format;
      surf->base.flags = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
      surf->dri_private = dri2_surf;

      dri2_surf->dri_drawable =
          (*dri2_dpy->dri2->createNewDrawable) (dri2_dpy->dri_screen,
                                                config,
                                                dri2_surf->gbm_surf);
   }
   else {
      dri2_surf->dri_drawable =
          (*dri2_dpy->dri2->createNewDrawable) (dri2_dpy->dri_screen,
                                                config,
                                                dri2_surf);
   }

   if (dri2_surf->dri_drawable == NULL) {
      _eglError(EGL_BAD_ALLOC, "dri2->createNewDrawable");
   }

   return &dri2_surf->base;

cleanup_surf:
   free(dri2_surf);
   return NULL;
}

static _EGLSurface *
dri2_rs_create_pixmap_surface(_EGLDriver *drv, _EGLDisplay *disp,
                              _EGLConfig *conf, void *native_window,
                              const EGLint *attrib_list)
{
   _eglError(EGL_BAD_PARAMETER, "EGL pixmap surfaces are unsupported on Mir (RS)");
   return NULL;
}

static EGLBoolean
dri2_rs_destroy_surface(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   int i;

   (void) drv;

   if (!_eglPutSurface(surf))
      return EGL_TRUE;

   clear_cached_buffers(dri2_surf);

   (*dri2_dpy->core->destroyDrawable)(dri2_surf->dri_drawable);

   for (i = 0; i < __DRI_BUFFER_COUNT; ++i) {
      if (dri2_surf->local_buffers[i]) {
         if ((i == __DRI_BUFFER_FRONT_LEFT) ||
             (i == __DRI_BUFFER_BACK_LEFT)) {
            free(dri2_surf->local_buffers[i]);
         }
         else {
            dri2_dpy->dri2->releaseBuffer(dri2_dpy->dri_screen,
                                          dri2_surf->local_buffers[i]);
         }
      }
   }

   free(dri2_surf->gbm_surf);

   SwapChain *sc = (SwapChain *)dri2_surf->sc;
   dri2_surf->sc = NULL;

   bool wait_for_buffers;
   do
   {
      sleep(0);
      wait_for_buffers = false;

      for (uint32_t i = 0; i < sc->buffer_count; i++)
      {
         if (sc->state[i] > buffer_state_available)
            wait_for_buffers = true;
      }
   } while(wait_for_buffers);

   for (uint32_t i = 0; i < sc->buffer_count; i++) {
      if(sc->buffers[i])
      {
         mir_buffer_release(sc->buffers[i]);
         sc->buffers[i] = NULL;
      }
   }

   pthread_mutex_destroy(&sc->lock);
   pthread_cond_destroy(&sc->cv);

   free(sc);
   free(surf);

   return EGL_TRUE;
}

static _EGLImage *
dri2_rs_create_image_khr(_EGLDriver *drv, _EGLDisplay *disp,
                         _EGLContext *ctx, EGLenum target,
                         EGLClientBuffer buffer, const EGLint *attr_list)
{
   (void) drv;

   switch (target) {
   case EGL_NATIVE_PIXMAP_KHR:
      _eglError(EGL_BAD_PARAMETER, "Mir has no native pixmaps");
      return NULL;
   default:
      return dri2_create_image_khr(drv, disp, ctx, target, buffer, attr_list);
   }
}

static EGLBoolean
dri2_rs_swap_interval(_EGLDriver *drv, _EGLDisplay *disp,
                       _EGLSurface *surf, EGLint interval)
{
    struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
    SwapChain* sc = (SwapChain *)dri2_surf->sc;
    MirPresentationChain* chain = sc->chain;
    MirPresentMode mode;

    switch (interval) {
    case 0:
       mode = mir_present_mode_mailbox;
       break;
    case 1:
       mode = mir_present_mode_fifo;
       break;
    default:
       _eglError(EGL_BAD_PARAMETER, "Mir only supports swap interval 0 and 1");
       return EGL_FALSE;
    }

    mir_presentation_chain_set_mode(chain, mode);
    return EGL_TRUE;
}

static EGLBoolean
dri2_rs_swap_buffers(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *draw)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);
   int rc;

   (*dri2_dpy->flush->flush)(dri2_surf->dri_drawable);

   rc = mir_submit_buffer(dri2_surf);

   if (rc)
      rc = mir_acquire_buffer(dri2_dpy, dri2_surf);

   (*dri2_dpy->flush->invalidate)(dri2_surf->dri_drawable);

   return rc;
}

static EGLint
dri2_rs_query_buffer_age(_EGLDriver *drv, _EGLDisplay *dpy,
                          _EGLSurface *surf)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   if (dri2_surf->back)
   {
      return dri2_surf->back->buffer_age;
   }
   return 0;
}

static __DRIbuffer*
dri2_rs_get_buffers_with_format(__DRIdrawable *driDrawable,
                                 int *width,
                                 int *height,
                                 unsigned int *attachments,
                                 int count,
                                 int *out_count,
                                 void *data)
{
   struct dri2_egl_surface *dri2_surf = data;
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   int i;

   for (i = 0; i < 2*count; i+=2) {
      assert(attachments[i] < __DRI_BUFFER_COUNT);
      assert((i/2) < ARRAY_SIZE(dri2_surf->buffers));

      if (dri2_surf->local_buffers[attachments[i]] == NULL) {
         /* Our frame callback must keep these buffers valid */
         assert(attachments[i] != __DRI_BUFFER_FRONT_LEFT);
         assert(attachments[i] != __DRI_BUFFER_BACK_LEFT);

         dri2_surf->local_buffers[attachments[i]] =
            dri2_dpy->dri2->allocateBuffer(dri2_dpy->dri_screen,
                  attachments[i], attachments[i+1],
                  dri2_surf->base.Width, dri2_surf->base.Height);

         if (!dri2_surf->local_buffers[attachments[i]]) {
            _eglError(EGL_BAD_ALLOC, "Failed to allocate auxiliary buffer");
            return NULL;
         }
      }

      memcpy(&dri2_surf->buffers[(i/2)],
             dri2_surf->local_buffers[attachments[i]],
             sizeof(__DRIbuffer));
   }

   assert(dri2_surf->base.Type == EGL_PIXMAP_BIT ||
          dri2_surf->local_buffers[__DRI_BUFFER_BACK_LEFT]);

   *out_count = i/2;
   if (i == 0)
       return NULL;

   *width = dri2_surf->base.Width;
   *height = dri2_surf->base.Height;

   return dri2_surf->buffers;
}

static __DRIbuffer*
dri2_rs_get_buffers(__DRIdrawable *driDrawable,
                     int *width,
                     int *height,
                     unsigned int *attachments,
                     int count,
                     int *out_count,
                     void *data)
{
   unsigned int *attachments_with_format;
   __DRIbuffer *buffer;
   const unsigned int format = 32;
   int i;

   attachments_with_format = calloc(count * 2, sizeof(unsigned int));
   if (!attachments_with_format) {
      *out_count = 0;
      return NULL;
   }

   for (i = 0; i < count; ++i) {
      attachments_with_format[2*i] = attachments[i];
      attachments_with_format[2*i + 1] = format;
   }

   buffer =
      dri2_rs_get_buffers_with_format(driDrawable,
                   width, height,
                   attachments_with_format, count,
                   out_count, data);

   free(attachments_with_format);

   return buffer;
}

static void
dri2_rs_flush_front_buffer(__DRIdrawable *driDrawable, void *data)
{
   (void) driDrawable;

   /* FIXME: Does EGL support front buffer rendering at all? */

#if 0
   struct dri2_egl_surface *dri2_surf = data;

   dri2WaitGL(dri2_surf);
#else
   (void) data;
#endif
}

static int
dri2_rs_image_get_buffers(__DRIdrawable *driDrawable,
                           unsigned int format,
                           uint32_t *stamp,
                           void *loaderPrivate,
                           uint32_t buffer_mask,
                           struct __DRIimageList *buffers)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   if (buffer_mask & __DRI_IMAGE_BUFFER_BACK) {
      if (!dri2_surf->back)
         return 0;

      buffers->back = ((struct gbm_dri_bo *)dri2_surf->back->bo)->image;
      buffers->image_mask = __DRI_IMAGE_BUFFER_BACK;

      return 1;
   }

   return 0;
}

static struct dri2_egl_display_vtbl dri2_rs_display_vtbl = {
   .authenticate = dri2_rs_authenticate,
   .create_window_surface = dri2_rs_create_window_surface,
   .create_pixmap_surface = dri2_rs_create_pixmap_surface,
   .destroy_surface = dri2_rs_destroy_surface,
   .create_image = dri2_rs_create_image_khr,
   .swap_interval = dri2_rs_swap_interval,
   .swap_buffers = dri2_rs_swap_buffers,
   .query_buffer_age = dri2_rs_query_buffer_age,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};

static void set_auth_fd(int auth_fd, void* context)
{
    struct dri2_egl_display *dri2_dpy = context;
    int dup_fd = dup(auth_fd);
    _eglLog(_EGL_INFO, "Initial fd=%d with dup=%d", auth_fd, dup_fd);

    pthread_mutex_lock(&dri2_dpy->lock);
    dri2_dpy->fd = dup_fd;
    pthread_mutex_unlock(&dri2_dpy->lock);
    pthread_cond_broadcast(&dri2_dpy->cv);
}

static EGLBoolean
mir_add_configs_for_visuals(_EGLDriver *drv, _EGLDisplay *dpy)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   static const struct {
      int format;
      int rgba_shifts[4];
      unsigned int rgba_sizes[4];
   } visuals[] = {
   { mir_pixel_format_rgb_565,   { 11, 5, 0, -1 }, { 5, 6, 5, 0 } },
   { mir_pixel_format_argb_8888, { 16, 8, 0, 24 }, { 8, 8, 8, 8 } },
   { mir_pixel_format_abgr_8888, { 0, 8, 16, 24 }, { 8, 8, 8, 8 } },
   { mir_pixel_format_xbgr_8888, { 0, 8, 16, -1 }, { 8, 8, 8, 0 } },
   { mir_pixel_format_xrgb_8888, { 16, 8, 0, -1 }, { 8, 8, 8, 0 } },
   };
   
   EGLint config_attrs[] = {
     EGL_NATIVE_VISUAL_ID,   0,
     EGL_NATIVE_VISUAL_TYPE, 0,
     EGL_NONE
   };
   unsigned int format_count[ARRAY_SIZE(visuals)] = { 0 };
   int count, i, j;

   count = 0;
   for (i = 0; dri2_dpy->driver_configs[i]; i++) {
      const EGLint surface_type = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
      struct dri2_egl_config *dri2_conf;

      for (j = 0; j < ARRAY_SIZE(visuals); j++) {
         config_attrs[1] = visuals[j].format;
         config_attrs[3] = visuals[j].format;

         dri2_conf = dri2_add_config(dpy, dri2_dpy->driver_configs[i],
               count + 1, surface_type, config_attrs, visuals[j].rgba_shifts, visuals[j].rgba_sizes);
         if (dri2_conf) {
             _eglLog(_EGL_INFO, "Added config for %d", visuals[j].format);
            count++;
            format_count[j]++;
         }
      }
   }

   for (i = 0; i < ARRAY_SIZE(format_count); i++) {
      if (!format_count[i]) {
         _eglLog(_EGL_DEBUG, "No DRI config supports mir format %d",
                 visuals[i].format);
      }
   }

   return (count != 0);
}

EGLBoolean
dri2_initialize_rs(_EGLDriver *drv, _EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy;
   struct gbm_device *gbm = NULL;

   loader_set_logger(_eglLog);

   dri2_dpy = calloc(1, sizeof *dri2_dpy);
   if (!dri2_dpy)
      return _eglError(EGL_BAD_ALLOC, "eglInitialize");

   disp->DriverData = (void *) dri2_dpy;
   dri2_dpy->mir_conn = disp->PlatformDisplay;

   pthread_mutex_init(&dri2_dpy->lock, NULL);
   pthread_cond_init(&dri2_dpy->cv, NULL);

   MirExtensionMesaDRMAuthV1 const* ext =
      mir_extension_mesa_drm_auth_v1(dri2_dpy->mir_conn);
   if (!ext)
      goto cleanup_dpy;
   ext->drm_auth_fd(dri2_dpy->mir_conn, set_auth_fd, dri2_dpy);

   pthread_mutex_lock(&dri2_dpy->lock);
   while (!dri2_dpy->fd)
       pthread_cond_wait(&dri2_dpy->cv, &dri2_dpy->lock);
   pthread_mutex_unlock(&dri2_dpy->lock);

   pthread_mutex_destroy(&dri2_dpy->lock);
   pthread_cond_destroy(&dri2_dpy->cv);

   dri2_dpy->own_device = 1;
   gbm = gbm_create_device(dri2_dpy->fd);
   if (gbm == NULL)
      goto cleanup_dpy;

   if (gbm) {
      struct gbm_dri_device *gbm_dri = gbm_dri_device(gbm);

      dri2_dpy->gbm_dri = gbm_dri;
      dri2_dpy->driver_name = strdup(gbm_dri->driver_name);
      dri2_dpy->dri_screen = gbm_dri->screen;
      dri2_dpy->core = gbm_dri->core;
      dri2_dpy->dri2 = gbm_dri->dri2;
      dri2_dpy->image = gbm_dri->image;
      dri2_dpy->flush = gbm_dri->flush;
      dri2_dpy->driver_configs = gbm_dri->driver_configs;

      gbm_dri->lookup_image = dri2_lookup_egl_image;
      gbm_dri->lookup_user_data = disp;

      gbm_dri->get_buffers = dri2_rs_get_buffers;
      gbm_dri->flush_front_buffer = dri2_rs_flush_front_buffer;
      gbm_dri->get_buffers_with_format = dri2_rs_get_buffers_with_format;
      gbm_dri->image_get_buffers = dri2_rs_image_get_buffers;

      if (!dri2_setup_extensions(disp))
         goto cleanup_dpy;

      dri2_setup_screen(disp);
   }

   if (!mir_add_configs_for_visuals(drv, disp)) {
      _eglLog(_EGL_FATAL, "DRI2: failed to add configs");
      goto cleanup_dpy;
   }

   disp->Extensions.EXT_buffer_age = EGL_TRUE;
/*
   disp->Extensions.EXT_swap_buffers_with_damage = EGL_FALSE;
   disp->Extensions.KHR_image_pixmap = EGL_FALSE;
*/
   dri2_dpy->vtbl = &dri2_rs_display_vtbl;

   return EGL_TRUE;

cleanup_dpy:
   free(dri2_dpy);
   disp->DriverData = NULL;

   return EGL_FALSE;
}