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

#ifndef __GST_NVSTABILIZE_H__
#define __GST_NVSTABILIZE_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/base/gstbasetransform.h>

#include "nvbuf_utils.h"

#include <cuda_runtime_api.h>

#include <NVX/nvx.h>
#include <NVX/nvx_timer.hpp>

#include <NVX/Application.hpp>
#include <OVX/FrameSourceOVX.hpp>
#include <OVX/RenderOVX.hpp>
#include <NVX/SyncTimer.hpp>
#include <OVX/UtilityOVX.hpp>
#include <NVX/nvxcu.h>

#include <NVX/FrameSource.hpp>
#include "Private/Types.hpp"
#include "OVX/Private/TypesOVX.hpp"
#include "FrameSource/FrameSourceImpl.hpp"

#include "video_stabilizer/stabilizer.hpp" 


G_BEGIN_DECLS
#define GST_TYPE_NVSTABILIZE \
  (gst_nvstabilize_get_type())
#define GST_NVSTABILIZE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_NVSTABILIZE,Gstnvstabilize))
#define GST_NVSTABILIZE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_NVSTABILIZE,GstnvstabilizeClass))
#define GST_IS_NVSTABILIZE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_NVSTABILIZE))
#define GST_IS_NVSTABILIZE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_NVSTABILIZE))

/* Name of package */
#define PACKAGE "gstreamer-nvstabilize-plugin"
/* Define to the full name of this package. */
#define PACKAGE_NAME "GStreamer nvstabilize Plugin"
/* Define to the full name and version of this package. */
#define PACKAGE_STRING "GStreamer nvstabilize 0.0.1"
/* Information about the purpose of the plugin. */
#define PACKAGE_DESCRIPTION "video Stabilizer"
/* Define to the home page for this package. */
#define PACKAGE_URL "http://autoro.co/"
/* Define to the version of this package. */
#define PACKAGE_VERSION "0.0.1"
/* Define under which licence the package has been released */
#define PACKAGE_LICENSE "Proprietary"
/* Version number of package */
#define VERSION "0.0.1"

#define NVRM_MAX_SURFACES                 3
#define NVFILTER_MAX_BUF                  4
#define GST_CAPS_FEATURE_MEMORY_NVMM      "memory:NVMM"
#define GST_NVSTREAM_MEMORY_TYPE          "nvstream"

typedef struct _Gstnvstabilize Gstnvstabilize;
typedef struct _GstnvstabilizeClass GstnvstabilizeClass;

typedef struct _GstNvStabilizeBuffer GstNvStabilizeBuffer;
typedef struct _GstNvInterBuffer GstNvInterBuffer;

/**
 * BufType:
 *
 * Buffer type enum.
 */
typedef enum
{
  BUF_TYPE_YUV,
  BUF_TYPE_GRAY,
  BUF_TYPE_RGB,
  BUF_NOT_SUPPORTED
} BufType;

/**
 * BufMemType:
 *
 * Buffer memory type enum.
 */
typedef enum
{
  BUF_MEM_SW,
  BUF_MEM_HW
} BufMemType;

/**
 * GstInterpolationMethods:
 *
 * Interpolation methods type enum.
 */
typedef enum
{
  GST_INTERPOLATION_NEAREST,
  GST_INTERPOLATION_BILINEAR,
  GST_INTERPOLATION_5_TAP,
  GST_INTERPOLATION_10_TAP,
  GST_INTERPOLATION_SMART,
  GST_INTERPOLATION_NICEST,
} GstInterpolationMethods;

/**
 * GstNvStabilizeBuffer:
 *
 * Nvfilter buffer.
 */
struct _GstNvStabilizeBuffer
{
  gint dmabuf_fd;
  GstBuffer *gst_buf;
};

/**
 * GstNvInterBuffer:
 *
 * Intermediate transform buffer.
 */
struct _GstNvInterBuffer
{
  gint idmabuf_fd;
};

/**
 * Gstnvvconv:
 *
 * Opaque object data structure.
 */
struct _Gstnvstabilize
{
  GstBaseTransform element;

  /* source and sink pad caps */
  GstCaps *sinkcaps;
  GstCaps *srccaps;

  gint to_width;
  gint to_height;
  gint from_width;
  gint from_height;
  gint tsurf_width;
  gint tsurf_height;

  gint crop_left;
  gint crop_right;
  gint crop_top;
  gint crop_bottom;

  BufType inbuf_type;
  BufMemType inbuf_memtype;
  BufMemType outbuf_memtype;

  NvBufferTransformParams transform_params;
  NvBufferColorFormat in_pix_fmt;
  NvBufferColorFormat out_pix_fmt;

  guint insurf_count;
  guint tsurf_count;
  guint isurf_count;
  guint ibuf_count;
  guint num_output_buf;
  gint interpolation_method;

  gboolean silent;
  gboolean no_dimension;
  gboolean do_scaling;
  gboolean do_cropping;
  gboolean need_intersurf;
  gboolean isurf_flag;
  gboolean negotiated;
  gboolean nvfilterpool;
  gboolean enable_blocklinear_output;

  GstBufferPool *pool;
  GMutex flow_lock;

  GstNvInterBuffer interbuf;

  // stabilize stuff
  int32_t cuda_device_id;
  nvxcu_stream_exec_target_t exec_target;
  bool is_cuda;

  bool use_pitch;
  int pitch;
  int depth;

  // temporary CUDA buffer
  void * dev_mem;
  size_t dev_mem_pitch;

  vx_image image1;
  vx_context context;

  guint queue_size;
  gfloat crop_margin;
  
  nvx::VideoStabilizer::VideoStabilizerParams params;
  nvidiaio::FrameSource::Parameters configuration;
  nvx::VideoStabilizer *stabilizer;

  vx_image frame_exemplar;
  vx_size orig_frame_delay_size;
  vx_delay orig_frame_delay;

  vx_image frame, lastFrame;
  bool initilize;
};

struct _GstnvstabilizeClass
{
  GstBaseTransformClass parent_class;
};

GType gst_nvstabilize_get_type (void);

G_END_DECLS
#endif /* __GST_NVSTABILIZE_H__ */