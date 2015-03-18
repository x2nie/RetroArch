/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2015 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <retro_miscellaneous.h>
#include "runloop.h"
#include "general.h"
#include "input/input_overlay.h"

#ifdef HAVE_THREADS
#include <rthreads/rthreads.h>
#endif

typedef int (*transfer_cb_t               )(void *data, size_t len);

#ifdef HAVE_NETWORKING
#include "net_http.h"

typedef struct http_handle
{
   struct
   {
      struct http_connection_t *handle;
      transfer_cb_t  cb;
      char elem1[PATH_MAX_LENGTH];
   } connection;
   msg_queue_t *msg_queue;
   struct http_t *handle;
   transfer_cb_t  cb;
} http_handle_t;
#endif

typedef struct nbio_image_handle
{
#ifndef IS_SALAMANDER
   struct texture_image ti;
#endif
   bool is_blocking;
   bool is_blocking_on_processing;
   bool is_finished;
   bool is_finished_with_processing;
   transfer_cb_t  cb;
   struct rpng_t *handle;
   unsigned processing_pos_increment;
   unsigned pos_increment;
   uint64_t frame_count;
   uint64_t processing_frame_count;
   int processing_final_state;
   msg_queue_t *msg_queue;
} nbio_image_handle_t;

typedef struct nbio_handle
{
   nbio_image_handle_t image;
   bool is_blocking;
   bool is_finished;
   transfer_cb_t  cb;
   struct nbio_t *handle;
   unsigned pos_increment;
   uint64_t frame_count;
   msg_queue_t *msg_queue;
} nbio_handle_t;


typedef struct data_runloop
{
#ifdef HAVE_NETWORKING
   http_handle_t http;
#endif

#if 0
#ifdef HAVE_LIBRETRODB
   struct
   {
   } db;
#endif
#endif

   nbio_handle_t nbio;
   bool inited;

#ifdef HAVE_THREADS
   bool thread_inited;
   bool thread_quit;

   slock_t *lock;
   slock_t *cond_lock;
   scond_t *cond;
   sthread_t *thread;
#endif
} data_runloop_t;

struct data_runloop g_data_runloop;

#ifdef HAVE_NETWORKING
int cb_core_updater_download(void *data_, size_t len);
int cb_core_updater_list(void *data_, size_t len);

/**
 * rarch_main_data_http_iterate_transfer:
 *
 * Resumes HTTP transfer update.
 *
 * Returns: 0 when finished, -1 when we should continue
 * with the transfer on the next frame.
 **/
static int rarch_main_data_http_iterate_transfer(http_handle_t *http)
{
   size_t pos = 0, tot = 0;

   if (!net_http_update(http->handle, &pos, &tot))
   {
#ifdef _WIN32
		RARCH_LOG("%.9I64u / %.9I64u       \r", (unsigned long long)pos, (unsigned long long)tot);
#else
		RARCH_LOG("%.9llu / %.9llu        \r", (unsigned long long)pos, (unsigned long long)tot);
#endif
      return -1;
   }

   return 0;
}

static int rarch_main_data_http_con_iterate_transfer(http_handle_t *http)
{
   if (!net_http_connection_iterate(http->connection.handle))
      return -1;
   return 0;
}

static int rarch_main_data_http_conn_iterate_transfer_parse(http_handle_t *http)
{
   if (net_http_connection_done(http->connection.handle))
   {
      if (http->connection.handle && http->connection.cb)
         http->connection.cb(http, 0);
   }
   
   net_http_connection_free(http->connection.handle);

   http->connection.handle = NULL;

   return 0;
}

static int rarch_main_data_http_iterate_transfer_parse(http_handle_t *http)
{
   size_t len;
   char *data = (char*)net_http_data(http->handle, &len, false);

   if (data && http->cb)
      http->cb(data, len);

   net_http_delete(http->handle);

   http->handle = NULL;
   msg_queue_clear(http->msg_queue);

   return 0;
}

static int cb_http_conn_default(void *data_, size_t len)
{
   http_handle_t *http = (http_handle_t*)data_;

   if (!http)
      return -1;

   http->handle = net_http_new(http->connection.handle);

   if (!http->handle)
   {
      RARCH_ERR("Could not create new HTTP session handle.\n");
      return -1;
   }

   http->cb     = NULL;

   if (http->connection.elem1[0] != '\0')
   {
      if (!strcmp(http->connection.elem1, "cb_core_updater_download"))
         http->cb = &cb_core_updater_download;
      if (!strcmp(http->connection.elem1, "cb_core_updater_list"))
         http->cb = &cb_core_updater_list;
   }

   return 0;
}

/**
 * rarch_main_data_http_iterate_poll:
 *
 * Polls HTTP message queue to see if any new URLs 
 * are pending.
 *
 * If handle is freed, will set up a new http handle. 
 * The transfer will be started on the next frame.
 *
 * Returns: 0 when an URL has been pulled and we will
 * begin transferring on the next frame. Returns -1 if
 * no HTTP URL has been pulled. Do nothing in that case.
 **/
static int rarch_main_data_http_iterate_poll(http_handle_t *http)
{
   char elem0[PATH_MAX_LENGTH];
   struct string_list *str_list = NULL;
   const char *url = msg_queue_pull(http->msg_queue);

   if (!url)
      return -1;

   /* Can only deal with one HTTP transfer at a time for now */
   if (http->handle)
      return -1; 

   str_list         = string_split(url, "|"); 

   if (!str_list)
      return -1;

   if (str_list->size > 0)
      strlcpy(elem0, str_list->elems[0].data, sizeof(elem0));

   http->connection.handle = net_http_connection_new(elem0);

   if (!http->connection.handle)
      return -1;

   http->connection.cb     = &cb_http_conn_default;

   if (str_list->size > 1)
      strlcpy(http->connection.elem1,
            str_list->elems[1].data,
            sizeof(http->connection.elem1));

   string_list_free(str_list);
   
   return 0;
}
#endif

#ifdef HAVE_MENU
static int cb_image_menu_wallpaper_upload(void *data, size_t len)
{
   nbio_handle_t *nbio = (nbio_handle_t*)data; 
   driver_t *driver = driver_get_ptr();

   if (!nbio || !data)
      return -1;

   if (nbio->image.processing_final_state == IMAGE_PROCESS_ERROR ||
         nbio->image.processing_final_state == IMAGE_PROCESS_ERROR_END)
      return -1;

   if (driver->menu_ctx && driver->menu_ctx->load_background)
      driver->menu_ctx->load_background(&nbio->image.ti);

   texture_image_free(&nbio->image.ti);

   nbio->image.is_blocking_on_processing         = false;
   nbio->image.is_finished_with_processing       = true;
   nbio->image.is_blocking                       = true;
   nbio->image.is_finished                       = true;
   nbio->is_blocking                             = true;
   nbio->is_finished                             = true;

   return 0;
}

static int cb_image_menu_wallpaper(void *data, size_t len)
{
   int retval;
   nbio_handle_t *nbio = (nbio_handle_t*)data; 

   if (!nbio || !data)
      return -1;

   if (  !nbio->image.handle->has_ihdr || 
         !nbio->image.handle->has_idat || 
         !nbio->image.handle->has_iend)
      return -1;

   retval = rpng_nbio_load_image_argb_process(nbio->image.handle,
         &nbio->image.ti.pixels, &nbio->image.ti.width, &nbio->image.ti.height);

   if (retval == IMAGE_PROCESS_ERROR || retval == IMAGE_PROCESS_ERROR_END)
      return -1;

   nbio->image.cb = &cb_image_menu_wallpaper_upload;

   nbio->image.is_blocking_on_processing         = true;
   nbio->image.is_finished_with_processing       = false;
   nbio->image.is_finished                       = false;

   return 0;
}

static int cb_nbio_image_menu_wallpaper(void *data, size_t len)
{
   void *ptr = NULL;
   nbio_handle_t *nbio = (nbio_handle_t*)data; 

   if (!nbio || !data)
      return -1;
   
   nbio->image.handle = (struct rpng_t*)calloc(1, sizeof(struct rpng_t));
   nbio->image.cb = &cb_image_menu_wallpaper;

   if (!nbio->image.handle)
   {
      nbio->image.cb = NULL;
      return -1;
   }

   ptr = nbio_get_ptr(nbio->handle, &len);

   if (!ptr)
   {
      free(nbio->image.handle);
      nbio->image.handle = NULL;
      nbio->image.cb     = NULL;

      return -1;
   }

   nbio->image.handle->buff_data = (uint8_t*)ptr;
   nbio->image.pos_increment            = (len / 2) ? (len / 2) : 1;
   nbio->image.processing_pos_increment = (len / 4) ? (len / 4) : 1;

   if (!rpng_nbio_load_image_argb_start(nbio->image.handle))
   {
      rpng_nbio_load_image_free(nbio->image.handle);
      return -1;
   }

   nbio->image.is_blocking   = false;
   nbio->image.is_finished   = false;
   nbio->is_blocking    = false;
   nbio->is_finished    = true;

   return 0;
}
#endif

static int rarch_main_data_image_iterate_poll(nbio_handle_t *nbio)
{
   const char *path    = NULL;

   if (!nbio)
      return -1;
   
   path = msg_queue_pull(nbio->image.msg_queue);

   if (!path)
      return -1;

   /* Can only deal with one image transfer at a time for now */
   if (nbio->image.handle)
      return -1; 

   /* We need to load the image file first. */
   msg_queue_clear(nbio->msg_queue);
   msg_queue_push(nbio->msg_queue, path, 0, 1);

   return 0;
}

static int rarch_main_data_image_iterate_transfer(nbio_handle_t *nbio)
{
   unsigned i;

   if (!nbio)
      return -1;

   if (nbio->image.is_finished)
      return 0;

   for (i = 0; i < nbio->image.pos_increment; i++)
   {
      if (!rpng_nbio_load_image_argb_iterate(
               nbio->image.handle->buff_data,
               nbio->image.handle))
         goto error;

      nbio->image.handle->buff_data += 
         4 + 4 + nbio->image.handle->chunk.size + 4;
   }

   nbio->image.frame_count++;
   return 0;

error:
   return -1;
}

static int rarch_main_data_image_iterate_process_transfer(nbio_handle_t *nbio)
{
   unsigned i;
   int retval = 0;

   if (!nbio)
      return -1;

   for (i = 0; i < nbio->image.processing_pos_increment; i++)
   {
      retval = rpng_nbio_load_image_argb_process(nbio->image.handle,
            &nbio->image.ti.pixels, &nbio->image.ti.width, &nbio->image.ti.height);

      if (retval != IMAGE_PROCESS_NEXT)
         break;
   }

   nbio->image.processing_frame_count++;

   if (retval == IMAGE_PROCESS_NEXT)
      return 0;

   nbio->image.processing_final_state = retval;
   return -1;
}

static int rarch_main_data_image_iterate_parse_free(nbio_handle_t *nbio)
{
   if (!nbio)
      return -1;

   rpng_nbio_load_image_free(nbio->image.handle);

   nbio->image.handle                 = NULL;
   nbio->image.frame_count            = 0;
   nbio->image.processing_frame_count = 0;

   msg_queue_clear(nbio->image.msg_queue);

   return 0;
}

static int rarch_main_data_image_iterate_process_transfer_parse(nbio_handle_t *nbio)
{
   size_t len = 0;
   if (nbio->image.handle && nbio->image.cb)
      nbio->image.cb(nbio, len);

   RARCH_LOG("Image transfer processing took %d frames.\n", (unsigned)nbio->image.processing_frame_count);

   return 0;
}

static int rarch_main_data_image_iterate_transfer_parse(nbio_handle_t *nbio)
{
   size_t len = 0;
   if (nbio->image.handle && nbio->image.cb)
      nbio->image.cb(nbio, len);

   RARCH_LOG("Image transfer took %d frames.\n", (unsigned)nbio->image.frame_count);

   return 0;
}

static int cb_nbio_default(void *data, size_t len)
{
   nbio_handle_t *nbio = (nbio_handle_t*)data;

   if (!data)
      return -1;

   (void)len;

   nbio->is_blocking = false;
   nbio->is_finished = true;

   return 0;
}

static int rarch_main_data_nbio_iterate_poll(nbio_handle_t *nbio)
{
   struct nbio_t* handle;
   char elem0[PATH_MAX_LENGTH], elem1[PATH_MAX_LENGTH];
   struct string_list *str_list = NULL;
   const char *path = NULL;

   if (!nbio)
      return -1;
   
   path = msg_queue_pull(nbio->msg_queue);

   if (!path)
      return -1;

   /* Can only deal with one NBIO transfer at a time for now */
   if (nbio->handle)
      return -1; 

   str_list         = string_split(path, "|"); 

   if (!str_list)
      goto error;

   if (str_list->size > 0)
      strlcpy(elem0, str_list->elems[0].data, sizeof(elem0));
   if (str_list->size > 1)
      strlcpy(elem1, str_list->elems[1].data, sizeof(elem1));

   handle = nbio_open(elem0, NBIO_READ);

   if (!handle)
   {
      RARCH_ERR("Could not create new file loading handle.\n");
      goto error;
   }

   nbio->handle      = handle;
   nbio->is_blocking = false;
   nbio->is_finished = false;
   nbio->cb          = &cb_nbio_default;

   if (elem1[0] != '\0')
   {
#ifdef HAVE_MENU
      if (!strcmp(elem1, "cb_menu_wallpaper"))
         nbio->cb = &cb_nbio_image_menu_wallpaper;
#endif
   }

   nbio_begin_read(handle);

   string_list_free(str_list);

   return 0;

error:
   if (str_list)
      string_list_free(str_list);

   return -1;
}

static int rarch_main_data_nbio_iterate_transfer(nbio_handle_t *nbio)
{
   size_t i;

   if (!nbio)
      return -1;
   
   nbio->pos_increment = 5;

   if (nbio->is_finished)
      return 0;

   for (i = 0; i < nbio->pos_increment; i++)
   {
      if (nbio_iterate(nbio->handle))
         goto error;
   }

   nbio->frame_count++;
   return 0;

error:
   return -1;
}

static int rarch_main_data_nbio_iterate_parse_free(nbio_handle_t *nbio)
{
   if (!nbio)
      return -1;
   if (!nbio->is_finished)
      return -1;

   nbio_free(nbio->handle);
   nbio->handle      = NULL;
   nbio->is_blocking = false;
   nbio->is_finished = false;
   nbio->frame_count = 0;

   msg_queue_clear(nbio->msg_queue);

   return 0;
}

static int rarch_main_data_nbio_iterate_parse(nbio_handle_t *nbio)
{
   int len = 0;

   if (!nbio)
      return -1;

   if (nbio->cb)
      nbio->cb(nbio, len);

   RARCH_LOG("File transfer took %d frames.\n", (unsigned)nbio->frame_count);

   return 0;
}

#ifdef HAVE_MENU
static void rarch_main_data_rdl_iterate(void)
{
   driver_t *driver = driver_get_ptr();

   if (!driver->menu->rdl)
      return;

   if (driver->menu->rdl->blocking)
   {
      /* Do nonblocking I/O transfers here. */
      return;
   }

#ifdef HAVE_LIBRETRODB
   if (!driver->menu->rdl->iterating)
   {
      database_info_write_rdl_free(driver->menu->rdl);
      driver->menu->rdl = NULL;
      return;
   }

   database_info_write_rdl_iterate(driver->menu->rdl);
#endif

}
#endif

static void rarch_main_data_nbio_iterate(nbio_handle_t *nbio)
{
   if (!nbio)
      return;

   if (nbio->handle)
   {
      if (!nbio->is_blocking)
      {
         if (rarch_main_data_nbio_iterate_transfer(nbio) == -1)
            rarch_main_data_nbio_iterate_parse(nbio);
      }
      else if (nbio->is_finished)
         rarch_main_data_nbio_iterate_parse_free(nbio);
   }
   else
      rarch_main_data_nbio_iterate_poll(nbio);

   if (nbio->image.handle)
   {
      if (nbio->image.is_blocking_on_processing)
      {
         if (rarch_main_data_image_iterate_process_transfer(nbio) == -1)
            rarch_main_data_image_iterate_process_transfer_parse(nbio);
      }
      else if (!nbio->image.is_blocking)
      {
         if (rarch_main_data_image_iterate_transfer(nbio) == -1)
            rarch_main_data_image_iterate_transfer_parse(nbio);
      }
      else if (nbio->image.is_finished)
         rarch_main_data_image_iterate_parse_free(nbio);
   }
   else
      rarch_main_data_image_iterate_poll(nbio);
}

#ifdef HAVE_NETWORKING
static void rarch_main_data_http_iterate(http_handle_t *http)
{
   if (!http)
      return;

   if (http->connection.handle)
   {
      if (!rarch_main_data_http_con_iterate_transfer(http))
         rarch_main_data_http_conn_iterate_transfer_parse(http);
   }

   if (http->handle)
   {
      if (!rarch_main_data_http_iterate_transfer(http))
         rarch_main_data_http_iterate_transfer_parse(http);
   }
   else
      rarch_main_data_http_iterate_poll(http);
}
#endif

static void rarch_main_data_db_iterate(void)
{
   driver_t *driver = driver_get_ptr();
#ifdef HAVE_MENU
   if (driver->menu && driver->menu->rdl)
      rarch_main_data_rdl_iterate();
#endif
}

#ifdef HAVE_OVERLAY
static void rarch_main_data_overlay_iterate(void)
{
   driver_t  *driver  = driver_get_ptr();

   if (rarch_main_is_idle())
      return;
   if (!driver->overlay)
      return;

   switch (driver->overlay->state)
   {
      case OVERLAY_STATUS_NONE:
      case OVERLAY_STATUS_ALIVE:
         break;
      case OVERLAY_STATUS_DEFERRED_LOAD:
         input_overlay_load_overlays(driver->overlay);
         break;
      case OVERLAY_STATUS_DEFERRED_LOADING:
         input_overlay_load_overlays_iterate(driver->overlay);
         break;
      case OVERLAY_STATUS_DEFERRED_LOADING_RESOLVE:
         input_overlay_load_overlays_resolve_iterate(driver->overlay);
         break;
      case OVERLAY_STATUS_DEFERRED_DONE:
         input_overlay_new_done(driver->overlay);
         break;
      case OVERLAY_STATUS_DEFERRED_ERROR:
         input_overlay_free(driver->overlay);
         break;
      default:
         break;
   }
}
#endif

static void rarch_main_data_deinit(void)
{
   data_runloop_t *runloop = &g_data_runloop;

   if (!runloop->inited)
      return;

#ifdef HAVE_THREADS
   if (runloop->thread_inited)
   {
      slock_lock(runloop->cond_lock);
      runloop->thread_quit = true;
      slock_unlock(runloop->cond_lock);
      scond_signal(runloop->cond);
      sthread_join(runloop->thread);

      slock_free(runloop->lock);
      slock_free(runloop->cond_lock);
      scond_free(runloop->cond);
   }
#endif

   runloop->inited = false;
}

static void data_runloop_iterate(data_runloop_t *runloop)
{
#ifdef HAVE_OVERLAY
   rarch_main_data_overlay_iterate();
#endif
   rarch_main_data_nbio_iterate(&runloop->nbio);
#ifdef HAVE_NETWORKING
   rarch_main_data_http_iterate(&runloop->http);
#endif
   rarch_main_data_db_iterate();
}

#ifdef HAVE_THREADS
static void data_thread_loop(void *data)
{
   data_runloop_t *runloop = (data_runloop_t*)data;

   while (!runloop->thread_quit)
   {
      slock_lock(runloop->lock);
      data_runloop_iterate(runloop);
      slock_unlock(runloop->lock);
   }
}
#endif

void rarch_main_data_iterate(void)
{
   if (g_data_runloop.thread_inited)
      return;

   data_runloop_iterate(&g_data_runloop);
}

static void rarch_main_data_init(void)
{
   if (g_data_runloop.inited)
      return;

   memset(&g_data_runloop, 0, sizeof(g_data_runloop));

#ifdef HAVE_THREADS
   if (g_settings.menu.threaded_data_runloop_enable)
   {
      if ((g_data_runloop.thread = sthread_create(data_thread_loop, &g_data_runloop)))
      {
         g_data_runloop.lock      = slock_new();
         g_data_runloop.cond_lock = slock_new();
         g_data_runloop.cond      = scond_new();
      }
      else
         g_data_runloop.thread = NULL;
   }
#endif

#ifdef HAVE_THREADS
   g_data_runloop.thread_inited = (g_data_runloop.thread != NULL);
#else
   g_data_runloop.thread_inited = false;
   g_data_runloop.thread_quit   = false;
#endif
   g_data_runloop.inited = true;

}

void rarch_main_data_clear_state(void)
{
   rarch_main_data_deinit();
   rarch_main_data_init();
}

void rarch_main_data_init_queues(void)
{
#ifdef HAVE_NETWORKING
   if (!g_data_runloop.http.msg_queue)
      rarch_assert(g_data_runloop.http.msg_queue = msg_queue_new(8));
#endif
   if (!g_data_runloop.nbio.msg_queue)
      rarch_assert(g_data_runloop.nbio.msg_queue = msg_queue_new(8));
   if (!g_data_runloop.nbio.image.msg_queue)
      rarch_assert(g_data_runloop.nbio.image.msg_queue = msg_queue_new(8));
}

void rarch_main_data_msg_queue_push(unsigned type,
      const char *msg, const char *msg2,
      unsigned prio, unsigned duration, bool flush)
{
   char new_msg[PATH_MAX_LENGTH];
   msg_queue_t *queue = NULL;

   snprintf(new_msg, sizeof(new_msg), "%s|%s", msg, msg2);

   switch(type)
   {
      case DATA_TYPE_NONE:
         break;
      case DATA_TYPE_FILE:
         queue = g_data_runloop.nbio.msg_queue;
         break;
      case DATA_TYPE_IMAGE:
         queue = g_data_runloop.nbio.image.msg_queue;
         break;
#ifdef HAVE_NETWORKING
      case DATA_TYPE_HTTP:
         queue = g_data_runloop.http.msg_queue;
         break;
#endif
#ifdef HAVE_OVERLAY
      case DATA_TYPE_OVERLAY:
         break;
#endif
   }

   if (!queue)
      return;

   if (flush)
      msg_queue_clear(queue);
   msg_queue_push(queue, new_msg, prio, duration);
}

