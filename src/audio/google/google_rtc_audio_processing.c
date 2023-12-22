// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2021 Google LLC.
//
// Author: Lionel Koenig <lionelk@google.com>
#include <errno.h>
#include <ipc/control.h>
#include <ipc/stream.h>
#include <ipc/topology.h>
#include <ipc4/aec.h>
#include <sof/audio/module_adapter/module/generic.h>
#include <sof/audio/buffer.h>
#include <sof/audio/component.h>
#include <sof/audio/data_blob.h>
#include <sof/audio/format.h>
#include <sof/audio/kpb.h>
#include <sof/audio/pipeline.h>
#include <sof/common.h>
#include <rtos/panic.h>
#include <sof/ipc/msg.h>
#include <rtos/alloc.h>
#include <rtos/init.h>
#include <sof/lib/memory.h>
#include <sof/lib/notifier.h>
#include <sof/lib/uuid.h>
#include <rtos/wait.h>
#include <sof/list.h>
#include <sof/math/numbers.h>
#include <rtos/string.h>
#include <sof/trace/trace.h>
#include <sof/ut.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <user/trace.h>

#include <google_rtc_audio_processing.h>
#include <google_rtc_audio_processing_platform.h>
#include <google_rtc_audio_processing_sof_message_reader.h>

#define GOOGLE_RTC_AUDIO_PROCESSING_FREQENCY_TO_PERIOD_FRAMES 100
#define GOOGLE_RTC_NUM_INPUT_PINS 2
#define GOOGLE_RTC_NUM_OUTPUT_PINS 1

#if CONFIG_COMP_GOOGLE_RTC_USE_32_BIT_FLOAT_API
#define BUF_TYPE float
#else
#define BUF_TYPE int16_t
#endif

LOG_MODULE_REGISTER(google_rtc_audio_processing, CONFIG_SOF_LOG_LEVEL);

/* b780a0a6-269f-466f-b477-23dfa05af758 */
DECLARE_SOF_RT_UUID("google-rtc-audio-processing", google_rtc_audio_processing_uuid,
					0xb780a0a6, 0x269f, 0x466f, 0xb4, 0x77, 0x23, 0xdf, 0xa0,
					0x5a, 0xf7, 0x58);

DECLARE_TR_CTX(google_rtc_audio_processing_tr, SOF_UUID(google_rtc_audio_processing_uuid),
			   LOG_LEVEL_INFO);

struct google_rtc_audio_processing_comp_data {
	struct sof_ipc4_aec_config config;
	uint32_t num_frames;
	int num_aec_reference_channels;
	int num_capture_channels;
	GoogleRtcAudioProcessingState *state;
	BUF_TYPE *aec_reference_buffer;
	BUF_TYPE *aec_reference_buffer_ptrs[SOF_IPC_MAX_CHANNELS];
	BUF_TYPE *process_buffer;
	BUF_TYPE *process_buffer_ptrs[SOF_IPC_MAX_CHANNELS];
	uint8_t *memory_buffer;
	struct comp_data_blob_handler *tuning_handler;
	bool reconfigure;
	int aec_reference_source;
	int raw_microphone_source;
	uint64_t perf;
};

void *GoogleRtcMalloc(size_t size)
{
	return rballoc(0, SOF_MEM_CAPS_RAM, size);
}

void GoogleRtcFree(void *ptr)
{
	return rfree(ptr);
}

static int google_rtc_audio_processing_reconfigure(struct processing_module *mod)
{
	struct google_rtc_audio_processing_comp_data *cd = module_get_private_data(mod);
	struct comp_dev *dev = mod->dev;
	uint8_t *config;
	size_t size;
	int ret;

	comp_dbg(dev, "google_rtc_audio_processing_reconfigure()");

	if (!comp_is_current_data_blob_valid(cd->tuning_handler) &&
	    !comp_is_new_data_blob_available(cd->tuning_handler)) {
		/*
		 * The data blob hasn't been available once so far.
		 *
		 * This looks redundant since the same check will be done in
		 * comp_get_data_blob() below. But without this early return,
		 * hundreds of warn message lines are produced per second by
		 * comp_get_data_blob() calls until the data blob is arrived.
		 */
		return 0;
	}

	config = comp_get_data_blob(cd->tuning_handler, &size, NULL);
	if (size == 0) {
		/* No data to be handled */
		return 0;
	}

	if (!config) {
		comp_err(dev, "google_rtc_audio_processing_reconfigure(): Tuning config not set");
		return -EINVAL;
	}

	comp_info(dev, "google_rtc_audio_processing_reconfigure(): New tuning config %p (%zu bytes)",
		  config, size);

	cd->reconfigure = false;

	uint8_t *google_rtc_audio_processing_config;
	size_t google_rtc_audio_processing_config_size;
	int num_capture_input_channels;
	int num_capture_output_channels;
	float aec_reference_delay;
	float mic_gain;
	bool google_rtc_audio_processing_config_present;
	bool num_capture_input_channels_present;
	bool num_capture_output_channels_present;
	bool aec_reference_delay_present;
	bool mic_gain_present;

	GoogleRtcAudioProcessingParseSofConfigMessage(config, size,
						      &google_rtc_audio_processing_config,
						      &google_rtc_audio_processing_config_size,
						      &num_capture_input_channels,
						      &num_capture_output_channels,
						      &aec_reference_delay,
						      &mic_gain,
						      &google_rtc_audio_processing_config_present,
						      &num_capture_input_channels_present,
						      &num_capture_output_channels_present,
						      &aec_reference_delay_present,
						      &mic_gain_present);

	if (google_rtc_audio_processing_config_present) {
		comp_info(dev,
			  "google_rtc_audio_processing_reconfigure(): Applying config of size %zu bytes",
			  google_rtc_audio_processing_config_size);

		ret = GoogleRtcAudioProcessingReconfigure(cd->state,
							  google_rtc_audio_processing_config,
							  google_rtc_audio_processing_config_size);
		if (ret) {
			comp_err(dev, "GoogleRtcAudioProcessingReconfigure failed: %d",
				 ret);
			return ret;
		}
	}

	if (num_capture_input_channels_present || num_capture_output_channels_present) {
		if (num_capture_input_channels_present && num_capture_output_channels_present) {
			if (num_capture_input_channels != num_capture_output_channels) {
				comp_err(dev, "GoogleRtcAudioProcessingReconfigure failed: unsupported channel counts");
				return -EINVAL;
			}
			cd->num_capture_channels = num_capture_input_channels;
		} else if (num_capture_input_channels_present) {
			cd->num_capture_channels = num_capture_output_channels;
		} else {
			cd->num_capture_channels = num_capture_output_channels;
		}
		comp_info(dev,
			  "google_rtc_audio_processing_reconfigure(): Applying num capture channels %d",
			  cd->num_capture_channels);


		ret = GoogleRtcAudioProcessingSetStreamFormats(cd->state,
							       CONFIG_COMP_GOOGLE_RTC_AUDIO_PROCESSING_SAMPLE_RATE_HZ,
							       cd->num_capture_channels,
							       cd->num_capture_channels,
							       CONFIG_COMP_GOOGLE_RTC_AUDIO_PROCESSING_SAMPLE_RATE_HZ,
							       cd->num_aec_reference_channels);

		if (ret) {
			comp_err(dev, "GoogleRtcAudioProcessingSetStreamFormats failed: %d",
				 ret);
			return ret;
		}
	}

	if (aec_reference_delay_present || mic_gain_present) {
		float *capture_headroom_linear_use = NULL;
		float *echo_path_delay_ms_use = NULL;

		if (mic_gain_present) {
			capture_headroom_linear_use = &mic_gain;

			/* Logging of linear headroom, using integer workaround to the broken printout of floats */
			comp_info(dev,
				  "google_rtc_audio_processing_reconfigure(): Applying capture linear headroom: %d.%d",
				  (int)mic_gain, (int)(100 * mic_gain) - 100 * ((int)mic_gain));
		}
		if (aec_reference_delay_present) {
			echo_path_delay_ms_use = &aec_reference_delay;

			/* Logging of delay, using integer workaround to the broken printout of floats */
			comp_info(dev,
				  "google_rtc_audio_processing_reconfigure(): Applying aec reference delay: %d.%d",
				  (int)aec_reference_delay,
				  (int)(100 * aec_reference_delay) -
				  100 * ((int)aec_reference_delay));
		}

		ret = GoogleRtcAudioProcessingParameters(cd->state,
							 capture_headroom_linear_use,
							 echo_path_delay_ms_use);

		if (ret) {
			comp_err(dev, "GoogleRtcAudioProcessingParameters failed: %d",
				 ret);
			return ret;
		}
	}

	return 0;
}

static int google_rtc_audio_processing_set_config(struct processing_module *mod, uint32_t param_id,
						  enum module_cfg_fragment_position pos,
						  uint32_t data_offset_size,
						  const uint8_t *fragment,
						  size_t fragment_size, uint8_t *response,
						  size_t response_size)
{
	struct google_rtc_audio_processing_comp_data *cd = module_get_private_data(mod);
	int ret;

	switch (param_id) {
	case SOF_IPC4_SWITCH_CONTROL_PARAM_ID:
	case SOF_IPC4_ENUM_CONTROL_PARAM_ID:
		comp_err(mod->dev, "google_rtc_audio_processing_ctrl_set_data(): Only binary controls supported");
		return -EINVAL;
	}

	ret = comp_data_blob_set(cd->tuning_handler, pos, data_offset_size,
				 fragment, fragment_size);
	if (ret)
		return ret;

	/* Accept the new blob immediately so that userspace can write
	 * the control in quick succession without error.
	 * This ensures the last successful control write from userspace
	 * before prepare/copy is applied.
	 * The config blob is not referenced after reconfigure() returns
	 * so it is safe to call comp_get_data_blob here which frees the
	 * old blob. This assumes cmd() and prepare()/copy() cannot run
	 * concurrently which is the case when there is no preemption.
	 *
	 * Note from review: A race condition is possible and should be
	 * further investigated and fixed.
	 */
	if (comp_is_new_data_blob_available(cd->tuning_handler)) {
		comp_get_data_blob(cd->tuning_handler, NULL, NULL);
		cd->reconfigure = true;
	}

	return 0;
}

static int google_rtc_audio_processing_get_config(struct processing_module *mod,
						  uint32_t param_id, uint32_t *data_offset_size,
						  uint8_t *fragment, size_t fragment_size)
{
	comp_err(mod->dev, "google_rtc_audio_processing_ctrl_get_config(): Not supported");
	return -EINVAL;
}

static int google_rtc_audio_processing_init(struct processing_module *mod)
{
	struct module_data *md = &mod->priv;
	struct comp_dev *dev = mod->dev;
	struct google_rtc_audio_processing_comp_data *cd;
	int ret;
	int channel;
	size_t buf_size;

	comp_info(dev, "google_rtc_audio_processing_init()");

	/* Create private component data */
	cd = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM, sizeof(*cd));
	if (!cd) {
		ret = -ENOMEM;
		goto fail;
	}

	md->private = cd;

	struct module_config *cfg = &md->cfg;
	const struct ipc4_base_module_extended_cfg *base_cfg = md->cfg.init_data;
	struct ipc4_input_pin_format reference_fmt, output_fmt;
	size_t in_fmt_size = sizeof(struct ipc4_input_pin_format);
	size_t out_fmt_size = sizeof(struct ipc4_output_pin_format);
	size_t size;

	cd->config.base_cfg = base_cfg->base_cfg;

	/* Copy the reference format from input pin 1 format */
	memcpy_s(&reference_fmt, in_fmt_size,
		 &base_cfg->base_cfg_ext.pin_formats[in_fmt_size], in_fmt_size);
	memcpy_s(&output_fmt, in_fmt_size,
		 &base_cfg->base_cfg_ext.pin_formats[in_fmt_size * GOOGLE_RTC_NUM_INPUT_PINS],
		 in_fmt_size);

	cd->config.reference_fmt = reference_fmt.audio_fmt;
	cd->config.output_fmt = output_fmt.audio_fmt;

	/* save the base config extension */
	size = base_cfg->base_cfg_ext.nb_input_pins * in_fmt_size +
		base_cfg->base_cfg_ext.nb_output_pins * out_fmt_size;

	md->cfg.basecfg_ext = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM,
				      sizeof(struct ipc4_base_module_cfg_ext) + size);
	if (!md->cfg.basecfg_ext) {
		ret = -ENOMEM;
		goto fail;
	}

	memcpy_s(md->cfg.basecfg_ext, size + sizeof(struct ipc4_base_module_cfg_ext),
		 &base_cfg->base_cfg_ext,
		 size + sizeof(struct ipc4_base_module_cfg_ext));

	cd->tuning_handler = comp_data_blob_handler_new(dev);
	if (!cd->tuning_handler) {
		ret = -ENOMEM;
		goto fail;
	}

	cd->num_aec_reference_channels = cd->config.reference_fmt.channels_count;
	cd->num_capture_channels = mod->priv.cfg.base_cfg.audio_fmt.channels_count;
	cd->num_frames = CONFIG_COMP_GOOGLE_RTC_AUDIO_PROCESSING_SAMPLE_RATE_HZ /
		GOOGLE_RTC_AUDIO_PROCESSING_FREQENCY_TO_PERIOD_FRAMES;

	if (CONFIG_COMP_GOOGLE_RTC_AUDIO_PROCESSING_MEMORY_BUFFER_SIZE_BYTES > 0) {
		cd->memory_buffer = rballoc(0, SOF_MEM_CAPS_RAM,
					    CONFIG_COMP_GOOGLE_RTC_AUDIO_PROCESSING_MEMORY_BUFFER_SIZE_BYTES *
					    sizeof(cd->memory_buffer[0]));
		if (!cd->memory_buffer) {
			comp_err(dev, "google_rtc_audio_processing_init: failed to allocate memory buffer");
			ret = -ENOMEM;
			goto fail;
		}

		GoogleRtcAudioProcessingAttachMemoryBuffer(cd->memory_buffer, CONFIG_COMP_GOOGLE_RTC_AUDIO_PROCESSING_MEMORY_BUFFER_SIZE_BYTES);
	}

	cd->state = GoogleRtcAudioProcessingCreateWithConfig(CONFIG_COMP_GOOGLE_RTC_AUDIO_PROCESSING_SAMPLE_RATE_HZ,
							     cd->num_capture_channels,
							     cd->num_capture_channels,
							     CONFIG_COMP_GOOGLE_RTC_AUDIO_PROCESSING_SAMPLE_RATE_HZ,
							     cd->num_aec_reference_channels,
							     /*config=*/NULL, /*config_size=*/0);

	if (!cd->state) {
		comp_err(dev, "Failed to initialized GoogleRtcAudioProcessing");
		ret = -EINVAL;
		goto fail;
	}

	float capture_headroom_linear = CONFIG_COMP_GOOGLE_RTC_AUDIO_PROCESSING_MIC_HEADROOM_LINEAR;
	float echo_path_delay_ms = CONFIG_COMP_GOOGLE_RTC_AUDIO_PROCESSING_ECHO_PATH_DELAY_MS;
	ret = GoogleRtcAudioProcessingParameters(cd->state,
						 &capture_headroom_linear,
						 &echo_path_delay_ms);

	if (ret < 0) {
		comp_err(dev, "Failed to apply GoogleRtcAudioProcessingParameters");
		goto fail;
	}

	buf_size = cd->num_frames * cd->num_capture_channels * sizeof(cd->process_buffer[0]);
	comp_dbg(dev, "Allocating process_buffer of size %u", buf_size);
	cd->process_buffer = rballoc(0, SOF_MEM_CAPS_RAM, buf_size);
	if (!cd->process_buffer) {
		comp_err(dev, "Allocating process_buffer failure");
		ret = -EINVAL;
		goto fail;
	}
	bzero(cd->process_buffer, buf_size);
	for (channel = 0; channel < cd->num_capture_channels; channel++)
		cd->process_buffer_ptrs[channel] = &cd->process_buffer[channel * cd->num_frames];

	buf_size = cd->num_frames * sizeof(cd->aec_reference_buffer[0]) *
			cd->num_aec_reference_channels;
	comp_dbg(dev, "Allocating aec_reference_buffer of size %u", buf_size);
	cd->aec_reference_buffer = rballoc(0, SOF_MEM_CAPS_RAM, buf_size);
	if (!cd->aec_reference_buffer) {
		comp_err(dev, "Allocating aec_reference_buffer failure");
		ret = -ENOMEM;
		goto fail;
	}
	bzero(cd->aec_reference_buffer, buf_size);
	for (channel = 0; channel < cd->num_aec_reference_channels; channel++)
		cd->aec_reference_buffer_ptrs[channel] =
			&cd->aec_reference_buffer[channel * cd->num_frames];

	/* comp_is_new_data_blob_available always returns false for the first
	 * control write with non-empty config. The first non-empty write may
	 * happen after prepare (e.g. during copy). Default to true so that
	 * copy keeps checking until a non-empty config is applied.
	 */
	cd->reconfigure = true;

	/* Mic and reference, needed for audio stream type copy module client */
	mod->max_sources = 2;

	comp_dbg(dev, "google_rtc_audio_processing_init(): Ready");
	return 0;

fail:
	comp_err(dev, "google_rtc_audio_processing_init(): Failed");
	if (cd) {
		rfree(cd->aec_reference_buffer);
		if (cd->state) {
			GoogleRtcAudioProcessingFree(cd->state);
		}
		GoogleRtcAudioProcessingDetachMemoryBuffer();
		rfree(cd->memory_buffer);
		rfree(md->cfg.basecfg_ext);
		rfree(cd->process_buffer);
		comp_data_blob_handler_free(cd->tuning_handler);
		rfree(cd);
	}

	return ret;
}

static int google_rtc_audio_processing_free(struct processing_module *mod)
{
	struct google_rtc_audio_processing_comp_data *cd = module_get_private_data(mod);
	struct module_data *md = &mod->priv;

	comp_dbg(mod->dev, "google_rtc_audio_processing_free()");

	GoogleRtcAudioProcessingFree(cd->state);
	cd->state = NULL;
	rfree(cd->aec_reference_buffer);
	GoogleRtcAudioProcessingDetachMemoryBuffer();
	rfree(cd->memory_buffer);
	rfree(cd->process_buffer);
	comp_data_blob_handler_free(cd->tuning_handler);
	rfree(md->cfg.basecfg_ext);
	rfree(cd);
	return 0;
}

static int google_rtc_audio_processing_prepare(struct processing_module *mod,
					       struct sof_source **sources,
					       int num_of_sources,
					       struct sof_sink **sinks,
					       int num_of_sinks)
{
	struct comp_dev *dev = mod->dev;
	struct google_rtc_audio_processing_comp_data *cd = module_get_private_data(mod);
	struct list_item *source_buffer_list_item;
	unsigned int aec_channels = 0, frame_fmt, rate;
	int microphone_stream_channels = 0;
	int output_stream_channels;
	int ret;
	int i = 0;

	comp_info(dev, "google_rtc_audio_processing_prepare()");

	if (num_of_sources != GOOGLE_RTC_NUM_INPUT_PINS) {
		comp_err(dev, "Expecting 2 sources - ref and mic, got %u", num_of_sources);
		return -EINVAL;
	}

	if (num_of_sinks != GOOGLE_RTC_NUM_OUTPUT_PINS) {
		comp_err(dev, "Expecting 1 sink, got %u", num_of_sinks);
		return -EINVAL;
	}

	/* searching for stream and feedback source buffers */
	for (i = 0; i < num_of_sources; i++) {

		if (IPC4_SINK_QUEUE_ID(source_get_id(sources[i])) == SOF_AEC_FEEDBACK_QUEUE_ID) {

			cd->aec_reference_source = i;
			aec_channels = source_get_channels(sources[i]);
			comp_dbg(dev, "reference index = %d, channels = %d", i, aec_channels);
		} else {
			cd->raw_microphone_source = i;
			microphone_stream_channels = source_get_channels(sources[i]);
			comp_dbg(dev, "microphone index = %d, channels = %d", i,
				 microphone_stream_channels);
		}
		source_set_alignment_constants(sources[i], 1, 1);
	}

	/* enforce format on pins */
	ipc4_update_source_format(sources[cd->aec_reference_source],
			&cd->config.reference_fmt);
	ipc4_update_source_format(sources[cd->raw_microphone_source],
			&mod->priv.cfg.base_cfg.audio_fmt);
	ipc4_update_sink_format(sinks[0], &mod->priv.cfg.base_cfg.audio_fmt);

	/* On some platform the playback output is left right left right due to a crossover
	 * later on the signal processing chain. That makes the aec_reference be 4 channels
	 * and the AEC should only use the 2 first.
	 */
	if (cd->num_aec_reference_channels > aec_channels) {
		comp_err(dev, "unsupported number of AEC reference channels: %d",
			 aec_channels);
		return -EINVAL;
	}

	sink_set_alignment_constants(sinks[0], 1, 1);
	frame_fmt = sink_get_frm_fmt(sinks[0]);
	rate = sink_get_rate(sinks[0]);
	output_stream_channels = sink_get_channels(sinks[0]);

	if (cd->num_capture_channels > microphone_stream_channels) {
		comp_err(dev, "unsupported number of microphone channels: %d",
			 microphone_stream_channels);
		return -EINVAL;
	}

	if (cd->num_capture_channels > output_stream_channels) {
		comp_err(dev, "unsupported number of output channels: %d",
			 output_stream_channels);
		return -EINVAL;
	}

	switch (frame_fmt) {
#if CONFIG_FORMAT_S16LE
	case SOF_IPC_FRAME_S16_LE:
		break;
#endif /* CONFIG_FORMAT_S16LE */
	default:
		comp_err(dev, "unsupported data format: %d", frame_fmt);
		return -EINVAL;
	}

	if (rate != CONFIG_COMP_GOOGLE_RTC_AUDIO_PROCESSING_SAMPLE_RATE_HZ) {
		comp_err(dev, "unsupported samplerate: %d", rate);
		return -EINVAL;
	}

	/* check IBS/OBS in streams */
	if (cd->num_frames * source_get_frame_bytes(sources[cd->raw_microphone_source]) !=
	    source_get_min_available(sources[cd->raw_microphone_source])) {
		comp_err(dev, "Incorrect IBS on microphone source: %d, expected %u",
			  source_get_min_available(sources[cd->raw_microphone_source]),
			  cd->num_frames *
				 source_get_frame_bytes(sources[cd->raw_microphone_source]));
		return -EINVAL;
	}
	if (cd->num_frames * sink_get_frame_bytes(sinks[0]) !=
	    sink_get_min_free_space(sinks[0])) {
		comp_err(dev, "Incorrect OBS on sink :%d, expected %u",
			  sink_get_min_free_space(sinks[0]),
			  cd->num_frames * sink_get_frame_bytes(sinks[0]));
		return -EINVAL;
	}
	if (cd->num_frames * source_get_frame_bytes(sources[cd->aec_reference_source]) !=
	    source_get_min_available(sources[cd->aec_reference_source])) {
		comp_err(dev, "Incorrect IBS on reference source: %d, expected %u",
			  source_get_min_available(sources[cd->aec_reference_source]),
			  cd->num_frames *
				source_get_frame_bytes(sources[cd->aec_reference_source]));
		return -EINVAL;
	}

	/* Blobs sent during COMP_STATE_READY is assigned to blob_handler->data
	 * directly, so comp_is_new_data_blob_available always returns false.
	 */
	ret = google_rtc_audio_processing_reconfigure(mod);
	if (ret)
		return ret;

	comp_dbg(dev, "google_rtc_audio_processing_prepare() success");
	return 0;
}

static int google_rtc_audio_processing_reset(struct processing_module *mod)
{
	comp_dbg(mod->dev, "google_rtc_audio_processing_reset()");

	return 0;
}

static inline int16_t convert_google_aec_format_to_int16(BUF_TYPE data)
{
#if CONFIG_COMP_GOOGLE_RTC_USE_32_BIT_FLOAT_API
	const xtfloat ratio = 2 << 14;
	xtfloat x0 = data;
	xtfloat x1;
	int16_t x;

	x1 = XT_MUL_S(x0, ratio);
	x = XT_TRUNC_S(x1, 0);

	return x;
#else /* CONFIG_COMP_GOOGLE_RTC_USE_32_BIT_FLOAT_API */
	return data;
#endif
}

static inline BUF_TYPE convert_int16_to_google_aec_format(int16_t data)
{
#if CONFIG_COMP_GOOGLE_RTC_USE_32_BIT_FLOAT_API
	const xtfloat ratio = 2 << 14;
	xtfloat x0 = data;
	float x;

	x = XT_DIV_S(x0, ratio);

	return x;
#else /* CONFIG_COMP_GOOGLE_RTC_USE_32_BIT_FLOAT_API */
	return data;
#endif
}

/* todo CONFIG_FORMAT_S32LE */
static int google_rtc_audio_processing_process(struct processing_module *mod,
					       struct sof_source **sources, int num_of_sources,
					       struct sof_sink **sinks, int num_of_sinks)
{

	int ret;
	uint16_t const *src;
	uint8_t const *src_buf_start;
	uint8_t const *src_buf_end;
	size_t src_buf_size;

	uint16_t const *ref;
	uint8_t const *ref_buf_start;
	uint8_t const *ref_buf_end;
	size_t ref_buf_size;

	uint16_t *dst;
	uint8_t *dst_buf_start;
	uint8_t *dst_buf_end;
	size_t dst_buf_size;

	size_t num_of_bytes_to_process;
	int num_samples_remaining;
	int num_frames_remaining;
	int channel;
	int nmax;

	struct sof_source *ref_stream, *src_stream;
	struct sof_sink *dst_stream;

	struct google_rtc_audio_processing_comp_data *cd = module_get_private_data(mod);

	cd->perf = sof_cycle_get_64();


	if (cd->reconfigure) {
		ret = google_rtc_audio_processing_reconfigure(mod);
		if (ret)
			return ret;
	}

	src_stream = sources[cd->raw_microphone_source];
	ref_stream = sources[cd->aec_reference_source];
	dst_stream = sinks[0];

	num_of_bytes_to_process = cd->num_frames * source_get_frame_bytes(ref_stream);
	ret = source_get_data(ref_stream, num_of_bytes_to_process, (const void **)&ref,
			      (const void **)&ref_buf_start, &ref_buf_size);

	/* problems here are extremely unlikely, as it has been checked that
	 * the buffer contains enough data
	 */
	assert(!ret);
	ref_buf_end = ref_buf_start + ref_buf_size;


	/* can't use source_get_data_frames_available as number of available data may have changed
	 * other processes may put some data to the buffer
	 */
	num_samples_remaining = num_of_bytes_to_process * source_get_channels(ref_stream) /
				source_get_frame_bytes(ref_stream);

	/* de-interlace ref buffer, convert it to float */
	for (int i = 0; i < cd->num_frames; i++) {
		for (channel = 0; channel < cd->num_aec_reference_channels; ++channel) {
			cd->aec_reference_buffer_ptrs[channel][i] =
					convert_int16_to_google_aec_format(ref[channel]);
		}
		ref += cd->num_aec_reference_channels;
		if ((void *)ref >= (void *)ref_buf_end)
			ref = (void *)ref_buf_start;
	}

#if CONFIG_COMP_GOOGLE_RTC_USE_32_BIT_FLOAT_API
	GoogleRtcAudioProcessingAnalyzeRender_float32(
			cd->state,
			(const float **)cd->aec_reference_buffer_ptrs);
#else
	GoogleRtcAudioProcessingAnalyzeRender_int16(
			cd->state,
			(const int16_t *)cd->aec_reference_buffer);
#endif
	uint32_t diff1  = sof_cycle_get_64() - cd->perf;
	
	source_release_data(ref_stream, num_of_bytes_to_process);

	/* process main stream - de interlace and convert */
	num_of_bytes_to_process = cd->num_frames * source_get_frame_bytes(src_stream);
	ret = source_get_data(src_stream, num_of_bytes_to_process, (const void **)&src,
			      (const void **)&src_buf_start,  &src_buf_size);
	assert(!ret);
	src_buf_end = src_buf_start + src_buf_size;

	for (int i = 0; i < cd->num_frames; i++) {
		for (channel = 0; channel < cd->num_capture_channels; channel++)
			cd->process_buffer_ptrs[channel][i] =
					convert_int16_to_google_aec_format(src[channel]);

		src += cd->num_capture_channels;
		if ((void *)src >= (void *)src_buf_end)
			src = (void *)src_buf_start;
	}

	source_release_data(src_stream, num_of_bytes_to_process);

	/* call the library, use same in/out buffers */
#if CONFIG_COMP_GOOGLE_RTC_USE_32_BIT_FLOAT_API
	GoogleRtcAudioProcessingProcessCapture_float32(cd->state,
						       (const float **)cd->process_buffer_ptrs,
						       cd->process_buffer_ptrs);
#else
	GoogleRtcAudioProcessingProcessCapture_int16(cd->state,
						       (const int16_t *)cd->process_buffer,
						       cd->process_buffer);
#endif

	/* same numnber of bytes to process for output stream as for mic stream */
	ret = sink_get_buffer(dst_stream, num_of_bytes_to_process, (void **)&dst,
			      (void **)&dst_buf_start, &dst_buf_size);
	assert(!ret);
	dst_buf_end = dst_buf_start + dst_buf_size;

	for (int i = 0; i < cd->num_frames; i++) {
		for (channel = 0; channel < cd->num_capture_channels; channel++)
			dst[channel] = convert_google_aec_format_to_int16(
					cd->process_buffer_ptrs[channel][i]);
		dst += cd->num_capture_channels;
		if ((void *)dst >= (void *)dst_buf_end)
			dst = (void *)dst_buf_start;
	}

	sink_commit_buffer(dst_stream, num_of_bytes_to_process);

	uint32_t diff2= sof_cycle_get_64() - cd->perf;
	comp_info(mod->dev, "AEC MODULE FINISH, cycles1 %u cycles2:", diff1, diff2 );

	return 0;
}

static struct module_interface google_rtc_audio_processing_interface = {
	.init  = google_rtc_audio_processing_init,
	.free = google_rtc_audio_processing_free,
	.process = google_rtc_audio_processing_process,
	.prepare = google_rtc_audio_processing_prepare,
	.set_configuration = google_rtc_audio_processing_set_config,
	.get_configuration = google_rtc_audio_processing_get_config,
	.reset = google_rtc_audio_processing_reset,
};

DECLARE_MODULE_ADAPTER(google_rtc_audio_processing_interface,
		       google_rtc_audio_processing_uuid, google_rtc_audio_processing_tr);
SOF_MODULE_INIT(google_rtc_audio_processing,
		sys_comp_module_google_rtc_audio_processing_interface_init);
