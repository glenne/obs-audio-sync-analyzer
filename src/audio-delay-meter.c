/*
Audio Delay Meter - High Quality Correlation
Copyright (C) 2025 

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <media-io/audio-io.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#define BUFFER_SECONDS 5u
#define MIN_WINDOW_MS 200u
#define MAX_WINDOW_MS 3000u
#define DEFAULT_WINDOW_MS 1000u
#define MAX_LAG_MS 1500u
#define MIN_CORR_THRESHOLD 0.6f
#define PRE_EMPHASIS_ALPHA 0.95f

struct delay_meter_data {
	obs_source_t *context;
	obs_source_t *target;
	char *target_name;

	pthread_mutex_t lock;

	float *ref_buffer;
	float *tgt_buffer;
	size_t capacity;
	size_t ref_pos;
	size_t tgt_pos;
	size_t ref_count;
	size_t tgt_count;

	uint32_t sample_rate;
	enum audio_format audio_format;
	uint32_t window_ms;
	uint32_t max_lag_ms;
	bool debug_enabled;

	char *last_delay_text;
	char *last_time_text;
	char *last_notes;
	double last_delay_ms;
	float last_correlation;
	bool ui_update_pending;
	bool last_delay_valid;
};

static size_t next_power_of_2(size_t n)
{
	size_t p = 1;
	while (p < n)
		p <<= 1;
	return p;
}

static size_t ms_to_samples(uint32_t ms, uint32_t sample_rate)
{
	return (size_t)(((uint64_t)ms * sample_rate + 500) / 1000);
}

static void apply_result_ui(void *param);

static void ring_write(float *buffer, size_t capacity, size_t *pos, size_t *count, const float *src, size_t frames)
{
	for (size_t i = 0; i < frames; ++i) {
		buffer[*pos] = src[i];
		*pos = (*pos + 1) % capacity;
		if (*count < capacity)
			(*count)++;
	}
}

static const float *as_float_channel(const uint8_t *const *planes, enum audio_format format)
{
	if (!planes)
		return NULL;

	if (format == AUDIO_FORMAT_FLOAT || format == AUDIO_FORMAT_FLOAT_PLANAR)
		return (const float *)planes[0];

	return NULL;
}

static void apply_pre_emphasis(float *data, size_t frames)
{
	if (frames < 2)
		return;

	float prev = data[0];
	for (size_t i = 1; i < frames; ++i) {
		float next = data[i];
		data[i] = next - PRE_EMPHASIS_ALPHA * prev;
		prev = next;
	}
}

static void apply_hann_window(float *data, size_t frames)
{
	if (frames <= 1)
		return;

	for (size_t i = 0; i < frames; ++i) {
		double w = 0.5 * (1.0 - cos(2.0 * M_PI * (double)i / (frames - 1.0)));
		data[i] *= (float)w;
	}
}

static bool copy_recent(struct delay_meter_data *dm, float **out_ref, float **out_tgt, size_t *out_frames)
{
	bool ok = false;
	float *ref = NULL;
	float *tgt = NULL;

	pthread_mutex_lock(&dm->lock);

	size_t available = dm->ref_count < dm->tgt_count ? dm->ref_count : dm->tgt_count;
	const size_t window_frames = ms_to_samples(dm->window_ms, dm->sample_rate);
	const size_t frames = available < window_frames ? available : window_frames;

	if (frames < 1024)
		goto unlock;

	ref = bmalloc(frames * sizeof(float));
	tgt = bmalloc(frames * sizeof(float));
	if (!ref || !tgt)
		goto unlock;

	const size_t ref_start = (dm->ref_pos + dm->capacity - frames) % dm->capacity;
	const size_t tgt_start = (dm->tgt_pos + dm->capacity - frames) % dm->capacity;

	for (size_t i = 0; i < frames; ++i) {
		ref[i] = dm->ref_buffer[(ref_start + i) % dm->capacity];
		tgt[i] = dm->tgt_buffer[(tgt_start + i) % dm->capacity];
	}

	apply_pre_emphasis(ref, frames);
	apply_pre_emphasis(tgt, frames);
	apply_hann_window(ref, frames);
	apply_hann_window(tgt, frames);

	*out_ref = ref;
	*out_tgt = tgt;
	*out_frames = frames;
	ok = true;

unlock:
	pthread_mutex_unlock(&dm->lock);

	if (!ok) {
		bfree(ref);
		bfree(tgt);
	}

	return ok;
}

static bool estimate_delay(struct delay_meter_data *dm, double *delay_ms_out, double *corr_out)
{
	float *ref = NULL;
	float *tgt = NULL;
	size_t frames = 0;

	if (!copy_recent(dm, &ref, &tgt, &frames)) {
		blog(LOG_INFO, "[ADM]  copy_recent failed");
		return false;
	}

	if (dm->debug_enabled) {
		blog(LOG_INFO, "[ADM DEBUG] frames=%zu max_lag=%d", frames,
		     (int)ms_to_samples(dm->max_lag_ms, dm->sample_rate));
	}

	int max_lag = (int)ms_to_samples(dm->max_lag_ms, dm->sample_rate);
	max_lag = max_lag > (int)frames / 2 ? (int)frames / 2 : max_lag;

	double best_corr = -1.0;
	int best_lag = 0;

	// Pre-compute means
	double ref_mean = 0.0, tgt_mean = 0.0;
	for (size_t i = 0; i < frames; ++i) {
		ref_mean += ref[i];
		tgt_mean += tgt[i];
	}
	ref_mean /= frames;
	tgt_mean /= frames;

	if (dm->debug_enabled) {
		blog(LOG_INFO, "[ADM DEBUG] ref_mean=%.6f tgt_mean=%.6f", ref_mean, tgt_mean);
	}

	int lag_count = 0;
	for (int lag = -max_lag; lag <= max_lag; ++lag) {
		int abs_lag = abs(lag);
		size_t overlap = frames - abs_lag;
		if (overlap < 1024)
			continue; // ✅ FIXED: 1024 minimum

		double sum_ab = 0.0, sum_a2 = 0.0, sum_b2 = 0.0;

		size_t start_a = lag >= 0 ? 0 : abs_lag;
		size_t start_b = lag >= 0 ? abs_lag : 0;

		for (size_t i = 0; i < overlap; ++i) {
			double da = ref[start_a + i] - ref_mean;
			double db = tgt[start_b + i] - tgt_mean;
			sum_ab += da * db;
			sum_a2 += da * da;
			sum_b2 += db * db;
		}

		double denom = sqrt(sum_a2 * sum_b2);
		if (denom < 1e-8)
			continue;

		double corr = sum_ab / denom;
		lag_count++;

		// ✅ DEBUG: Log BEST 5 correlations
		if (corr > best_corr) {
			best_corr = corr;
			best_lag = lag;
			// if (dm->debug_enabled && best_corr > 0.5) { // Only log good ones
			// 	blog(LOG_INFO, "[ADM DEBUG] lag=%d corr=%.4f", best_lag, best_corr);
			// }
		}
	}

	bfree(ref);
	bfree(tgt);

	if (dm->debug_enabled) {
		blog(LOG_INFO, "[ADM DEBUG] FINAL: best_corr=%.4f best_lag=%d lag_count=%d", best_corr, best_lag,
		     lag_count);
	}

	if (best_corr < MIN_CORR_THRESHOLD) {
		blog(LOG_INFO, "[ADM]  CORRELATION TOO LOW: %.4f < %.1f", best_corr, MIN_CORR_THRESHOLD);
		return false;
	}

	*delay_ms_out = ((double)best_lag * 1000.0) / (double)dm->sample_rate;
	*corr_out = best_corr;
	return true;
}

static void set_result(struct delay_meter_data *dm, const char *delay_text, const char *notes_text)
{
	if (!dm || !delay_text)
		return;

	blog(LOG_INFO, "[ADM DEBUG] Set Result=%s", delay_text);

	time_t now = time(NULL);
	struct tm *tm_info = localtime(&now);
	char timestamp[10];
	strftime(timestamp, sizeof(timestamp), "%H:%M:%S", tm_info);

	bool queue = false;
	pthread_mutex_lock(&dm->lock);
	bfree(dm->last_delay_text);
	bfree(dm->last_time_text);
	bfree(dm->last_notes);
	dm->last_delay_text = bstrdup(delay_text);
	dm->last_time_text = bstrdup(timestamp);
	dm->last_notes = notes_text ? bstrdup(notes_text) : bstrdup("");
	dm->last_delay_valid = true;
	if (!dm->ui_update_pending) {
		dm->ui_update_pending = true;
		queue = true;
	}
	pthread_mutex_unlock(&dm->lock);

	if (queue)
		obs_queue_task(OBS_TASK_UI, apply_result_ui, dm, false);
}

static void apply_result_ui(void *param)
{
	struct delay_meter_data *dm = param;
	if (!dm || !dm->context)
		return;

	blog(LOG_INFO, "[ADM DEBUG] Apply Result");
	char *result = NULL;
	char *time_txt = NULL;
	char *notes_txt = NULL;
	pthread_mutex_lock(&dm->lock);
	dm->ui_update_pending = false;
	if (dm->last_delay_text)
		result = bstrdup(dm->last_delay_text);
	if (dm->last_time_text)
		time_txt = bstrdup(dm->last_time_text);
	if (dm->last_notes)
		notes_txt = bstrdup(dm->last_notes);
	pthread_mutex_unlock(&dm->lock);

	if (!result)
		return;

	obs_data_t *settings = obs_source_get_settings(dm->context);
	if (settings) {
		obs_data_set_string(settings, "time_result", time_txt ? time_txt : "");
		obs_data_set_string(settings, "delay_result", result);
		obs_data_set_string(settings, "notes", notes_txt ? notes_txt : "");
		obs_source_update(dm->context, settings);
		obs_data_release(settings);
	}

	bfree(result);
	bfree(time_txt);
	bfree(notes_txt);
}

static void capture_target(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(muted);

	struct delay_meter_data *dm = param;
	if (!dm)
		return;

	const float *samples = as_float_channel((const uint8_t *const *)audio->data, dm->audio_format);
	if (!samples)
		return;

	pthread_mutex_lock(&dm->lock);
	ring_write(dm->tgt_buffer, dm->capacity, &dm->tgt_pos, &dm->tgt_count, samples, audio->frames);
	// blog(LOG_INFO, "[DEBUG] Target buffer: %zu frames", dm->tgt_count);
	// if (audio->frames > 0) {
	//     blog(LOG_DEBUG, "[ADM] Target: %u → %zu", audio->frames, dm->tgt_count);
	// }
	pthread_mutex_unlock(&dm->lock);
}

static void connect_target(struct delay_meter_data *dm, const char *name, bool log_missing)
{
	if (dm->target && dm->target_name && name && strcmp(dm->target_name, name) == 0)
		return;

	if (dm->target) {
		blog(LOG_INFO, "Releasing prior audio callback");
		obs_source_remove_audio_capture_callback(dm->target, capture_target, dm);
		obs_source_release(dm->target);
		dm->target = NULL;
	}

	blog(LOG_INFO, "[ADM Info] Connecting to %s", name);

	if (dm->target_name) {
		bfree(dm->target_name);
	}
	dm->target_name = name && *name ? bstrdup(name) : NULL;

	if (!dm->target_name)
		return;

	obs_source_t *src = obs_get_source_by_name(dm->target_name);
	if (!src) {
		if (log_missing) {
			blog(LOG_INFO, "[ADM] Target '%s' not yet available", dm->target_name);
		}
		return;
	}

	dm->target = src;
	obs_source_add_audio_capture_callback(dm->target, capture_target, dm);
}

static void delay_meter_update(void *data, obs_data_t *settings)
{
	struct delay_meter_data *dm = data;
	if (!dm)
		return;

	blog(LOG_INFO, "[ADM TRACE] Meter Update");
	dm->window_ms = (uint32_t)obs_data_get_int(settings, "window_ms");
	dm->max_lag_ms = (uint32_t)obs_data_get_int(settings, "max_lag_ms");
	dm->debug_enabled = obs_data_get_bool(settings, "debug_enabled");

	const char *target = obs_data_get_string(settings, "target_source");
	connect_target(dm, target, true);
}

static bool perform_measure(struct delay_meter_data *dm)
{
	if (dm->debug_enabled) {
		blog(LOG_INFO, "[ADM DIAG] Starting measurement");
	}

	if (!dm || !dm->target) {
		set_result(dm, "No target source", "Select a delayed source to compare against.");
		dm->last_delay_valid = false;
		return false;
	}

	pthread_mutex_lock(&dm->lock);
	if (dm->debug_enabled) {
		blog(LOG_INFO, "[ADM DIAG] ref=%zu tgt=%zu", dm->ref_count, dm->tgt_count);
	}
	bool enough = dm->ref_count >= 1024 && dm->tgt_count >= 1024;
	pthread_mutex_unlock(&dm->lock);

	if (!enough) {
		set_result(dm, "Buffers too small",
			   "Need more buffered audio from both reference and target before measuring.");
		dm->last_delay_valid = false;
		return false;
	}

	double delay_ms = 0.0;
	double corr = 0.0;

	blog(LOG_INFO, "[ADM] Estimating Audio Delay");
	if (estimate_delay(dm, &delay_ms, &corr)) {
		char *target_name_copy = NULL;

		pthread_mutex_lock(&dm->lock);
		target_name_copy = dm->target_name ? bstrdup(dm->target_name) : NULL;
		pthread_mutex_unlock(&dm->lock);

		char buffer[256];
		snprintf(buffer, sizeof(buffer), "%+6.1f ms (correlation: %.3f)", delay_ms, corr);
		char notes_buf[256];
		if (delay_ms > 0) {
			snprintf(notes_buf, sizeof(notes_buf), "Target '%s' lags reference by %.1f ms",
				 target_name_copy ? target_name_copy : "<target>", delay_ms);
		} else if (delay_ms < 0) {
			snprintf(notes_buf, sizeof(notes_buf), "Target '%s' leads reference by %.1f ms",
				 target_name_copy ? target_name_copy : "<target>", fabs(delay_ms));
		} else {
			snprintf(notes_buf, sizeof(notes_buf), "Target '%s' is aligned with reference",
				 target_name_copy ? target_name_copy : "<target>");
		}

		set_result(dm, buffer, notes_buf);
		blog(LOG_INFO, "[ADM] Result=%s", buffer);
		dm->last_delay_ms = delay_ms;
		dm->last_delay_valid = true;

		bfree(target_name_copy);
		return true;
	}

	set_result(dm, "Insufficient correlation - check audio levels and similarity",
		   "Insufficient correlation; ensure both sources carry similar program audio.");
	dm->last_delay_valid = false;
	return false;
}

// ✅ FIXED: Correct signature - only 3 parameters (void* data)
static bool measure_now_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct delay_meter_data *dm = data;
	/* Always return true so OBS refreshes the properties view even when measurement
	 * fails (e.g., missing target or not enough buffered audio). */
	perform_measure(dm);
	return true;
}

static bool apply_sync_offset_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct delay_meter_data *dm = data;
	if (!dm)
		return true;

	pthread_mutex_lock(&dm->lock);
	bool valid = dm->last_delay_valid;
	double delay_ms = dm->last_delay_ms;
	pthread_mutex_unlock(&dm->lock);

	if (!valid) {
		set_result(dm, " No recent measurement", "Run Measure Now before applying offset.");
		return true;
	}

	obs_source_t *parent = obs_filter_get_parent(dm->context);
	if (!parent) {
		set_result(dm, " No parent source", "Cannot apply offset without a parent source.");
		return true;
	}

	parent = obs_source_get_ref(parent);
	if (!parent) {
		set_result(dm, " Parent unavailable", "Parent source vanished before applying offset.");
		return true;
	}

	int64_t offset_ns = (int64_t)llround(delay_ms * 1000000.0);
	obs_source_set_sync_offset(parent, offset_ns);
	obs_source_release(parent);

	char msg[128];
	snprintf(msg, sizeof(msg), "Applied %+0.1f ms to Sync Offset", delay_ms);
	set_result(dm, msg, "Sync Offset updated on reference source.");
	return true;
}

static struct obs_audio_data *delay_meter_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct delay_meter_data *dm = data;
	if (!dm)
		return audio;

	if (!dm->target && dm->target_name)
		connect_target(dm, dm->target_name, false);

	if (!dm->target) {
		return audio;
	}

	const float *samples = as_float_channel((const uint8_t *const *)audio->data, dm->audio_format);
	if (samples) {
		pthread_mutex_lock(&dm->lock);
		ring_write(dm->ref_buffer, dm->capacity, &dm->ref_pos, &dm->ref_count, samples, audio->frames);
		pthread_mutex_unlock(&dm->lock);
	}
	return audio;
}

static void delay_meter_destroy(void *data)
{
	blog(LOG_INFO, "[ADM Trace] Destroy");
	struct delay_meter_data *dm = data;
	if (!dm)
		return;

	if (dm->target) {
		obs_source_remove_audio_capture_callback(dm->target, capture_target, dm);
		obs_source_release(dm->target);
	}

	pthread_mutex_destroy(&dm->lock);
	bfree(dm->ref_buffer);
	bfree(dm->tgt_buffer);
	bfree(dm->target_name);
	bfree(dm->last_delay_text);
	bfree(dm->last_time_text);
	bfree(dm->last_notes);
	bfree(dm);
}

static void *delay_meter_create(obs_data_t *settings, obs_source_t *context)
{
	struct delay_meter_data *dm = bzalloc(sizeof(*dm));
	dm->context = context;
	pthread_mutex_init(&dm->lock, NULL);

	dm->sample_rate = audio_output_get_sample_rate(obs_get_audio());
	dm->audio_format = AUDIO_FORMAT_FLOAT_PLANAR;

	dm->capacity = ms_to_samples(BUFFER_SECONDS * 1000u, dm->sample_rate);
	dm->ref_buffer = bzalloc(dm->capacity * sizeof(float));
	dm->tgt_buffer = bzalloc(dm->capacity * sizeof(float));
	dm->tgt_pos = dm->ref_pos = 0;
	dm->tgt_count = dm->ref_count = 0;

	dm->last_delay_valid = false;
	dm->debug_enabled = obs_data_get_bool(settings, "debug_enabled");
	dm->window_ms = (uint32_t)obs_data_get_int(settings, "window_ms");
	dm->max_lag_ms = (uint32_t)obs_data_get_int(settings, "max_lag_ms");

	obs_data_set_string(settings, "time_result", "");
	obs_data_set_string(settings, "delay_result", "Ready...");
	obs_data_set_string(settings, "notes", "");

	const char *target = obs_data_get_string(settings, "target_source");
	connect_target(dm, target, true);

	blog(LOG_INFO, "[ADM TRACE] Created");
	return dm;
}

static bool add_audio_sources(void *priv, obs_source_t *src)
{
	obs_property_t *list = priv;
	uint32_t flags = obs_source_get_output_flags(src);
	if (!(flags & OBS_SOURCE_AUDIO))
		return true;

	const char *name = obs_source_get_name(src);
	obs_property_list_add_string(list, name, name);
	return true;
}

static obs_properties_t *delay_meter_properties(void *data)
{
	UNUSED_PARAMETER(data);
	blog(LOG_INFO, "[ADM TRACE] Creating Properties");
	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, "target_source", "Delayed Source", OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(add_audio_sources, list);

	obs_properties_add_int_slider(props, "window_ms", "Analysis Window (ms)", MIN_WINDOW_MS, MAX_WINDOW_MS, 50);
	obs_properties_add_int_slider(props, "max_lag_ms", "Max Lag Search (ms)", 50, MAX_LAG_MS, 25);

	obs_property_t *time_prop = obs_properties_add_text(props, "time_result", "Time", OBS_TEXT_INFO);
	obs_property_set_enabled(time_prop, false);

	obs_property_t *result = obs_properties_add_text(props, "delay_result", "Delay", OBS_TEXT_INFO);
	obs_property_set_enabled(result, false);

	obs_property_t *notes = obs_properties_add_text(props, "notes", "Notes", OBS_TEXT_INFO);
	obs_property_set_enabled(notes, false);

	obs_properties_add_button2(props, "measure_now", "Measure Now", measure_now_clicked, data);
	obs_properties_add_button2(props, "apply_sync_offset", "Apply to Sync Offset", apply_sync_offset_clicked, data);

	obs_properties_add_bool(props, "debug_enabled", "Enable Debug Logging");

	return props;
}

static void delay_meter_defaults(obs_data_t *settings)
{
	blog(LOG_INFO, "[ADM TRACE] Setting Defaults");
	obs_data_set_default_int(settings, "window_ms", DEFAULT_WINDOW_MS);
	obs_data_set_default_int(settings, "max_lag_ms", 500);
	obs_data_set_default_bool(settings, "debug_enabled", false);
	obs_data_set_default_string(settings, "delay_result", "Ready...");
	obs_data_set_default_string(settings, "time_result", "");
	obs_data_set_default_string(settings, "notes", "");
	obs_data_set_default_string(settings, "target_source", "");
}

static const char *delay_meter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Audio Delay Meter";
}

struct obs_source_info delay_meter_filter = {
	.id = "audio_delay_meter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = delay_meter_get_name,
	.create = delay_meter_create,
	.destroy = delay_meter_destroy,
	.update = delay_meter_update,
	.get_properties = delay_meter_properties,
	.get_defaults = delay_meter_defaults,
	.filter_audio = delay_meter_filter_audio,
};
