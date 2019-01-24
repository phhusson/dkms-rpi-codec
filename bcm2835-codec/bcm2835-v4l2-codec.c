// SPDX-License-Identifier: GPL-2.0

/*
 * A v4l2-mem2mem device that wraps the video codec MMAL component.
 *
 * Copyright 2018 Raspberry Pi (Trading) Ltd.
 * Author: Dave Stevenson (dave.stevenson@raspberrypi.org)
 *
 * Loosely based on the vim2m virtual driver by Pawel Osciak
 * Copyright (c) 2009-2010 Samsung Electronics Co., Ltd.
 * Pawel Osciak, <pawel@osciak.com>
 * Marek Szyprowski, <m.szyprowski@samsung.com>
 *
 * Whilst this driver uses the v4l2_mem2mem framework, it does not need the
 * scheduling aspects, so will always take the buffers, pass them to the VPU,
 * and then signal the job as complete.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/syscalls.h>

#include <media/v4l2-mem2mem.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-dma-contig.h>

#include "vchiq-mmal/mmal-encodings.h"
#include "vchiq-mmal/mmal-msg.h"
#include "vchiq-mmal/mmal-parameters.h"
#include "vchiq-mmal/mmal-vchiq.h"

/*
 * Default /dev/videoN node numbers for decode and encode.
 * Deliberately avoid the very low numbers as these are often taken by webcams
 * etc, and simple apps tend to only go for /dev/video0.
 */
static int decode_video_nr = 10;
module_param(decode_video_nr, int, 0644);
MODULE_PARM_DESC(decode_video_nr, "decoder video device number");

static int encode_video_nr = 11;
module_param(encode_video_nr, int, 0644);
MODULE_PARM_DESC(encode_video_nr, "encoder video device number");

static unsigned int debug;
module_param(debug, uint, 0644);
MODULE_PARM_DESC(debug, "activates debug info (0-3)");

#define MIN_W		32
#define MIN_H		32
#define MAX_W		1920
#define MAX_H		1088
#define BPL_ALIGN	32
#define DEFAULT_WIDTH	640
#define DEFAULT_HEIGHT	480
/*
 * The unanswered question - what is the maximum size of a compressed frame?
 * V4L2 mandates that the encoded frame must fit in a single buffer. Sizing
 * that buffer is a compromise between wasting memory and risking not fitting.
 * The 1080P version of Big Buck Bunny has some frames that exceed 512kB.
 * Adopt a moderately arbitrary split at 720P for switching between 512 and
 * 768kB buffers.
 */
#define DEF_COMP_BUF_SIZE_GREATER_720P	(768 << 10)
#define DEF_COMP_BUF_SIZE_720P_OR_LESS	(512 << 10)

/* Flags that indicate a format can be used for capture/output */
#define MEM2MEM_CAPTURE		BIT(0)
#define MEM2MEM_OUTPUT		BIT(1)

#define MEM2MEM_NAME		"bcm2835-codec"

struct bcm2835_codec_fmt {
	u32	fourcc;
	int	depth;
	int	bytesperline_align;
	u32	flags;
	u32	mmal_fmt;
	bool	decode_only;
	bool	encode_only;
	int	size_multiplier_x2;
};

/* Supported raw pixel formats. Those supported for both encode and decode
 * must come first, with those only supported for decode coming after (there
 * are no formats supported for encode only).
 */
static struct bcm2835_codec_fmt raw_formats[] = {
	{
		.fourcc			= V4L2_PIX_FMT_YUV420,
		.depth			= 8,
		.bytesperline_align	= 32,
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_I420,
		.size_multiplier_x2	= 3,
	}, {
		.fourcc			= V4L2_PIX_FMT_YVU420,
		.depth			= 8,
		.bytesperline_align	= 32,
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_YV12,
		.size_multiplier_x2	= 3,
	}, {
		.fourcc			= V4L2_PIX_FMT_NV12,
		.depth			= 8,
		.bytesperline_align	= 32,
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_NV12,
		.size_multiplier_x2	= 3,
	}, {
		.fourcc			= V4L2_PIX_FMT_NV21,
		.depth			= 8,
		.bytesperline_align	= 32,
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_NV21,
		.size_multiplier_x2	= 3,
	}, {
		.fourcc			= V4L2_PIX_FMT_RGB565,
		.depth			= 16,
		.bytesperline_align	= 32,
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_RGB16,
		.size_multiplier_x2	= 2,
	}, {
		.fourcc			= V4L2_PIX_FMT_YUYV,
		.depth			= 16,
		.bytesperline_align	= 32,
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_YUYV,
		.encode_only		= true,
		.size_multiplier_x2	= 2,
	}, {
		.fourcc			= V4L2_PIX_FMT_UYVY,
		.depth			= 16,
		.bytesperline_align	= 32,
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_UYVY,
		.encode_only		= true,
		.size_multiplier_x2	= 2,
	}, {
		.fourcc			= V4L2_PIX_FMT_YVYU,
		.depth			= 16,
		.bytesperline_align	= 32,
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_YVYU,
		.encode_only		= true,
		.size_multiplier_x2	= 2,
	}, {
		.fourcc			= V4L2_PIX_FMT_VYUY,
		.depth			= 16,
		.bytesperline_align	= 32,
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_VYUY,
		.encode_only		= true,
		.size_multiplier_x2	= 2,
	}, {
		.fourcc			= V4L2_PIX_FMT_RGB24,
		.depth			= 24,
		.bytesperline_align	= 32,
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_RGB24,
		.encode_only		= true,
		.size_multiplier_x2	= 2,
	}, {
		.fourcc			= V4L2_PIX_FMT_BGR24,
		.depth			= 24,
		.bytesperline_align	= 32,
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BGR24,
		.encode_only		= true,
		.size_multiplier_x2	= 2,
	}, {
		.fourcc			= V4L2_PIX_FMT_BGR32,
		.depth			= 32,
		.bytesperline_align	= 32,
		.flags			= 0,
		.mmal_fmt		= MMAL_ENCODING_BGRA,
		.encode_only		= true,
		.size_multiplier_x2	= 2,
	},
};

/* Supported encoded formats. Those supported for both encode and decode
 * must come first, with those only supported for decode coming after (there
 * are no formats supported for encode only).
 */
static struct bcm2835_codec_fmt encoded_formats[] = {
	{
		.fourcc			= V4L2_PIX_FMT_H264,
		.depth			= 0,
		.flags			= V4L2_FMT_FLAG_COMPRESSED,
		.mmal_fmt		= MMAL_ENCODING_H264,
	}, {
		.fourcc			= V4L2_PIX_FMT_MJPEG,
		.depth			= 0,
		.flags			= V4L2_FMT_FLAG_COMPRESSED,
		.mmal_fmt		= MMAL_ENCODING_MJPEG,
	}, {
		.fourcc			= V4L2_PIX_FMT_MPEG4,
		.depth			= 0,
		.flags			= V4L2_FMT_FLAG_COMPRESSED,
		.mmal_fmt		= MMAL_ENCODING_MP4V,
		.decode_only		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_H263,
		.depth			= 0,
		.flags			= V4L2_FMT_FLAG_COMPRESSED,
		.mmal_fmt		= MMAL_ENCODING_H263,
		.decode_only		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_MPEG2,
		.depth			= 0,
		.flags			= V4L2_FMT_FLAG_COMPRESSED,
		.mmal_fmt		= MMAL_ENCODING_MP2V,
		.decode_only		= true,
	}, {
		.fourcc			= V4L2_PIX_FMT_VP8,
		.depth			= 0,
		.flags			= V4L2_FMT_FLAG_COMPRESSED,
		.mmal_fmt		= MMAL_ENCODING_VP8,
		.decode_only		= true,
	},
	/*
	 * This list couold include VP6 and Theorafor decode, but V4L2 doesn't
	 * support them.
	 */
};

struct bcm2835_codec_fmt_list {
	struct bcm2835_codec_fmt *list;
	unsigned int num_entries;
};

#define RAW_LIST	0
#define ENCODED_LIST	1

struct bcm2835_codec_fmt_list formats[] = {
	{
		.list = raw_formats,
		.num_entries = ARRAY_SIZE(raw_formats),
	}, {
		.list = encoded_formats,
		.num_entries = ARRAY_SIZE(encoded_formats),
	},
};

struct m2m_mmal_buffer {
	struct v4l2_m2m_buffer	m2m;
	struct mmal_buffer	mmal;
};

/* Per-queue, driver-specific private data */
struct bcm2835_codec_q_data {
	/*
	 * These parameters should be treated as gospel, with everything else
	 * being determined from them.
	 */
	/* Buffer width/height */
	unsigned int		bytesperline;
	unsigned int		height;
	/* Crop size used for selection handling */
	unsigned int		crop_width;
	unsigned int		crop_height;
	bool			selection_set;

	unsigned int		sizeimage;
	unsigned int		sequence;
	struct bcm2835_codec_fmt	*fmt;

	/* One extra buffer header so we can send an EOS. */
	struct m2m_mmal_buffer	eos_buffer;
	bool			eos_buffer_in_use;	/* debug only */
};

enum {
	V4L2_M2M_SRC = 0,
	V4L2_M2M_DST = 1,
};

static inline struct bcm2835_codec_fmt_list *get_format_list(bool decode,
							     bool capture)
{
	return decode ^ capture ? &formats[ENCODED_LIST] : &formats[RAW_LIST];
}

static struct bcm2835_codec_fmt *get_default_format(bool decode, bool capture)
{
	return &get_format_list(decode, capture)->list[0];
}

static struct bcm2835_codec_fmt *find_format(struct v4l2_format *f, bool decode,
					     bool capture)
{
	struct bcm2835_codec_fmt *fmt;
	unsigned int k;
	struct bcm2835_codec_fmt_list *fmts = get_format_list(decode, capture);

	for (k = 0; k < fmts->num_entries; k++) {
		fmt = &fmts->list[k];
		if (fmt->fourcc == f->fmt.pix.pixelformat)
			break;
	}

	/*
	 * Some compressed formats are only supported for decoding, not
	 * encoding.
	 */
	if (!decode && fmts->list[k].decode_only)
		return NULL;

	/* Some pixel formats are only supported for encoding, not decoding. */
	if (decode && fmts->list[k].encode_only)
		return NULL;

	if (k == fmts->num_entries)
		return NULL;

	return &fmts->list[k];
}

struct bcm2835_codec_dev {
	struct platform_device *pdev;

	/* v4l2 devices */
	struct v4l2_device	v4l2_dev;
	struct video_device	vfd;
	/* mutex for the v4l2 device */
	struct mutex		dev_mutex;
	atomic_t		num_inst;

	/* allocated mmal instance and components */
	bool			decode;	 /* Is this instance a decoder? */
	struct vchiq_mmal_instance	*instance;

	struct v4l2_m2m_dev	*m2m_dev;
};

struct bcm2835_codec_ctx {
	struct v4l2_fh		fh;
	struct bcm2835_codec_dev	*dev;

	struct v4l2_ctrl_handler hdl;

	struct vchiq_mmal_component  *component;
	bool component_enabled;

	enum v4l2_colorspace	colorspace;
	enum v4l2_ycbcr_encoding ycbcr_enc;
	enum v4l2_xfer_func	xfer_func;
	enum v4l2_quantization	quant;

	/* Source and destination queue data */
	struct bcm2835_codec_q_data   q_data[2];
	s32  bitrate;

	bool aborting;
	int num_ip_buffers;
	int num_op_buffers;
	struct completion frame_cmplt;
};

struct bcm2835_codec_driver {
	struct bcm2835_codec_dev *encode;
	struct bcm2835_codec_dev *decode;
};

static inline struct bcm2835_codec_ctx *file2ctx(struct file *file)
{
	return container_of(file->private_data, struct bcm2835_codec_ctx, fh);
}

static struct bcm2835_codec_q_data *get_q_data(struct bcm2835_codec_ctx *ctx,
					       enum v4l2_buf_type type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		return &ctx->q_data[V4L2_M2M_SRC];
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		return &ctx->q_data[V4L2_M2M_DST];
	default:
		v4l2_err(&ctx->dev->v4l2_dev, "%s: Invalid queue type %u\n",
			 __func__, type);
		break;
	}
	return NULL;
}

static struct vchiq_mmal_port *get_port_data(struct bcm2835_codec_ctx *ctx,
					     enum v4l2_buf_type type)
{
	if (!ctx->component)
		return NULL;

	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		return &ctx->component->input[0];
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		return &ctx->component->output[0];
	default:
		v4l2_err(&ctx->dev->v4l2_dev, "%s: Invalid queue type %u\n",
			 __func__, type);
		break;
	}
	return NULL;
}

/*
 * mem2mem callbacks
 */

/**
 * job_ready() - check whether an instance is ready to be scheduled to run
 */
static int job_ready(void *priv)
{
	struct bcm2835_codec_ctx *ctx = priv;

	if (!v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx) &&
	    !v4l2_m2m_num_dst_bufs_ready(ctx->fh.m2m_ctx))
		return 0;

	return 1;
}

static void job_abort(void *priv)
{
	struct bcm2835_codec_ctx *ctx = priv;

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s\n", __func__);
	/* Will cancel the transaction in the next interrupt handler */
	ctx->aborting = 1;
}

static inline unsigned int get_sizeimage(int bpl, int height,
					 struct bcm2835_codec_fmt *fmt)
{
	return (bpl * height * fmt->size_multiplier_x2) >> 1;
}

static inline unsigned int get_bytesperline(int width,
					    struct bcm2835_codec_fmt *fmt)
{
	return ALIGN((width * fmt->depth) >> 3, fmt->bytesperline_align);
}

static void setup_mmal_port_format(struct bcm2835_codec_ctx *ctx,
				   bool decode,
				   struct bcm2835_codec_q_data *q_data,
				   struct vchiq_mmal_port *port)
{
	port->format.encoding = q_data->fmt->mmal_fmt;

	if (!(q_data->fmt->flags & V4L2_FMT_FLAG_COMPRESSED)) {
		/* Raw image format - set width/height */
		port->es.video.width = q_data->bytesperline /
						(q_data->fmt->depth >> 3);
		port->es.video.height = q_data->height;
		port->es.video.crop.width = q_data->crop_width;
		port->es.video.crop.height = q_data->crop_height;
		port->es.video.frame_rate.num = 0;
		port->es.video.frame_rate.den = 1;
	} else {
		/* Compressed format - leave resolution as 0 for decode */
		if (decode) {
			port->es.video.width = 0;
			port->es.video.height = 0;
			port->es.video.crop.width = 0;
			port->es.video.crop.height = 0;
		} else {
			port->es.video.width = q_data->crop_width;
			port->es.video.height = q_data->height;
			port->es.video.crop.width = q_data->crop_width;
			port->es.video.crop.height = q_data->crop_height;
			port->format.bitrate = ctx->bitrate;
		}
		port->es.video.frame_rate.num = 0;
		port->es.video.frame_rate.den = 1;
	}
	port->es.video.crop.x = 0;
	port->es.video.crop.y = 0;

	port->current_buffer.size = q_data->sizeimage;
};

static void ip_buffer_cb(struct vchiq_mmal_instance *instance,
			 struct vchiq_mmal_port *port, int status,
			 struct mmal_buffer *mmal_buf)
{
	struct bcm2835_codec_ctx *ctx = port->cb_ctx/*, *curr_ctx*/;
	struct m2m_mmal_buffer *buf =
			container_of(mmal_buf, struct m2m_mmal_buffer, mmal);

	v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s: port %p buf %p length %lu, flags %x\n",
		 __func__, port, mmal_buf, mmal_buf->length,
		 mmal_buf->mmal_flags);

	if (buf == &ctx->q_data[V4L2_M2M_SRC].eos_buffer) {
		/* Do we need to add lcoking to prevent multiple submission of
		 * the EOS, and therefore handle mutliple return here?
		 */
		v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: eos buffer returned.\n",
			 __func__);
		ctx->q_data[V4L2_M2M_SRC].eos_buffer_in_use = false;
		return;
	}

	if (status) {
		/* error in transfer */
		if (buf)
			/* there was a buffer with the error so return it */
			vb2_buffer_done(&buf->m2m.vb.vb2_buf,
					VB2_BUF_STATE_ERROR);
		return;
	}
	if (mmal_buf->cmd) {
		v4l2_err(&ctx->dev->v4l2_dev, "%s: Not expecting cmd msgs on ip callback - %08x\n",
			 __func__, mmal_buf->cmd);
		/*
		 * CHECKME: Should we return here. The buffer shouldn't have a
		 * message context or vb2 buf associated.
		 */
	}

	v4l2_dbg(3, debug, &ctx->dev->v4l2_dev, "%s: no error. Return buffer %p\n",
		 __func__, &buf->m2m.vb.vb2_buf);
	vb2_buffer_done(&buf->m2m.vb.vb2_buf, VB2_BUF_STATE_DONE);

	ctx->num_ip_buffers++;
	v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s: done %d input buffers\n",
		 __func__, ctx->num_ip_buffers);

	if (!port->enabled)
		complete(&ctx->frame_cmplt);
}

static void queue_res_chg_event(struct bcm2835_codec_ctx *ctx)
{
	static const struct v4l2_event ev_src_ch = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes =
		V4L2_EVENT_SRC_CH_RESOLUTION,
	};

	v4l2_event_queue_fh(&ctx->fh, &ev_src_ch);
}

static void send_eos_event(struct bcm2835_codec_ctx *ctx)
{
	static const struct v4l2_event ev = {
		.type = V4L2_EVENT_EOS,
	};

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "Sending EOS event\n");

	v4l2_event_queue_fh(&ctx->fh, &ev);
}

static void color_mmal2v4l(struct bcm2835_codec_ctx *ctx, u32 mmal_color_space)
{
	switch (mmal_color_space) {
	case MMAL_COLOR_SPACE_ITUR_BT601:
		ctx->colorspace = V4L2_COLORSPACE_REC709;
		ctx->xfer_func = V4L2_XFER_FUNC_709;
		ctx->ycbcr_enc = V4L2_YCBCR_ENC_601;
		ctx->quant = V4L2_QUANTIZATION_LIM_RANGE;
		break;

	case MMAL_COLOR_SPACE_ITUR_BT709:
		ctx->colorspace = V4L2_COLORSPACE_REC709;
		ctx->xfer_func = V4L2_XFER_FUNC_709;
		ctx->ycbcr_enc = V4L2_YCBCR_ENC_709;
		ctx->quant = V4L2_QUANTIZATION_LIM_RANGE;
		break;
	}
}

static void handle_fmt_changed(struct bcm2835_codec_ctx *ctx,
			       struct mmal_buffer *mmal_buf)
{
	struct bcm2835_codec_q_data *q_data;
	struct mmal_msg_event_format_changed *format =
		(struct mmal_msg_event_format_changed *)mmal_buf->buffer;
	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: Format changed: buff size min %u, rec %u, buff num min %u, rec %u\n",
		 __func__,
		 format->buffer_size_min,
		 format->buffer_size_recommended,
		 format->buffer_num_min,
		 format->buffer_num_recommended
		);
	if (format->format.type != MMAL_ES_TYPE_VIDEO) {
		v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: Format changed but not video %u\n",
			 __func__, format->format.type);
		return;
	}
	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: Format changed to %ux%u, crop %ux%u, colourspace %08X\n",
		 __func__, format->es.video.width, format->es.video.height,
		 format->es.video.crop.width, format->es.video.crop.height,
		 format->es.video.color_space);

	q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	q_data->crop_width = format->es.video.crop.width;
	q_data->crop_height = format->es.video.crop.height;
	q_data->bytesperline = format->es.video.crop.width;
	q_data->height = format->es.video.height;
	q_data->sizeimage = format->buffer_size_min;
	if (format->es.video.color_space)
		color_mmal2v4l(ctx, format->es.video.color_space);

	queue_res_chg_event(ctx);
}

static void op_buffer_cb(struct vchiq_mmal_instance *instance,
			 struct vchiq_mmal_port *port, int status,
			 struct mmal_buffer *mmal_buf)
{
	struct bcm2835_codec_ctx *ctx = port->cb_ctx;
	struct m2m_mmal_buffer *buf;
	struct vb2_v4l2_buffer *vb2;

	v4l2_dbg(2, debug, &ctx->dev->v4l2_dev,
		 "%s: status:%d, buf:%p, length:%lu, flags %u, pts %lld\n",
		 __func__, status, mmal_buf, mmal_buf->length,
		 mmal_buf->mmal_flags, mmal_buf->pts);

	buf = container_of(mmal_buf, struct m2m_mmal_buffer, mmal);
	vb2 = &buf->m2m.vb;

	if (status) {
		/* error in transfer */
		if (vb2) {
			/* there was a buffer with the error so return it */
			vb2_buffer_done(&vb2->vb2_buf, VB2_BUF_STATE_ERROR);
		}
		return;
	}

	if (mmal_buf->cmd) {
		switch (mmal_buf->cmd) {
		case MMAL_EVENT_FORMAT_CHANGED:
		{
			handle_fmt_changed(ctx, mmal_buf);
			break;
		}
		default:
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Unexpected event on output callback - %08x\n",
				 __func__, mmal_buf->cmd);
			break;
		}
		return;
	}

	v4l2_dbg(3, debug, &ctx->dev->v4l2_dev, "%s: length %lu, flags %x, idx %u\n",
		 __func__, mmal_buf->length, mmal_buf->mmal_flags,
		 vb2->vb2_buf.index);

	if (mmal_buf->length == 0) {
		/* stream ended, or buffer being returned during disable. */
		v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s: Empty buffer - flags %04x",
			 __func__, mmal_buf->mmal_flags);
		if (!mmal_buf->mmal_flags & MMAL_BUFFER_HEADER_FLAG_EOS) {
			vb2_buffer_done(&vb2->vb2_buf, VB2_BUF_STATE_ERROR);
			if (!port->enabled)
				complete(&ctx->frame_cmplt);
			return;
		}
	}
	if (mmal_buf->mmal_flags & MMAL_BUFFER_HEADER_FLAG_EOS) {
		/* EOS packet from the VPU */
		send_eos_event(ctx);
		vb2->flags |= V4L2_BUF_FLAG_LAST;
	}

	vb2->vb2_buf.timestamp = mmal_buf->pts;

	vb2_set_plane_payload(&vb2->vb2_buf, 0, mmal_buf->length);
	if (mmal_buf->mmal_flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME)
		vb2->flags |= V4L2_BUF_FLAG_KEYFRAME;

	vb2_buffer_done(&vb2->vb2_buf, VB2_BUF_STATE_DONE);
	ctx->num_op_buffers++;

	v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s: done %d output buffers\n",
		 __func__, ctx->num_op_buffers);

	if (!port->enabled)
		complete(&ctx->frame_cmplt);
}

/* vb2_to_mmal_buffer() - converts vb2 buffer header to MMAL
 *
 * Copies all the required fields from a VB2 buffer to the MMAL buffer header,
 * ready for sending to the VPU.
 */
static void vb2_to_mmal_buffer(struct m2m_mmal_buffer *buf,
			       struct vb2_v4l2_buffer *vb2)
{
	buf->mmal.mmal_flags = 0;
	if (vb2->flags & V4L2_BUF_FLAG_KEYFRAME)
		buf->mmal.mmal_flags |= MMAL_BUFFER_HEADER_FLAG_KEYFRAME;

	/*
	 * Adding this means that the data must be framed correctly as one frame
	 * per buffer. The underlying decoder has no such requirement, but it
	 * will reduce latency as the bistream parser will be kicked immediately
	 * to parse the frame, rather than relying on its own heuristics for
	 * when to wake up.
	 */
	buf->mmal.mmal_flags |= MMAL_BUFFER_HEADER_FLAG_FRAME_END;

	buf->mmal.length = vb2->vb2_buf.planes[0].bytesused;
	/*
	 * Minor ambiguity in the V4L2 spec as to whether passing in a 0 length
	 * buffer, or one with V4L2_BUF_FLAG_LAST set denotes end of stream.
	 * Handle either.
	 */
	if (!buf->mmal.length || vb2->flags & V4L2_BUF_FLAG_LAST)
		buf->mmal.mmal_flags |= MMAL_BUFFER_HEADER_FLAG_EOS;

	buf->mmal.pts = vb2->vb2_buf.timestamp;
	buf->mmal.dts = MMAL_TIME_UNKNOWN;
}

/* device_run() - prepares and starts the device
 *
 * This simulates all the immediate preparations required before starting
 * a device. This will be called by the framework when it decides to schedule
 * a particular instance.
 */
static void device_run(void *priv)
{
	struct bcm2835_codec_ctx *ctx = priv;
	struct bcm2835_codec_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct m2m_mmal_buffer *src_m2m_buf, *dst_m2m_buf;
	struct v4l2_m2m_buffer *m2m;
	int ret;

	v4l2_dbg(3, debug, &ctx->dev->v4l2_dev, "%s: off we go\n", __func__);

	src_buf = v4l2_m2m_buf_remove(&ctx->fh.m2m_ctx->out_q_ctx);
	if (src_buf) {
		m2m = container_of(src_buf, struct v4l2_m2m_buffer, vb);
		src_m2m_buf = container_of(m2m, struct m2m_mmal_buffer, m2m);
		vb2_to_mmal_buffer(src_m2m_buf, src_buf);

		ret = vchiq_mmal_submit_buffer(dev->instance,
					       &ctx->component->input[0],
					       &src_m2m_buf->mmal);
		v4l2_dbg(3, debug, &ctx->dev->v4l2_dev, "%s: Submitted ip buffer len %lu, pts %llu, flags %04x\n",
			 __func__, src_m2m_buf->mmal.length,
			 src_m2m_buf->mmal.pts, src_m2m_buf->mmal.mmal_flags);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed submitting ip buffer\n",
				 __func__);
	}

	dst_buf = v4l2_m2m_buf_remove(&ctx->fh.m2m_ctx->cap_q_ctx);
	if (dst_buf) {
		m2m = container_of(dst_buf, struct v4l2_m2m_buffer, vb);
		dst_m2m_buf = container_of(m2m, struct m2m_mmal_buffer, m2m);
		vb2_to_mmal_buffer(dst_m2m_buf, dst_buf);

		ret = vchiq_mmal_submit_buffer(dev->instance,
					       &ctx->component->output[0],
					       &dst_m2m_buf->mmal);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed submitting op buffer\n",
				 __func__);
	}

	v4l2_dbg(3, debug, &ctx->dev->v4l2_dev, "%s: Submitted src %p, dst %p\n",
		 __func__, src_m2m_buf, dst_m2m_buf);

	/* Complete the job here. */
	v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx);
}

/*
 * video ioctls
 */
static int vidioc_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	strncpy(cap->driver, MEM2MEM_NAME, sizeof(cap->driver) - 1);
	strncpy(cap->card, MEM2MEM_NAME, sizeof(cap->card) - 1);
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 MEM2MEM_NAME);
	cap->device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int enum_fmt(struct v4l2_fmtdesc *f, bool decode, bool capture)
{
	struct bcm2835_codec_fmt *fmt;
	struct bcm2835_codec_fmt_list *fmts = get_format_list(decode, capture);

	if (f->index < fmts->num_entries) {
		/* Format found */
		/* Check format isn't a decode only format when encoding */
		if (!decode &&
		    fmts->list[f->index].decode_only)
			return -EINVAL;
		/* Check format isn't a decode only format when encoding */
		if (decode &&
		    fmts->list[f->index].encode_only)
			return -EINVAL;

		fmt = &fmts->list[f->index];
		f->pixelformat = fmt->fourcc;
		f->flags = fmt->flags;
		return 0;
	}

	/* Format not found */
	return -EINVAL;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);

	return enum_fmt(f, ctx->dev->decode, true);
}

static int vidioc_enum_fmt_vid_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);

	return enum_fmt(f, ctx->dev->decode, false);
}

static int vidioc_g_fmt(struct bcm2835_codec_ctx *ctx, struct v4l2_format *f)
{
	struct vb2_queue *vq;
	struct bcm2835_codec_q_data *q_data;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;

	q_data = get_q_data(ctx, f->type);

	f->fmt.pix.width	= q_data->crop_width;
	f->fmt.pix.height	= q_data->height;
	f->fmt.pix.field	= V4L2_FIELD_NONE;
	f->fmt.pix.pixelformat	= q_data->fmt->fourcc;
	f->fmt.pix.bytesperline	= q_data->bytesperline;
	f->fmt.pix.sizeimage	= q_data->sizeimage;
	f->fmt.pix.colorspace	= ctx->colorspace;
	f->fmt.pix.xfer_func	= ctx->xfer_func;
	f->fmt.pix.ycbcr_enc	= ctx->ycbcr_enc;
	f->fmt.pix.quantization	= ctx->quant;

	return 0;
}

static int vidioc_g_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	return vidioc_g_fmt(file2ctx(file), f);
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	return vidioc_g_fmt(file2ctx(file), f);
}

static int vidioc_try_fmt(struct v4l2_format *f, struct bcm2835_codec_fmt *fmt)
{
	/*
	 * The V4L2 specification requires the driver to correct the format
	 * struct if any of the dimensions is unsupported
	 */
	if (f->fmt.pix.width > MAX_W)
		f->fmt.pix.width = MAX_W;
	if (f->fmt.pix.height > MAX_H)
		f->fmt.pix.height = MAX_H;

	if (!fmt->flags & V4L2_FMT_FLAG_COMPRESSED) {
		/* Only clip min w/h on capture. Treat 0x0 as unknown. */
		if (f->fmt.pix.width < MIN_W)
			f->fmt.pix.width = MIN_W;
		if (f->fmt.pix.height < MIN_H)
			f->fmt.pix.height = MIN_H;

		/*
		 * Buffer must have a vertical alignment of 16 lines.
		 * The selection will reflect any cropping rectangle when only
		 * some of the pixels are active.
		 */
		f->fmt.pix.height = ALIGN(f->fmt.pix.height, 16);

		f->fmt.pix.bytesperline = get_bytesperline(f->fmt.pix.width,
							   fmt);
		f->fmt.pix.sizeimage = get_sizeimage(f->fmt.pix.bytesperline,
						     f->fmt.pix.height,
						     fmt);
	} else {
		u32 min_size = f->fmt.pix.width > 1280 ||
			       f->fmt.pix.height > 720 ?
			       DEF_COMP_BUF_SIZE_GREATER_720P :
			       DEF_COMP_BUF_SIZE_720P_OR_LESS;

		f->fmt.pix.bytesperline = 0;
		if (f->fmt.pix.sizeimage < min_size)
			f->fmt.pix.sizeimage = min_size;
	}

	f->fmt.pix.field = V4L2_FIELD_NONE;

	return 0;
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct bcm2835_codec_fmt *fmt;
	struct bcm2835_codec_ctx *ctx = file2ctx(file);

	fmt = find_format(f, ctx->dev->decode, true);
	if (!fmt) {
		f->fmt.pix.pixelformat = get_default_format(ctx->dev->decode,
							    true)->fourcc;
		fmt = find_format(f, ctx->dev->decode, true);
	}

	return vidioc_try_fmt(f, fmt);
}

static int vidioc_try_fmt_vid_out(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct bcm2835_codec_fmt *fmt;
	struct bcm2835_codec_ctx *ctx = file2ctx(file);

	fmt = find_format(f, ctx->dev->decode, false);
	if (!fmt) {
		f->fmt.pix.pixelformat = get_default_format(ctx->dev->decode,
							    false)->fourcc;
		fmt = find_format(f, ctx->dev->decode, false);
	}

	if (!f->fmt.pix.colorspace)
		f->fmt.pix.colorspace = ctx->colorspace;

	return vidioc_try_fmt(f, fmt);
}

static int vidioc_s_fmt(struct bcm2835_codec_ctx *ctx, struct v4l2_format *f,
			unsigned int requested_height)
{
	struct bcm2835_codec_q_data *q_data;
	struct vb2_queue *vq;
	struct vchiq_mmal_port *port;
	bool update_capture_port = false;
	int ret;

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev,	"Setting format for type %d, wxh: %dx%d, fmt: %08x, size %u\n",
		 f->type, f->fmt.pix.width, f->fmt.pix.height,
		 f->fmt.pix.pixelformat, f->fmt.pix.sizeimage);

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;

	q_data = get_q_data(ctx, f->type);
	if (!q_data)
		return -EINVAL;

	if (vb2_is_busy(vq)) {
		v4l2_err(&ctx->dev->v4l2_dev, "%s queue busy\n", __func__);
		return -EBUSY;
	}

	q_data->fmt = find_format(f, ctx->dev->decode,
				  f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE);
	q_data->crop_width = f->fmt.pix.width;
	q_data->height = f->fmt.pix.height;
	if (!q_data->selection_set)
		q_data->crop_height = requested_height;

	/*
	 * Copying the behaviour of vicodec which retains a single set of
	 * colorspace parameters for both input and output.
	 */
	ctx->colorspace = f->fmt.pix.colorspace;
	ctx->xfer_func = f->fmt.pix.xfer_func;
	ctx->ycbcr_enc = f->fmt.pix.ycbcr_enc;
	ctx->quant = f->fmt.pix.quantization;

	/* All parameters should have been set correctly by try_fmt */
	q_data->bytesperline = f->fmt.pix.bytesperline;
	q_data->sizeimage = f->fmt.pix.sizeimage;

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev,	"Calulated bpl as %u, size %u\n",
		 q_data->bytesperline, q_data->sizeimage);

	if (ctx->dev->decode && q_data->fmt->flags & V4L2_FMT_FLAG_COMPRESSED &&
	    f->fmt.pix.width && f->fmt.pix.height) {
		/*
		 * On the decoder, if provided with a resolution on the input
		 * side, then replicate that to the output side.
		 * GStreamer appears not to support V4L2_EVENT_SOURCE_CHANGE,
		 * nor set up a resolution on the output side, therefore
		 * we can't decode anything at a resolution other than the
		 * default one.
		 */
		struct bcm2835_codec_q_data *q_data_dst =
						&ctx->q_data[V4L2_M2M_DST];

		q_data_dst->crop_width = q_data->crop_width;
		q_data_dst->crop_height = q_data->crop_height;
		q_data_dst->height = ALIGN(q_data->crop_height, 16);

		q_data_dst->bytesperline =
			get_bytesperline(f->fmt.pix.width, q_data_dst->fmt);
		q_data_dst->sizeimage = get_sizeimage(q_data_dst->bytesperline,
						      q_data_dst->height,
						      q_data_dst->fmt);
		update_capture_port = true;
	}

	/* If we have a component then setup the port as well */
	port = get_port_data(ctx, vq->type);
	if (!port)
		return 0;

	setup_mmal_port_format(ctx, ctx->dev->decode, q_data, port);
	ret = vchiq_mmal_port_set_format(ctx->dev->instance, port);
	if (ret) {
		v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed vchiq_mmal_port_set_format on port, ret %d\n",
			 __func__, ret);
		ret = -EINVAL;
	}

	if (q_data->sizeimage < port->minimum_buffer.size) {
		v4l2_err(&ctx->dev->v4l2_dev, "%s: Current buffer size of %u < min buf size %u - driver mismatch to MMAL\n",
			 __func__, q_data->sizeimage,
			 port->minimum_buffer.size);
	}

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev,	"Set format for type %d, wxh: %dx%d, fmt: %08x, size %u\n",
		 f->type, q_data->crop_width, q_data->height,
		 q_data->fmt->fourcc, q_data->sizeimage);

	if (update_capture_port) {
		struct vchiq_mmal_port *port_dst = &ctx->component->output[0];
		struct bcm2835_codec_q_data *q_data_dst =
						&ctx->q_data[V4L2_M2M_DST];

		setup_mmal_port_format(ctx, ctx->dev->decode, q_data_dst,
				       port_dst);
		ret = vchiq_mmal_port_set_format(ctx->dev->instance, port_dst);
		if (ret) {
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed vchiq_mmal_port_set_format on output port, ret %d\n",
				 __func__, ret);
			ret = -EINVAL;
		}
	}
	return ret;
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	unsigned int height = f->fmt.pix.height;
	int ret;

	ret = vidioc_try_fmt_vid_cap(file, priv, f);
	if (ret)
		return ret;

	return vidioc_s_fmt(file2ctx(file), f, height);
}

static int vidioc_s_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	unsigned int height = f->fmt.pix.height;
	int ret;

	ret = vidioc_try_fmt_vid_out(file, priv, f);
	if (ret)
		return ret;

	ret = vidioc_s_fmt(file2ctx(file), f, height);
	return ret;
}

static int vidioc_g_selection(struct file *file, void *priv,
			      struct v4l2_selection *s)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);
	struct bcm2835_codec_q_data *q_data;
	bool capture_queue = s->type == V4L2_BUF_TYPE_VIDEO_CAPTURE ?
								true : false;

	if (capture_queue ^ ctx->dev->decode)
		/* OUTPUT on decoder and CAPTURE on encoder are not valid. */
		return -EINVAL;

	q_data = get_q_data(ctx, s->type);
	if (!q_data)
		return -EINVAL;

	if (ctx->dev->decode) {
		switch (s->target) {
		case V4L2_SEL_TGT_COMPOSE_DEFAULT:
		case V4L2_SEL_TGT_COMPOSE:
			s->r.left = 0;
			s->r.top = 0;
			s->r.width = q_data->crop_width;
			s->r.height = q_data->crop_height;
			break;
		case V4L2_SEL_TGT_COMPOSE_BOUNDS:
			s->r.left = 0;
			s->r.top = 0;
			s->r.width = q_data->crop_width;
			s->r.height = q_data->crop_height;
			break;
		default:
			return -EINVAL;
		}
	} else {
		switch (s->target) {
		case V4L2_SEL_TGT_CROP_DEFAULT:
		case V4L2_SEL_TGT_CROP_BOUNDS:
			s->r.top = 0;
			s->r.left = 0;
			s->r.width = q_data->bytesperline;
			s->r.height = q_data->height;
			break;
		case V4L2_SEL_TGT_CROP:
			s->r.top = 0;
			s->r.left = 0;
			s->r.width = q_data->crop_width;
			s->r.height = q_data->crop_height;
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

static int vidioc_s_selection(struct file *file, void *priv,
			      struct v4l2_selection *s)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);
	struct bcm2835_codec_q_data *q_data = NULL;
	bool capture_queue = s->type == V4L2_BUF_TYPE_VIDEO_CAPTURE ?
								true : false;

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: ctx %p, type %d, q_data %p, target %d, rect x/y %d/%d, w/h %ux%u\n",
		 __func__, ctx, s->type, q_data, s->target, s->r.left, s->r.top,
		 s->r.width, s->r.height);

	if (capture_queue ^ ctx->dev->decode)
		/* OUTPUT on decoder and CAPTURE on encoder are not valid. */
		return -EINVAL;

	q_data = get_q_data(ctx, s->type);
	if (!q_data)
		return -EINVAL;

	if (ctx->dev->decode) {
		switch (s->target) {
		case V4L2_SEL_TGT_COMPOSE:
			/* Accept cropped image */
			s->r.left = 0;
			s->r.top = 0;
			s->r.width = min(s->r.width, q_data->crop_width);
			s->r.height = min(s->r.height, q_data->height);
			q_data->crop_width = s->r.width;
			q_data->crop_height = s->r.height;
			q_data->selection_set = true;
			break;
		default:
			return -EINVAL;
		}
	} else {
		switch (s->target) {
		case V4L2_SEL_TGT_CROP:
			/* Only support crop from (0,0) */
			s->r.top = 0;
			s->r.left = 0;
			s->r.width = min(s->r.width, q_data->crop_width);
			s->r.height = min(s->r.height, q_data->crop_height);
			q_data->crop_width = s->r.width;
			q_data->crop_height = s->r.height;
			q_data->selection_set = true;
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

static int vidioc_subscribe_evt(struct v4l2_fh *fh,
				const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_EOS:
		return v4l2_event_subscribe(fh, sub, 2, NULL);
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subscribe(fh, sub);
	default:
		return v4l2_ctrl_subscribe_event(fh, sub);
	}
}

static int bcm2835_codec_set_level_profile(struct bcm2835_codec_ctx *ctx,
					   struct v4l2_ctrl *ctrl)
{
	struct mmal_parameter_video_profile param;
	int param_size = sizeof(param);
	int ret;

	/*
	 * Level and Profile are set via the same MMAL parameter.
	 * Retrieve the current settings and amend the one that has changed.
	 */
	ret = vchiq_mmal_port_parameter_get(ctx->dev->instance,
					    &ctx->component->output[0],
					    MMAL_PARAMETER_PROFILE,
					    &param,
					    &param_size);
	if (ret)
		return ret;

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE:
			param.profile = MMAL_VIDEO_PROFILE_H264_BASELINE;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE:
			param.profile =
				MMAL_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_MAIN:
			param.profile = MMAL_VIDEO_PROFILE_H264_MAIN;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH:
			param.profile = MMAL_VIDEO_PROFILE_H264_HIGH;
			break;
		default:
			/* Should never get here */
			break;
		}
		break;

	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_0:
			param.level = MMAL_VIDEO_LEVEL_H264_1;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1B:
			param.level = MMAL_VIDEO_LEVEL_H264_1b;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_1:
			param.level = MMAL_VIDEO_LEVEL_H264_11;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_2:
			param.level = MMAL_VIDEO_LEVEL_H264_12;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_3:
			param.level = MMAL_VIDEO_LEVEL_H264_13;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_2_0:
			param.level = MMAL_VIDEO_LEVEL_H264_2;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_2_1:
			param.level = MMAL_VIDEO_LEVEL_H264_21;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_2_2:
			param.level = MMAL_VIDEO_LEVEL_H264_22;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_3_0:
			param.level = MMAL_VIDEO_LEVEL_H264_3;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_3_1:
			param.level = MMAL_VIDEO_LEVEL_H264_31;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_3_2:
			param.level = MMAL_VIDEO_LEVEL_H264_32;
			break;
		case V4L2_MPEG_VIDEO_H264_LEVEL_4_0:
			param.level = MMAL_VIDEO_LEVEL_H264_4;
			break;
		default:
			/* Should never get here */
			break;
		}
	}
	ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
					    &ctx->component->output[0],
					    MMAL_PARAMETER_PROFILE,
					    &param,
					    param_size);

	return ret;
}

static int bcm2835_codec_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct bcm2835_codec_ctx *ctx =
		container_of(ctrl->handler, struct bcm2835_codec_ctx, hdl);
	int ret = 0;

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		ctx->bitrate = ctrl->val;
		if (!ctx->component)
			break;

		ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
						    &ctx->component->output[0],
						    MMAL_PARAMETER_VIDEO_BIT_RATE,
						    &ctrl->val,
						    sizeof(ctrl->val));
		break;

	case V4L2_CID_MPEG_VIDEO_BITRATE_MODE: {
		u32 bitrate_mode;

		if (!ctx->component)
			break;

		switch (ctrl->val) {
		default:
		case V4L2_MPEG_VIDEO_BITRATE_MODE_VBR:
			bitrate_mode = MMAL_VIDEO_RATECONTROL_VARIABLE;
			break;
		case V4L2_MPEG_VIDEO_BITRATE_MODE_CBR:
			bitrate_mode = MMAL_VIDEO_RATECONTROL_CONSTANT;
			break;
		}

		ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
						    &ctx->component->output[0],
						    MMAL_PARAMETER_RATECONTROL,
						    &bitrate_mode,
						    sizeof(bitrate_mode));
		break;
	}
	case V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER:
		if (!ctx->component)
			break;

		ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
						    &ctx->component->output[0],
						    MMAL_PARAMETER_VIDEO_ENCODE_INLINE_HEADER,
						    &ctrl->val,
						    sizeof(ctrl->val));
		break;

	case V4L2_CID_MPEG_VIDEO_H264_I_PERIOD:
		if (!ctx->component)
			break;

		ret = vchiq_mmal_port_parameter_set(ctx->dev->instance,
						    &ctx->component->output[0],
						    MMAL_PARAMETER_INTRAPERIOD,
						    &ctrl->val,
						    sizeof(ctrl->val));
		break;

	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		if (!ctx->component)
			break;

		ret = bcm2835_codec_set_level_profile(ctx, ctrl);
		break;

	default:
		v4l2_err(&ctx->dev->v4l2_dev, "Invalid control\n");
		return -EINVAL;
	}

	if (ret)
		v4l2_err(&ctx->dev->v4l2_dev, "Failed setting ctrl %08x, ret %d\n",
			 ctrl->id, ret);
	return ret ? -EINVAL : 0;
}

static const struct v4l2_ctrl_ops bcm2835_codec_ctrl_ops = {
	.s_ctrl = bcm2835_codec_s_ctrl,
};

static int vidioc_try_decoder_cmd(struct file *file, void *priv,
				  struct v4l2_decoder_cmd *cmd)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);

	if (!ctx->dev->decode)
		return -EINVAL;

	switch (cmd->cmd) {
	case V4L2_DEC_CMD_STOP:
		if (cmd->flags & V4L2_DEC_CMD_STOP_TO_BLACK) {
			v4l2_err(&ctx->dev->v4l2_dev, "%s: DEC cmd->flags=%u stop to black not supported",
				 __func__, cmd->flags);
			return -EINVAL;
		}
		break;
	case V4L2_DEC_CMD_START:
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int vidioc_decoder_cmd(struct file *file, void *priv,
			      struct v4l2_decoder_cmd *cmd)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);
	struct bcm2835_codec_q_data *q_data = &ctx->q_data[V4L2_M2M_SRC];
	int ret;

	v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s, cmd %u", __func__,
		 cmd->cmd);
	ret = vidioc_try_decoder_cmd(file, priv, cmd);
	if (ret)
		return ret;

	switch (cmd->cmd) {
	case V4L2_DEC_CMD_STOP:
		if (q_data->eos_buffer_in_use)
			v4l2_err(&ctx->dev->v4l2_dev, "EOS buffers already in use\n");
		q_data->eos_buffer_in_use = true;

		q_data->eos_buffer.mmal.buffer_size = 0;
		q_data->eos_buffer.mmal.length = 0;
		q_data->eos_buffer.mmal.mmal_flags =
						MMAL_BUFFER_HEADER_FLAG_EOS;
		q_data->eos_buffer.mmal.pts = 0;
		q_data->eos_buffer.mmal.dts = 0;

		if (!ctx->component)
			break;

		ret = vchiq_mmal_submit_buffer(ctx->dev->instance,
					       &ctx->component->input[0],
					       &q_data->eos_buffer.mmal);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev,
				 "%s: EOS buffer submit failed %d\n",
				 __func__, ret);

		break;

	case V4L2_DEC_CMD_START:
		/* Do we need to do anything here? */
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int vidioc_try_encoder_cmd(struct file *file, void *priv,
				  struct v4l2_encoder_cmd *cmd)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);

	if (ctx->dev->decode)
		return -EINVAL;

	switch (cmd->cmd) {
	case V4L2_ENC_CMD_STOP:
		break;

	case V4L2_ENC_CMD_START:
		/* Do we need to do anything here? */
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int vidioc_encoder_cmd(struct file *file, void *priv,
			      struct v4l2_encoder_cmd *cmd)
{
	struct bcm2835_codec_ctx *ctx = file2ctx(file);
	struct bcm2835_codec_q_data *q_data = &ctx->q_data[V4L2_M2M_SRC];
	int ret;

	v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s, cmd %u", __func__,
		 cmd->cmd);
	ret = vidioc_try_encoder_cmd(file, priv, cmd);
	if (ret)
		return ret;

	switch (cmd->cmd) {
	case V4L2_ENC_CMD_STOP:
		if (q_data->eos_buffer_in_use)
			v4l2_err(&ctx->dev->v4l2_dev, "EOS buffers already in use\n");
		q_data->eos_buffer_in_use = true;

		q_data->eos_buffer.mmal.buffer_size = 0;
		q_data->eos_buffer.mmal.length = 0;
		q_data->eos_buffer.mmal.mmal_flags =
						MMAL_BUFFER_HEADER_FLAG_EOS;
		q_data->eos_buffer.mmal.pts = 0;
		q_data->eos_buffer.mmal.dts = 0;

		if (!ctx->component)
			break;

		ret = vchiq_mmal_submit_buffer(ctx->dev->instance,
					       &ctx->component->input[0],
					       &q_data->eos_buffer.mmal);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev,
				 "%s: EOS buffer submit failed %d\n",
				 __func__, ret);

		break;
	case V4L2_ENC_CMD_START:
		/* Do we need to do anything here? */
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ioctl_ops bcm2835_codec_ioctl_ops = {
	.vidioc_querycap	= vidioc_querycap,

	.vidioc_enum_fmt_vid_cap = vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap	= vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap	= vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap	= vidioc_s_fmt_vid_cap,

	.vidioc_enum_fmt_vid_out = vidioc_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out	= vidioc_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out	= vidioc_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out	= vidioc_s_fmt_vid_out,

	.vidioc_reqbufs		= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf	= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf		= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf		= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf	= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs	= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf		= v4l2_m2m_ioctl_expbuf,

	.vidioc_streamon	= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff	= v4l2_m2m_ioctl_streamoff,

	.vidioc_g_selection	= vidioc_g_selection,
	.vidioc_s_selection	= vidioc_s_selection,

	.vidioc_subscribe_event = vidioc_subscribe_evt,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_decoder_cmd = vidioc_decoder_cmd,
	.vidioc_try_decoder_cmd = vidioc_try_decoder_cmd,
	.vidioc_encoder_cmd = vidioc_encoder_cmd,
	.vidioc_try_encoder_cmd = vidioc_try_encoder_cmd,
};

static int bcm2835_codec_set_ctrls(struct bcm2835_codec_ctx *ctx)
{
	/*
	 * Query the control handler for the value of the various controls and
	 * set them.
	 */
	const u32 control_ids[] = {
		V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
		V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER,
		V4L2_CID_MPEG_VIDEO_H264_I_PERIOD,
		V4L2_CID_MPEG_VIDEO_H264_LEVEL,
		V4L2_CID_MPEG_VIDEO_H264_PROFILE,
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(control_ids); i++) {
		struct v4l2_ctrl *ctrl;

		ctrl = v4l2_ctrl_find(&ctx->hdl, control_ids[i]);
		if (ctrl)
			bcm2835_codec_s_ctrl(ctrl);
	}

	return 0;
}

static int bcm2835_codec_create_component(struct bcm2835_codec_ctx *ctx)
{
	struct bcm2835_codec_dev *dev = ctx->dev;
	unsigned int enable = 1;
	int ret;

	ret = vchiq_mmal_component_init(dev->instance, dev->decode ?
					"ril.video_decode" : "ril.video_encode",
					&ctx->component);
	if (ret < 0) {
		v4l2_err(&dev->v4l2_dev, "%s: failed to create component for %s\n",
			 __func__, dev->decode ? "decode" : "encode");
		return -ENOMEM;
	}

	vchiq_mmal_port_parameter_set(dev->instance, &ctx->component->input[0],
				      MMAL_PARAMETER_ZERO_COPY, &enable,
				      sizeof(enable));
	vchiq_mmal_port_parameter_set(dev->instance, &ctx->component->output[0],
				      MMAL_PARAMETER_ZERO_COPY, &enable,
				      sizeof(enable));

	setup_mmal_port_format(ctx, dev->decode, &ctx->q_data[V4L2_M2M_SRC],
			       &ctx->component->input[0]);

	setup_mmal_port_format(ctx, dev->decode, &ctx->q_data[V4L2_M2M_DST],
			       &ctx->component->output[0]);

	ret = vchiq_mmal_port_set_format(dev->instance,
					 &ctx->component->input[0]);
	if (ret < 0)
		goto destroy_component;

	ret = vchiq_mmal_port_set_format(dev->instance,
					 &ctx->component->output[0]);
	if (ret < 0)
		goto destroy_component;

	if (dev->decode) {
		if (ctx->q_data[V4L2_M2M_DST].sizeimage <
			ctx->component->output[0].minimum_buffer.size)
			v4l2_err(&dev->v4l2_dev, "buffer size mismatch sizeimage %u < min size %u\n",
				 ctx->q_data[V4L2_M2M_DST].sizeimage,
				 ctx->component->output[0].minimum_buffer.size);
	} else {
		if (ctx->q_data[V4L2_M2M_SRC].sizeimage <
			ctx->component->output[0].minimum_buffer.size)
			v4l2_err(&dev->v4l2_dev, "buffer size mismatch sizeimage %u < min size %u\n",
				 ctx->q_data[V4L2_M2M_SRC].sizeimage,
				 ctx->component->output[0].minimum_buffer.size);

		/* Now we have a component we can set all the ctrls */
		bcm2835_codec_set_ctrls(ctx);
	}

	return 0;

destroy_component:
	vchiq_mmal_component_finalise(ctx->dev->instance, ctx->component);

	return ret;
}

/*
 * Queue operations
 */

static int bcm2835_codec_queue_setup(struct vb2_queue *vq,
				     unsigned int *nbuffers,
				     unsigned int *nplanes,
				     unsigned int sizes[],
				     struct device *alloc_devs[])
{
	struct bcm2835_codec_ctx *ctx = vb2_get_drv_priv(vq);
	struct bcm2835_codec_q_data *q_data;
	struct vchiq_mmal_port *port;
	unsigned int size;

	q_data = get_q_data(ctx, vq->type);
	if (!q_data)
		return -EINVAL;

	if (!ctx->component)
		if (bcm2835_codec_create_component(ctx))
			return -EINVAL;

	port = get_port_data(ctx, vq->type);

	size = q_data->sizeimage;

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;

	*nplanes = 1;

	sizes[0] = size;
	port->current_buffer.size = size;

	if (*nbuffers < port->minimum_buffer.num)
		*nbuffers = port->minimum_buffer.num;
	/* Add one buffer to take an EOS */
	port->current_buffer.num = *nbuffers + 1;

	return 0;
}

static int bcm2835_codec_buf_init(struct vb2_buffer *vb)
{
	struct bcm2835_codec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vb2 = to_vb2_v4l2_buffer(vb);
	struct v4l2_m2m_buffer *m2m = container_of(vb2, struct v4l2_m2m_buffer,
						   vb);
	struct m2m_mmal_buffer *buf = container_of(m2m, struct m2m_mmal_buffer,
						   m2m);

	v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s: ctx:%p, vb %p\n",
		 __func__, ctx, vb);
	buf->mmal.buffer = vb2_plane_vaddr(&buf->m2m.vb.vb2_buf, 0);
	buf->mmal.buffer_size = vb2_plane_size(&buf->m2m.vb.vb2_buf, 0);

	mmal_vchi_buffer_init(ctx->dev->instance, &buf->mmal);

	return 0;
}

static int bcm2835_codec_buf_prepare(struct vb2_buffer *vb)
{
	struct bcm2835_codec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct bcm2835_codec_q_data *q_data;
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct v4l2_m2m_buffer *m2m = container_of(vbuf, struct v4l2_m2m_buffer,
						   vb);
	struct m2m_mmal_buffer *buf = container_of(m2m, struct m2m_mmal_buffer,
						   m2m);
	int ret;

	v4l2_dbg(4, debug, &ctx->dev->v4l2_dev, "%s: type: %d ptr %p\n",
		 __func__, vb->vb2_queue->type, vb);

	q_data = get_q_data(ctx, vb->vb2_queue->type);
	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type)) {
		if (vbuf->field == V4L2_FIELD_ANY)
			vbuf->field = V4L2_FIELD_NONE;
		if (vbuf->field != V4L2_FIELD_NONE) {
			v4l2_err(&ctx->dev->v4l2_dev, "%s field isn't supported\n",
				 __func__);
			return -EINVAL;
		}
	}

	if (vb2_plane_size(vb, 0) < q_data->sizeimage) {
		v4l2_err(&ctx->dev->v4l2_dev, "%s data will not fit into plane (%lu < %lu)\n",
			 __func__, vb2_plane_size(vb, 0),
			 (long)q_data->sizeimage);
		return -EINVAL;
	}

	if (!V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type))
		vb2_set_plane_payload(vb, 0, q_data->sizeimage);

	/*
	 * We want to do this at init, but vb2_core_expbuf checks that the
	 * index < q->num_buffers, and q->num_buffers only gets updated once
	 * all the buffers are allocated.
	 */
	if (!buf->mmal.dma_buf) {
		ret = vb2_core_expbuf_dmabuf(vb->vb2_queue,
					     vb->vb2_queue->type, vb->index, 0,
					     O_CLOEXEC, &buf->mmal.dma_buf);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed to expbuf idx %d, ret %d\n",
				 __func__, vb->index, ret);
	} else {
		ret = 0;
	}

	return ret;
}

static void bcm2835_codec_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct bcm2835_codec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_dbg(4, debug, &ctx->dev->v4l2_dev, "%s: type: %d ptr %p vbuf->flags %u, seq %u, bytesused %u\n",
		 __func__, vb->vb2_queue->type, vb, vbuf->flags, vbuf->sequence,
		 vb->planes[0].bytesused);
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static void bcm2835_codec_buffer_cleanup(struct vb2_buffer *vb)
{
	struct bcm2835_codec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vb2 = to_vb2_v4l2_buffer(vb);
	struct v4l2_m2m_buffer *m2m = container_of(vb2, struct v4l2_m2m_buffer,
						   vb);
	struct m2m_mmal_buffer *buf = container_of(m2m, struct m2m_mmal_buffer,
						   m2m);

	v4l2_dbg(2, debug, &ctx->dev->v4l2_dev, "%s: ctx:%p, vb %p\n",
		 __func__, ctx, vb);

	mmal_vchi_buffer_cleanup(&buf->mmal);

	if (buf->mmal.dma_buf) {
		dma_buf_put(buf->mmal.dma_buf);
		buf->mmal.dma_buf = NULL;
	}
}

static int bcm2835_codec_start_streaming(struct vb2_queue *q,
					 unsigned int count)
{
	struct bcm2835_codec_ctx *ctx = vb2_get_drv_priv(q);
	struct bcm2835_codec_dev *dev = ctx->dev;
	struct bcm2835_codec_q_data *q_data = get_q_data(ctx, q->type);
	int ret;

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: type: %d count %d\n",
		 __func__, q->type, count);
	q_data->sequence = 0;

	if (!ctx->component_enabled) {
		ret = vchiq_mmal_component_enable(dev->instance,
						  ctx->component);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed enabling component, ret %d\n",
				 __func__, ret);
		ctx->component_enabled = true;
	}

	if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
		/*
		 * Create the EOS buffer.
		 * We only need the MMAL part, and want to NOT attach a memory
		 * buffer to it as it should only take flags.
		 */
		memset(&q_data->eos_buffer, 0, sizeof(q_data->eos_buffer));
		mmal_vchi_buffer_init(dev->instance,
				      &q_data->eos_buffer.mmal);
		q_data->eos_buffer_in_use = false;

		ctx->component->input[0].cb_ctx = ctx;
		ret = vchiq_mmal_port_enable(dev->instance,
					     &ctx->component->input[0],
					     ip_buffer_cb);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed enabling i/p port, ret %d\n",
				 __func__, ret);
	} else {
		ctx->component->output[0].cb_ctx = ctx;
		ret = vchiq_mmal_port_enable(dev->instance,
					     &ctx->component->output[0],
					     op_buffer_cb);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed enabling o/p port, ret %d\n",
				 __func__, ret);
	}
	return ret;
}

static void bcm2835_codec_stop_streaming(struct vb2_queue *q)
{
	struct bcm2835_codec_ctx *ctx = vb2_get_drv_priv(q);
	struct bcm2835_codec_dev *dev = ctx->dev;
	struct bcm2835_codec_q_data *q_data = get_q_data(ctx, q->type);
	struct vchiq_mmal_port *port = get_port_data(ctx, q->type);
	struct vb2_v4l2_buffer *vbuf;
	struct vb2_v4l2_buffer *vb2;
	struct v4l2_m2m_buffer *m2m;
	struct m2m_mmal_buffer *buf;
	int ret, i;

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: type: %d - return buffers\n",
		 __func__, q->type);

	init_completion(&ctx->frame_cmplt);

	/* Clear out all buffers held by m2m framework */
	for (;;) {
		if (V4L2_TYPE_IS_OUTPUT(q->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: return buffer %p\n",
			 __func__, vbuf);

		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}

	/* Disable MMAL port - this will flush buffers back */
	ret = vchiq_mmal_port_disable(dev->instance, port);
	if (ret)
		v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed disabling %s port, ret %d\n",
			 __func__, V4L2_TYPE_IS_OUTPUT(q->type) ? "i/p" : "o/p",
			 ret);

	while (atomic_read(&port->buffers_with_vpu)) {
		v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: Waiting for buffers to be returned - %d outstanding\n",
			 __func__, atomic_read(&port->buffers_with_vpu));
		ret = wait_for_completion_timeout(&ctx->frame_cmplt, HZ);
		if (ret <= 0) {
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Timeout waiting for buffers to be returned - %d outstanding\n",
				 __func__,
				 atomic_read(&port->buffers_with_vpu));
			break;
		}
	}

	/*
	 * Release the VCSM handle here as otherwise REQBUFS(0) aborts because
	 * someone is using the dmabuf before giving the driver a chance to do
	 * anything about it.
	 */
	for (i = 0; i < q->num_buffers; i++) {
		vb2 = to_vb2_v4l2_buffer(q->bufs[i]);
		m2m = container_of(vb2, struct v4l2_m2m_buffer, vb);
		buf = container_of(m2m, struct m2m_mmal_buffer, m2m);

		mmal_vchi_buffer_cleanup(&buf->mmal);
		if (buf->mmal.dma_buf) {
			dma_buf_put(buf->mmal.dma_buf);
			buf->mmal.dma_buf = NULL;
		}
	}

	/* If both ports disabled, then disable the component */
	if (!ctx->component->input[0].enabled &&
	    !ctx->component->output[0].enabled) {
		ret = vchiq_mmal_component_disable(dev->instance,
						   ctx->component);
		if (ret)
			v4l2_err(&ctx->dev->v4l2_dev, "%s: Failed enabling component, ret %d\n",
				 __func__, ret);
	}

	if (V4L2_TYPE_IS_OUTPUT(q->type))
		mmal_vchi_buffer_cleanup(&q_data->eos_buffer.mmal);

	v4l2_dbg(1, debug, &ctx->dev->v4l2_dev, "%s: done\n", __func__);
}

static const struct vb2_ops bcm2835_codec_qops = {
	.queue_setup	 = bcm2835_codec_queue_setup,
	.buf_init	 = bcm2835_codec_buf_init,
	.buf_prepare	 = bcm2835_codec_buf_prepare,
	.buf_queue	 = bcm2835_codec_buf_queue,
	.buf_cleanup	 = bcm2835_codec_buffer_cleanup,
	.start_streaming = bcm2835_codec_start_streaming,
	.stop_streaming  = bcm2835_codec_stop_streaming,
	.wait_prepare	 = vb2_ops_wait_prepare,
	.wait_finish	 = vb2_ops_wait_finish,
};

static int queue_init(void *priv, struct vb2_queue *src_vq,
		      struct vb2_queue *dst_vq)
{
	struct bcm2835_codec_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct m2m_mmal_buffer);
	src_vq->ops = &bcm2835_codec_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->dev = &ctx->dev->pdev->dev;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->dev_mutex;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct m2m_mmal_buffer);
	dst_vq->ops = &bcm2835_codec_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->dev = &ctx->dev->pdev->dev;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->dev_mutex;

	return vb2_queue_init(dst_vq);
}

/*
 * File operations
 */
static int bcm2835_codec_open(struct file *file)
{
	struct bcm2835_codec_dev *dev = video_drvdata(file);
	struct bcm2835_codec_ctx *ctx = NULL;
	struct v4l2_ctrl_handler *hdl;
	int rc = 0;

	v4l2_dbg(1, debug, &dev->v4l2_dev, "Creating instance for %s\n",
		 dev->decode ? "decode" : "encode");
	if (mutex_lock_interruptible(&dev->dev_mutex)) {
		v4l2_err(&dev->v4l2_dev, "Mutex fail\n");
		return -ERESTARTSYS;
	}
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		rc = -ENOMEM;
		goto open_unlock;
	}

	ctx->q_data[V4L2_M2M_SRC].fmt = get_default_format(dev->decode, false);
	ctx->q_data[V4L2_M2M_DST].fmt = get_default_format(dev->decode, true);
	if (dev->decode) {
		/*
		 * Input width and height are irrelevant as they will be defined
		 * by the bitstream not the format. Required by V4L2 though.
		 */
		ctx->q_data[V4L2_M2M_SRC].crop_width = DEFAULT_WIDTH;
		ctx->q_data[V4L2_M2M_SRC].crop_height = DEFAULT_HEIGHT;
		ctx->q_data[V4L2_M2M_SRC].height = DEFAULT_HEIGHT;
		ctx->q_data[V4L2_M2M_SRC].bytesperline = 0;
		ctx->q_data[V4L2_M2M_SRC].sizeimage =
						DEF_COMP_BUF_SIZE_720P_OR_LESS;

		ctx->q_data[V4L2_M2M_DST].crop_width = DEFAULT_WIDTH;
		ctx->q_data[V4L2_M2M_DST].crop_height = DEFAULT_HEIGHT;
		ctx->q_data[V4L2_M2M_DST].height = DEFAULT_HEIGHT;
		ctx->q_data[V4L2_M2M_DST].bytesperline =
				get_bytesperline(DEFAULT_WIDTH,
						 ctx->q_data[V4L2_M2M_DST].fmt);
		ctx->q_data[V4L2_M2M_DST].sizeimage =
			get_sizeimage(ctx->q_data[V4L2_M2M_DST].bytesperline,
				      ctx->q_data[V4L2_M2M_DST].height,
				      ctx->q_data[V4L2_M2M_DST].fmt);
	} else {
		ctx->q_data[V4L2_M2M_SRC].crop_width = DEFAULT_WIDTH;
		ctx->q_data[V4L2_M2M_SRC].crop_height = DEFAULT_HEIGHT;
		ctx->q_data[V4L2_M2M_SRC].height = DEFAULT_HEIGHT;
		ctx->q_data[V4L2_M2M_SRC].bytesperline =
				get_bytesperline(DEFAULT_WIDTH,
						 ctx->q_data[V4L2_M2M_SRC].fmt);
		ctx->q_data[V4L2_M2M_SRC].sizeimage =
			get_sizeimage(ctx->q_data[V4L2_M2M_SRC].bytesperline,
				      ctx->q_data[V4L2_M2M_SRC].height,
				      ctx->q_data[V4L2_M2M_SRC].fmt);

		ctx->q_data[V4L2_M2M_DST].crop_width = DEFAULT_WIDTH;
		ctx->q_data[V4L2_M2M_DST].crop_height = DEFAULT_HEIGHT;
		ctx->q_data[V4L2_M2M_DST].bytesperline = 0;
		ctx->q_data[V4L2_M2M_DST].height = DEFAULT_HEIGHT;
		ctx->q_data[V4L2_M2M_DST].sizeimage =
						DEF_COMP_BUF_SIZE_720P_OR_LESS;
	}

	ctx->colorspace = V4L2_COLORSPACE_REC709;
	ctx->bitrate = 10 * 1000 * 1000;

	/* Initialise V4L2 contexts */
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	ctx->dev = dev;
	hdl = &ctx->hdl;
	if (!dev->decode) {
		/* Encode controls */
		v4l2_ctrl_handler_init(hdl, 6);

		v4l2_ctrl_new_std_menu(hdl, &bcm2835_codec_ctrl_ops,
				       V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
				       V4L2_MPEG_VIDEO_BITRATE_MODE_CBR, 0,
				       V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_BITRATE,
				  25 * 1000, 25 * 1000 * 1000,
				  25 * 1000, 10 * 1000 * 1000);
		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER,
				  0, 1,
				  1, 0);
		v4l2_ctrl_new_std(hdl, &bcm2835_codec_ctrl_ops,
				  V4L2_CID_MPEG_VIDEO_H264_I_PERIOD,
				  0, 0x7FFFFFFF,
				  1, 60);
		v4l2_ctrl_new_std_menu(hdl, &bcm2835_codec_ctrl_ops,
				       V4L2_CID_MPEG_VIDEO_H264_LEVEL,
				       V4L2_MPEG_VIDEO_H264_LEVEL_4_2,
				       ~(BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1_0) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1B) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1_1) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1_2) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1_3) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_2_0) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_2_1) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_2_2) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_3_0) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_3_1) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_3_2) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_4_0) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_4_1) |
					 BIT(V4L2_MPEG_VIDEO_H264_LEVEL_4_2)),
				       V4L2_MPEG_VIDEO_H264_LEVEL_4_0);
		v4l2_ctrl_new_std_menu(hdl, &bcm2835_codec_ctrl_ops,
				       V4L2_CID_MPEG_VIDEO_H264_PROFILE,
				       V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
				       ~(BIT(V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE) |
					 BIT(V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE) |
					 BIT(V4L2_MPEG_VIDEO_H264_PROFILE_MAIN) |
					 BIT(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH)),
					V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);
		if (hdl->error) {
			rc = hdl->error;
			goto free_ctrl_handler;
		}
		ctx->fh.ctrl_handler = hdl;
		v4l2_ctrl_handler_setup(hdl);
	}

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx, &queue_init);

	if (IS_ERR(ctx->fh.m2m_ctx)) {
		rc = PTR_ERR(ctx->fh.m2m_ctx);

		goto free_ctrl_handler;
	}

	/* Set both queues as buffered as we have buffering in the VPU. That
	 * means that we will be scheduled whenever either an input or output
	 * buffer is available (otherwise one of each are required).
	 */
	v4l2_m2m_set_src_buffered(ctx->fh.m2m_ctx, true);
	v4l2_m2m_set_dst_buffered(ctx->fh.m2m_ctx, true);

	v4l2_fh_add(&ctx->fh);
	atomic_inc(&dev->num_inst);

	mutex_unlock(&dev->dev_mutex);
	return 0;

free_ctrl_handler:
	v4l2_ctrl_handler_free(hdl);
	kfree(ctx);
open_unlock:
	mutex_unlock(&dev->dev_mutex);
	return rc;
}

static int bcm2835_codec_release(struct file *file)
{
	struct bcm2835_codec_dev *dev = video_drvdata(file);
	struct bcm2835_codec_ctx *ctx = file2ctx(file);

	v4l2_dbg(1, debug, &dev->v4l2_dev, "%s: Releasing instance %p\n",
		 __func__, ctx);

	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	v4l2_ctrl_handler_free(&ctx->hdl);
	mutex_lock(&dev->dev_mutex);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);

	if (ctx->component)
		vchiq_mmal_component_finalise(dev->instance, ctx->component);

	mutex_unlock(&dev->dev_mutex);
	kfree(ctx);

	atomic_dec(&dev->num_inst);

	return 0;
}

static const struct v4l2_file_operations bcm2835_codec_fops = {
	.owner		= THIS_MODULE,
	.open		= bcm2835_codec_open,
	.release	= bcm2835_codec_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static const struct video_device bcm2835_codec_videodev = {
	.name		= MEM2MEM_NAME,
	.vfl_dir	= VFL_DIR_M2M,
	.fops		= &bcm2835_codec_fops,
	.ioctl_ops	= &bcm2835_codec_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release_empty,
};

static const struct v4l2_m2m_ops m2m_ops = {
	.device_run	= device_run,
	.job_ready	= job_ready,
	.job_abort	= job_abort,
};

static int bcm2835_codec_create(struct platform_device *pdev,
				struct bcm2835_codec_dev **new_dev,
				bool decode)
{
	struct bcm2835_codec_dev *dev;
	struct video_device *vfd;
	struct vchiq_mmal_instance *instance = NULL;
	int video_nr;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->pdev = pdev;

	dev->decode = decode;

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret)
		return ret;

	atomic_set(&dev->num_inst, 0);
	mutex_init(&dev->dev_mutex);

	dev->vfd = bcm2835_codec_videodev;
	vfd = &dev->vfd;
	vfd->lock = &dev->dev_mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;

	if (dev->decode) {
		v4l2_disable_ioctl(vfd, VIDIOC_ENCODER_CMD);
		v4l2_disable_ioctl(vfd, VIDIOC_TRY_ENCODER_CMD);
		video_nr = decode_video_nr;
	} else {
		v4l2_disable_ioctl(vfd, VIDIOC_DECODER_CMD);
		v4l2_disable_ioctl(vfd, VIDIOC_TRY_DECODER_CMD);
		video_nr = encode_video_nr;
	}

	ret = video_register_device(vfd, VFL_TYPE_GRABBER, video_nr);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register video device\n");
		goto unreg_dev;
	}

	video_set_drvdata(vfd, dev);
	snprintf(vfd->name, sizeof(vfd->name), "%s",
		 bcm2835_codec_videodev.name);
	v4l2_info(&dev->v4l2_dev, "Device registered as /dev/video%d\n",
		  vfd->num);

	*new_dev = dev;

	dev->m2m_dev = v4l2_m2m_init(&m2m_ops);
	if (IS_ERR(dev->m2m_dev)) {
		v4l2_err(&dev->v4l2_dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(dev->m2m_dev);
		goto err_m2m;
	}

	ret = vchiq_mmal_init(&instance);
	if (ret < 0)
		goto err_m2m;
	dev->instance = instance;

	v4l2_info(&dev->v4l2_dev, "Loaded V4L2 %s codec\n",
		  dev->decode ? "decode" : "encode");
	return 0;

err_m2m:
	v4l2_m2m_release(dev->m2m_dev);
	video_unregister_device(&dev->vfd);
unreg_dev:
	v4l2_device_unregister(&dev->v4l2_dev);

	return ret;
}

static int bcm2835_codec_destroy(struct bcm2835_codec_dev *dev)
{
	if (!dev)
		return -ENODEV;

	v4l2_info(&dev->v4l2_dev, "Removing " MEM2MEM_NAME);
	v4l2_m2m_release(dev->m2m_dev);
	video_unregister_device(&dev->vfd);
	v4l2_device_unregister(&dev->v4l2_dev);

	return 0;
}

static int bcm2835_codec_probe(struct platform_device *pdev)
{
	struct bcm2835_codec_driver *drv;
	int ret = 0;

	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	ret = bcm2835_codec_create(pdev, &drv->encode, false);
	if (ret)
		goto out;

	ret = bcm2835_codec_create(pdev, &drv->decode, true);
	if (ret)
		goto out;

	platform_set_drvdata(pdev, drv);

	return 0;

out:
	if (drv->encode) {
		bcm2835_codec_destroy(drv->encode);
		drv->encode = NULL;
	}
	return ret;
}

static int bcm2835_codec_remove(struct platform_device *pdev)
{
	struct bcm2835_codec_driver *drv = platform_get_drvdata(pdev);

	bcm2835_codec_destroy(drv->encode);

	bcm2835_codec_destroy(drv->decode);

	return 0;
}

static struct platform_driver bcm2835_v4l2_codec_driver = {
	.probe = bcm2835_codec_probe,
	.remove = bcm2835_codec_remove,
	.driver = {
		   .name = "bcm2835-codec",
		   .owner = THIS_MODULE,
		   },
};

module_platform_driver(bcm2835_v4l2_codec_driver);

MODULE_DESCRIPTION("BCM2835 codec V4L2 driver");
MODULE_AUTHOR("Dave Stevenson, <dave.stevenson@raspberrypi.org>");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.0.1");
MODULE_ALIAS("platform:bcm2835-codec");