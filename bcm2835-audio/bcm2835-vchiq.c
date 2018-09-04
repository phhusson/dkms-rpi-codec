// SPDX-License-Identifier: GPL-2.0
/* Copyright 2011 Broadcom Corporation.  All rights reserved. */

#include <linux/device.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/completion.h>

#include "bcm2835.h"

/* ---- Include Files -------------------------------------------------------- */

#include "vc_vchi_audioserv_defs.h"

/* ---- Private Constants and Types ------------------------------------------ */

/* Logging macros (for remapping to other logging mechanisms, i.e., printf) */
#ifdef AUDIO_DEBUG_ENABLE
#define LOG_ERR(fmt, arg...)   pr_err("%s:%d " fmt, __func__, __LINE__, ##arg)
#define LOG_WARN(fmt, arg...)  pr_info("%s:%d " fmt, __func__, __LINE__, ##arg)
#define LOG_INFO(fmt, arg...)  pr_info("%s:%d " fmt, __func__, __LINE__, ##arg)
#define LOG_DBG(fmt, arg...)   pr_info("%s:%d " fmt, __func__, __LINE__, ##arg)
#else
#define LOG_ERR(fmt, arg...)   pr_err("%s:%d " fmt, __func__, __LINE__, ##arg)
#define LOG_WARN(fmt, arg...)	 no_printk(fmt, ##arg)
#define LOG_INFO(fmt, arg...)	 no_printk(fmt, ##arg)
#define LOG_DBG(fmt, arg...)	 no_printk(fmt, ##arg)
#endif

struct bcm2835_audio_instance {
	VCHI_SERVICE_HANDLE_T vchi_handle;
	struct completion msg_avail_comp;
	struct mutex vchi_mutex;
	struct bcm2835_alsa_stream *alsa_stream;
	int result;
	unsigned int max_packet;
	short peer_version;
};

static bool force_bulk;

static void bcm2835_audio_lock(struct bcm2835_audio_instance *instance)
{
	mutex_lock(&instance->vchi_mutex);
	vchi_service_use(instance->vchi_handle);
}

static void bcm2835_audio_unlock(struct bcm2835_audio_instance *instance)
{
	vchi_service_release(instance->vchi_handle);
	mutex_unlock(&instance->vchi_mutex);
}

static int bcm2835_audio_send_msg_locked(struct bcm2835_audio_instance *instance,
					 struct vc_audio_msg *m, bool wait)
{
	int status;

	if (wait) {
		instance->result = -1;
		init_completion(&instance->msg_avail_comp);
	}

	status = vchi_queue_kernel_message(instance->vchi_handle,
					   m, sizeof(*m));
	if (status) {
		LOG_ERR("vchi message queue failed: %d, msg=%d\n",
			status, m->type);
		return -EIO;
	}

	if (wait) {
		if (!wait_for_completion_timeout(&instance->msg_avail_comp,
						 msecs_to_jiffies(10 * 1000))) {
			LOG_ERR("vchi message timeout, msg=%d\n", m->type);
			return -ETIMEDOUT;
		} else if (instance->result) {
			LOG_ERR("vchi message response error:%d, msg=%d\n",
				instance->result, m->type);
			return -EIO;
		}
	}

	return 0;
}

static int bcm2835_audio_send_msg(struct bcm2835_audio_instance *instance,
				  struct vc_audio_msg *m, bool wait)
{
	int err;

	bcm2835_audio_lock(instance);
	err = bcm2835_audio_send_msg_locked(instance, m, wait);
	bcm2835_audio_unlock(instance);
	return err;
}

static int bcm2835_audio_send_simple(struct bcm2835_audio_instance *instance,
				     int type, bool wait)
{
	struct vc_audio_msg m = { .type = type };

	return bcm2835_audio_send_msg(instance, &m, wait);
}

static const u32 BCM2835_AUDIO_WRITE_COOKIE1 = ('B' << 24 | 'C' << 16 |
						'M' << 8  | 'A');
static const u32 BCM2835_AUDIO_WRITE_COOKIE2 = ('D' << 24 | 'A' << 16 |
						'T' << 8  | 'A');

static void audio_vchi_callback(void *param,
				const VCHI_CALLBACK_REASON_T reason,
				void *msg_handle)
{
	struct bcm2835_audio_instance *instance = param;
	int status;
	int msg_len;
	struct vc_audio_msg m;

	if (reason != VCHI_CALLBACK_MSG_AVAILABLE)
		return;

	status = vchi_msg_dequeue(instance->vchi_handle,
				  &m, sizeof(m), &msg_len, VCHI_FLAGS_NONE);
	if (m.type == VC_AUDIO_MSG_TYPE_RESULT) {
		instance->result = m.u.result.success;
		complete(&instance->msg_avail_comp);
	} else if (m.type == VC_AUDIO_MSG_TYPE_COMPLETE) {
		if (m.u.complete.cookie1 != BCM2835_AUDIO_WRITE_COOKIE1 ||
		    m.u.complete.cookie2 != BCM2835_AUDIO_WRITE_COOKIE2)
			LOG_ERR("invalid cookie\n");
		else
			bcm2835_playback_fifo(instance->alsa_stream,
					      m.u.complete.count);
	} else {
		LOG_ERR("unexpected callback type=%d\n", m.type);
	}
}

static int
vc_vchi_audio_init(VCHI_INSTANCE_T vchi_instance,
		   VCHI_CONNECTION_T *vchi_connection,
		   struct bcm2835_audio_instance *instance)
{
	SERVICE_CREATION_T params = {
		.version		= VCHI_VERSION_EX(VC_AUDIOSERV_VER, VC_AUDIOSERV_MIN_VER),
		.service_id		= VC_AUDIO_SERVER_NAME,
		.connection		= vchi_connection,
		.rx_fifo_size		= 0,
		.tx_fifo_size		= 0,
		.callback		= audio_vchi_callback,
		.callback_param		= instance,
		.want_unaligned_bulk_rx = 1, //TODO: remove VCOS_FALSE
		.want_unaligned_bulk_tx = 1, //TODO: remove VCOS_FALSE
		.want_crc		= 0
	};
	int status;

	/* Open the VCHI service connections */
	status = vchi_service_open(vchi_instance, &params,
				   &instance->vchi_handle);

	if (status) {
		LOG_ERR("%s: failed to open VCHI service connection (status=%d)\n",
			__func__, status);
		kfree(instance);
		return -EPERM;
	}

	/* Finished with the service for now */
	vchi_service_release(instance->vchi_handle);

	return 0;
}

static void vc_vchi_audio_deinit(struct bcm2835_audio_instance *instance)
{
	int status;

	mutex_lock(&instance->vchi_mutex);
	vchi_service_use(instance->vchi_handle);

	/* Close all VCHI service connections */
	status = vchi_service_close(instance->vchi_handle);
	if (status) {
		LOG_DBG("%s: failed to close VCHI service connection (status=%d)\n",
			__func__, status);
	}

	mutex_unlock(&instance->vchi_mutex);
}

int bcm2835_new_vchi_ctx(struct bcm2835_vchi_ctx *vchi_ctx)
{
	int ret;

	/* Initialize and create a VCHI connection */
	ret = vchi_initialise(&vchi_ctx->vchi_instance);
	if (ret) {
		LOG_ERR("%s: failed to initialise VCHI instance (ret=%d)\n",
			__func__, ret);

		return -EIO;
	}

	ret = vchi_connect(NULL, 0, vchi_ctx->vchi_instance);
	if (ret) {
		LOG_ERR("%s: failed to connect VCHI instance (ret=%d)\n",
			__func__, ret);

		kfree(vchi_ctx->vchi_instance);
		vchi_ctx->vchi_instance = NULL;

		return -EIO;
	}

	return 0;
}

void bcm2835_free_vchi_ctx(struct bcm2835_vchi_ctx *vchi_ctx)
{
	/* Close the VCHI connection - it will also free vchi_instance */
	WARN_ON(vchi_disconnect(vchi_ctx->vchi_instance));

	vchi_ctx->vchi_instance = NULL;
}

int bcm2835_audio_open(struct bcm2835_alsa_stream *alsa_stream)
{
	struct bcm2835_vchi_ctx *vchi_ctx = alsa_stream->chip->vchi_ctx;
	struct bcm2835_audio_instance *instance;
	int err;

	/* Allocate memory for this instance */
	instance = kzalloc(sizeof(*instance), GFP_KERNEL);
	if (!instance)
		return -ENOMEM;
	mutex_init(&instance->vchi_mutex);
	instance->alsa_stream = alsa_stream;
	alsa_stream->instance = instance;

	err = vc_vchi_audio_init(vchi_ctx->vchi_instance,
				 vchi_ctx->vchi_connection,
				 instance);
	if (err < 0)
		goto free_instance;

	err = bcm2835_audio_send_simple(instance, VC_AUDIO_MSG_TYPE_OPEN,
					false);
	if (err < 0)
		goto deinit;

	bcm2835_audio_lock(instance);
	vchi_get_peer_version(instance->vchi_handle, &instance->peer_version);
	bcm2835_audio_unlock(instance);
	if (instance->peer_version < 2 || force_bulk)
		instance->max_packet = 0; /* bulk transfer */
	else
		instance->max_packet = 4000;

	return 0;

 deinit:
	vc_vchi_audio_deinit(instance);
 free_instance:
	alsa_stream->instance = NULL;
	kfree(instance);
	return err;
}

int bcm2835_audio_set_ctls(struct bcm2835_alsa_stream *alsa_stream)
{
	struct bcm2835_chip *chip = alsa_stream->chip;
	struct vc_audio_msg m = {};

	m.type = VC_AUDIO_MSG_TYPE_CONTROL;
	m.u.control.dest = chip->dest;
	if (!chip->mute)
		m.u.control.volume = CHIP_MIN_VOLUME;
	else
		m.u.control.volume = alsa2chip(chip->volume);

	return bcm2835_audio_send_msg(alsa_stream->instance, &m, true);
}

int bcm2835_audio_set_params(struct bcm2835_alsa_stream *alsa_stream,
			     unsigned int channels, unsigned int samplerate,
			     unsigned int bps)
{
	struct vc_audio_msg m = {
		 .type = VC_AUDIO_MSG_TYPE_CONFIG,
		 .u.config.channels = channels,
		 .u.config.samplerate = samplerate,
		 .u.config.bps = bps,
	};
	int err;

	/* resend ctls - alsa_stream may not have been open when first send */
	err = bcm2835_audio_set_ctls(alsa_stream);
	if (err)
		return err;

	return bcm2835_audio_send_msg(alsa_stream->instance, &m, true);
}

int bcm2835_audio_start(struct bcm2835_alsa_stream *alsa_stream)
{
	return bcm2835_audio_send_simple(alsa_stream->instance,
					 VC_AUDIO_MSG_TYPE_START, false);
}

int bcm2835_audio_stop(struct bcm2835_alsa_stream *alsa_stream)
{
	return bcm2835_audio_send_simple(alsa_stream->instance,
					 VC_AUDIO_MSG_TYPE_STOP, false);
}

int bcm2835_audio_drain(struct bcm2835_alsa_stream *alsa_stream)
{
	struct vc_audio_msg m = {
		.type = VC_AUDIO_MSG_TYPE_STOP,
		.u.stop.draining = 1,
	};

	return bcm2835_audio_send_msg(alsa_stream->instance, &m, false);
}

int bcm2835_audio_close(struct bcm2835_alsa_stream *alsa_stream)
{
	struct bcm2835_audio_instance *instance = alsa_stream->instance;
	int err;

	err = bcm2835_audio_send_simple(alsa_stream->instance,
					VC_AUDIO_MSG_TYPE_CLOSE, true);

	/* Stop the audio service */
	vc_vchi_audio_deinit(instance);
	alsa_stream->instance = NULL;
	kfree(instance);

	return err;
}

int bcm2835_audio_write(struct bcm2835_alsa_stream *alsa_stream,
			unsigned int size, void *src)
{
	struct bcm2835_audio_instance *instance = alsa_stream->instance;
	struct vc_audio_msg m = {
		.type = VC_AUDIO_MSG_TYPE_WRITE,
		.u.write.count = size,
		.u.write.max_packet = instance->max_packet,
		.u.write.cookie1 = BCM2835_AUDIO_WRITE_COOKIE1,
		.u.write.cookie2 = BCM2835_AUDIO_WRITE_COOKIE2,
	};
	unsigned int count;
	int err, status;

	if (!size)
		return 0;

	bcm2835_audio_lock(instance);
	err = bcm2835_audio_send_msg_locked(instance, &m, false);
	if (err < 0)
		goto unlock;

	count = size;
	if (!instance->max_packet) {
		/* Send the message to the videocore */
		status = vchi_bulk_queue_transmit(instance->vchi_handle,
						  src, count,
						  VCHI_FLAGS_BLOCK_UNTIL_DATA_READ,
						  NULL);
	} else {
		while (count > 0) {
			int bytes = min(instance->max_packet, count);

			status = vchi_queue_kernel_message(instance->vchi_handle,
							   src, bytes);
			src += bytes;
			count -= bytes;
		}
	}

	if (status) {
		LOG_ERR("failed on %d bytes transfer (status=%d)\n",
			size, status);
		err = -EIO;
	}

 unlock:
	bcm2835_audio_unlock(instance);
	return err;
}

module_param(force_bulk, bool, 0444);
MODULE_PARM_DESC(force_bulk, "Force use of vchiq bulk for audio");
