#include <whisper.h>

#include <obs-module.h>

#include <util/profiler.hpp>

#include "plugin-support.h"
#include "transcription-filter-data.h"
#include "whisper-processing.h"
#include "whisper-utils.h"
#include "transcription-utils.h"

#ifdef _WIN32
#include <fstream>
#define NOMINMAX
#include <Windows.h>
#endif

#include "model-utils/model-find-utils.h"

#include <algorithm>
#include <chrono>
#include <regex>

struct vad_state {
	bool vad_on;
	uint64_t start_ts_offest_ms;
	uint64_t end_ts_offset_ms;
	uint64_t last_partial_segment_end_ts;
};

struct whisper_context *init_whisper_context(const std::string &model_path_in,
					     struct transcription_filter_data *gf)
{
	std::string model_path = model_path_in;

	obs_log(LOG_INFO, "Loading whisper model from %s", model_path.c_str());

	if (std::filesystem::is_directory(model_path)) {
		obs_log(LOG_INFO,
			"Model path is a directory, not a file, looking for .bin file in folder");
		// look for .bin file
		const std::string model_bin_file = find_bin_file_in_folder(model_path);
		if (model_bin_file.empty()) {
			obs_log(LOG_ERROR, "Model bin file not found in folder: %s",
				model_path.c_str());
			return nullptr;
		}
		model_path = model_bin_file;
	}

	whisper_log_set(
		[](enum ggml_log_level level, const char *text, void *user_data) {
			UNUSED_PARAMETER(level);
			struct transcription_filter_data *ctx =
				static_cast<struct transcription_filter_data *>(user_data);
			// remove trailing newline
			char *text_copy = bstrdup(text);
			text_copy[strcspn(text_copy, "\n")] = 0;
			obs_log(ctx->log_level, "Whisper: %s", text_copy);
			bfree(text_copy);
		},
		gf);

	struct whisper_context_params cparams = whisper_context_default_params();
#ifdef LOCALVOCAL_WITH_CUDA
	cparams.use_gpu = true;
	obs_log(LOG_INFO, "Using CUDA GPU for inference, device %d", cparams.gpu_device);
#elif defined(LOCALVOCAL_WITH_HIPBLAS)
	cparams.use_gpu = true;
	obs_log(LOG_INFO, "Using hipBLAS for inference");
#elif defined(__APPLE__)
	cparams.use_gpu = true;
	obs_log(LOG_INFO, "Using Metal/CoreML for inference");
#else
	cparams.use_gpu = false;
	obs_log(LOG_INFO, "Using CPU for inference");
#endif

	cparams.dtw_token_timestamps = gf->enable_token_ts_dtw;
	if (gf->enable_token_ts_dtw) {
		obs_log(LOG_INFO, "DTW token timestamps enabled");
		cparams.dtw_aheads_preset = WHISPER_AHEADS_TINY_EN;
		// cparams.dtw_n_top = 4;
	} else {
		obs_log(LOG_INFO, "DTW token timestamps disabled");
		cparams.dtw_aheads_preset = WHISPER_AHEADS_NONE;
	}

	struct whisper_context *ctx = nullptr;
	try {
#ifdef _WIN32
		// convert model path UTF8 to wstring (wchar_t) for whisper
		int count = MultiByteToWideChar(CP_UTF8, 0, model_path.c_str(),
						(int)model_path.length(), NULL, 0);
		std::wstring model_path_ws(count, 0);
		MultiByteToWideChar(CP_UTF8, 0, model_path.c_str(), (int)model_path.length(),
				    &model_path_ws[0], count);

		// Read model into buffer
		std::ifstream modelFile(model_path_ws, std::ios::binary);
		if (!modelFile.is_open()) {
			obs_log(LOG_ERROR, "Failed to open whisper model file %s",
				model_path.c_str());
			return nullptr;
		}
		modelFile.seekg(0, std::ios::end);
		const size_t modelFileSize = modelFile.tellg();
		modelFile.seekg(0, std::ios::beg);
		std::vector<char> modelBuffer(modelFileSize);
		modelFile.read(modelBuffer.data(), modelFileSize);
		modelFile.close();

		// Initialize whisper
		ctx = whisper_init_from_buffer_with_params(modelBuffer.data(), modelFileSize,
							   cparams);
#else
		ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);
#endif
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "Exception while loading whisper model: %s", e.what());
		return nullptr;
	}
	if (ctx == nullptr) {
		obs_log(LOG_ERROR, "Failed to load whisper model");
		return nullptr;
	}

	obs_log(LOG_INFO, "Whisper model loaded: %s", whisper_print_system_info());
	return ctx;
}

struct DetectionResultWithText run_whisper_inference(struct transcription_filter_data *gf,
						     const float *pcm32f_data_,
						     size_t pcm32f_num_samples, uint64_t t0 = 0,
						     uint64_t t1 = 0,
						     int vad_state = VAD_STATE_WAS_OFF)
{
	if (gf == nullptr) {
		obs_log(LOG_ERROR, "run_whisper_inference: gf is null");
		return {DETECTION_RESULT_UNKNOWN, "", t0, t1, {}, ""};
	}

	if (pcm32f_data_ == nullptr || pcm32f_num_samples == 0) {
		obs_log(LOG_ERROR, "run_whisper_inference: pcm32f_data is null or size is 0");
		return {DETECTION_RESULT_UNKNOWN, "", t0, t1, {}, ""};
	}

	// if the time difference between t0 and t1 is less than 50 ms - skip
	if (t1 - t0 < 50) {
		obs_log(gf->log_level,
			"Time difference between t0 and t1 is less than 50 ms, skipping");
		return {DETECTION_RESULT_UNKNOWN, "", t0, t1, {}, ""};
	}

	obs_log(gf->log_level, "%s: processing %d samples, %.3f sec, %d threads", __func__,
		int(pcm32f_num_samples), float(pcm32f_num_samples) / WHISPER_SAMPLE_RATE,
		gf->whisper_params.n_threads);

	bool should_free_buffer = false;
	float *pcm32f_data = (float *)pcm32f_data_;
	size_t pcm32f_size = pcm32f_num_samples;

	if (pcm32f_num_samples < WHISPER_SAMPLE_RATE) {
		obs_log(gf->log_level,
			"Speech segment is less than 1 second, padding with zeros to 1 second");
		const size_t new_size = (size_t)(1.01f * (float)(WHISPER_SAMPLE_RATE));
		// create a new buffer and copy the data to it in the middle
		pcm32f_data = (float *)bzalloc(new_size * sizeof(float));
		memset(pcm32f_data, 0, new_size * sizeof(float));
		memcpy(pcm32f_data + (new_size - pcm32f_num_samples) / 2, pcm32f_data_,
		       pcm32f_num_samples * sizeof(float));
		pcm32f_size = new_size;
		should_free_buffer = true;
	}

	// duration in ms
	const uint64_t duration_ms = (uint64_t)(pcm32f_size * 1000 / WHISPER_SAMPLE_RATE);

	std::lock_guard<std::mutex> lock(gf->whisper_ctx_mutex);
	if (gf->whisper_context == nullptr) {
		obs_log(LOG_WARNING, "whisper context is null");
		return {DETECTION_RESULT_UNKNOWN, "", t0, t1, {}, ""};
	}

	// run the inference
	int whisper_full_result = -1;
	gf->whisper_params.duration_ms = (int)(duration_ms);
	try {
		whisper_full_result = whisper_full(gf->whisper_context, gf->whisper_params,
						   pcm32f_data, (int)pcm32f_size);
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "Whisper exception: %s. Filter restart is required", e.what());
		whisper_free(gf->whisper_context);
		gf->whisper_context = nullptr;
		if (should_free_buffer) {
			bfree(pcm32f_data);
		}
		return {DETECTION_RESULT_UNKNOWN, "", t0, t1, {}, ""};
	}
	if (should_free_buffer) {
		bfree(pcm32f_data);
	}

	std::string language = gf->whisper_params.language;
	if (gf->whisper_params.language == nullptr || strlen(gf->whisper_params.language) == 0 ||
	    strcmp(gf->whisper_params.language, "auto") == 0) {
		int lang_id = whisper_lang_auto_detect(gf->whisper_context, 0, 1, nullptr);
		language = whisper_lang_str(lang_id);
		obs_log(gf->log_level, "Detected language: %s", language.c_str());
	}

	if (whisper_full_result != 0) {
		obs_log(LOG_WARNING, "failed to process audio, error %d", whisper_full_result);
		return {DETECTION_RESULT_UNKNOWN, "", t0, t1, {}, ""};
	}

	float sentence_p = 0.0f;
	std::string text = "";
	std::string tokenIds = "";
	std::vector<whisper_token_data> tokens;
	for (int n_segment = 0; n_segment < whisper_full_n_segments(gf->whisper_context);
	     ++n_segment) {
		const int n_tokens = whisper_full_n_tokens(gf->whisper_context, n_segment);
		for (int j = 0; j < n_tokens; ++j) {
			// get token
			whisper_token_data token =
				whisper_full_get_token_data(gf->whisper_context, n_segment, j);
			const char *token_str = whisper_token_to_str(gf->whisper_context, token.id);
			bool keep = true;
			// if the token starts with '[' and ends with ']', don't keep it
			if (token_str[0] == '[' && token_str[strlen(token_str) - 1] == ']') {
				keep = false;
			}
			// if this is a special token, don't keep it
			if (token.id >= 50256) {
				keep = false;
			}
			// if the second to last token is .id == 13 ('.'), don't keep it
			if (j == n_tokens - 2 && token.id == 13) {
				keep = false;
			}
			// token ids https://huggingface.co/openai/whisper-large-v3/raw/main/tokenizer.json
			if (token.id > 50365 && token.id <= 51865) {
				const float time = ((float)token.id - 50365.0f) * 0.02f;
				const float duration_s = (float)duration_ms / 1000.0f;
				const float ratio =
					std::max(time, duration_s) / std::min(time, duration_s);
				obs_log(gf->log_level,
					"Time token found %d -> %.3f. Duration: %.3f. Ratio: %.3f.",
					token.id, time, duration_s, ratio);
				if (ratio > 3.0f) {
					// ratio is too high, skip this detection
					obs_log(gf->log_level,
						"Time token ratio too high, skipping");
					return {DETECTION_RESULT_SILENCE, "", t0, t1, {}, language};
				}
				keep = false;
			}

			if (keep) {
				sentence_p += token.p;
				text += token_str;
				tokens.push_back(token);
			}
			obs_log(gf->log_level, "S %d, Token %d: %d\t%s\tp: %.3f [keep: %d]",
				n_segment, j, token.id, token_str, token.p, keep);
		}
	}
	sentence_p /= (float)tokens.size();
	if (sentence_p < gf->sentence_psum_accept_thresh) {
		obs_log(gf->log_level, "Sentence psum %.3f below threshold %.3f, skipping",
			sentence_p, gf->sentence_psum_accept_thresh);
		return {DETECTION_RESULT_SILENCE, "", t0, t1, {}, language};
	}

	obs_log(gf->log_level, "Decoded sentence: '%s'", text.c_str());

	if (gf->log_words) {
		obs_log(LOG_INFO, "[%s --> %s]%s(%.3f) %s", to_timestamp(t0).c_str(),
			to_timestamp(t1).c_str(), vad_state == VAD_STATE_PARTIAL ? "P" : " ",
			sentence_p, text.c_str());
	}

	if (text.empty() || text == "." || text == " " || text == "\n") {
		return {DETECTION_RESULT_SILENCE, "", t0, t1, {}, language};
	}

	return {vad_state == VAD_STATE_PARTIAL ? DETECTION_RESULT_PARTIAL : DETECTION_RESULT_SPEECH,
		text,
		t0,
		t1,
		tokens,
		language};
}

void run_inference_and_callbacks(transcription_filter_data *gf, uint64_t start_offset_ms,
				 uint64_t end_offset_ms, int vad_state)
{
	// get the data from the entire whisper buffer
	// add 50ms of silence to the beginning and end of the buffer
	const size_t pcm32f_size = gf->whisper_buffer.size / sizeof(float);
	const size_t pcm32f_size_with_silence = pcm32f_size + 2 * WHISPER_SAMPLE_RATE / 100;
	// allocate a new buffer and copy the data to it
	float *pcm32f_data = (float *)bzalloc(pcm32f_size_with_silence * sizeof(float));
	if (vad_state == VAD_STATE_PARTIAL) {
		// peek instead of pop, since this is a partial run that keeps the data in the buffer
		circlebuf_peek_front(&gf->whisper_buffer, pcm32f_data + WHISPER_SAMPLE_RATE / 100,
				     pcm32f_size * sizeof(float));
	} else {
		circlebuf_pop_front(&gf->whisper_buffer, pcm32f_data + WHISPER_SAMPLE_RATE / 100,
				    pcm32f_size * sizeof(float));
	}

	struct DetectionResultWithText inference_result =
		run_whisper_inference(gf, pcm32f_data, pcm32f_size_with_silence, start_offset_ms,
				      end_offset_ms, vad_state);
	// output inference result to a text source
	set_text_callback(gf, inference_result);

	if (gf->enable_audio_chunks_callback && vad_state != VAD_STATE_PARTIAL) {
		audio_chunk_callback(gf, pcm32f_data, pcm32f_size_with_silence, vad_state,
				     inference_result);
	}

	// free the buffer
	bfree(pcm32f_data);
}

vad_state vad_based_segmentation(transcription_filter_data *gf, vad_state last_vad_state)
{
	uint32_t num_frames_from_infos = 0;
	uint64_t start_timestamp_offset_ns = 0;
	uint64_t end_timestamp_offset_ns = 0;
	size_t overlap_size = 0;

	for (size_t c = 0; c < gf->channels; c++) {
		// zero the rest of copy_buffers
		memset(gf->copy_buffers[c] + overlap_size, 0,
		       (gf->frames - overlap_size) * sizeof(float));
	}

	{
		// scoped lock the buffer mutex
		std::lock_guard<std::mutex> lock(gf->whisper_buf_mutex);

		obs_log(gf->log_level,
			"vad based segmentation. currently %lu bytes in the audio input buffer",
			gf->input_buffers[0].size);

		// max number of frames is 10 seconds worth of audio
		const size_t max_num_frames = gf->sample_rate * 10;

		// pop all infos from the info buffer and mark the beginning timestamp from the first
		// info as the beginning timestamp of the segment
		struct transcription_filter_audio_info info_from_buf = {0};
		const size_t size_of_audio_info = sizeof(transcription_filter_audio_info);
		while (gf->info_buffer.size >= size_of_audio_info) {
			circlebuf_pop_front(&gf->info_buffer, &info_from_buf, size_of_audio_info);
			num_frames_from_infos += info_from_buf.frames;
			if (start_timestamp_offset_ns == 0) {
				start_timestamp_offset_ns = info_from_buf.timestamp_offset_ns;
			}
			// Check if we're within the needed segment length
			if (num_frames_from_infos > max_num_frames) {
				// too big, push the last info into the buffer's front where it was
				num_frames_from_infos -= info_from_buf.frames;
				circlebuf_push_front(&gf->info_buffer, &info_from_buf,
						     size_of_audio_info);
				break;
			}
		}
		end_timestamp_offset_ns = info_from_buf.timestamp_offset_ns;

		if (start_timestamp_offset_ns > end_timestamp_offset_ns) {
			// this may happen when the incoming media has a timestamp reset
			// in this case, we should figure out the start timestamp from the end timestamp
			// and the number of frames
			start_timestamp_offset_ns =
				end_timestamp_offset_ns -
				num_frames_from_infos * 1000000000 / gf->sample_rate;
		}

		/* Pop from input circlebuf */
		for (size_t c = 0; c < gf->channels; c++) {
			// Push the new data to copy_buffers[c]
			circlebuf_pop_front(&gf->input_buffers[c],
					    gf->copy_buffers[c] + overlap_size,
					    num_frames_from_infos * sizeof(float));
		}
	}

	obs_log(gf->log_level, "found %d frames from info buffer. %lu in overlap",
		num_frames_from_infos, overlap_size);
	gf->last_num_frames = num_frames_from_infos + overlap_size;

	{
		// resample to 16kHz
		float *resampled_16khz[MAX_PREPROC_CHANNELS];
		uint32_t resampled_16khz_frames;
		uint64_t ts_offset;
		{
			ProfileScope("resample");
			audio_resampler_resample(gf->resampler_to_whisper,
						 (uint8_t **)resampled_16khz,
						 &resampled_16khz_frames, &ts_offset,
						 (const uint8_t **)gf->copy_buffers,
						 (uint32_t)num_frames_from_infos);
		}

		obs_log(gf->log_level, "resampled: %d channels, %d frames, %f ms",
			(int)gf->channels, (int)resampled_16khz_frames,
			(float)resampled_16khz_frames / WHISPER_SAMPLE_RATE * 1000.0f);
		circlebuf_push_back(&gf->resampled_buffer, resampled_16khz[0],
				    resampled_16khz_frames * sizeof(float));
	}

	if (gf->resampled_buffer.size < (gf->vad->get_window_size_samples() * sizeof(float)))
		return last_vad_state;

	size_t len =
		gf->resampled_buffer.size / (gf->vad->get_window_size_samples() * sizeof(float));

	std::vector<float> vad_input;
	vad_input.resize(len * gf->vad->get_window_size_samples());
	circlebuf_pop_front(&gf->resampled_buffer, vad_input.data(),
			    vad_input.size() * sizeof(float));

	obs_log(gf->log_level, "sending %d frames to vad", vad_input.size());
	{
		ProfileScope("vad->process");
		gf->vad->process(vad_input, !last_vad_state.vad_on);
	}

	const uint64_t start_ts_offset_ms = start_timestamp_offset_ns / 1000000;
	const uint64_t end_ts_offset_ms = end_timestamp_offset_ns / 1000000;

	vad_state current_vad_state = {false, start_ts_offset_ms, end_ts_offset_ms,
				       last_vad_state.last_partial_segment_end_ts};

	std::vector<timestamp_t> stamps = gf->vad->get_speech_timestamps();
	if (stamps.size() == 0) {
		obs_log(gf->log_level, "VAD detected no speech in %u frames", vad_input.size());
		if (last_vad_state.vad_on) {
			obs_log(gf->log_level, "Last VAD was ON: segment end -> send to inference");
			run_inference_and_callbacks(gf, last_vad_state.start_ts_offest_ms,
						    last_vad_state.end_ts_offset_ms,
						    VAD_STATE_WAS_ON);
			current_vad_state.last_partial_segment_end_ts = 0;
		}

		if (gf->enable_audio_chunks_callback) {
			audio_chunk_callback(gf, vad_input.data(), vad_input.size(),
					     VAD_STATE_IS_OFF,
					     {DETECTION_RESULT_SILENCE,
					      "[silence]",
					      current_vad_state.start_ts_offest_ms,
					      current_vad_state.end_ts_offset_ms,
					      {}});
		}

		return current_vad_state;
	}

	// process vad segments
	for (size_t i = 0; i < stamps.size(); i++) {
		int start_frame = stamps[i].start;
		if (i > 0) {
			// if this is not the first segment, start from the end of the previous segment
			start_frame = stamps[i - 1].end;
		} else {
			// take at least 100ms of audio before the first speech segment, if available
			start_frame = std::max(0, start_frame - WHISPER_SAMPLE_RATE / 10);
		}

		int end_frame = stamps[i].end;
		if (i == stamps.size() - 1 && stamps[i].end < (int)vad_input.size()) {
			// take at least 100ms of audio after the last speech segment, if available
			end_frame = std::min(end_frame + WHISPER_SAMPLE_RATE / 10,
					     (int)vad_input.size());
		}

		const int number_of_frames = end_frame - start_frame;

		// push the data into gf-whisper_buffer
		circlebuf_push_back(&gf->whisper_buffer, vad_input.data() + start_frame,
				    number_of_frames * sizeof(float));

		obs_log(gf->log_level,
			"VAD segment %d. pushed %d to %d (%d frames / %lu ms). current size: %lu bytes / %lu frames / %lu ms",
			i, start_frame, end_frame, number_of_frames,
			number_of_frames * 1000 / WHISPER_SAMPLE_RATE, gf->whisper_buffer.size,
			gf->whisper_buffer.size / sizeof(float),
			gf->whisper_buffer.size / sizeof(float) * 1000 / WHISPER_SAMPLE_RATE);

		// segment "end" is in the middle of the buffer, send it to inference
		if (stamps[i].end < (int)vad_input.size()) {
			// new "ending" segment (not up to the end of the buffer)
			obs_log(gf->log_level, "VAD segment end -> send to inference");
			// find the end timestamp of the segment
			const uint64_t segment_end_ts =
				start_ts_offset_ms + end_frame * 1000 / WHISPER_SAMPLE_RATE;
			run_inference_and_callbacks(
				gf, last_vad_state.start_ts_offest_ms, segment_end_ts,
				last_vad_state.vad_on ? VAD_STATE_WAS_ON : VAD_STATE_WAS_OFF);
			current_vad_state.vad_on = false;
			current_vad_state.start_ts_offest_ms = current_vad_state.end_ts_offset_ms;
			current_vad_state.end_ts_offset_ms = 0;
			current_vad_state.last_partial_segment_end_ts = 0;
			last_vad_state = current_vad_state;
			continue;
		}

		// end not reached - speech is ongoing
		current_vad_state.vad_on = true;
		if (last_vad_state.vad_on) {
			current_vad_state.start_ts_offest_ms = last_vad_state.start_ts_offest_ms;
		} else {
			current_vad_state.start_ts_offest_ms =
				start_ts_offset_ms + start_frame * 1000 / WHISPER_SAMPLE_RATE;
		}
		obs_log(gf->log_level, "end not reached. vad state: start ts: %llu, end ts: %llu",
			current_vad_state.start_ts_offest_ms, current_vad_state.end_ts_offset_ms);

		last_vad_state = current_vad_state;

		// if partial transcription is enabled, check if we should send a partial segment
		if (!gf->partial_transcription) {
			continue;
		}

		// current length of audio in buffer
		const uint64_t current_length_ms =
			(current_vad_state.end_ts_offset_ms > 0
				 ? current_vad_state.end_ts_offset_ms
				 : current_vad_state.start_ts_offest_ms) -
			(current_vad_state.last_partial_segment_end_ts > 0
				 ? current_vad_state.last_partial_segment_end_ts
				 : current_vad_state.start_ts_offest_ms);
		obs_log(gf->log_level, "current buffer length after last partial (%lu): %lu ms",
			current_vad_state.last_partial_segment_end_ts, current_length_ms);

		if (current_length_ms > (uint64_t)gf->partial_latency) {
			current_vad_state.last_partial_segment_end_ts =
				current_vad_state.end_ts_offset_ms;
			// send partial segment to inference
			obs_log(gf->log_level, "Partial segment -> send to inference");
			run_inference_and_callbacks(gf, current_vad_state.start_ts_offest_ms,
						    current_vad_state.end_ts_offset_ms,
						    VAD_STATE_PARTIAL);
		}
	}

	return current_vad_state;
}

void whisper_loop(void *data)
{
	if (data == nullptr) {
		obs_log(LOG_ERROR, "whisper_loop: data is null");
		return;
	}

	struct transcription_filter_data *gf =
		static_cast<struct transcription_filter_data *>(data);

	obs_log(gf->log_level, "Starting whisper thread");

	vad_state current_vad_state = {false, 0, 0, 0};

	const char *whisper_loop_name = "Whisper loop";
	profile_register_root(whisper_loop_name, 50 * 1000 * 1000);

	// Thread main loop
	while (true) {
		ProfileScope(whisper_loop_name);
		{
			ProfileScope("lock whisper ctx");
			std::lock_guard<std::mutex> lock(gf->whisper_ctx_mutex);
			ProfileScope("locked whisper ctx");
			if (gf->whisper_context == nullptr) {
				obs_log(LOG_WARNING, "Whisper context is null, exiting thread");
				break;
			}
		}

		current_vad_state = vad_based_segmentation(gf, current_vad_state);

		if (!gf->cleared_last_sub) {
			// check if we should clear the current sub depending on the minimum subtitle duration
			uint64_t now = now_ms();
			if ((now - gf->last_sub_render_time) > gf->min_sub_duration) {
				// clear the current sub, call the callback with an empty string
				obs_log(gf->log_level,
					"Clearing current subtitle. now: %lu ms, last: %lu ms", now,
					gf->last_sub_render_time);
				set_text_callback(gf, {DETECTION_RESULT_UNKNOWN, "", 0, 0, {}});
				gf->cleared_last_sub = true;
			}
		}

		if (gf->input_cv.has_value())
			gf->input_cv->notify_one();

		// Sleep using the condition variable wshiper_thread_cv
		// This will wake up the thread if there is new data in the input buffer
		// or if the whisper context is null
		std::unique_lock<std::mutex> lock(gf->whisper_ctx_mutex);
		if (gf->input_buffers->size == 0) {
			gf->wshiper_thread_cv.wait_for(lock, std::chrono::milliseconds(50));
		}
	}

	obs_log(gf->log_level, "Exiting whisper thread");
}
