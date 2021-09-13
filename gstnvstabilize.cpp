/*
 * Copyright (c) 2021, AUTORO CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Relation between width, height, PAR(Pixel Aspect Ratio), DAR(Display Aspect Ration):
 *
 *                  dar_n   par_d
 * width = height * ----- * -----
 *                  dar_d   par_n
 *
 *                  dar_d   par_n
 * height = width * ----- * -----
 *                  dar_n   par_d
 *
 * par_n    height   dar_n
 * ----- =  ------ * -----
 * par_d    width    dar_d
 *
 * dar_n   width    par_n
 * ----- = ------ * -----
 * dar_d   height   par_d
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "timing.h"

#include "gstnvstabilize.h"
//#include "nvtx_helper.h"

#define NVBUF_MAGIC_NUM 0x70807580

GST_DEBUG_CATEGORY_STATIC (gst_nvstabilize_debug);
#define GST_CAT_DEFAULT gst_nvstabilize_debug

/* Filter signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

/* Filter properties */
enum
{
  PROP_0,
  PROP_SILENT,
  PROP_CROP_MARGIN,
  PROP_QUEUE_SIZE
};

#undef MAX_NUM_PLANES
#include "nvbufsurface.h"

#define GST_TYPE_INTERPOLATION_METHOD (gst_video_interpolation_method_get_type())

static const GEnumValue video_interpolation_methods[] = {
  {GST_INTERPOLATION_NEAREST, "Nearest", "Nearest"},
  {GST_INTERPOLATION_BILINEAR, "Bilinear", "Bilinear"},
  {GST_INTERPOLATION_5_TAP, "5-Tap", "5-Tap"},
  {GST_INTERPOLATION_10_TAP, "10-Tap", "10-Tap"},
  {GST_INTERPOLATION_SMART, "Smart", "Smart"},
  {GST_INTERPOLATION_NICEST, "Nicest", "Nicest"},
  {0, NULL, NULL},
};

static GType
gst_video_interpolation_method_get_type (void)
{
  static GType video_interpolation_method_type = 0;

  if (!video_interpolation_method_type) {
      video_interpolation_method_type = g_enum_register_static ("GstInterpolationMethod",
        video_interpolation_methods);
  }
  return video_interpolation_method_type;
}

/* capabilities of the inputs and outputs */

/* Input capabilities. */
static GstStaticPadTemplate gst_nvstabilize_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NVMM,
            "{ " "NV12, RGBA, YUY2 }") ";" GST_VIDEO_CAPS_MAKE ("{ "
            "I420, NV12, RGBA, YUY2 }")));

/* Output capabilities. */
static GstStaticPadTemplate gst_nvstabilize_src_template =
    GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NVMM,
            "{ " "NV12, RGBA, YUY2 }") ";" GST_VIDEO_CAPS_MAKE ("{ "
            "I420, NV12, RGBA, YUY2 }")));

static GstElementClass *gparent_class = NULL;

#define gst_nvstabilize_parent_class parent_class
G_DEFINE_TYPE (Gstnvstabilize, gst_nvstabilize, GST_TYPE_BASE_TRANSFORM);

/* internal methods */
static void gst_nvstabilize_init_params (Gstnvstabilize * filter);
static gboolean gst_nvstabilize_get_pix_fmt (GstVideoInfo * info,
    NvBufferColorFormat * pix_fmt, gint * isurf_count);
static GstCaps *gst_nvstabilize_caps_remove_format_info (GstCaps * caps);
static gboolean gst_nvstabilize_do_nv2rawconv (Gstnvstabilize * filter,
    gint dmabuf_fd, guint8 * outdata);
static gboolean gst_nvstabilize_do_raw2nvconv (Gstnvstabilize * filter,
    guint8 * indata, gint dmabuf_fd);
static gboolean gst_nvstabilize_do_clearchroma (Gstnvstabilize * filter,
    gint dmabuf_fd);
static void gst_nvstabilize_free_buf (Gstnvstabilize * filter);

/* base transform vmethods */
static gboolean gst_nvstabilize_start (GstBaseTransform * btrans);
static gboolean gst_nvstabilize_stop (GstBaseTransform * btrans);
static void gst_nvstabilize_finalize (GObject * object);
static GstStateChangeReturn gst_nvstabilize_change_state (GstElement * element,
    GstStateChange transition);
static GstFlowReturn gst_nvstabilize_transform (GstBaseTransform * btrans,
    GstBuffer * inbuf, GstBuffer * outbuf);
static gboolean gst_nvstabilize_set_caps (GstBaseTransform * btrans,
    GstCaps * incaps, GstCaps * outcaps);
static GstCaps *gst_nvstabilize_transform_caps (GstBaseTransform * btrans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static gboolean gst_nvstabilize_accept_caps (GstBaseTransform * btrans,
    GstPadDirection direction, GstCaps * caps);
static gboolean gst_nvstabilize_transform_size (GstBaseTransform * btrans,
    GstPadDirection direction, GstCaps * caps, gsize size,
    GstCaps * othercaps, gsize * othersize);
static gboolean gst_nvstabilize_get_unit_size (GstBaseTransform * btrans,
    GstCaps * caps, gsize * size);
static GstCaps *gst_nvstabilize_fixate_caps (GstBaseTransform * btrans,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps);
static gboolean gst_nvstabilize_decide_allocation (GstBaseTransform * btrans,
    GstQuery * query);

static void gst_nvstabilize_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_nvstabilize_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

#define GST_NV_FILTER_MEMORY_TYPE "nvfilter"

/* NvFilter memory allocator Implementation */

typedef struct _GstNvFilterMemory GstNvFilterMemory;
typedef struct _GstNvFilterMemoryAllocator GstNvFilterMemoryAllocator;
typedef struct _GstNvFilterMemoryAllocatorClass GstNvFilterMemoryAllocatorClass;

struct _GstNvFilterMemory
{
  GstMemory mem;
  GstNvStabilizeBuffer *buf;
};

struct _GstNvFilterMemoryAllocator
{
  GstAllocator parent;
};

struct _GstNvFilterMemoryAllocatorClass
{
  GstAllocatorClass parent_class;
};

/**
  * implementation that acquire memory.
  *
  * @param allocator : gst memory allocatot object
  * @param size      : memory size
  * @param params    : allovcation params
  */
static GstMemory *
gst_nv_filter_memory_allocator_alloc_dummy (GstAllocator * allocator,
    gsize size, GstAllocationParams * params)
{
  GST_WARNING("");
  g_assert_not_reached ();
  return NULL;
}



/**
  * implementation that releases memory.
  *
  * @param allocator : gst memory allocatot object
  * @param mem       : gst memory
  */
static void
gst_nv_filter_memory_allocator_free (GstAllocator * allocator, GstMemory * mem)
{
  GST_WARNING("");
  gint ret = 0;
  GstNvFilterMemory *omem = (GstNvFilterMemory *) mem;
  GstNvStabilizeBuffer *nvbuf = omem->buf;

  ret = NvBufferDestroy (nvbuf->dmabuf_fd);
  if (ret != 0) {
    GST_ERROR ("%s: NvBufferDestroy Failed \n", __func__);
    goto error;
  }

error:
  g_slice_free (GstNvStabilizeBuffer, nvbuf);
  g_slice_free (GstNvFilterMemory, omem);
}

/**
  * memory map function.
  *
  * @param mem     : gst memory
  * @param maxsize : memory max size
  * @param flags   : Flags for wrapped memory
  */
static gpointer
gst_nv_filter_memory_map (GstMemory * mem, gsize maxsize, GstMapFlags flags)
{
  GST_WARNING("");
  gint ret = 0;
  GstNvFilterMemory *omem = (GstNvFilterMemory *) mem;
  NvBufferParams params = {0};

  ret = NvBufferGetParams (omem->buf->dmabuf_fd, &params);
  if (ret != 0) {
    GST_ERROR ("%s: NvBufferGetParams Failed \n", __func__);
    goto error;
  }

  return (gpointer)(params.nv_buffer);

error:
  return NULL;
}

/**
  * memory unmap function.
  *
  * @param mem : gst memory
  */
static void
gst_nv_filter_memory_unmap (GstMemory * mem)
{
  GST_WARNING("");
}

/**
  * memory share function.
  *
  * @param mem : gst memory
  */
static GstMemory *
gst_nv_filter_memory_share (GstMemory * mem, gssize offset, gssize size)
{
  GST_WARNING("");
  g_assert_not_reached ();
  return NULL;
}

GType gst_nv_filter_memory_allocator_get_type (void);
G_DEFINE_TYPE (GstNvFilterMemoryAllocator, gst_nv_filter_memory_allocator,
    GST_TYPE_ALLOCATOR);

#define GST_TYPE_NV_FILTER_MEMORY_ALLOCATOR   (gst_nv_filter_memory_allocator_get_type())
#define GST_IS_NV_FILTER_MEMORY_ALLOCATOR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_NV_FILTER_MEMORY_ALLOCATOR))

/**
  * initialize the nvfilter allocator's class.
  *
  * @param klass : nvfilter memory allocator objectclass
  */
static void
gst_nv_filter_memory_allocator_class_init (GstNvFilterMemoryAllocatorClass *
    klass)
{
  GST_WARNING("");
  GstAllocatorClass *allocator_class;

  allocator_class = GST_ALLOCATOR_CLASS (klass);

  allocator_class->alloc = gst_nv_filter_memory_allocator_alloc_dummy;
  allocator_class->free = gst_nv_filter_memory_allocator_free;
}

/**
  * nvfilter allocator init function.
  *
  * @param allocator : nvfilter allocator object instance
  */
static void
gst_nv_filter_memory_allocator_init (GstNvFilterMemoryAllocator * allocator)
{
  GST_WARNING("");
  GstAllocator *alloc = GST_ALLOCATOR_CAST (allocator);

  alloc->mem_type = GST_NV_FILTER_MEMORY_TYPE;
  alloc->mem_map = gst_nv_filter_memory_map;
  alloc->mem_unmap = gst_nv_filter_memory_unmap;
  alloc->mem_share = gst_nv_filter_memory_share;

  GST_OBJECT_FLAG_SET (allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

/**
  * custom memory allocation.
  *
  * @param allocator : nvfilter bufferpool allocator
  * @param flags     : Flags for wrapped memory
  * @param space     : nvvidconv object instance
  */
static GstMemory *
gst_nv_filter_memory_allocator_alloc (GstAllocator * allocator,
    GstMemoryFlags flags, Gstnvstabilize * space)
{
  GST_WARNING("");
  gint ret = 0;
  GstNvFilterMemory *mem = NULL;
  GstNvStabilizeBuffer *nvbuf = NULL;
  NvBufferParams params = {0};
  NvBufferCreateParams input_params = {0};

  mem = g_slice_new0 (GstNvFilterMemory);
  nvbuf = g_slice_new0 (GstNvStabilizeBuffer);

  input_params.width = space->to_width;
  input_params.height = space->to_height;
  if (space->enable_blocklinear_output &&
     (space->out_pix_fmt == NvBufferColorFormat_NV12 ||
      space->out_pix_fmt == NvBufferColorFormat_NV12_10LE))
    input_params.layout = NvBufferLayout_BlockLinear;
  else
    input_params.layout = NvBufferLayout_Pitch;
  input_params.colorFormat = space->out_pix_fmt;
  input_params.payloadType = NvBufferPayload_SurfArray;
  input_params.nvbuf_tag = NvBufferTag_VIDEO_CONVERT;

  ret = NvBufferCreateEx (&nvbuf->dmabuf_fd, &input_params);
  if (ret != 0) {
    GST_ERROR ("%s: NvBufferCreateEx Failed \n", __func__);
    goto error;
  }

  ret = NvBufferGetParams (nvbuf->dmabuf_fd, &params);
  if (ret != 0) {
    GST_ERROR ("%s: NvBufferGetParams Failed \n", __func__);
    goto error;
  }

  flags |= GST_MEMORY_FLAG_NO_SHARE;

  /* Check for init params */
  gst_memory_init (GST_MEMORY_CAST (mem), flags, allocator, NULL,
      params.nv_buffer_size, 1 /* Alignment */,
      0, params.nv_buffer_size);
  mem->buf = nvbuf;

  return GST_MEMORY_CAST (mem);

error:
  g_slice_free (GstNvStabilizeBuffer, nvbuf);
  g_slice_free (GstNvFilterMemory, mem);

  return NULL;
}

/* nvfilter Buffer Pool for nvmm buffers */

GQuark gst_nv_filter_data_quark = 0;
typedef struct _GstNvFilterBufferPool GstNvFilterBufferPool;
typedef struct _GstNvFilterBufferPoolClass GstNvFilterBufferPoolClass;
#define GST_NV_FILTER_BUFFER_POOL(pool)  ((GstNvFilterBufferPool *) pool)

struct _GstNvFilterBufferPool
{
  GstBufferPool parent;

  GstElement *element;

  GstCaps *caps;
  gboolean add_videometa;
  GstVideoInfo video_info;

  GstAllocator *allocator;

  guint current_buffer_index;
};

struct _GstNvFilterBufferPoolClass
{
  GstBufferPoolClass parent_class;
};

GType gst_nv_filter_buffer_pool_get_type (void);

G_DEFINE_TYPE (GstNvFilterBufferPool, gst_nv_filter_buffer_pool,
    GST_TYPE_BUFFER_POOL);

#define GST_TYPE_NV_FILTER_BUFFER_POOL (gst_nv_filter_buffer_pool_get_type())

/**
  * object class finallize.
  *
  * @param object : object
  */
static void
gst_nv_filter_buffer_pool_finalize (GObject * object)
{
  GST_WARNING("");
  GstNvFilterBufferPool *pool = GST_NV_FILTER_BUFFER_POOL (object);

  if (pool->element)
    gst_object_unref (pool->element);
  pool->element = NULL;

  if (pool->allocator)
    gst_object_unref (pool->allocator);
  pool->allocator = NULL;

  if (pool->caps)
    gst_caps_unref (pool->caps);
  pool->caps = NULL;

  G_OBJECT_CLASS (gst_nv_filter_buffer_pool_parent_class)->finalize (object);
}

/**
  * start the bufferpool.
  *
  * @param bpool : nvfilter bufferpool object
  */
static gboolean
gst_nv_filter_buffer_pool_start (GstBufferPool * bpool)
{
  GST_WARNING("");
  GstNvFilterBufferPool *pool = GST_NV_FILTER_BUFFER_POOL (bpool);

  GST_DEBUG_OBJECT (pool, "start");

  GST_OBJECT_LOCK (pool);
  /* Start the pool only if we have component attached to it. */
  if (!pool->element) {
    GST_OBJECT_UNLOCK (pool);
    return FALSE;
  }
  GST_OBJECT_UNLOCK (pool);

  return
      GST_BUFFER_POOL_CLASS (gst_nv_filter_buffer_pool_parent_class)->start
      (bpool);
}

/**
  * stop the bufferpool.
  *
  * @param bpool : nvfilter bufferpool object
  */
static gboolean
gst_nv_filter_buffer_pool_stop (GstBufferPool * bpool)
{
  GST_WARNING("");
  GstNvFilterBufferPool *pool = GST_NV_FILTER_BUFFER_POOL (bpool);

  GST_DEBUG_OBJECT (pool, "stop");

  if (pool->caps)
    gst_caps_unref (pool->caps);

  pool->caps = NULL;
  pool->add_videometa = FALSE;

  return
      GST_BUFFER_POOL_CLASS (gst_nv_filter_buffer_pool_parent_class)->stop
      (bpool);
}


/**
  * get a list of options supported by this pool.
  *
  * @param bpool : nvfilter bufferpool object
  */
static const gchar **
gst_nv_filter_buffer_pool_get_options (GstBufferPool * bpool)
{
  GST_WARNING("");
  static const gchar *video_options[] =
      { GST_BUFFER_POOL_OPTION_VIDEO_META, NULL };

   /* Currently, we are only providing VIDEO_META option by default. */

  return video_options;
}

/**
  * apply the bufferpool configuration.
  *
  * @param bpool  : nvfilter bufferpool object
  * @param config : config parameters
  */
static gboolean
gst_nv_filter_buffer_pool_set_config (GstBufferPool * bpool,
    GstStructure * config)
{
  GST_WARNING("");
  GstNvFilterBufferPool *pool = GST_NV_FILTER_BUFFER_POOL (bpool);
  GstCaps *caps;

  GST_DEBUG_OBJECT (pool, "set_config");

  GST_OBJECT_LOCK (pool);

  if (!gst_buffer_pool_config_get_params (config, &caps, NULL, NULL, NULL))
    goto wrong_config;

  if (caps == NULL)
    goto no_caps;

  GstVideoInfo info;

  /* now parse the caps from the config */
  if (!gst_video_info_from_caps (&info, caps))
    goto wrong_video_caps;

  /* enable metadata based on config of the pool */
  pool->add_videometa =
      gst_buffer_pool_config_has_option (config,
      GST_BUFFER_POOL_OPTION_VIDEO_META);

  pool->video_info = info;

  if (pool->caps)
    gst_caps_unref (pool->caps);
  pool->caps = gst_caps_ref (caps);

  GST_OBJECT_UNLOCK (pool);

  return
      GST_BUFFER_POOL_CLASS (gst_nv_filter_buffer_pool_parent_class)->set_config
      (bpool, config);

  /* ERRORS */
wrong_config:
  {
    GST_OBJECT_UNLOCK (pool);
    GST_WARNING_OBJECT (pool, "invalid config");
    return FALSE;
  }
no_caps:
  {
    GST_OBJECT_UNLOCK (pool);
    GST_WARNING_OBJECT (pool, "no caps in config");
    return FALSE;
  }
wrong_video_caps:
  {
    GST_OBJECT_UNLOCK (pool);
    GST_WARNING_OBJECT (pool,
        "failed getting geometry from caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
}

/**
  * allocate a buffer.
  *
  * @param bpool  : nvfilter bufferpool object
  * @param buffer : GstBuffer of pool
  * @param params : pool acquire parameters
  */
static GstFlowReturn
gst_nv_filter_buffer_pool_alloc_buffer (GstBufferPool * bpool,
    GstBuffer ** buffer, GstBufferPoolAcquireParams * params)
{
  GST_WARNING("");
  GstNvFilterBufferPool *pool = GST_NV_FILTER_BUFFER_POOL (bpool);
  Gstnvstabilize *space = GST_NVSTABILIZE (pool->element);
  GstBuffer *buf = NULL;
  GstMemory *mem = NULL;

  GST_DEBUG_OBJECT (pool, "alloc_buffer");

  mem = gst_nv_filter_memory_allocator_alloc (pool->allocator, 0, space);
  g_return_val_if_fail (mem, GST_FLOW_ERROR);

  buf = gst_buffer_new ();
  gst_buffer_append_memory (buf, mem);

  if (pool->add_videometa) {
    /* TODO : Add video meta to buffer */
  }

  gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (mem),
      gst_nv_filter_data_quark, buf, NULL);

  *buffer = buf;

  pool->current_buffer_index++;

  return GST_FLOW_OK;
}


/**
  * free a buffer.
  *
  * @param bpool  : nvfilter bufferpool object
  * @param buffer : GstBuffer of pool
  */
static void
gst_nv_filter_buffer_pool_free_buffer (GstBufferPool * bpool,
    GstBuffer * buffer)
{
  GST_WARNING("");
  GstMemory *memory;
  GstNvFilterBufferPool *pool = GST_NV_FILTER_BUFFER_POOL (bpool);

  GST_DEBUG_OBJECT (pool, "free_buffer");

  memory = gst_buffer_peek_memory (buffer, 0);

  gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (memory),
      gst_nv_filter_data_quark, NULL, NULL);

  GST_BUFFER_POOL_CLASS (gst_nv_filter_buffer_pool_parent_class)->free_buffer
      (bpool, buffer);
}

/**
  * get a new buffer from the nvfilter bufferpool.
  *
  * @param bpool  : nvfilter bufferpool object
  * @param buffer : GstBuffer of pool
  * @param params : pool acquire parameters
  */
static GstFlowReturn
gst_nv_filter_buffer_pool_acquire_buffer (GstBufferPool * bpool,
    GstBuffer ** buffer, GstBufferPoolAcquireParams * params)
{
  GST_WARNING("");
  GstFlowReturn ret;
  GstNvFilterBufferPool *pool = GST_NV_FILTER_BUFFER_POOL (bpool);

  GST_DEBUG_OBJECT (pool, "acquire_buffer");

  ret =
      GST_BUFFER_POOL_CLASS (gst_nv_filter_buffer_pool_parent_class)->
      acquire_buffer (bpool, buffer, params);

  return ret;
}

/**
  * release a buffer back in the nvfilter bufferpool.
  *
  * @param bpool  : nvfilter bufferpool object
  * @param buffer : GstBuffer of pool
  */
static void
gst_nv_filter_buffer_pool_release_buffer (GstBufferPool * bpool,
    GstBuffer * buffer)
{
  GST_WARNING("");
  GstNvFilterBufferPool *pool = GST_NV_FILTER_BUFFER_POOL (bpool);

  GST_DEBUG_OBJECT (pool, "release_buffer");

  GST_BUFFER_POOL_CLASS (gst_nv_filter_buffer_pool_parent_class)->release_buffer
      (bpool, buffer);
}

/**
  * initialize the nvfilter bufferpool's class.
  *
  * @param klass : nvfilter bufferpool objectclass
  */
static void
gst_nv_filter_buffer_pool_class_init (GstNvFilterBufferPoolClass * klass)
{
  GST_WARNING("");
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstBufferPoolClass *gstbufferpool_class = (GstBufferPoolClass *) klass;

  gst_nv_filter_data_quark =
      g_quark_from_static_string ("GstNvFilterBufferData");

  gobject_class->finalize = gst_nv_filter_buffer_pool_finalize;
  gstbufferpool_class->start = gst_nv_filter_buffer_pool_start;
  gstbufferpool_class->stop = gst_nv_filter_buffer_pool_stop;
  gstbufferpool_class->get_options = gst_nv_filter_buffer_pool_get_options;
  gstbufferpool_class->set_config = gst_nv_filter_buffer_pool_set_config;
  gstbufferpool_class->alloc_buffer = gst_nv_filter_buffer_pool_alloc_buffer;
  gstbufferpool_class->free_buffer = gst_nv_filter_buffer_pool_free_buffer;
  gstbufferpool_class->acquire_buffer =
      gst_nv_filter_buffer_pool_acquire_buffer;
  gstbufferpool_class->release_buffer =
      gst_nv_filter_buffer_pool_release_buffer;
}

/**
  * nvfilter bufferpool init function.
  *
  * @param pool : nvfilter bufferpool object instance
  */
static void
gst_nv_filter_buffer_pool_init (GstNvFilterBufferPool * pool)
{
  GST_WARNING("");
  pool->allocator =
      g_object_new (gst_nv_filter_memory_allocator_get_type (), NULL);
  pool->current_buffer_index = 0;
}

/**
  * Create nvfilter bufferpool object instance.
  *
  * @param element : GstElement object instance
  */
static GstBufferPool *
gst_nv_filter_buffer_pool_new (GstElement * element)
{
  GST_WARNING("");
  GstNvFilterBufferPool *pool;

  pool = g_object_new (GST_TYPE_NV_FILTER_BUFFER_POOL, NULL);
  pool->element = gst_object_ref (element);

  return GST_BUFFER_POOL (pool);
}

/**
  * copies the given caps.
  *
  * @param caps : given pad caps
  */
static GstCaps *
gst_nvstabilize_caps_remove_format_info (GstCaps * caps)
{
  GST_WARNING("");
  GstStructure *str;
  GstCapsFeatures *features;
  gint i, n;
  GstCaps *ret;

  ret = gst_caps_new_empty ();

  n = gst_caps_get_size (caps);
  for (i = 0; i < n; i++) {
    str = gst_caps_get_structure (caps, i);
    features = gst_caps_get_features (caps, i);

    /* If this is already expressed by the existing caps
     * skip this structure */
    if (i > 0 && gst_caps_is_subset_structure_full (ret, str, features))
      continue;

    str = gst_structure_copy (str);
    /* Only remove format info for the cases when we can actually convert */
    {
      gst_structure_remove_fields (str, "format", "colorimetry", "chroma-site",
          NULL);

      gst_structure_set (str, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
          "height", GST_TYPE_INT_RANGE, 1, G_MAXINT, NULL);

      /* if pixel aspect ratio, make a range */
      if (gst_structure_has_field (str, "pixel-aspect-ratio")) {
        gst_structure_set (str, "pixel-aspect-ratio",
            GST_TYPE_FRACTION_RANGE, 1, G_MAXINT, G_MAXINT, 1, NULL);
      }
    }
    gst_caps_append_structure_full (ret, str,
        gst_caps_features_copy (features));
  }

  return ret;
}

/**
  * Determine pixel format.
  *
  * @param info        : Information describing frame properties
  * @param pix_fmt     : pixel format
  * @param isurf_count : intermediate surface count
  */
static gboolean
gst_nvstabilize_get_pix_fmt (GstVideoInfo * info,
    NvBufferColorFormat * pix_fmt, gint * isurf_count)
{
  GST_WARNING("");
  gboolean ret = TRUE;

  if (GST_VIDEO_INFO_IS_YUV (info)) {
    switch (GST_VIDEO_FORMAT_INFO_FORMAT (info->finfo)) {
      case GST_VIDEO_FORMAT_I420:
        *pix_fmt = NvBufferColorFormat_YUV420;
        GST_WARNING("");
        break;
      case GST_VIDEO_FORMAT_NV12:
        *pix_fmt = NvBufferColorFormat_NV12;
        *isurf_count = 2;
        GST_WARNING("");
        break;
      default:
        ret = FALSE;
        GST_WARNING("");
        break;
    }
  } else if (GST_VIDEO_INFO_IS_RGB (info)) {
    switch (GST_VIDEO_FORMAT_INFO_FORMAT (info->finfo)) {
      case GST_VIDEO_FORMAT_BGRx:
        *pix_fmt = NvBufferColorFormat_XRGB32;
        *isurf_count = 1;
        GST_WARNING("");
        break;
      case GST_VIDEO_FORMAT_RGBA:
        *pix_fmt = NvBufferColorFormat_ABGR32;
        *isurf_count = 1;
        GST_WARNING("");
        break;
      default:
        ret = FALSE;
        GST_WARNING("");
        break;
    }
  }

  return ret;
}

/**
  * Initialize nvstabilize instance structure members.
  *
  * @param filter : Gstnvstabilize object instance
  */
static void
gst_nvstabilize_init_params (Gstnvstabilize * filter)
{
  GST_WARNING("");

  filter->silent = FALSE;
  filter->to_width = 0;
  filter->to_height = 0;
  filter->from_width = 0;
  filter->from_height = 0;
  filter->tsurf_width = 0;
  filter->tsurf_height = 0;

  filter->inbuf_type = BUF_NOT_SUPPORTED;
  filter->inbuf_memtype = BUF_MEM_SW;
  filter->outbuf_memtype = BUF_MEM_SW;

  memset(&filter->transform_params, 0, sizeof(NvBufferTransformParams));
  filter->in_pix_fmt = NvBufferColorFormat_Invalid;
  filter->out_pix_fmt = NvBufferColorFormat_Invalid;

  filter->do_scaling = FALSE;
  filter->need_intersurf = FALSE;
  filter->isurf_flag = FALSE;
  filter->nvfilterpool = FALSE;

  filter->insurf_count = 0;
  filter->isurf_count = 0;
  filter->tsurf_count = 0;
  filter->ibuf_count = 0;

  filter->silent = FALSE;
  filter->no_dimension = FALSE;
  filter->interpolation_method = GST_INTERPOLATION_NEAREST;
  filter->negotiated = FALSE;
  filter->num_output_buf = NVFILTER_MAX_BUF;
  filter->enable_blocklinear_output = TRUE;

  filter->do_cropping = FALSE;
  filter->crop_right = 0;
  filter->crop_left = 0;
  filter->crop_top = 0;
  filter->crop_bottom = 0;

  filter->sinkcaps =
      gst_static_pad_template_get_caps (&gst_nvstabilize_sink_template);
  filter->srccaps =
      gst_static_pad_template_get_caps (&gst_nvstabilize_src_template);

  g_mutex_init (&filter->flow_lock);

  /** stabilizer stuff **/

  // init context 
  // ovxio::ContextGuard context;
  // vxRegisterLogCallback(context, &ovxio::stdoutLogCallback, vx_false_e);
  // vxDirective(context, VX_DIRECTIVE_ENABLE_PERFORMANCE);

  // // stabilizer image initialize
  // // space->frame_exemplar = vxCreateImage(space->context, out_info.width, out_info.height, VX_DF_IMAGE_RGBX);
  // vx_image frame_exemplar = vxCreateImage(context, 1280, 720, VX_DF_IMAGE_RGBX);

  // vx_delay orig_frame_delay = vxCreateDelay(context, (vx_reference)frame_exemplar, 7 /*filter->orig_frame_delay_size*/);
  // NVXIO_CHECK_REFERENCE(orig_frame_delay);
  // NVXIO_SAFE_CALL( nvx::initDelayOfImages(context, orig_frame_delay) );
  // NVXIO_SAFE_CALL(vxReleaseImage(&frame_exemplar));

  // filter->frame = (vx_image)vxGetReferenceFromDelay(orig_frame_delay, 0);
  // filter->lastFrame = (vx_image)vxGetReferenceFromDelay(orig_frame_delay, 1 - static_cast<vx_int32>(7/*filter->orig_frame_delay_size*/));
  

  CUDA_SAFE_CALL(cudaGetDevice(&filter->cuda_device_id));
  filter->exec_target.base.exec_target_type = NVXCU_STREAM_EXEC_TARGET;
  filter->exec_target.stream = nullptr;
  CUDA_SAFE_CALL(cudaGetDeviceProperties(&filter->exec_target.dev_prop, filter->cuda_device_id) );

  filter->is_cuda = false;
  filter->use_pitch = false;
  filter->pitch = 0;
  filter->depth = 0;

  filter->dev_mem = nullptr;
  filter->dev_mem_pitch = 0ul;

  filter->queue_size = 5;
  filter->crop_margin = 0.07f;

  // filter->image = new ovxio::image_t(filter->image1, VX_WRITE_ONLY, NVX_MEMORY_TYPE_CUDA);


  // filter->stabilizer = nvx::VideoStabilizer::createImageBasedVStab(filter->context, filter->params);
  
  filter->orig_frame_delay_size = filter->queue_size + 2; //must have such size to be synchronized with the stabilized frames

  filter->configuration.format = NVXCU_DF_IMAGE_NONE;
  filter->initilize = false;

}


/**
  * Convert Raw buffer to RmSurfaces using Rm APIs.
  *
  * @param filter    : Gstnvstabilize object instance
  * @param outdata   : inbuffer data pointer
  * @param dmabuf_fd : process buffer fd
  */
static gboolean
gst_nvstabilize_do_raw2nvconv (Gstnvstabilize * filter,
    guint8 * indata, gint dmabuf_fd)
{
  GST_WARNING("");
  guint i = 0;
  gint retn = 0;
  gboolean ret = TRUE;
  guint SrcWidth[3] = {0};
  guint SrcHeight[3] = {0};
  guint Bufsize = 0;
  guchar *pSrc = NULL;

  pSrc = (guchar *)indata;

  if (filter->need_intersurf) {
    switch (filter->in_pix_fmt) {
      case NvBufferColorFormat_XRGB32:
      case NvBufferColorFormat_ABGR32:
        SrcWidth[0] = filter->from_width;
        SrcHeight[0] = filter->from_height;
        retn = Raw2NvBuffer (pSrc, 0, SrcWidth[0], SrcHeight[0], dmabuf_fd);
        if (retn != 0) {
          g_print ("%s: Raw2NvBuffer Failed for plane %d\n", __func__,i);
          return FALSE;
        }
        break;
      case NvBufferColorFormat_UYVY:
      case NvBufferColorFormat_YUYV:
      case NvBufferColorFormat_YVYU:
        SrcWidth[0] = GST_ROUND_UP_2 (filter->from_width);
        SrcHeight[0] = filter->from_height;
        retn = Raw2NvBuffer (pSrc, 0, SrcWidth[0], SrcHeight[0], dmabuf_fd);
        if (retn != 0) {
          g_print ("%s: Raw2NvBuffer Failed for plane %d\n", __func__,i);
          return FALSE;
        }
        break;
      case NvBufferColorFormat_NV12:
        SrcWidth[0] = GST_ROUND_UP_4 (filter->from_width);
        SrcHeight[0] = GST_ROUND_UP_2 (filter->from_height);
        SrcWidth[1] = GST_ROUND_UP_2 (SrcWidth[0] / 2);
        SrcHeight[1] = SrcHeight[0] / 2;
        for (i = 0; i < filter->insurf_count; i++) {
          retn = Raw2NvBuffer (pSrc + Bufsize, i, SrcWidth[i], SrcHeight[i], dmabuf_fd);
          if (retn != 0) {
            g_print ("%s: Raw2NvBuffer Failed for plane %d\n", __func__,i);
            return FALSE;
          }
          Bufsize += SrcWidth[i] * SrcHeight[i];
        }
        break;
      case NvBufferColorFormat_YUV420:
        SrcWidth[0] = GST_ROUND_UP_4 (filter->from_width);
        SrcHeight[0] = GST_ROUND_UP_2 (filter->from_height);
        SrcWidth[1] = GST_ROUND_UP_4 (filter->from_width / 2);
        SrcHeight[1] = SrcHeight[0] / 2;
        SrcWidth[2] = SrcWidth[1];
        SrcHeight[2] = SrcHeight[1];
        for (i = 0; i < filter->insurf_count; i++) {
          retn = Raw2NvBuffer (pSrc + Bufsize, i, SrcWidth[i], SrcHeight[i], dmabuf_fd);
          if (retn != 0) {
            g_print ("%s: Raw2NvBuffer Failed for plane %d\n", __func__,i);
            return FALSE;
          }
          Bufsize += SrcWidth[i] * SrcHeight[i];
        }
        break;
      case NvBufferColorFormat_YUV422:
        SrcWidth[0] = GST_ROUND_UP_4 (filter->from_width);
        SrcHeight[0] = GST_ROUND_UP_2 (filter->from_height);
        SrcWidth[1] = GST_ROUND_UP_4 (filter->from_width / 2);
        SrcHeight[1] = SrcHeight[0];
        SrcWidth[2] = SrcWidth[1];
        SrcHeight[2] = SrcHeight[1];
        for (i = 0; i < filter->insurf_count; i++) {
          retn = Raw2NvBuffer (pSrc + Bufsize, i, SrcWidth[i], SrcHeight[i], dmabuf_fd);
          if (retn != 0) {
            g_print ("%s: Raw2NvBuffer Failed for plane %d\n", __func__,i);
            return FALSE;
          }
          Bufsize += SrcWidth[i] * SrcHeight[i];
        }
        break;
      default:
        GST_ERROR ("%s: Not supoprted in_pix_fmt\n", __func__);
        return FALSE;
        break;
    }
  } else {
    SrcWidth[0] = GST_ROUND_UP_4 (filter->from_width);
    SrcHeight[0] = GST_ROUND_UP_2 (filter->from_height);
    SrcWidth[1] = GST_ROUND_UP_4 (filter->from_width / 2);
    SrcHeight[1] = SrcHeight[0] / 2;
    SrcWidth[2] = SrcWidth[1];
    SrcHeight[2] = SrcHeight[1];
    for (i = 0; i < NVRM_MAX_SURFACES; i++) {
      retn = Raw2NvBuffer (pSrc + Bufsize, i, SrcWidth[i], SrcHeight[i], dmabuf_fd);
      if (retn != 0) {
        g_print ("%s: Raw2NvBuffer Failed for plane %d\n", __func__,i);
        return FALSE;
      }
      Bufsize += SrcWidth[i] * SrcHeight[i];
    }
  }

  return ret;
}

/**
  * Convert RmSurfaces to raw buffer using Rm APIs.
  *
  * @param filter    : Gstnvstabilize object instance
  * @param dmabuf_fd : process buffer fd
  * @param outdata   : outbuffer data pointer
  */
static gboolean
gst_nvstabilize_do_nv2rawconv (Gstnvstabilize * filter, gint dmabuf_fd,
    guint8 * outdata)
{
  GST_WARNING("");
  guint i = 0;
  gint retn = 0;
  gboolean ret = TRUE;
  guint SrcWidth[3] = { 0 };
  guint SrcHeight[3] = { 0 };
  guint Bufsize = 0;

    switch (filter->out_pix_fmt) {
      case NvBufferColorFormat_XRGB32:
      case NvBufferColorFormat_ABGR32:
        SrcWidth[0] = filter->to_width;
        SrcHeight[0] = filter->to_height;
        retn = NvBuffer2Raw (dmabuf_fd, 0, SrcWidth[0], SrcHeight[0], outdata);
        if (retn != 0) {
          g_print ("%s: NvBuffer2Raw Failed for plane %d\n", __func__,i);
          return FALSE;
        }
        break;
      case NvBufferColorFormat_UYVY:
      case NvBufferColorFormat_YUYV:
      case NvBufferColorFormat_YVYU:
        SrcWidth[0] = GST_ROUND_UP_2 (filter->to_width);
        SrcHeight[0] = filter->to_height;
        retn = NvBuffer2Raw (dmabuf_fd, 0, SrcWidth[0], SrcHeight[0], outdata);
        if (retn != 0) {
          g_print ("%s: NvBuffer2Raw Failed for plane %d\n", __func__,i);
          return FALSE;
        }
        break;
      case NvBufferColorFormat_NV12:
        SrcWidth[0] = GST_ROUND_UP_4 (filter->to_width);
        SrcHeight[0] = GST_ROUND_UP_2 (filter->to_height);
        SrcWidth[1] = GST_ROUND_UP_8 (filter->to_width) / 2;
        SrcHeight[1] = SrcHeight[0] / 2;
        for (i = 0; i < filter->tsurf_count; i++) {
          retn = NvBuffer2Raw (dmabuf_fd, i, SrcWidth[i], SrcHeight[i], outdata + Bufsize);
          if (retn != 0) {
            g_print ("%s: NvBuffer2Raw Failed for plane %d\n", __func__,i);
            return FALSE;
          }
          Bufsize += (SrcWidth[i] * SrcHeight[i]);
        }
        break;
      case NvBufferColorFormat_YUV422:
        SrcWidth[0] = GST_ROUND_UP_4 (filter->to_width);
        SrcHeight[0] = GST_ROUND_UP_2 (filter->to_height);
        SrcWidth[1] = GST_ROUND_UP_8 (filter->to_width) / 2;
        SrcHeight[1] = SrcHeight[0];
        SrcWidth[2] = SrcWidth[1];
        SrcHeight[2] = SrcHeight[1];
        for (i = 0; i < filter->tsurf_count; i++) {
          retn = NvBuffer2Raw (dmabuf_fd, i, SrcWidth[i], SrcHeight[i], outdata + Bufsize);
          if (retn != 0) {
            g_print ("%s: NvBuffer2Raw Failed for plane %d\n", __func__,i);
            return FALSE;
          }
          Bufsize += (SrcWidth[i] * SrcHeight[i]);
        }
        break;
      case NvBufferColorFormat_YUV420:
        SrcWidth[0] = GST_ROUND_UP_4 (filter->to_width);
        SrcHeight[0] = GST_ROUND_UP_2 (filter->to_height);
        SrcWidth[1] = GST_ROUND_UP_8 (filter->to_width) / 2;
        SrcHeight[1] = SrcHeight[0] / 2;
        SrcWidth[2] = SrcWidth[1];
        SrcHeight[2] = SrcHeight[1];
        for (i = 0; i < filter->tsurf_count; i++) {
          retn = NvBuffer2Raw (dmabuf_fd, i, SrcWidth[i], SrcHeight[i], outdata + Bufsize);
          if (retn != 0) {
            g_print ("%s: NvBuffer2Raw Failed for plane %d\n", __func__,i);
            return FALSE;
          }
          Bufsize += (SrcWidth[i] * SrcHeight[i]);
        }

        if (filter->in_pix_fmt == NvBufferColorFormat_GRAY8)
        {
          Bufsize = 0;
          for (i = 1; i < filter->tsurf_count; i++) {
            Bufsize += (SrcWidth[i-1] * SrcHeight[i-1]);
            memset (outdata + Bufsize, 0x80, (SrcWidth[i] * SrcHeight[i]));
          }
        }
        break;
      default:
        GST_ERROR ("%s: Not supoprted out_pix_fmt\n", __func__);
        return FALSE;
        break;
    }

  return ret;
}

/**
  * clear the chroma
  *
  * @param filter    : Gstnvstabilize object instance
  * @param dmabuf_fd : process buffer fd
  */
static gboolean
gst_nvstabilize_do_clearchroma (Gstnvstabilize * filter, gint dmabuf_fd)
{
  GST_WARNING("");
  NvBufferParams params = {0};
  void *sBaseAddr[3] = {NULL};
  gint ret = 0;
  guint i, size;

  ret = NvBufferGetParams (dmabuf_fd, &params);
  if (ret != 0) {
    g_print ("%s: NvBufferGetParams Failed \n", __func__);
    return FALSE;
  }

  for (i = 1; i < filter->tsurf_count; i++) {
    ret = NvBufferMemMap (dmabuf_fd, i, NvBufferMem_Read_Write, &sBaseAddr[i]);
    if (ret != 0) {
      g_print ("%s: NvBufferMemMap Failed \n", __func__);
      return FALSE;
    }

    ret = NvBufferMemSyncForCpu (dmabuf_fd, i, &sBaseAddr[i]);
    if (ret != 0) {
      g_print ("%s: NvBufferMemSyncForCpu Failed \n", __func__);
      return FALSE;
    }

    size = params.height[i] * params.pitch[i];
    memset (sBaseAddr[i], 0x80, size);

    ret = NvBufferMemSyncForDevice (dmabuf_fd, i, &sBaseAddr[i]);
    if (ret != 0) {
      GST_ERROR ("%s: NvBufferMemSyncForDevice Failed \n", __func__);
      return FALSE;
    }

    ret = NvBufferMemUnMap (dmabuf_fd, i, &sBaseAddr[i]);
    if (ret != 0) {
      g_print ("%s: NvBufferMemUnMap Failed \n", __func__);
      return FALSE;
    }
  }

  return TRUE;
}

/* GObject vmethod implementations */

/**
  * initialize the nvstabilize's class.
  *
  * @param klass : Gstnvstabilize objectclass
  */
static void
gst_nvstabilize_class_init (GstnvstabilizeClass * klass)
{
  GST_WARNING("");

  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseTransformClass *gstbasetransform_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasetransform_class = (GstBaseTransformClass *) klass;

  gparent_class = g_type_class_peek_parent (gstbasetransform_class);

  gobject_class->set_property = gst_nvstabilize_set_property;
  gobject_class->get_property = gst_nvstabilize_get_property;
  gobject_class->finalize = gst_nvstabilize_finalize;

  gstelement_class->change_state = gst_nvstabilize_change_state;

  gstbasetransform_class->set_caps = GST_DEBUG_FUNCPTR (gst_nvstabilize_set_caps);
  gstbasetransform_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_nvstabilize_transform_caps);
  gstbasetransform_class->accept_caps =
      GST_DEBUG_FUNCPTR (gst_nvstabilize_accept_caps);
  gstbasetransform_class->transform_size =
      GST_DEBUG_FUNCPTR (gst_nvstabilize_transform_size);
  gstbasetransform_class->get_unit_size =
      GST_DEBUG_FUNCPTR (gst_nvstabilize_get_unit_size);
  gstbasetransform_class->transform = GST_DEBUG_FUNCPTR (gst_nvstabilize_transform);
  gstbasetransform_class->start = GST_DEBUG_FUNCPTR (gst_nvstabilize_start);
  gstbasetransform_class->stop = GST_DEBUG_FUNCPTR (gst_nvstabilize_stop);
  gstbasetransform_class->fixate_caps =
      GST_DEBUG_FUNCPTR (gst_nvstabilize_fixate_caps);
  gstbasetransform_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_nvstabilize_decide_allocation);

  gstbasetransform_class->passthrough_on_same_caps = FALSE;

  g_object_class_install_property (gobject_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Produce verbose output ?",
          FALSE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_CROP_MARGIN,
      g_param_spec_float ("crop-margin", "crop-margin",
          "Max width/height to crop (0 = disabled, negative = inferred from caps)",
           G_MINFLOAT, 0.5, 0.07, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_QUEUE_SIZE,
    g_param_spec_uint ("queue-size", "queue-size", "Queue size",
        1, 6, 5, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));


  gst_element_class_set_details_simple (gstelement_class,
      "NvStabilize Plugin",
      "Stabilizer",
      "Stablizes video frames",
      "mostafa shahbazzadeh <shahbazzadehm@yahoo.com>, ali haddad tabrizi <a.tabrizi.mech@gmail.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_nvstabilize_src_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_nvstabilize_sink_template));
}

/**
  * initialize nvstabilize instance structure.
  *
  * @param filter : Gstnvstabilize object instance
  */
static void
gst_nvstabilize_init (Gstnvstabilize * filter)
{
  GST_WARNING("");
  gst_nvstabilize_init_params (filter);
}

static void
get_NvBufferTransform_filter(Gstnvstabilize * filter)
{
  GST_WARNING("");
  switch(filter->interpolation_method)
  {
    case GST_INTERPOLATION_NEAREST:
        filter->transform_params.transform_filter = NvBufferTransform_Filter_Nearest;
      break;
    case GST_INTERPOLATION_BILINEAR:
        filter->transform_params.transform_filter = NvBufferTransform_Filter_Bilinear;
      break;
    case GST_INTERPOLATION_5_TAP:
        filter->transform_params.transform_filter = NvBufferTransform_Filter_5_Tap;
      break;
    case GST_INTERPOLATION_10_TAP:
        filter->transform_params.transform_filter = NvBufferTransform_Filter_10_Tap;
      break;
    case GST_INTERPOLATION_SMART:
        filter->transform_params.transform_filter = NvBufferTransform_Filter_Smart;
      break;
    case GST_INTERPOLATION_NICEST:
        filter->transform_params.transform_filter = NvBufferTransform_Filter_Nicest;
      break;
    default:
        filter->transform_params.transform_filter = NvBufferTransform_Filter_Smart;
      break;
  }
}

/**
  * initialize nvstabilize instance structure.
  *
  * @param filter : Gstnvstabilize object instance
  */
static void
gst_nvstabilize_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GST_WARNING("");
  Gstnvstabilize *filter = GST_NVSTABILIZE (object);

  switch (prop_id) {
    case PROP_SILENT:
      filter->silent = g_value_get_boolean (value);
      break;
    case PROP_CROP_MARGIN:
      filter->crop_margin = g_value_get_float (value);
      break;
    case PROP_QUEUE_SIZE:
      filter->queue_size = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/**
  * initialize nvstabilize instance structure.
  *
  * @param filter : Gstnvstabilize object instance
  */
static void
gst_nvstabilize_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GST_WARNING("");
  Gstnvstabilize *filter = GST_NVSTABILIZE (object);

  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean (value, filter->silent);
      break;
    case PROP_CROP_MARGIN:
      g_value_set_float (value, filter->crop_margin);
      break;
    case PROP_QUEUE_SIZE:
      g_value_set_uint (value, filter->queue_size);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/**
  * Free all allocated resources(Rmsurface).
  *
  * @param filter : Gstnvstabilize object instance
  */
static void
gst_nvstabilize_free_buf (Gstnvstabilize * filter)
{
  GST_WARNING("");
  gint ret;

  if (filter->isurf_count) {
    ret = NvBufferDestroy (filter->interbuf.idmabuf_fd);
    if (ret != 0) {
      GST_ERROR ("%s: intermediate NvBufferDestroy Failed \n", __func__);
    }
  }
  filter->isurf_count = 0;
  filter->ibuf_count = 0;

  delete filter->stabilizer;
  vxReleaseDelay(&filter->orig_frame_delay);
  vxReleaseContext(&filter->context);
}

/**
  * nvstabilize element state change function.
  *
  * @param element    : Gstnvstabilize element instance
  * @param transition : state transition
  */
static GstStateChangeReturn
gst_nvstabilize_change_state (GstElement * element, GstStateChange transition)
{
  GST_WARNING("");
  GstStateChangeReturn result = GST_STATE_CHANGE_SUCCESS;
  Gstnvstabilize *space;

  space = GST_NVSTABILIZE (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:{
    }
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:{
    }
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:{
    }
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:{
    }
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:{
    }
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:{
    }
      break;
    case GST_STATE_CHANGE_NULL_TO_NULL:{
    }
      break;
    case GST_STATE_CHANGE_READY_TO_READY:{
    }
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PAUSED:{
    }
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PLAYING:{
    }
      break;
  }

  GST_ELEMENT_CLASS (gparent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:{
    }
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:{
    }
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:{
    }
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:{
    }
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:{
      gst_nvstabilize_free_buf (space);
    }
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:{
    }
      break;
    case GST_STATE_CHANGE_NULL_TO_NULL:{
    }
      break;
    case GST_STATE_CHANGE_READY_TO_READY:{
    }
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PAUSED:{
    }
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PLAYING:{
    }
      break;
  }

  return result;
}

/**
  * nvstabilize element finalize function.
  *
  * @param object : element object instance
  */
static void
gst_nvstabilize_finalize (GObject * object)
{
  GST_WARNING("");
  Gstnvstabilize *filter;

  filter = GST_NVSTABILIZE(object);

  if (filter->sinkcaps) {
    gst_caps_unref (filter->sinkcaps);
    filter->sinkcaps = NULL;
  }

  if (filter->srccaps) {
    gst_caps_unref (filter->srccaps);
    filter->sinkcaps = NULL;
  }

  g_mutex_clear (&filter->flow_lock);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/* GstBaseTransform vmethod implementations */

/**
  * notified of the actual caps set.
  *
  * @param btrans  : basetransform  object instance
  * @param incaps  : input capabilities
  * @param outcaps : output capabilities
  */
static gboolean
gst_nvstabilize_set_caps (GstBaseTransform * btrans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GST_WARNING("");
  gboolean ret = TRUE;
  Gstnvstabilize *space;
  gint from_dar_n, from_dar_d, to_dar_n, to_dar_d;
  GstVideoInfo in_info, out_info;
  GstBufferPool *newpool, *oldpool;
  GstStructure *config;
  gint min, surf_count = 0;
  GstCapsFeatures *ift = NULL;
  GstCapsFeatures *oft = NULL;

  space = GST_NVSTABILIZE (btrans);

  /* input caps */
  if (!gst_video_info_from_caps (&in_info, incaps))
    goto invalid_caps;

  /* output caps */
  if (!gst_video_info_from_caps (&out_info, outcaps))
    goto invalid_caps;

  space->from_width = GST_VIDEO_INFO_WIDTH (&in_info);
  space->from_height = GST_VIDEO_INFO_HEIGHT (&in_info);

  space->to_width = GST_VIDEO_INFO_WIDTH (&out_info);
  space->to_height = GST_VIDEO_INFO_HEIGHT (&out_info);

  if ((space->from_width != space->to_width)
      || (space->from_height != space->to_height))
    space->do_scaling = TRUE;

  /* get input pixel format */
  ret =
      gst_nvstabilize_get_pix_fmt (&in_info, &space->in_pix_fmt,
      &surf_count);
  if (ret != TRUE)
    goto invalid_pix_fmt;

  /* get output pixel format */
  ret =
      gst_nvstabilize_get_pix_fmt (&out_info, &space->out_pix_fmt,
      &surf_count);
  if (ret != TRUE)
    goto invalid_pix_fmt;

  ift = gst_caps_get_features (incaps, 0);
  if (gst_caps_features_contains (ift, GST_CAPS_FEATURE_MEMORY_NVMM))
    space->inbuf_memtype = BUF_MEM_HW;

  oft = gst_caps_get_features (outcaps, 0);
  if (gst_caps_features_contains (oft, GST_CAPS_FEATURE_MEMORY_NVMM))
    space->outbuf_memtype = BUF_MEM_HW;

  // if (gst_caps_features_is_equal (ift, oft) &&
  //     (space->in_pix_fmt == space->out_pix_fmt) &&
  //     (!space->do_scaling) &&
  //     (!space->do_cropping) &&
  //     (space->enable_blocklinear_output)) {
  //   /* We are not processing input buffer. Initializations/allocations in this
  //      function can be skipped */
  //   gst_base_transform_set_passthrough (btrans, TRUE);
  //   return TRUE;
  // }

  switch (space->in_pix_fmt) {
    case NvBufferColorFormat_YUV420:
    case NvBufferColorFormat_YUV422:
      space->inbuf_type = BUF_TYPE_YUV;
      space->insurf_count = 3;
      break;
    case NvBufferColorFormat_NV12:
      space->inbuf_type = BUF_TYPE_YUV;
      space->insurf_count = 2;
      space->configuration.format = NVXCU_DF_IMAGE_NV12;
      space->depth = 0;
      break;
    case NvBufferColorFormat_UYVY:
    case NvBufferColorFormat_YUYV:
    case NvBufferColorFormat_YVYU:
      space->inbuf_type = BUF_TYPE_YUV;
      space->insurf_count = 1;
      break;
    case NvBufferColorFormat_XRGB32:
    case NvBufferColorFormat_ABGR32:
      space->inbuf_type = BUF_TYPE_RGB;
      space->insurf_count = 1;
      space->configuration.format = NVXCU_DF_IMAGE_RGBX;
      space->depth = 4;
      break;
    default:
      goto not_supported_inbuf;
      break;
  }

  min = space->num_output_buf;

  space->tsurf_width = space->to_width;
  space->tsurf_height = space->to_height;

  if (surf_count) {
    space->need_intersurf = TRUE;
    space->isurf_flag = TRUE;
  }

  switch (space->out_pix_fmt) {
    case NvBufferColorFormat_YUV420:
    case NvBufferColorFormat_YUV422:
      space->tsurf_count = 3;
      break;
    case NvBufferColorFormat_NV12:
      space->tsurf_count = 2;
      break;
    case NvBufferColorFormat_UYVY:
    case NvBufferColorFormat_YUYV:
    case NvBufferColorFormat_YVYU:
      space->tsurf_count = 1;
      break;
    case NvBufferColorFormat_XRGB32:
    case NvBufferColorFormat_ABGR32:
      space->tsurf_count = 1;
      break;
    default:
      goto not_supported_outbuf;
      break;
  }

  if ((space->do_scaling == TRUE) &&
      (space->in_pix_fmt == NvBufferColorFormat_YUV420)) {
    space->isurf_flag = TRUE;
  }

  if (!gst_util_fraction_multiply (in_info.width,
          in_info.height, in_info.par_n, in_info.par_d, &from_dar_n,
          &from_dar_d)) {
    from_dar_n = from_dar_d = -1;
  }

  if (!gst_util_fraction_multiply (out_info.width,
          out_info.height, out_info.par_n, out_info.par_d, &to_dar_n,
          &to_dar_d)) {
    to_dar_n = to_dar_d = -1;
  }

  if (to_dar_n != from_dar_n || to_dar_d != from_dar_d) {
    GST_WARNING_OBJECT (space, "Cannot keep DAR");
  }

  /* check for outcaps feature */
  ift = gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_NVMM, NULL);
  if (gst_caps_features_is_equal (gst_caps_get_features (outcaps, 0), ift)) {
    space->nvfilterpool = TRUE;
  }
  gst_caps_features_free (ift);

  if (space->nvfilterpool) {
    g_mutex_lock (&space->flow_lock);
    newpool = gst_nv_filter_buffer_pool_new (GST_ELEMENT_CAST (space));

    config = gst_buffer_pool_get_config (newpool);
    gst_buffer_pool_config_set_params (config, outcaps, NvBufferGetSize(), min, min);
    gst_buffer_pool_config_set_allocator (config,
        ((GstNvFilterBufferPool *) newpool)->allocator, NULL);
    if (!gst_buffer_pool_set_config (newpool, config))
      goto config_failed;

    oldpool = space->pool;
    space->pool = newpool;

    g_mutex_unlock (&space->flow_lock);

    /* unref the old nvfilter bufferpool */
    if (oldpool) {
      gst_object_unref (oldpool);
    }
  }

  GST_WARNING_OBJECT (space, "from=%dx%d (par=%d/%d dar=%d/%d), size %"
      G_GSIZE_FORMAT " -> to=%dx%d (par=%d/%d dar=%d/%d), "
      "size %" G_GSIZE_FORMAT,
      in_info.width, in_info.height, in_info.par_n, in_info.par_d,
      from_dar_n, from_dar_d, in_info.size, out_info.width,
      out_info.height, out_info.par_n, out_info.par_d, to_dar_n, to_dar_d,
      out_info.size);

  space->negotiated = ret;
  GST_WARNING("%d", gst_base_transform_is_passthrough(btrans));

  GST_WARNING("%d, %d \n", space->out_pix_fmt, space->in_pix_fmt);

  // ovxio::ContextGuard context;
  space->context = vxCreateContext();
  vxRegisterLogCallback(space->context, &ovxio::stdoutLogCallback, vx_false_e);
  vxDirective((vx_reference)space->context, VX_DIRECTIVE_ENABLE_PERFORMANCE);

  // stabilizer image initialize
  // space->frame_exemplar = vxCreateImage(space->context, out_info.width, out_info.height, VX_DF_IMAGE_RGBX);

  // space->orig_frame_delay = vxCreateDelay(space->context, (vx_reference)space->frame_exemplar, space->orig_frame_delay_size);
  // NVXIO_CHECK_REFERENCE(space->orig_frame_delay);
  // NVXIO_SAFE_CALL( nvx::initDelayOfImages(space->context, space->orig_frame_delay) );
  // NVXIO_SAFE_CALL(vxReleaseImage(&space->frame_exemplar));

  // space->frame = (vx_image)vxGetReferenceFromDelay(space->orig_frame_delay, 0);
  // space->lastFrame = (vx_image)vxGetReferenceFromDelay(space->orig_frame_delay, 1 - static_cast<vx_int32>(space->orig_frame_delay_size));
  
  // space->stabilizer = nvx::VideoStabilizer::createImageBasedVStab(space->context, space->params);
  // space->stabilizer->init(space->frame);

  return ret;

  /* ERRORS */
config_failed:
  {
    GST_ERROR ("failed to set config on bufferpool");
    g_mutex_unlock (&space->flow_lock);
    return FALSE;
  }
not_supported_inbuf:
  {
    GST_ERROR ("input buffer type not supported");
    return FALSE;
  }
not_supported_outbuf:
  {
    GST_ERROR ("output buffer type not supported");
    return FALSE;
  }
invalid_pix_fmt:
  {
    GST_ERROR ("could not configure for input/output format");
    space->in_pix_fmt = NvBufferColorFormat_Invalid;
    space->out_pix_fmt = NvBufferColorFormat_Invalid;
    return FALSE;
  }
invalid_caps:
  {
    GST_ERROR ("invalid caps");
    space->negotiated = FALSE;
    return FALSE;
  }
}

/**
  * Open external resources.
  *
  * @param btrans  : basetransform  object instance
  */
static gboolean
gst_nvstabilize_start (GstBaseTransform * btrans)
{
  GST_WARNING("");

  Gstnvstabilize *space;

  space = GST_NVSTABILIZE (btrans);

  space->transform_params.session = NvBufferSessionCreate();
  if (!space->transform_params.session) {
    GST_ERROR ("NvBufferSessionCreate Failed");
    return FALSE;
  }

  return TRUE;
}

/**
  * Close external resources.
  *
  * @param btrans : basetransform  object instance
  */
static gboolean
gst_nvstabilize_stop (GstBaseTransform * btrans)
{
  GST_WARNING("");

  Gstnvstabilize *space;

  space = GST_NVSTABILIZE (btrans);

  if (space->transform_params.session) {
    NvBufferSessionDestroy (space->transform_params.session);
    space->transform_params.session = NULL;
  }

  if (space->pool) {
    gst_object_unref (space->pool);
    space->pool = NULL;
  }

  return TRUE;
}

/**
  * calculate the size in bytes of a buffer on the other pad
  * with the given other caps, output size only depends on the caps,
  * not on the input caps.
  *
  * @param btrans    : basetransform object instance
  * @param direction : pad direction
  * @param caps      : input caps
  * @param size      : input buffer size
  * @param othercaps : other caps
  * @param othersize : otherpad buffer size
  */
static gboolean
gst_nvstabilize_transform_size (GstBaseTransform * btrans,
    GstPadDirection direction, GstCaps * caps, gsize size,
    GstCaps * othercaps, gsize * othersize)
{
  gboolean ret = TRUE;
  GstVideoInfo vinfo;

  /* size of input buffer cannot be zero */
  g_assert (size);

  ret = gst_video_info_from_caps (&vinfo, othercaps);
  if (ret) {
    *othersize = vinfo.size;
  }

  GST_DEBUG_OBJECT (btrans, "Othersize %" G_GSIZE_FORMAT " bytes"
      "for othercaps %" GST_PTR_FORMAT, *othersize, othercaps);

  return ret;
}

/**
  * Get the size in bytes of one unit for the given caps.
  *
  * @param btrans : basetransform object instance
  * @param caps   : given caps
  * @param size   : size of one unit
  */
static gboolean
gst_nvstabilize_get_unit_size (GstBaseTransform * btrans, GstCaps * caps,
    gsize * size)
{
  GST_WARNING("");
  gboolean ret = TRUE;
  GstVideoInfo vinfo;

  if (!gst_video_info_from_caps (&vinfo, caps)) {
    GST_WARNING_OBJECT (btrans, "Parsing failed for caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }

  *size = vinfo.size;

  GST_WARNING_OBJECT (btrans, "size %" G_GSIZE_FORMAT " bytes"
      "for caps %" GST_PTR_FORMAT, *size, caps);

  return ret;
}

/**
  * Given the pad in direction and the given caps,
  * fixate the caps on the other pad.
  *
  * @param btrans    : basetransform object instance
  * @param direction : pad direction
  * @param caps      : given caps
  * @param othercaps : other caps
  */
static GstCaps *
gst_nvstabilize_fixate_caps (GstBaseTransform * btrans,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GST_WARNING("");

  Gstnvstabilize *space;
  gint tt_width = 0, tt_height = 0;
  GstStructure *in_struct, *out_struct;
  const GValue *from_pix_ar, *to_pix_ar;
  const gchar *from_fmt = NULL, *to_fmt = NULL;
  const gchar *from_interlace_mode = NULL;
  const gchar *to_interlace_mode = NULL;
  GValue from_par = { 0, }, to_par = {
  0,};
  gint n, i, index = 0;
  GstCapsFeatures *features = NULL;
  gboolean have_nvfeature = FALSE;

  space = GST_NVSTABILIZE (btrans);
  GstCapsFeatures *ift = NULL;
  ift = gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_NVMM, NULL);

  n = gst_caps_get_size (othercaps);
  for (i = 0; i < n; i++) {
    features = gst_caps_get_features (othercaps, i);
    if (gst_caps_features_is_equal (features, ift)) {
      index = i;
      have_nvfeature = TRUE;
    }
  }
  gst_caps_features_free (ift);

  if (have_nvfeature) {
    for (i = 0; i < index; i++) {
      gst_caps_remove_structure (othercaps, i);
    }
  }

  othercaps = gst_caps_truncate (othercaps);
  othercaps = gst_caps_make_writable (othercaps);

  GST_DEBUG_OBJECT (space, "trying to fixate othercaps %" GST_PTR_FORMAT
      " based on caps %" GST_PTR_FORMAT, othercaps, caps);

  in_struct = gst_caps_get_structure (caps, 0);
  out_struct = gst_caps_get_structure (othercaps, 0);

  from_pix_ar = gst_structure_get_value (in_struct, "pixel-aspect-ratio");
  to_pix_ar = gst_structure_get_value (out_struct, "pixel-aspect-ratio");

  from_fmt = gst_structure_get_string (in_struct, "format");
  to_fmt = gst_structure_get_string (out_struct, "format");

  if (!to_fmt) {
    /* Output format not fixed */
    if (!gst_structure_fixate_field_string (out_struct, "format", from_fmt)) {
      GST_ERROR_OBJECT (space, "Failed to fixate output format");
      goto finish;
    }
  }

  if (gst_structure_has_field (out_struct, "interlace-mode")) {
    /* interlace-mode present */
    to_interlace_mode = gst_structure_get_string (out_struct, "interlace-mode");
    if (!to_interlace_mode) {
      /* interlace-mode not fixed */
      from_interlace_mode = gst_structure_get_string (in_struct, "interlace-mode");
      if (from_interlace_mode)
        gst_structure_fixate_field_string (out_struct, "interlace-mode", from_interlace_mode);
      else
        gst_structure_fixate_field_string (out_struct, "interlace-mode", "progessive");
    }
  }

  /* If fixating from the sinkpad always set the PAR and
   * assume that missing PAR on the sinkpad means 1/1 and
   * missing PAR on the srcpad means undefined
   */
  if (direction == GST_PAD_SINK) {
    if (!from_pix_ar) {
      g_value_init (&from_par, GST_TYPE_FRACTION);
      gst_value_set_fraction (&from_par, 1, 1);
      from_pix_ar = &from_par;
    }
    if (!to_pix_ar) {
      g_value_init (&to_par, GST_TYPE_FRACTION_RANGE);
      gst_value_set_fraction_range_full (&to_par, 1, G_MAXINT, G_MAXINT, 1);
      to_pix_ar = &to_par;
    }
  } else {
    if (!to_pix_ar) {
      g_value_init (&to_par, GST_TYPE_FRACTION);
      gst_value_set_fraction (&to_par, 1, 1);
      to_pix_ar = &to_par;

      gst_structure_set (out_struct, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1,
          1, NULL);
    }
    if (!from_pix_ar) {
      g_value_init (&from_par, GST_TYPE_FRACTION);
      gst_value_set_fraction (&from_par, 1, 1);
      from_pix_ar = &from_par;
    }
  }

  /* have both PAR but they might not be fixated */
  {
    gint f_width, f_height, f_par_n, f_par_d, t_par_n, t_par_d;
    gint t_width = 0, t_height = 0;
    gint f_dar_n, f_dar_d;
    gint numerator, denominator;

    /* from_pix_ar should be fixed */
    g_return_val_if_fail (gst_value_is_fixed (from_pix_ar), othercaps);

    f_par_n = gst_value_get_fraction_numerator (from_pix_ar);
    f_par_d = gst_value_get_fraction_denominator (from_pix_ar);

    gst_structure_get_int (in_struct, "width", &f_width);
    gst_structure_get_int (in_struct, "height", &f_height);

    gst_structure_get_int (out_struct, "width", &t_width);
    gst_structure_get_int (out_struct, "height", &t_height);

    /* if both width and height are already fixed, can't do anything
     * about it anymore */
    if (t_width && t_height) {
      guint num, den;

      GST_DEBUG_OBJECT (space, "dimensions already set to %dx%d, not fixating",
          t_width, t_height);
      if (!gst_value_is_fixed (to_pix_ar)) {
        if (gst_video_calculate_display_ratio (&num, &den, f_width, f_height,
                f_par_n, f_par_d, t_width, t_height)) {
          GST_DEBUG_OBJECT (space, "fixating to_pix_ar to %dx%d", num, den);
          if (gst_structure_has_field (out_struct, "pixel-aspect-ratio")) {
            gst_structure_fixate_field_nearest_fraction (out_struct,
                "pixel-aspect-ratio", num, den);
          } else if (num != den) {
            gst_structure_set (out_struct, "pixel-aspect-ratio",
                GST_TYPE_FRACTION, num, den, NULL);
          }
        }
      }
      goto finish;
    }

    /* Calc input DAR */
    if (!gst_util_fraction_multiply (f_width, f_height, f_par_n, f_par_d,
            &f_dar_n, &f_dar_d)) {
      GST_ERROR_OBJECT (space, "calculation of the output"
          "scaled size error");
      goto finish;
    }

    GST_DEBUG_OBJECT (space, "Input DAR: %d / %d", f_dar_n, f_dar_d);

    /* If either w or h are fixed either except choose a height or
     * width and PAR that matches the DAR as near as possible
     */
    if (t_width) {
      /* width is already fixed */
      gint set_par_n;
      gint set_par_d;

      gint s_height = 0;
      GstStructure *tmp_struct = NULL;

      /* Choose the height nearest to
       * height with same DAR, as PAR is fixed */
      if (gst_value_is_fixed (to_pix_ar)) {
        /* get PAR denominator */
        t_par_d = gst_value_get_fraction_denominator (to_pix_ar);
        /* get PAR numerator */
        t_par_n = gst_value_get_fraction_numerator (to_pix_ar);

        if (!gst_util_fraction_multiply (f_dar_n, f_dar_d,
                t_par_d, t_par_n,
                &numerator, &denominator)) {
          GST_ERROR_OBJECT (space, "calculation of the output"
              "scaled size error");
          goto finish;
        }

        /* calc height */
        t_height = (guint) gst_util_uint64_scale_int (t_width,
                   denominator, numerator);
        /* set height */
        gst_structure_fixate_field_nearest_int (out_struct, "height", t_height);

        goto finish;
      }

      /* The PAR is not fixed set arbitrary PAR. */

      /* can keep the input height check  */
      tmp_struct = gst_structure_copy (out_struct);
      gst_structure_fixate_field_nearest_int (tmp_struct, "height", f_height);
      gst_structure_get_int (tmp_struct, "height", &s_height);

      /* May failed but try to keep the DAR however by
       * adjusting the PAR */
      if (!gst_util_fraction_multiply (f_dar_n, f_dar_d, s_height, t_width,
              &t_par_n, &t_par_d)) {
        GST_ERROR_OBJECT (space, "calculation of the output"
            "scaled size error");
        gst_structure_free (tmp_struct);
        goto finish;
      }

      if (!gst_structure_has_field (tmp_struct, "pixel-aspect-ratio")) {
        gst_structure_set_value (tmp_struct, "pixel-aspect-ratio", to_pix_ar);
      }

      /* set fixate PAR */
      if (gst_structure_fixate_field_nearest_fraction (tmp_struct,
            "pixel-aspect-ratio",
            t_par_n, t_par_d)) {
        /* get PAR */
        if (gst_structure_get_field_type (tmp_struct, "pixel-aspect-ratio") !=
            G_TYPE_INVALID) {
          if(!gst_structure_get_fraction (tmp_struct,
              "pixel-aspect-ratio",
              &set_par_n, &set_par_d))
            GST_ERROR_OBJECT (space, "PAR values set failed");
        }
        /* values set correctly */
        if (tmp_struct) {
          gst_structure_free (tmp_struct);
          tmp_struct = NULL;
        }
      }

      if (set_par_n == t_par_n) {
        if (set_par_d == t_par_d) {
          /* Check for PAR field */
          if (gst_structure_has_field (out_struct, "pixel-aspect-ratio") ||
              !(set_par_n == set_par_d)) {
            /* set height & PAR */
            gst_structure_set (out_struct,
                "height", G_TYPE_INT, s_height,
                "pixel-aspect-ratio", GST_TYPE_FRACTION,
                set_par_n, set_par_d,
                NULL);
          }
          goto finish;
        }
      }

      if (!gst_util_fraction_multiply (f_dar_n, f_dar_d,
           set_par_d, set_par_n,
           &numerator, &denominator)) {
        GST_ERROR_OBJECT (space, "calculation of the output"
            "scaled size error");
        goto finish;
      }

      /* Calc height */
      t_height = (guint) gst_util_uint64_scale_int (t_width,
                    denominator, numerator);
      /* Set height */
      gst_structure_fixate_field_nearest_int (out_struct, "height", t_height);

      /* If struct has field PAR then set PAR */
      if (gst_structure_has_field (out_struct, "pixel-aspect-ratio") ||
          !(set_par_n == set_par_d)) {
        /* set PAR */
        gst_structure_set (out_struct, "pixel-aspect-ratio", GST_TYPE_FRACTION,
            set_par_n, set_par_d,
            NULL);
      }
      goto finish;
    } else if (t_height) {
      /* height is already fixed */
      gint set_par_n;
      gint set_par_d;

      gint s_width = 0;
      GstStructure *tmp_struct = NULL;

      /* Choose the width nearest to the
       * width with same DAR, as PAR is fixed */
      if (gst_value_is_fixed (to_pix_ar)) {
        /* get PAR denominator */
        t_par_d = gst_value_get_fraction_denominator (to_pix_ar);
        /* get PAR numerator */
        t_par_n = gst_value_get_fraction_numerator (to_pix_ar);

        if (!gst_util_fraction_multiply (f_dar_n, f_dar_d, t_par_d,
                t_par_n, &numerator, &denominator)) {
          GST_ERROR_OBJECT (space, "calculation of the output"
              "scaled size error");
          goto finish;
        }

        /* calc width */
        t_width =
            (guint) gst_util_uint64_scale_int (t_height, numerator,
            denominator);
        /* set width */
        gst_structure_fixate_field_nearest_int (out_struct, "width", t_width);

        goto finish;
      }

      /* PAR is not fixed set arbitrary PAR */

      tmp_struct = gst_structure_copy (out_struct);
      gst_structure_fixate_field_nearest_int (tmp_struct, "width", f_width);
      gst_structure_get_int (tmp_struct, "width", &s_width);

      /* May failed but try to keep the DAR however by
       * adjusting the PAR */
      if (!gst_util_fraction_multiply (f_dar_n, f_dar_d, t_height, s_width,
              &t_par_n, &t_par_d)) {
        GST_ERROR_OBJECT (space, "calculation of the output"
            "scaled size error");
        gst_structure_free (tmp_struct);
        goto finish;
      }

      if (!gst_structure_has_field (tmp_struct, "pixel-aspect-ratio")) {
        gst_structure_set_value (tmp_struct, "pixel-aspect-ratio", to_pix_ar);
      }

      /* set fixate PAR */
      if (gst_structure_fixate_field_nearest_fraction (tmp_struct,
            "pixel-aspect-ratio",
            t_par_n, t_par_d)) {
        if (gst_structure_get_field_type (tmp_struct, "pixel-aspect-ratio") !=
            G_TYPE_INVALID) {
          /* get PAR */
          if (!gst_structure_get_fraction (tmp_struct,
               "pixel-aspect-ratio",
               &set_par_n, &set_par_d))
            GST_ERROR_OBJECT (space, "PAR values set failed");
        }
        /* values set correctly */
        if (tmp_struct) {
          gst_structure_free (tmp_struct);
          tmp_struct = NULL;
        }
      }

      if (set_par_n == t_par_n) {
        if (set_par_d == t_par_d) {
          /* check for PAR field */
          if (gst_structure_has_field (out_struct, "pixel-aspect-ratio") ||
              !(set_par_n == set_par_d)) {
            /* set width & PAR */
            gst_structure_set (out_struct,
                "width", G_TYPE_INT, s_width,
                "pixel-aspect-ratio", GST_TYPE_FRACTION,
                set_par_n, set_par_d,
                NULL);
          }
          goto finish;
        }
      }

      if (!gst_util_fraction_multiply (f_dar_n, f_dar_d,
              set_par_d, set_par_n,
              &numerator, &denominator)) {
        GST_ERROR_OBJECT (space, "calculation of the output"
            "scaled size error");
        goto finish;
      }

      /* Calc width */
      t_width = (guint) gst_util_uint64_scale_int (t_height,
                 numerator, denominator);

      /* Set width */
      gst_structure_fixate_field_nearest_int (out_struct, "width", t_width);

      /* If struct has field PAR then set PAR */
      if (gst_structure_has_field (out_struct, "pixel-aspect-ratio") ||
          !(set_par_n == set_par_d)) {
        /* set PAR*/
        gst_structure_set (out_struct, "pixel-aspect-ratio", GST_TYPE_FRACTION,
            set_par_n, set_par_d,
            NULL);
      }

      goto finish;
    } else if (gst_value_is_fixed (to_pix_ar)) {

      gint s_height = 0;
      gint s_width = 0;
      gint from_hight = 0;
      gint from_width = 0;
      GstStructure *tmp_struct = NULL;

      /* Get PAR denominator */
      t_par_d = gst_value_get_fraction_denominator (to_pix_ar);
      /* Get PAR numerator */
      t_par_n = gst_value_get_fraction_numerator (to_pix_ar);

      /* find scale factor for change in PAR */
      if (!gst_util_fraction_multiply (f_dar_n, f_dar_d,
              t_par_n, t_par_d,
              &numerator, &denominator)) {
        GST_ERROR_OBJECT (space, "calculation of the output"
            "scaled size error");
        goto finish;
      }

      tmp_struct = gst_structure_copy (out_struct);

      gst_structure_fixate_field_nearest_int (tmp_struct, "height", f_height);
      gst_structure_get_int (tmp_struct, "height", &s_height);

      /* This may failed but however scale the width to keep DAR */
      t_width =
          (guint) gst_util_uint64_scale_int (s_height, numerator, denominator);
      gst_structure_fixate_field_nearest_int (tmp_struct, "width", t_width);
      gst_structure_get_int (tmp_struct, "width", &s_width);
      gst_structure_free (tmp_struct);

      /* kept DAR and the height is nearest to the original height */
      if (s_width == t_width) {
        gst_structure_set (out_struct, "width", G_TYPE_INT, s_width, "height",
            G_TYPE_INT, s_height, NULL);
        goto finish;
      }

      from_hight = s_height;
      from_width = s_width;

      /* If former failed, try to keep the input width at least */
      tmp_struct = gst_structure_copy (out_struct);
      gst_structure_fixate_field_nearest_int (tmp_struct, "width", f_width);
      gst_structure_get_int (tmp_struct, "width", &s_width);

      /* This may failed but however try to scale the width to keep DAR */
      t_height =
          (guint) gst_util_uint64_scale_int (s_width, denominator, numerator);
      gst_structure_fixate_field_nearest_int (tmp_struct, "height", t_height);
      gst_structure_get_int (tmp_struct, "height", &s_height);
      gst_structure_free (tmp_struct);

      /* We kept the DAR and the width is nearest to the original width */
      if (s_height == t_height) {
        gst_structure_set (out_struct, "width", G_TYPE_INT, s_width, "height",
            G_TYPE_INT, s_height, NULL);
        goto finish;
      }

      /* If all failed, keep the height that nearest to the orignal
       * height and the nearest possible width.
       */
      gst_structure_set (out_struct, "width", G_TYPE_INT, from_width, "height",
          G_TYPE_INT, from_hight, NULL);
      goto finish;
    } else {
      gint tmp_struct2;
      gint set_par_n;
      gint set_par_d;
      gint s_height = 0;
      gint s_width = 0;
      GstStructure *tmp_struct = NULL;

      /* width, height and PAR are not fixed though passthrough impossible */

      /* keep height and width as fine as possible & scale PAR */
      tmp_struct = gst_structure_copy (out_struct);

      if (gst_structure_fixate_field_nearest_int (tmp_struct, "height", f_height))
        gst_structure_get_int (tmp_struct, "height", &s_height);

      if (gst_structure_fixate_field_nearest_int (tmp_struct, "width", f_width))
        gst_structure_get_int (tmp_struct, "width", &s_width);

      if (!gst_util_fraction_multiply (f_dar_n, f_dar_d,
              s_height, s_width,
              &t_par_n, &t_par_d)) {
        GST_ERROR_OBJECT (space, "calculation of the output"
            "scaled size error");
        goto finish;
      }

      if (!gst_structure_has_field (tmp_struct, "pixel-aspect-ratio")) {
        gst_structure_set_value (tmp_struct, "pixel-aspect-ratio", to_pix_ar);
      }

      if (gst_structure_fixate_field_nearest_fraction (tmp_struct, "pixel-aspect-ratio",
          t_par_n, t_par_d)) {
        gst_structure_get_fraction (tmp_struct, "pixel-aspect-ratio",
            &set_par_n, &set_par_d);
      }
      gst_structure_free (tmp_struct);

      if (set_par_n == t_par_n) {
        if (set_par_d == t_par_d) {
          gst_structure_set (out_struct,
              "width", G_TYPE_INT, s_width,
              "height", G_TYPE_INT, s_height,
              NULL);

          if (gst_structure_has_field (out_struct, "pixel-aspect-ratio") ||
              !(set_par_n == set_par_d))
            gst_structure_set (out_struct, "pixel-aspect-ratio", GST_TYPE_FRACTION,
                set_par_n, set_par_d,
                NULL);
          space->no_dimension = TRUE;
          goto finish;
        }
      }

      /* Or scale width to keep the DAR with the set
       * PAR and height */
      if (!gst_util_fraction_multiply (f_dar_n, f_dar_d,
              set_par_d, set_par_n,
              &numerator, &denominator)) {
        GST_ERROR_OBJECT (space, "calculation of the output"
            "scaled size error");
        goto finish;
      }

      t_width =
          (guint) gst_util_uint64_scale_int (s_height, numerator, denominator);
      tmp_struct = gst_structure_copy (out_struct);

      if (gst_structure_fixate_field_nearest_int (tmp_struct, "width", t_width)) {
        gst_structure_get_int (tmp_struct, "width", &tmp_struct2);
      }
      gst_structure_free (tmp_struct);

      if (tmp_struct2 == t_width) {
        gst_structure_set (out_struct,
            "width", G_TYPE_INT, tmp_struct2,
            "height", G_TYPE_INT, s_height,
            NULL);
        if (gst_structure_has_field (out_struct, "pixel-aspect-ratio")
            || (set_par_n != set_par_d))
          gst_structure_set (out_struct, "pixel-aspect-ratio",
              GST_TYPE_FRACTION, set_par_n, set_par_d,
              NULL);
        space->no_dimension = TRUE;
        goto finish;
      }

      t_height =
          (guint) gst_util_uint64_scale_int (s_width, denominator, numerator);
      tmp_struct = gst_structure_copy (out_struct);

      if (gst_structure_fixate_field_nearest_int (tmp_struct, "height", t_height)) {
        gst_structure_get_int (tmp_struct, "height", &tmp_struct2);
      }
      gst_structure_free (tmp_struct);

      if (tmp_struct2 == t_height) {
        gst_structure_set (out_struct,
            "width", G_TYPE_INT, s_width,
            "height", G_TYPE_INT, tmp_struct2,
            NULL);
        if (gst_structure_has_field (out_struct, "pixel-aspect-ratio") ||
            set_par_n != set_par_d)
          gst_structure_set (out_struct, "pixel-aspect-ratio",
              GST_TYPE_FRACTION, set_par_n, set_par_d,
              NULL);
        space->no_dimension = TRUE;
        goto finish;
      }

      /* If all failed can't keep DAR & take nearest values for all */
      gst_structure_set (out_struct,
          "width", G_TYPE_INT, s_width,
          "height", G_TYPE_INT, s_height,
          NULL);
      if (gst_structure_has_field (out_struct, "pixel-aspect-ratio") ||
          (set_par_n != set_par_d))
        gst_structure_set (out_struct, "pixel-aspect-ratio", GST_TYPE_FRACTION,
            set_par_n, set_par_d,
            NULL);
      space->no_dimension = TRUE;
    }
  }

finish:
  if (space->no_dimension) {
      g_assert_not_reached ();
  }
  

  GST_DEBUG_OBJECT (space, "fixated othercaps to %" GST_PTR_FORMAT, othercaps);

  if (from_pix_ar == &from_par)
    g_value_unset (&from_par);
  if (to_pix_ar == &to_par)
    g_value_unset (&to_par);

  return othercaps;
}

/**
  * Given the pad in direction and the given caps,
  * provide allowed caps on the other pad.
  *
  * @param btrans    : basetransform object instance
  * @param direction : pad direction
  * @param caps      : given caps
  * @param filter    : other caps
  */
static GstCaps *
gst_nvstabilize_transform_caps (GstBaseTransform * btrans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GST_WARNING("");

  GstCaps *ret = NULL;
  GstCaps *tmp1, *tmp2;
  GstCapsFeatures *features = NULL;

  GST_WARNING_OBJECT (btrans,
      "Transforming caps %" GST_PTR_FORMAT " in direction %s", caps,
      (direction == GST_PAD_SINK) ? "sink" : "src");

  /* Get all possible caps that we can transform into */
  tmp1 = gst_nvstabilize_caps_remove_format_info (caps);

  if (filter) {
    if (direction == GST_PAD_SRC) {
      GstCapsFeatures *ift = NULL;
      ift = gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_NVMM, NULL);
      features = gst_caps_get_features (filter, 0);
      if (!gst_caps_features_is_equal (features, ift)) {
        gint n, i;
        GstCapsFeatures *tft;
        n = gst_caps_get_size (tmp1);
        for (i = 0; i < n; i++) {
          tft = gst_caps_get_features (tmp1, i);
          if (gst_caps_features_get_size (tft))
            gst_caps_features_remove (tft, GST_CAPS_FEATURE_MEMORY_NVMM);
        }
      }
      gst_caps_features_free (ift);
    }

    tmp2 = gst_caps_intersect_full (filter, tmp1, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (tmp1);
    tmp1 = tmp2;
  }

  if (gst_caps_is_empty(tmp1))
    ret = gst_caps_copy(filter);
  else
    ret = tmp1;

  if (!filter) {
    GstStructure *str;
    str = gst_structure_copy (gst_caps_get_structure (ret, 0));

    GstCapsFeatures *ift;
    ift = gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_NVMM, NULL);

    gst_caps_append_structure_full (ret, str, ift);

    str = gst_structure_copy (gst_caps_get_structure (ret, 0));
    gst_caps_append_structure_full (ret, str, NULL);
  }

  GST_DEBUG_OBJECT (btrans, "transformed %" GST_PTR_FORMAT " into %"
      GST_PTR_FORMAT, caps, ret);

  return ret;
}

/**
  * check if caps can be handled by the element.
  *
  * @param btrans    : basetransform object instance
  * @param direction : pad direction
  * @param caps      : given caps
  */
static gboolean
gst_nvstabilize_accept_caps (GstBaseTransform * btrans,
    GstPadDirection direction, GstCaps * caps)
{
  GST_WARNING("");

  gboolean ret = TRUE;
  Gstnvstabilize *space = NULL;
  GstCaps *allowed = NULL;

  space = GST_NVSTABILIZE (btrans);

  GST_WARNING_OBJECT (btrans, "accept caps %" GST_PTR_FORMAT, caps);

  /* get all the formats we can handle on this pad */
  if (direction == GST_PAD_SINK)
    allowed = space->sinkcaps;
  else
    allowed = space->srccaps;

  if (!allowed) {
    GST_WARNING_OBJECT (btrans, "failed to get allowed caps");
    goto no_transform_possible;
  }

  GST_WARNING_OBJECT (btrans, "allowed caps %" GST_PTR_FORMAT, allowed);

  /* intersect with the requested format */
  ret = gst_caps_is_subset (caps, allowed);
  if (!ret) {
    goto no_transform_possible;
  }

done:
  return ret;

  /* ERRORS */
no_transform_possible:
  {
    GST_WARNING_OBJECT (btrans,
        "could not transform %" GST_PTR_FORMAT " in anything we support", caps);
    ret = FALSE;
    goto done;
  }
}

/**
  * Setup the allocation parameters for allocating output buffers.
  *
  * @param btrans : basetransform object instance
  * @param query  : downstream allocation query
  */
static gboolean
gst_nvstabilize_decide_allocation (GstBaseTransform * btrans, GstQuery * query)
{
  GST_WARNING("");

  guint j, metas_no;
  Gstnvstabilize *space = NULL;
  GstCaps *outcaps = NULL;
  GstCaps *myoutcaps = NULL;
  GstBufferPool *pool = NULL;
  guint size, minimum, maximum;
  GstAllocator *allocator = NULL;
  GstAllocationParams params = { 0, 0, 0, 0 };
  GstStructure *config = NULL;
  GstVideoInfo info;
  gboolean modify_allocator;

  space = GST_NVSTABILIZE (btrans);

  metas_no = gst_query_get_n_allocation_metas (query);
  for (j = 0; j < metas_no; j++) {
    gboolean remove_meta;
    GType meta_api;
    const GstStructure *param_str = NULL;

    meta_api = gst_query_parse_nth_allocation_meta (query, j, &param_str);

    if (gst_meta_api_type_has_tag (meta_api, GST_META_TAG_MEMORY)) {
      /* Different memory will get allocated for input and output.
         remove all memory dependent metadata */
      GST_DEBUG_OBJECT (space, "remove memory specific metadata %s",
          g_type_name (meta_api));
      remove_meta = TRUE;
    } else {
      /* Default remove all metadata */
      GST_DEBUG_OBJECT (space, "remove metadata %s", g_type_name (meta_api));
      remove_meta = TRUE;
    }

    if (remove_meta) {
      gst_query_remove_nth_allocation_meta (query, j);
      j--;
      metas_no--;
    }
  }

  gst_query_parse_allocation (query, &outcaps, NULL);
  if (outcaps == NULL)
    goto no_caps;

  /* Use nvfilter custom buffer pool */
  if (space->nvfilterpool) {
    g_mutex_lock (&space->flow_lock);
    pool = space->pool;
    if (pool)
      gst_object_ref (pool);
    g_mutex_unlock (&space->flow_lock);

    if (pool != NULL) {
      config = gst_buffer_pool_get_config (pool);
      gst_buffer_pool_config_get_params (config, &myoutcaps, &size, NULL, NULL);

      GST_DEBUG_OBJECT (space, "we have a pool with caps %" GST_PTR_FORMAT,
          myoutcaps);

      if (!gst_caps_is_equal (outcaps, myoutcaps)) {
        /* different caps, we can't use current pool */
        GST_DEBUG_OBJECT (space, "pool has different caps");
        gst_object_unref (pool);
        pool = NULL;
      }
      gst_structure_free (config);
    }

    if (pool == NULL) {
      if (!gst_video_info_from_caps (&info, outcaps))
        goto invalid_caps;

      size = info.size;
      minimum = space->num_output_buf;

      GST_DEBUG_OBJECT (space, "create new pool");

      g_mutex_lock (&space->flow_lock);
      pool = gst_nv_filter_buffer_pool_new (GST_ELEMENT_CAST (space));

      config = gst_buffer_pool_get_config (pool);
      gst_buffer_pool_config_set_params (config, outcaps, NvBufferGetSize(), minimum, minimum);
      gst_buffer_pool_config_set_allocator (config,
          ((GstNvFilterBufferPool *) pool)->allocator, NULL);
      if (!gst_buffer_pool_set_config (pool, config))
        goto config_failed;

      space->pool = gst_object_ref (pool);

      g_mutex_unlock (&space->flow_lock);
    }

    if (pool) {
      config = gst_buffer_pool_get_config (pool);
      gst_buffer_pool_config_get_allocator (config, &allocator, &params);
      gst_buffer_pool_config_get_params (config, &myoutcaps, &size, &minimum, &maximum);

      /* Add check, params may be empty e.g. fakesink */
      if (gst_query_get_n_allocation_params (query) > 0) {
        /* Set allocation params */
        gst_query_set_nth_allocation_param (query, 0, allocator, &params);
      } else {
        /* Add allocation params */
        gst_query_add_allocation_param (query, allocator, &params);
      }

      /* Set allocation pool */
      if (gst_query_get_n_allocation_pools (query) > 0) {
        gst_query_set_nth_allocation_pool (query, 0, pool, size, minimum, maximum);
      } else {
        gst_query_add_allocation_pool (query, pool, size, minimum, maximum);
      }

      gst_structure_free (config);
      gst_object_unref (pool);
    }
  } else {
    /* Use oss buffer pool */
    GST_WARNING("");

    if (gst_query_get_n_allocation_params (query) > 0) {
      /* Get allocation params */
      gst_query_parse_nth_allocation_param (query, 0, &allocator, &params);
      modify_allocator = TRUE;
    } else {
      allocator = NULL;
      gst_allocation_params_init (&params);
      modify_allocator = FALSE;
    }

    if (gst_query_get_n_allocation_pools (query) > 0) {
      /* Parse pool to get size, min & max  */
      gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &minimum, &maximum);
      if (pool == NULL) {
        GST_DEBUG_OBJECT (btrans, "no pool available, creating new oss pool");
        pool = gst_buffer_pool_new ();
      }
    } else {
      pool = NULL;
      size = 0;
      minimum = 0;
      maximum = 0;
    }

    if (pool) {
      config = gst_buffer_pool_get_config (pool);
      /* Set params on config */
      gst_buffer_pool_config_set_params (config, outcaps, size, minimum, maximum);
      /* Set allocator on config */
      gst_buffer_pool_config_set_allocator (config, allocator, &params);
      /* Set config on pool */
      gst_buffer_pool_set_config (pool, config);
    }

    if (modify_allocator) {
      /* Set allocation params */
      gst_query_set_nth_allocation_param (query, 0, allocator, &params);
    } else {
      /* Add allocation params */
      gst_query_add_allocation_param (query, allocator, &params);
    }

    if (allocator) {
      gst_object_unref (allocator);
    }

    if (pool) {
      gst_query_set_nth_allocation_pool (query, 0, pool, size, minimum, maximum);
      gst_object_unref (pool);
    }
  }

  return TRUE;
/* ERROR */
no_caps:
  {
    GST_ERROR ("no caps specified");
    return FALSE;
  }
invalid_caps:
  {
    GST_ERROR ("invalid caps specified");
    return FALSE;
  }
config_failed:
  {
    GST_ERROR ("failed to set config on bufferpool");
    g_mutex_unlock (&space->flow_lock);
    return FALSE;
  }
}

/**
  * Convert and copy incomming frame to vx image
  *
  * @param exec_target  : input buffer
  * @param image : output buffer
  */
void convertFrame(nvxcu_stream_exec_target_t &exec_target,
                  const ovxio::image_t & image,
                  const nvidiaio::FrameSource::Parameters & configuration,
                  int width, int height,
                  bool usePitch, size_t pitch,
                  int depth, void * decodedPtr,
                  bool is_cuda,
                  void *& devMem,
                  size_t & devMemPitch) 

{
  // nvxio::ProfilerRange range(nvxio::COLOR_ARGB_FUSCHIA, "ConvertFrame (NVXIO)");

  cudaStream_t stream = nullptr;
  bool needConvert = image.format != configuration.format;
  bool canCopyDirectly = !needConvert ||
          (configuration.format == NVXCU_DF_IMAGE_NV12 && image.format == NVXCU_DF_IMAGE_U8);

  // allocate CUDA memory to copy decoded image to
  if (!is_cuda && !canCopyDirectly)
  {
    if (!devMem)
    {
      size_t height_dec = height;

      if (configuration.format == NVXCU_DF_IMAGE_NV12)
        height_dec += height_dec >> 1;

      // we assume that decoded image will have no more than 4 channels per pixel
      NVXIO_ASSERT( cudaSuccess == cudaMallocPitch(&devMem, &devMemPitch,
                                                    width * 4, height_dec) );
    }
  }
  void * devMems[2] = { devMem, nullptr };

  if (!canCopyDirectly)
  {
    NVXIO_ASSERT(needConvert);
    if (!is_cuda)
    {
      NVXIO_ASSERT(devMem);
      // a. upload decoded image to CUDA buffer
      if (configuration.format == NVXCU_DF_IMAGE_U8 ||
              configuration.format == NVXCU_DF_IMAGE_RGB ||
              configuration.format == NVXCU_DF_IMAGE_RGBX)
      {
        vx_int32 stride_y = usePitch ? pitch : ((width * depth + 3) >> 2) << 2;
        vx_int32 stride_x = depth;

        CUDA_SAFE_CALL (
                cudaMemcpy2DAsync(devMems[0], devMemPitch,
                                  decodedPtr, stride_y,
                                  width * stride_x,
                                  height, cudaMemcpyHostToDevice, stream) );
        CUDA_SAFE_CALL( cudaStreamSynchronize(stream) );
      }
      else if (configuration.format == NVXCU_DF_IMAGE_NV12)
      {
        vx_int32 stride_y = usePitch ? pitch : ((width + 3) >> 2) << 2;
        vx_int32 stride_x = sizeof(uint8_t);
        CUDA_SAFE_CALL (
                cudaMemcpy2DAsync(devMems[0], devMemPitch,
                                  decodedPtr, stride_y,
                                  width * stride_x,
                                  height, cudaMemcpyHostToDevice, stream) );

        devMems[1] = (uint8_t *)devMem + devMemPitch * height;
        stride_x = sizeof(uint16_t);
        decodedPtr = (void *) ((uint8_t *)decodedPtr + stride_y * height);
        width >>= 1;
        height >>= 1;
        CUDA_SAFE_CALL (
                    cudaMemcpy2DAsync(devMems[1], devMemPitch,
                                      decodedPtr, stride_y,
                                      width * stride_x,
                                      height, cudaMemcpyHostToDevice, stream) );
        CUDA_SAFE_CALL( cudaStreamSynchronize(stream) );
      }
      else
      {
        NVXIO_THROW_EXCEPTION("Unsupported image format");
      }
    }
    else
    {
      if (configuration.format == NVXCU_DF_IMAGE_U8 ||
              configuration.format == NVXCU_DF_IMAGE_RGB ||
              configuration.format == NVXCU_DF_IMAGE_RGBX)
      {
        vx_int32 stride_y = usePitch ? pitch : ((width * depth + 3) >> 2) << 2;
        devMems[0] = decodedPtr;
        devMemPitch = stride_y;
      }
      else if (configuration.format == NVXCU_DF_IMAGE_NV12)
      {
        vx_int32 stride_y = usePitch ? pitch : ((width + 3) >> 2) << 2;
        devMems[0] = decodedPtr;
        devMemPitch = stride_y;
        devMems[1] = (void *) ((uint8_t *)decodedPtr + stride_y * height);
      }
      else
      {
        NVXIO_THROW_EXCEPTION("Unsupported image format");
      }
    }
  }
  if (canCopyDirectly)
  {
    cudaMemcpyKind copyKind = is_cuda ? cudaMemcpyDeviceToDevice :
                                        cudaMemcpyHostToDevice;
    void * framePtr = nullptr;

    if (configuration.format == NVXCU_DF_IMAGE_U8 ||
            configuration.format == NVXCU_DF_IMAGE_RGB ||
            configuration.format == NVXCU_DF_IMAGE_RGBX)
    {
      vx_int32 stride_y = usePitch ? pitch : ((width * depth + 3) >> 2) << 2;
      vx_int32 stride_x = depth;
      framePtr = image.planes[0].ptr;
      CUDA_SAFE_CALL (
                  cudaMemcpy2DAsync(framePtr, image.planes[0].pitch_in_bytes,
                  decodedPtr, stride_y,
                  width * stride_x,
                  height, copyKind, stream) );

      CUDA_SAFE_CALL( cudaStreamSynchronize(stream) );
    }
    else if (configuration.format == NVXCU_DF_IMAGE_NV12 && image.format == NVXCU_DF_IMAGE_NV12)
    {
      vx_int32 stride_y = usePitch ? pitch : ((width + 3) >> 2) << 2;
      vx_int32 stride_x = sizeof(uint8_t);;
      framePtr = image.planes[0].ptr;
      CUDA_SAFE_CALL (
                  cudaMemcpy2DAsync(framePtr, image.planes[0].pitch_in_bytes,
                  decodedPtr, stride_y,
                  width * stride_x,
                  height, copyKind, stream) );

      framePtr = image.planes[1].ptr;
      stride_x = sizeof(uint16_t);
      width >>= 1;
      height >>= 1;
      decodedPtr = (void *) ((uint8_t *)decodedPtr + stride_y * height);
      CUDA_SAFE_CALL (
                  cudaMemcpy2DAsync(framePtr, image.planes[1].pitch_in_bytes,
                  decodedPtr, stride_y,
                  width * stride_x,
                  height, copyKind, stream) );
      CUDA_SAFE_CALL( cudaStreamSynchronize(stream) );
    }
    else
    {
      NVXIO_THROW_EXCEPTION("Unsupported image format");
    }
  }
  else
  {
    nvxcu_pitch_linear_image_t input, output;
    input.base.format = configuration.format;
    input.base.width = configuration.frameWidth;
    input.base.height = configuration.frameHeight;
    input.base.image_type = NVXCU_PITCH_LINEAR_IMAGE;
    NVXIO_ASSERT(input.base.format == NVXCU_DF_IMAGE_U8 ||
                  input.base.format == NVXCU_DF_IMAGE_RGB ||
                  input.base.format == NVXCU_DF_IMAGE_RGBX ||
                  input.base.format == NVXCU_DF_IMAGE_NV12);
    input.planes[0].dev_ptr = devMems[0];
    input.planes[0].pitch_in_bytes = devMemPitch;
    if (input.base.format == NVXCU_DF_IMAGE_NV12)
    {
      input.planes[1].dev_ptr = devMems[1];
      input.planes[1].pitch_in_bytes = devMemPitch;
    }

    output.base.format = image.format;
    output.base.width = image.width;
    output.base.height = image.height;
    output.base.image_type = NVXCU_PITCH_LINEAR_IMAGE;
    NVXIO_ASSERT(output.base.format == NVXCU_DF_IMAGE_U8 ||
                  output.base.format == NVXCU_DF_IMAGE_RGB ||
                  output.base.format == NVXCU_DF_IMAGE_RGBX ||
                  output.base.format == NVXCU_DF_IMAGE_NV12);
    output.planes[0].dev_ptr = image.planes[0].ptr;
    output.planes[0].pitch_in_bytes = image.planes[0].pitch_in_bytes;
    if (output.base.format == NVXCU_DF_IMAGE_NV12)
    {
      output.planes[1].dev_ptr = image.planes[1].ptr;
      output.planes[1].pitch_in_bytes = image.planes[1].pitch_in_bytes;
    }

    NVXCU_SAFE_CALL( nvxcuColorConvert(&input.base, &output.base, NVXCU_COLOR_SPACE_DEFAULT,
                                        NVXCU_CHANNEL_RANGE_FULL, &exec_target.base) );
  }
}

/**
  * Copies a buffer from CUDA memory to host memory
  *
  * @param image  : mapped image of vx_image
  * @param outmap : mapped output buffer
  */
void cuda_to_host_copy (const ovxio::image_t & image, GstMapInfo *outmap)
{
    NVXIO_ASSERT(image.format == NVXCU_DF_IMAGE_U8 ||
                 image.format == NVXCU_DF_IMAGE_RGB ||
                 image.format == NVXCU_DF_IMAGE_RGBX ||
                 image.format == NVXCU_DF_IMAGE_2F32);

    size_t pitch = image.width *
            (image.format == NVXCU_DF_IMAGE_U8 ? sizeof(uint8_t) :
             image.format == NVXCU_DF_IMAGE_RGB ? 3 * sizeof(uint8_t) :
             image.format == NVXCU_DF_IMAGE_RGBX ? 4 * sizeof(uint8_t) :
             image.format == NVXCU_DF_IMAGE_2F32 ? 2 * sizeof(float) : 0ul);
    size_t size = pitch * image.height;
    NVXIO_ASSERT(size > 0ul);

    cudaStream_t stream = nullptr;
    NVXIO_CUDA_SAFE_CALL( cudaMemcpy2DAsync(outmap->data, pitch,
                                            image.planes[0].ptr, image.planes[0].pitch_in_bytes,
                                            pitch, image.height,
                                            cudaMemcpyDeviceToHost, stream) );
    NVXIO_CUDA_SAFE_CALL( cudaStreamSynchronize(stream) );
}

/**
  * Transforms one incoming buffer to one outgoing buffer.
  *
  * @param inbuf  : input buffer
  * @param outbuf : output buffer
  */
static GstFlowReturn
gst_nvstabilize_transform (GstBaseTransform * btrans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  gint retn = 0;
  gboolean ret = TRUE;
  GstFlowReturn flow_ret = GST_FLOW_OK;

  Gstnvstabilize *space = NULL;

  GstMemory *inmem = NULL;
  GstMemory *outmem = NULL;
  GstNvFilterMemory *omem = NULL;

  GstMapInfo inmap = GST_MAP_INFO_INIT;
  GstMapInfo outmap = GST_MAP_INFO_INIT;

  gint input_dmabuf_fd = -1;
  NvBufferParams inbuf_params = {0};
  NvBufferCreateParams input_params = {0};

  gpointer data = NULL;

  /* Get metadata. Update rectangle and text params */

  space = GST_NVSTABILIZE (btrans);
  char context[100];
  sprintf(context,"gst_nvstabilize_transform()_ctx=%p",space);
  // nvtx_helper_push_pop(context);

  if (G_UNLIKELY (!space->negotiated))
    goto unknown_format;

  inmem = gst_buffer_peek_memory (inbuf, 0);
  if (!inmem)
    goto no_memory;

  outmem = gst_buffer_peek_memory (outbuf, 0);
  if (!outmem)
    goto no_memory;
  omem = (GstNvFilterMemory *) outmem;

  if (!gst_buffer_map (inbuf, &inmap, GST_MAP_READ))
    goto invalid_inbuf;

  if (!gst_buffer_map (outbuf, &outmap, GST_MAP_WRITE))
    goto invalid_outbuf;

  if (!gst_buffer_copy_into (outbuf, inbuf, GST_BUFFER_COPY_META, 0, -1)) {
    GST_DEBUG ("Buffer metadata copy failed \n");
  }

  data = gst_mini_object_get_qdata ((GstMiniObject *)inbuf, g_quark_from_static_string("NV_BUF"));

  if(data == (gpointer)NVBUF_MAGIC_NUM)
  {
    space->inbuf_memtype = BUF_MEM_HW;
  }

  // GstMapInfo info;
  // gboolean success = gst_buffer_map(inbuf, &info, (GstMapFlags)GST_MAP_READ);
  // if (!success)
  // {
  //     g_print("%s: unable to map buffer", __func__);
  //     goto done;
  // }
  void * decodedPtr = inmap.data;

  // gsize g = gst_buffer_extract(inbuf,
  //                     0,
  //                     outmap.data,
  //                     gst_buffer_get_size(outbuf));

  // {
  //   FILE *dump_gst = fopen("/home/autoro/gst-nvstabilize/gst.nv12", "wb");
  //   fwrite(output.data, gst_buffer_get_size(outbuf), sizeof(uint8_t), dump_gst);
  //   fclose(dump_gst);
  //   assert(1==2);
  // }
  if(!space->initilize) {

    space->frame_exemplar = vxCreateImage(space->context, space->from_width, space->from_height, VX_DF_IMAGE_RGBX);

    space->orig_frame_delay = vxCreateDelay(space->context, (vx_reference)space->frame_exemplar, space->orig_frame_delay_size);
    NVXIO_CHECK_REFERENCE(space->orig_frame_delay);
    NVXIO_SAFE_CALL( nvx::initDelayOfImages(space->context, space->orig_frame_delay) );
    NVXIO_SAFE_CALL(vxReleaseImage(&space->frame_exemplar));

    space->frame = (vx_image)vxGetReferenceFromDelay(space->orig_frame_delay, 0);
    space->lastFrame = (vx_image)vxGetReferenceFromDelay(space->orig_frame_delay, 1 - static_cast<vx_int32>(space->orig_frame_delay_size));
    
    space->params.numOfSmoothingFrames_ = space->queue_size;
    space->params.cropMargin_ = space->crop_margin;
    space->stabilizer = nvx::VideoStabilizer::createImageBasedVStab(space->context, space->params);
  }
  // GST_WARNING("queue size= %d, crop_margin=%f\n", space->queue_size, space->crop_margin);

  double t1, t2, t3, t4;
  t1 = millis_since_boot();

  // convert frame and copy it into cuda memory 
  convertFrame(space->exec_target,
            ovxio::image_t(space->frame, VX_WRITE_ONLY, NVX_MEMORY_TYPE_CUDA),
            space->configuration,
            space->from_width, space->from_height,
            false, 0,
            space->depth, decodedPtr,
            false,
            space->dev_mem,
            space->dev_mem_pitch);
  
  t2 = millis_since_boot();

  // init stabilizer using the very first frame
  if(!space->initilize) {
    space->stabilizer->init(space->frame);
    space->initilize = true;
  }

  // process the incomming frame and add it to vx delay
  space->stabilizer->process(space->frame);
  NVXIO_SAFE_CALL( vxAgeDelay(space->orig_frame_delay) );

  // space->stabilizer->printPerfs();

  t3 = millis_since_boot();
  // copy stabilized image from CUDA to host memory
  cuda_to_host_copy(ovxio::image_t(space->stabilizer->getStabilizedFrame(), VX_READ_ONLY, NVX_MEMORY_TYPE_CUDA), &outmap);
  t4 = millis_since_boot();

  GST_DEBUG("t1:%.2fms, t2:%.2fms, t3:%.2fms\n",t2-t1, t3-t2, t4-t3);

done:
  gst_buffer_unmap (inbuf, &inmap);
  gst_buffer_unmap (outbuf, &outmap);
  //nvtx_helper_push_pop(NULL);

  return flow_ret;

  /* ERRORS */
no_memory:
  {
    GST_ERROR ("no memory block");
    return GST_FLOW_ERROR;
  }
unknown_format:
  {
    GST_ERROR ("unknown format");
    return GST_FLOW_NOT_NEGOTIATED;
  }
invalid_inbuf:
  {
    GST_ERROR ("input buffer mapinfo failed");
    return GST_FLOW_ERROR;
  }
invalid_outbuf:
  {
    GST_ERROR ("output buffer mapinfo failed");
    gst_buffer_unmap (inbuf, &inmap);
    return GST_FLOW_ERROR;
  }
}

/**
  * nvstabilize plugin init.
  *
  * @param nvstabilize : plugin instance
  */
static gboolean
nvstabilize_init (GstPlugin * nvstabilize)
{
  GST_WARNING("");
  GST_DEBUG_CATEGORY_INIT (gst_nvstabilize_debug, "nvstabilize",
      0, "nvstabilize plugin");

  return gst_element_register (nvstabilize, "nvstabilize", GST_RANK_PRIMARY,
      GST_TYPE_NVSTABILIZE);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    nvstabilize,
    PACKAGE_DESCRIPTION,
    nvstabilize_init, VERSION, PACKAGE_LICENSE, PACKAGE_NAME, PACKAGE_URL)