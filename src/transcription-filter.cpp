#include <obs-module.h>
#include <obs-frontend-api.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <bitset>
#include <regex>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

#include <QString>

#include "plugin-support.h"
#include "transcription-filter.h"
#include "transcription-filter-callbacks.h"
#include "transcription-filter-data.h"
#include "transcription-filter-utils.h"
#include "transcription-utils.h"
#include "model-utils/model-downloader.h"
#include "whisper-utils/whisper-processing.h"
#include "whisper-utils/whisper-language.h"
#include "whisper-utils/whisper-model-utils.h"
#include "whisper-utils/whisper-utils.h"
#include "translation/language_codes.h"
#include "translation/translation-utils.h"
#include "translation/translation.h"
#include "translation/translation-includes.h"
#include "ui/filter-replace-dialog.h"

void set_source_signals(transcription_filter_data *gf, obs_source_t *parent_source)
{
	signal_handler_t *sh = obs_source_get_signal_handler(parent_source);
	signal_handler_connect(sh, "media_play", media_play_callback, gf);
	signal_handler_connect(sh, "media_started", media_started_callback, gf);
	signal_handler_connect(sh, "media_pause", media_pause_callback, gf);
	signal_handler_connect(sh, "media_restart", media_restart_callback, gf);
	signal_handler_connect(sh, "media_stopped", media_stopped_callback, gf);
	gf->source_signals_set = true;
}

void disconnect_source_signals(transcription_filter_data *gf, obs_source_t *parent_source)
{
	signal_handler_t *sh = obs_source_get_signal_handler(parent_source);
	signal_handler_disconnect(sh, "media_play", media_play_callback, gf);
	signal_handler_disconnect(sh, "media_started", media_started_callback, gf);
	signal_handler_disconnect(sh, "media_pause", media_pause_callback, gf);
	signal_handler_disconnect(sh, "media_restart", media_restart_callback, gf);
	signal_handler_disconnect(sh, "media_stopped", media_stopped_callback, gf);
	gf->source_signals_set = false;
}

struct obs_audio_data *transcription_filter_filter_audio(void *data, struct obs_audio_data *audio)
{
	if (!audio) {
		return nullptr;
	}

	if (data == nullptr) {
		return audio;
	}

	struct transcription_filter_data *gf =
		static_cast<struct transcription_filter_data *>(data);

	// Lazy initialization of source signals
	if (!gf->source_signals_set) {
		// obs_filter_get_parent only works in the filter function
		obs_source_t *parent_source = obs_filter_get_parent(gf->context);
		if (parent_source != nullptr) {
			set_source_signals(gf, parent_source);
		}
	}

	if (!gf->active) {
		return audio;
	}

	if (gf->whisper_context == nullptr) {
		// Whisper not initialized, just pass through
		return audio;
	}

	// Check if process while muted is not enabled (e.g. the user wants to avoid processing audio
	// when the source is muted)
	if (!gf->process_while_muted) {
		// Check if the parent source is muted
		obs_source_t *parent_source = obs_filter_get_parent(gf->context);
		if (parent_source != nullptr && obs_source_muted(parent_source)) {
			// Source is muted, do not process audio
			return audio;
		}
	}

	{
		std::lock_guard<std::mutex> lock(gf->whisper_buf_mutex); // scoped lock
		// push back current audio data to input circlebuf
		for (size_t c = 0; c < gf->channels; c++) {
			circlebuf_push_back(&gf->input_buffers[c], audio->data[c],
					    audio->frames * sizeof(float));
		}
		// push audio packet info (timestamp/frame count) to info circlebuf
		struct transcription_filter_audio_info info = {0};
		info.frames = audio->frames; // number of frames in this packet
		// check if the timestamp is a false "negative" value for uint64_t
		if (audio->timestamp > (std::numeric_limits<uint64_t>::max() - 100000000)) {
			// set the timestamp to the current time
			info.timestamp_offset_ns = 0;
		} else {
			info.timestamp_offset_ns = audio->timestamp; // timestamp of this packet
		}
		circlebuf_push_back(&gf->info_buffer, &info, sizeof(info));
	}

	return audio;
}

const char *transcription_filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return MT_("transcription_filterAudioFilter");
}

void transcription_filter_remove(void *data, obs_source_t *source)
{
	struct transcription_filter_data *gf =
		static_cast<struct transcription_filter_data *>(data);

	obs_log(gf->log_level, "filter remove");

	disconnect_source_signals(gf, source);
}

void transcription_filter_destroy(void *data)
{
	struct transcription_filter_data *gf =
		static_cast<struct transcription_filter_data *>(data);

	signal_handler_t *sh_filter = obs_source_get_signal_handler(gf->context);
	signal_handler_disconnect(sh_filter, "enable", enable_callback, gf);

	obs_log(gf->log_level, "filter destroy");
	shutdown_whisper_thread(gf);

	if (gf->resampler_to_whisper) {
		audio_resampler_destroy(gf->resampler_to_whisper);
	}

	{
		std::lock_guard<std::mutex> lockbuf(gf->whisper_buf_mutex);
		bfree(gf->copy_buffers[0]);
		gf->copy_buffers[0] = nullptr;
		for (size_t i = 0; i < gf->channels; i++) {
			circlebuf_free(&gf->input_buffers[i]);
		}
	}
	circlebuf_free(&gf->info_buffer);

	if (gf->captions_monitor.isEnabled()) {
		gf->captions_monitor.stopThread();
	}

	bfree(gf);
}

void transcription_filter_update(void *data, obs_data_t *s)
{
	struct transcription_filter_data *gf =
		static_cast<struct transcription_filter_data *>(data);
	obs_log(gf->log_level, "LocalVocal filter update");

	gf->log_level = (int)obs_data_get_int(s, "log_level");
	gf->vad_enabled = obs_data_get_bool(s, "vad_enabled");
	gf->log_words = obs_data_get_bool(s, "log_words");
	gf->caption_to_stream = obs_data_get_bool(s, "caption_to_stream");
	gf->save_srt = obs_data_get_bool(s, "subtitle_save_srt");
	gf->truncate_output_file = obs_data_get_bool(s, "truncate_output_file");
	gf->save_only_while_recording = obs_data_get_bool(s, "only_while_recording");
	gf->rename_file_to_match_recording = obs_data_get_bool(s, "rename_file_to_match_recording");
	// Get the current timestamp using the system clock
	gf->start_timestamp_ms = now_ms();
	gf->sentence_number = 1;
	gf->process_while_muted = obs_data_get_bool(s, "process_while_muted");
	gf->min_sub_duration = (int)obs_data_get_int(s, "min_sub_duration");
	gf->last_sub_render_time = 0;
	bool new_buffered_output = obs_data_get_bool(s, "buffered_output");
	int new_buffer_num_lines = (int)obs_data_get_int(s, "buffer_num_lines");
	int new_buffer_num_chars_per_line = (int)obs_data_get_int(s, "buffer_num_chars_per_line");
	TokenBufferSegmentation new_buffer_output_type =
		(TokenBufferSegmentation)obs_data_get_int(s, "buffer_output_type");
	gf->filter_words_replace =
		deserialize_filter_words_replace(obs_data_get_string(s, "filter_words_replace"));

	if (new_buffered_output) {
		obs_log(gf->log_level, "buffered_output enable");
		if (!gf->buffered_output || !gf->captions_monitor.isEnabled()) {
			obs_log(gf->log_level, "buffered_output currently disabled, enabling");
			gf->buffered_output = true;
			gf->captions_monitor.initialize(
				gf,
				[gf](const std::string &text) {
					if (gf->buffered_output) {
						send_caption_to_source(gf->text_source_name, text,
								       gf);
					}
				},
				[gf](const std::string &sentence) {
					obs_log(LOG_INFO, "sentence: %s", sentence.c_str());
					if (gf->buffered_output && gf->translate) {
						send_sentence_to_translation(sentence, gf);
					}
				},
				new_buffer_num_lines, new_buffer_num_chars_per_line,
				std::chrono::seconds(3), new_buffer_output_type);
		} else {
			if (new_buffer_num_lines != gf->buffered_output_num_lines ||
			    new_buffer_num_chars_per_line != gf->buffered_output_num_chars ||
			    new_buffer_output_type != gf->buffered_output_output_type) {
				obs_log(gf->log_level,
					"buffered_output parameters changed, updating");
				gf->captions_monitor.clear();
				gf->captions_monitor.setNumSentences(new_buffer_num_lines);
				gf->captions_monitor.setNumPerSentence(
					new_buffer_num_chars_per_line);
				gf->captions_monitor.setSegmentation(new_buffer_output_type);
			}
		}
		gf->buffered_output_num_lines = new_buffer_num_lines;
		gf->buffered_output_num_chars = new_buffer_num_chars_per_line;
		gf->buffered_output_output_type = new_buffer_output_type;
	} else {
		obs_log(gf->log_level, "buffered_output disable");
		if (gf->buffered_output) {
			obs_log(gf->log_level, "buffered_output currently enabled, disabling");
			if (gf->captions_monitor.isEnabled()) {
				gf->captions_monitor.clear();
				gf->captions_monitor.stopThread();
			}
			gf->buffered_output = false;
		}
	}

	bool new_translate = obs_data_get_bool(s, "translate");
	gf->source_lang = obs_data_get_string(s, "translate_source_language");
	gf->target_lang = obs_data_get_string(s, "translate_target_language");
	gf->translation_ctx.add_context = obs_data_get_bool(s, "translate_add_context");
	gf->translation_ctx.input_tokenization_style =
		(InputTokenizationStyle)obs_data_get_int(s, "translate_input_tokenization_style");
	gf->translation_output = obs_data_get_string(s, "translate_output");
	std::string new_translate_model_index = obs_data_get_string(s, "translate_model");
	std::string new_translation_model_path_external =
		obs_data_get_string(s, "translation_model_path_external");

	if (new_translate != gf->translate ||
	    new_translate_model_index != gf->translation_model_index ||
	    new_translation_model_path_external != gf->translation_model_path_external) {
		if (new_translate) {
			gf->translation_model_index = new_translate_model_index;
			gf->translation_model_path_external = new_translation_model_path_external;
			if (gf->translation_model_index != "whisper-based-translation") {
				start_translation(gf);
			} else {
				// whisper-based translation
				obs_log(gf->log_level, "Starting whisper-based translation...");
				gf->translate = false;
			}
		} else {
			gf->translate = false;
		}
	}

	// translation options
	if (gf->translate) {
		if (gf->translation_ctx.options) {
			gf->translation_ctx.options->sampling_temperature =
				(float)obs_data_get_double(s, "translation_sampling_temperature");
			gf->translation_ctx.options->repetition_penalty =
				(float)obs_data_get_double(s, "translation_repetition_penalty");
			gf->translation_ctx.options->beam_size =
				(int)obs_data_get_int(s, "translation_beam_size");
			gf->translation_ctx.options->max_decoding_length =
				(int)obs_data_get_int(s, "translation_max_decoding_length");
			gf->translation_ctx.options->no_repeat_ngram_size =
				(int)obs_data_get_int(s, "translation_no_repeat_ngram_size");
			gf->translation_ctx.options->max_input_length =
				(int)obs_data_get_int(s, "translation_max_input_length");
		}
	}

	obs_log(gf->log_level, "update text source");
	// update the text source
	const char *new_text_source_name = obs_data_get_string(s, "subtitle_sources");

	if (new_text_source_name == nullptr || strcmp(new_text_source_name, "none") == 0 ||
	    strcmp(new_text_source_name, "(null)") == 0 ||
	    strcmp(new_text_source_name, "text_file") == 0 || strlen(new_text_source_name) == 0) {
		// new selected text source is not valid, release the old one
		gf->text_source_name.clear();
		gf->output_file_path = "";
		if (strcmp(new_text_source_name, "text_file") == 0) {
			// set the output file path
			const char *output_file_path =
				obs_data_get_string(s, "subtitle_output_filename");
			if (output_file_path != nullptr && strlen(output_file_path) > 0) {
				gf->output_file_path = output_file_path;
			}
		}
	} else {
		gf->text_source_name = new_text_source_name;
	}

	obs_log(gf->log_level, "update whisper params");
	{
		std::lock_guard<std::mutex> lock(gf->whisper_ctx_mutex);

		gf->sentence_psum_accept_thresh =
			(float)obs_data_get_double(s, "sentence_psum_accept_thresh");

		gf->whisper_params = whisper_full_default_params(
			(whisper_sampling_strategy)obs_data_get_int(s, "whisper_sampling_method"));
		gf->whisper_params.duration_ms = (int)obs_data_get_int(s, "buffer_size_msec");
		if (!new_translate || gf->translation_model_index != "whisper-based-translation") {
			gf->whisper_params.language =
				obs_data_get_string(s, "whisper_language_select");
		} else {
			// take the language from gf->target_lang
			if (language_codes_2_reverse.count(gf->target_lang) > 0) {
				gf->whisper_params.language =
					language_codes_2_reverse[gf->target_lang].c_str();
			} else {
				gf->whisper_params.language = "auto";
			}
		}
		gf->whisper_params.initial_prompt = obs_data_get_string(s, "initial_prompt");
		gf->whisper_params.n_threads = (int)obs_data_get_int(s, "n_threads");
		gf->whisper_params.n_max_text_ctx = (int)obs_data_get_int(s, "n_max_text_ctx");
		gf->whisper_params.translate = obs_data_get_bool(s, "whisper_translate");
		gf->whisper_params.no_context = obs_data_get_bool(s, "no_context");
		gf->whisper_params.single_segment = obs_data_get_bool(s, "single_segment");
		gf->whisper_params.print_special = obs_data_get_bool(s, "print_special");
		gf->whisper_params.print_progress = obs_data_get_bool(s, "print_progress");
		gf->whisper_params.print_realtime = obs_data_get_bool(s, "print_realtime");
		gf->whisper_params.print_timestamps = obs_data_get_bool(s, "print_timestamps");
		gf->whisper_params.token_timestamps = obs_data_get_bool(s, "token_timestamps");
		gf->whisper_params.thold_pt = (float)obs_data_get_double(s, "thold_pt");
		gf->whisper_params.thold_ptsum = (float)obs_data_get_double(s, "thold_ptsum");
		gf->whisper_params.max_len = (int)obs_data_get_int(s, "max_len");
		gf->whisper_params.split_on_word = obs_data_get_bool(s, "split_on_word");
		gf->whisper_params.max_tokens = (int)obs_data_get_int(s, "max_tokens");
		gf->whisper_params.speed_up = obs_data_get_bool(s, "speed_up");
		gf->whisper_params.suppress_blank = obs_data_get_bool(s, "suppress_blank");
		gf->whisper_params.suppress_non_speech_tokens =
			obs_data_get_bool(s, "suppress_non_speech_tokens");
		gf->whisper_params.temperature = (float)obs_data_get_double(s, "temperature");
		gf->whisper_params.max_initial_ts = (float)obs_data_get_double(s, "max_initial_ts");
		gf->whisper_params.length_penalty = (float)obs_data_get_double(s, "length_penalty");

		if (gf->vad_enabled && gf->vad) {
			const float vad_threshold = (float)obs_data_get_double(s, "vad_threshold");
			gf->vad->set_threshold(vad_threshold);
		}
	}

	if (gf->context != nullptr && obs_source_enabled(gf->context)) {
		if (gf->initial_creation) {
			obs_log(LOG_INFO, "Initial filter creation and source enabled");

			// source was enabled on creation
			update_whisper_model(gf);
			gf->active = true;
			gf->initial_creation = false;
		} else {
			// check if the whisper model selection has changed
			const std::string new_model_path =
				obs_data_get_string(s, "whisper_model_path");
			if (gf->whisper_model_path != new_model_path) {
				obs_log(LOG_INFO, "New model selected: %s", new_model_path.c_str());
				update_whisper_model(gf);
			}
		}
	}
}

void *transcription_filter_create(obs_data_t *settings, obs_source_t *filter)
{
	obs_log(LOG_INFO, "LocalVocal filter create");

	void *data = bmalloc(sizeof(struct transcription_filter_data));
	struct transcription_filter_data *gf = new (data) transcription_filter_data();

	// Get the number of channels for the input source
	gf->channels = audio_output_get_channels(obs_get_audio());
	gf->sample_rate = audio_output_get_sample_rate(obs_get_audio());
	gf->frames = (size_t)((float)gf->sample_rate / (1000.0f / MAX_MS_WORK_BUFFER));
	gf->last_num_frames = 0;
	gf->min_sub_duration = (int)obs_data_get_int(settings, "min_sub_duration");
	gf->last_sub_render_time = 0;
	gf->log_level = (int)obs_data_get_int(settings, "log_level");
	gf->save_srt = obs_data_get_bool(settings, "subtitle_save_srt");
	gf->truncate_output_file = obs_data_get_bool(settings, "truncate_output_file");
	gf->save_only_while_recording = obs_data_get_bool(settings, "only_while_recording");
	gf->rename_file_to_match_recording =
		obs_data_get_bool(settings, "rename_file_to_match_recording");
	gf->process_while_muted = obs_data_get_bool(settings, "process_while_muted");
	gf->buffered_output = obs_data_get_bool(settings, "buffered_output");

	for (size_t i = 0; i < gf->channels; i++) {
		circlebuf_init(&gf->input_buffers[i]);
	}
	circlebuf_init(&gf->info_buffer);
	circlebuf_init(&gf->whisper_buffer);

	// allocate copy buffers
	gf->copy_buffers[0] =
		static_cast<float *>(bzalloc(gf->channels * gf->frames * sizeof(float)));
	for (size_t c = 1; c < gf->channels; c++) { // set the channel pointers
		gf->copy_buffers[c] = gf->copy_buffers[0] + c * gf->frames;
	}
	memset(gf->copy_buffers[0], 0, gf->channels * gf->frames * sizeof(float));

	gf->context = filter;

	obs_log(gf->log_level, "channels %d, frames %d, sample_rate %d", (int)gf->channels,
		(int)gf->frames, gf->sample_rate);

	obs_log(gf->log_level, "setup audio resampler");
	struct resample_info src, dst;
	src.samples_per_sec = gf->sample_rate;
	src.format = AUDIO_FORMAT_FLOAT_PLANAR;
	src.speakers = convert_speaker_layout((uint8_t)gf->channels);

	dst.samples_per_sec = WHISPER_SAMPLE_RATE;
	dst.format = AUDIO_FORMAT_FLOAT_PLANAR;
	dst.speakers = convert_speaker_layout((uint8_t)1);

	gf->resampler_to_whisper = audio_resampler_create(&dst, &src);

	obs_log(gf->log_level, "clear text source data");
	const char *subtitle_sources = obs_data_get_string(settings, "subtitle_sources");
	if (subtitle_sources == nullptr || strcmp(subtitle_sources, "none") == 0 ||
	    strcmp(subtitle_sources, "(null)") == 0 || strlen(subtitle_sources) == 0) {
		obs_log(gf->log_level, "Create text source");
		// check if a source called "LocalVocal Subtitles" exists
		obs_source_t *source = obs_get_source_by_name("LocalVocal Subtitles");
		if (source) {
			// source exists, release it
			obs_source_release(source);
		} else {
			// create a new OBS text source called "LocalVocal Subtitles"
			create_obs_text_source();
		}
		gf->text_source_name = "LocalVocal Subtitles";
		obs_data_set_string(settings, "subtitle_sources", "LocalVocal Subtitles");
	} else {
		// set the text source name
		gf->text_source_name = subtitle_sources;
	}
	obs_log(gf->log_level, "clear paths and whisper context");
	gf->whisper_model_file_currently_loaded = "";
	gf->output_file_path = std::string("");
	gf->whisper_model_path = std::string(""); // The update function will set the model path
	gf->whisper_context = nullptr;

	signal_handler_t *sh_filter = obs_source_get_signal_handler(gf->context);
	signal_handler_connect(sh_filter, "enable", enable_callback, gf);

	obs_log(gf->log_level, "run update");
	// get the settings updated on the filter data struct
	transcription_filter_update(gf, settings);

	// handle the event OBS_FRONTEND_EVENT_RECORDING_STARTING to reset the srt sentence number
	// to match the subtitles with the recording
	obs_frontend_add_event_callback(recording_state_callback, gf);

	obs_log(gf->log_level, "filter created.");
	return gf;
}

bool subs_output_select_changed(obs_properties_t *props, obs_property_t *property,
				obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	// Show or hide the output filename selection input
	const char *new_output = obs_data_get_string(settings, "subtitle_sources");
	const bool show_hide = (strcmp(new_output, "text_file") == 0);
	for (const std::string &prop_name :
	     {"subtitle_output_filename", "subtitle_save_srt", "truncate_output_file",
	      "only_while_recording", "rename_file_to_match_recording"}) {
		obs_property_set_visible(obs_properties_get(props, prop_name.c_str()), show_hide);
	}
	return true;
}

void transcription_filter_activate(void *data)
{
	struct transcription_filter_data *gf =
		static_cast<struct transcription_filter_data *>(data);
	obs_log(gf->log_level, "filter activated");
	gf->active = true;
}

void transcription_filter_deactivate(void *data)
{
	struct transcription_filter_data *gf =
		static_cast<struct transcription_filter_data *>(data);
	obs_log(gf->log_level, "filter deactivated");
	gf->active = false;
}

void transcription_filter_show(void *data)
{
	struct transcription_filter_data *gf =
		static_cast<struct transcription_filter_data *>(data);
	obs_log(gf->log_level, "filter show");
}

void transcription_filter_hide(void *data)
{
	struct transcription_filter_data *gf =
		static_cast<struct transcription_filter_data *>(data);
	obs_log(gf->log_level, "filter hide");
}

void transcription_filter_defaults(obs_data_t *s)
{
	obs_log(LOG_DEBUG, "filter defaults");

	obs_data_set_default_bool(s, "buffered_output", false);
	obs_data_set_default_int(s, "buffer_num_lines", 2);
	obs_data_set_default_int(s, "buffer_num_chars_per_line", 30);
	obs_data_set_default_int(s, "buffer_output_type",
				 (int)TokenBufferSegmentation::SEGMENTATION_TOKEN);

	obs_data_set_default_bool(s, "vad_enabled", true);
	obs_data_set_default_double(s, "vad_threshold", 0.65);
	obs_data_set_default_int(s, "log_level", LOG_DEBUG);
	obs_data_set_default_bool(s, "log_words", false);
	obs_data_set_default_bool(s, "caption_to_stream", false);
	obs_data_set_default_string(s, "whisper_model_path", "Whisper Tiny English (74Mb)");
	obs_data_set_default_string(s, "whisper_language_select", "en");
	obs_data_set_default_string(s, "subtitle_sources", "none");
	obs_data_set_default_bool(s, "process_while_muted", false);
	obs_data_set_default_bool(s, "subtitle_save_srt", false);
	obs_data_set_default_bool(s, "truncate_output_file", false);
	obs_data_set_default_bool(s, "only_while_recording", false);
	obs_data_set_default_bool(s, "rename_file_to_match_recording", true);
	obs_data_set_default_int(s, "min_sub_duration", 3000);
	obs_data_set_default_bool(s, "advanced_settings", false);
	obs_data_set_default_bool(s, "translate", false);
	obs_data_set_default_string(s, "translate_target_language", "__es__");
	obs_data_set_default_string(s, "translate_source_language", "__en__");
	obs_data_set_default_bool(s, "translate_add_context", true);
	obs_data_set_default_string(s, "translate_model", "whisper-based-translation");
	obs_data_set_default_string(s, "translation_model_path_external", "");
	obs_data_set_default_int(s, "translate_input_tokenization_style", INPUT_TOKENIZAION_M2M100);
	obs_data_set_default_double(s, "sentence_psum_accept_thresh", 0.4);

	// translation options
	obs_data_set_default_double(s, "translation_sampling_temperature", 0.1);
	obs_data_set_default_double(s, "translation_repetition_penalty", 2.0);
	obs_data_set_default_int(s, "translation_beam_size", 1);
	obs_data_set_default_int(s, "translation_max_decoding_length", 65);
	obs_data_set_default_int(s, "translation_no_repeat_ngram_size", 1);
	obs_data_set_default_int(s, "translation_max_input_length", 65);

	// Whisper parameters
	obs_data_set_default_int(s, "whisper_sampling_method", WHISPER_SAMPLING_BEAM_SEARCH);
	obs_data_set_default_string(s, "initial_prompt", "");
	obs_data_set_default_int(s, "n_threads", 4);
	obs_data_set_default_int(s, "n_max_text_ctx", 16384);
	obs_data_set_default_bool(s, "whisper_translate", false);
	obs_data_set_default_bool(s, "no_context", true);
	obs_data_set_default_bool(s, "single_segment", true);
	obs_data_set_default_bool(s, "print_special", false);
	obs_data_set_default_bool(s, "print_progress", false);
	obs_data_set_default_bool(s, "print_realtime", false);
	obs_data_set_default_bool(s, "print_timestamps", false);
	obs_data_set_default_bool(s, "token_timestamps", false);
	obs_data_set_default_bool(s, "dtw_token_timestamps", false);
	obs_data_set_default_double(s, "thold_pt", 0.01);
	obs_data_set_default_double(s, "thold_ptsum", 0.01);
	obs_data_set_default_int(s, "max_len", 0);
	obs_data_set_default_bool(s, "split_on_word", true);
	obs_data_set_default_int(s, "max_tokens", 0);
	obs_data_set_default_bool(s, "speed_up", false);
	obs_data_set_default_bool(s, "suppress_blank", false);
	obs_data_set_default_bool(s, "suppress_non_speech_tokens", true);
	obs_data_set_default_double(s, "temperature", 0.1);
	obs_data_set_default_double(s, "max_initial_ts", 1.0);
	obs_data_set_default_double(s, "length_penalty", -1.0);
}

obs_properties_t *transcription_filter_properties(void *data)
{
	struct transcription_filter_data *gf =
		static_cast<struct transcription_filter_data *>(data);

	obs_properties_t *ppts = obs_properties_create();

	obs_property_t *subs_output =
		obs_properties_add_list(ppts, "subtitle_sources", MT_("subtitle_sources"),
					OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	// Add "none" option
	obs_property_list_add_string(subs_output, MT_("none_no_output"), "none");
	obs_property_list_add_string(subs_output, MT_("text_file_output"), "text_file");
	// Add text sources
	obs_enum_sources(add_sources_to_list, subs_output);

	obs_properties_add_path(ppts, "subtitle_output_filename", MT_("output_filename"),
				OBS_PATH_FILE_SAVE, "Text (*.txt)", NULL);
	obs_properties_add_bool(ppts, "subtitle_save_srt", MT_("save_srt"));
	obs_properties_add_bool(ppts, "truncate_output_file", MT_("truncate_output_file"));
	obs_properties_add_bool(ppts, "only_while_recording", MT_("only_while_recording"));
	obs_properties_add_bool(ppts, "rename_file_to_match_recording",
				MT_("rename_file_to_match_recording"));

	obs_property_set_modified_callback(subs_output, subs_output_select_changed);

	// Add a list of available whisper models to download
	obs_property_t *whisper_models_list =
		obs_properties_add_list(ppts, "whisper_model_path", MT_("whisper_model"),
					OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	// Add models from models_info map
	for (const auto &model_info : models_info) {
		if (model_info.second.type == MODEL_TYPE_TRANSCRIPTION) {
			obs_property_list_add_string(whisper_models_list, model_info.first.c_str(),
						     model_info.first.c_str());
		}
	}

	obs_property_list_add_string(whisper_models_list, "Load external model file",
				     "!!!external!!!");

	// Add a file selection input to select an external model file
	obs_property_t *whisper_model_path_external = obs_properties_add_path(
		ppts, "whisper_model_path_external", MT_("external_model_file"), OBS_PATH_FILE,
		"Model (*.bin)", NULL);
	// Hide the external model file selection input
	obs_property_set_visible(obs_properties_get(ppts, "whisper_model_path_external"), false);

	obs_property_set_modified_callback2(
		whisper_model_path_external,
		[](void *data_, obs_properties_t *props, obs_property_t *property,
		   obs_data_t *settings) {
			UNUSED_PARAMETER(property);
			UNUSED_PARAMETER(props);
			struct transcription_filter_data *gf_ =
				static_cast<struct transcription_filter_data *>(data_);
			obs_log(gf_->log_level, "whisper_model_path_external modified");
			transcription_filter_update(gf_, settings);
			return true;
		},
		gf);

	// Add a callback to the model list to handle the external model file selection
	obs_property_set_modified_callback(whisper_models_list, [](obs_properties_t *props,
								   obs_property_t *property,
								   obs_data_t *settings) {
		UNUSED_PARAMETER(property);
		// If the selected model is the external model, show the external model file selection
		// input
		const char *new_model_path = obs_data_get_string(settings, "whisper_model_path");
		if (strcmp(new_model_path, "!!!external!!!") == 0) {
			obs_property_set_visible(
				obs_properties_get(props, "whisper_model_path_external"), true);
		} else {
			obs_property_set_visible(
				obs_properties_get(props, "whisper_model_path_external"), false);
			const std::string model_name = new_model_path;
			// if the model is english-only -> hide all the languages but english
			const bool is_english_only =
				(model_name.find("English") != std::string::npos);
			// clear the language selection list ("whisper_language_select")
			obs_property_t *prop_lang =
				obs_properties_get(props, "whisper_language_select");
			obs_property_list_clear(prop_lang);
			if (is_english_only) {
				// add only the english language
				obs_property_list_add_string(prop_lang, "English", "en");
				// set the language to english
				obs_data_set_string(settings, "whisper_language_select", "en");
			} else {
				// add all the languages
				for (const auto &lang : whisper_available_lang) {
					obs_property_list_add_string(prop_lang, lang.second.c_str(),
								     lang.first.c_str());
				}
				// set the language to auto (default)
				obs_data_set_string(settings, "whisper_language_select", "auto");
			}
		}
		return true;
	});

	// Add language selector
	obs_property_t *whisper_language_select_list =
		obs_properties_add_list(ppts, "whisper_language_select", MT_("language"),
					OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	// iterate over all available languages and add them to the list
	for (auto const &pair : whisper_available_lang_reverse) {
		obs_property_list_add_string(whisper_language_select_list, pair.first.c_str(),
					     pair.second.c_str());
	}

	// add translation option group
	obs_properties_t *translation_group = obs_properties_create();
	obs_property_t *translation_group_prop = obs_properties_add_group(
		ppts, "translate", MT_("translate"), OBS_GROUP_CHECKABLE, translation_group);

	// add translatio model selection
	obs_property_t *prop_translate_model = obs_properties_add_list(
		translation_group, "translate_model", MT_("translate_model"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	// Populate the dropdown with the translation models
	// add "Whisper-Based Translation" option
	obs_property_list_add_string(prop_translate_model, MT_("Whisper-Based-Translation"),
				     "whisper-based-translation");
	for (const auto &model_info : models_info) {
		if (model_info.second.type == MODEL_TYPE_TRANSLATION) {
			obs_property_list_add_string(prop_translate_model, model_info.first.c_str(),
						     model_info.first.c_str());
		}
	}
	// add external model option
	obs_property_list_add_string(prop_translate_model, MT_("load_external_model"),
				     "!!!external!!!");
	// add callback to handle the external model file selection
	obs_properties_add_path(translation_group, "translation_model_path_external",
				MT_("external_model_folder"), OBS_PATH_DIRECTORY,
				"CT2 Model folder", NULL);
	// Hide the external model file selection input
	obs_property_set_visible(obs_properties_get(ppts, "translation_model_path_external"),
				 false);
	// Add a callback to the model list to handle the external model file selection
	obs_property_set_modified_callback(prop_translate_model, [](obs_properties_t *props,
								    obs_property_t *property,
								    obs_data_t *settings) {
		UNUSED_PARAMETER(property);
		// If the selected model is the external model, show the external model file selection
		// input
		const char *new_model_path = obs_data_get_string(settings, "translate_model");
		const bool is_external = (strcmp(new_model_path, "!!!external!!!") == 0);
		const bool is_whisper = (strcmp(new_model_path, "whisper-based-translation") == 0);
		obs_property_set_visible(
			obs_properties_get(props, "translation_model_path_external"), is_external);
		obs_property_set_visible(obs_properties_get(props, "translate_source_language"),
					 !is_whisper);
		obs_property_set_visible(obs_properties_get(props, "translate_add_context"),
					 !is_whisper);
		obs_property_set_visible(obs_properties_get(props,
							    "translate_input_tokenization_style"),
					 !is_whisper);
		obs_property_set_visible(obs_properties_get(props, "translate_output"),
					 !is_whisper);
		return true;
	});
	// add target language selection
	obs_property_t *prop_tgt = obs_properties_add_list(
		translation_group, "translate_target_language", MT_("target_language"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_t *prop_src = obs_properties_add_list(
		translation_group, "translate_source_language", MT_("source_language"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_properties_add_bool(translation_group, "translate_add_context",
				MT_("translate_add_context"));

	// Populate the dropdown with the language codes
	for (const auto &language : language_codes) {
		obs_property_list_add_string(prop_tgt, language.second.c_str(),
					     language.first.c_str());
		obs_property_list_add_string(prop_src, language.second.c_str(),
					     language.first.c_str());
	}
	// add option for routing the translation to an output source
	obs_property_t *prop_output = obs_properties_add_list(translation_group, "translate_output",
							      MT_("translate_output"),
							      OBS_COMBO_TYPE_LIST,
							      OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop_output, "Write to captions output", "none");
	// TODO add file output option
	// obs_property_list_add_string(...
	obs_enum_sources(add_sources_to_list, prop_output);

	// add callback to enable/disable translation group
	obs_property_set_modified_callback(translation_group_prop, [](obs_properties_t *props,
								      obs_property_t *property,
								      obs_data_t *settings) {
		UNUSED_PARAMETER(property);
		// Show/Hide the translation group
		const bool translate_enabled = obs_data_get_bool(settings, "translate");
		for (const auto &prop :
		     {"translate_target_language", "translate_source_language",
		      "translate_add_context", "translate_output", "translate_model",
		      "translate_input_tokenization_style", "translation_sampling_temperature",
		      "translation_repetition_penalty", "translation_beam_size",
		      "translation_max_decoding_length", "translation_no_repeat_ngram_size",
		      "translation_max_input_length"}) {
			obs_property_set_visible(obs_properties_get(props, prop),
						 translate_enabled);
		}
		const bool is_external = (strcmp(obs_data_get_string(settings, "translate_model"),
						 "!!!external!!!") == 0);
		obs_property_set_visible(obs_properties_get(props,
							    "translation_model_path_external"),
					 is_external && translate_enabled);
		return true;
	});
	// add tokenization style options
	obs_property_t *prop_token_style =
		obs_properties_add_list(translation_group, "translate_input_tokenization_style",
					MT_("translate_input_tokenization_style"),
					OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop_token_style, "M2M100 Tokens", INPUT_TOKENIZAION_M2M100);
	obs_property_list_add_int(prop_token_style, "T5 Tokens", INPUT_TOKENIZAION_T5);

	// add translation options: beam_size, max_decoding_length, repetition_penalty, no_repeat_ngram_size, max_input_length, sampling_temperature
	obs_properties_add_float_slider(translation_group, "translation_sampling_temperature",
					MT_("translation_sampling_temperature"), 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(translation_group, "translation_repetition_penalty",
					MT_("translation_repetition_penalty"), 1.0, 5.0, 0.25);
	obs_properties_add_int_slider(translation_group, "translation_beam_size",
				      MT_("translation_beam_size"), 1, 10, 1);
	obs_properties_add_int_slider(translation_group, "translation_max_decoding_length",
				      MT_("translation_max_decoding_length"), 1, 100, 5);
	obs_properties_add_int_slider(translation_group, "translation_max_input_length",
				      MT_("translation_max_input_length"), 1, 100, 5);
	obs_properties_add_int_slider(translation_group, "translation_no_repeat_ngram_size",
				      MT_("translation_no_repeat_ngram_size"), 1, 10, 1);

	obs_property_t *advanced_settings_prop =
		obs_properties_add_bool(ppts, "advanced_settings", MT_("advanced_settings"));
	obs_property_set_modified_callback(advanced_settings_prop, [](obs_properties_t *props,
								      obs_property_t *property,
								      obs_data_t *settings) {
		UNUSED_PARAMETER(property);
		// If advanced settings is enabled, show the advanced settings group
		const bool show_hide = obs_data_get_bool(settings, "advanced_settings");
		for (const std::string &prop_name :
		     {"whisper_params_group", "log_words", "caption_to_stream", "buffer_size_msec",
		      "overlap_size_msec", "step_by_step_processing", "min_sub_duration",
		      "process_while_muted", "buffered_output", "vad_enabled", "log_level",
		      "open_filter_ui", "sentence_psum_accept_thresh", "vad_threshold",
		      "buffered_output_group"}) {
			obs_property_set_visible(obs_properties_get(props, prop_name.c_str()),
						 show_hide);
		}
		return true;
	});

	obs_property_t *buffered_output_prop =
		obs_properties_add_bool(ppts, "buffered_output", MT_("buffered_output"));

	// add buffered output options group
	obs_properties_t *buffered_output_group = obs_properties_create();
	obs_properties_add_group(ppts, "buffered_output_group", MT_("buffered_output_parameters"),
				 OBS_GROUP_NORMAL, buffered_output_group);
	// add buffer "type" character or word
	obs_property_t *buffer_type_list = obs_properties_add_list(
		buffered_output_group, "buffer_output_type", MT_("buffer_output_type"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(buffer_type_list, "Character", SEGMENTATION_TOKEN);
	obs_property_list_add_int(buffer_type_list, "Word", SEGMENTATION_WORD);
	// add buffer lines parameter
	obs_properties_add_int_slider(buffered_output_group, "buffer_num_lines",
				      MT_("buffer_num_lines"), 1, 5, 1);
	// add buffer number of characters per line parameter
	obs_properties_add_int_slider(buffered_output_group, "buffer_num_chars_per_line",
				      MT_("buffer_num_chars_per_line"), 1, 100, 1);

	// on enable/disable buffered output, show/hide the group
	obs_property_set_modified_callback(buffered_output_prop, [](obs_properties_t *props,
								    obs_property_t *property,
								    obs_data_t *settings) {
		UNUSED_PARAMETER(property);
		// If buffered output is enabled, show the buffered output group
		const bool show_hide = obs_data_get_bool(settings, "buffered_output");
		obs_property_set_visible(obs_properties_get(props, "buffered_output_group"),
					 show_hide);
		return true;
	});

	obs_properties_add_bool(ppts, "log_words", MT_("log_words"));
	obs_properties_add_bool(ppts, "caption_to_stream", MT_("caption_to_stream"));

	obs_properties_add_int_slider(ppts, "min_sub_duration", MT_("min_sub_duration"), 1000, 5000,
				      50);
	obs_properties_add_float_slider(ppts, "sentence_psum_accept_thresh",
					MT_("sentence_psum_accept_thresh"), 0.0, 1.0, 0.05);

	obs_properties_add_bool(ppts, "process_while_muted", MT_("process_while_muted"));

	obs_properties_add_bool(ppts, "vad_enabled", MT_("vad_enabled"));
	// add vad threshold slider
	obs_properties_add_float_slider(ppts, "vad_threshold", MT_("vad_threshold"), 0.0, 1.0,
					0.05);

	obs_property_t *list = obs_properties_add_list(ppts, "log_level", MT_("log_level"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(list, "DEBUG (Won't show)", LOG_DEBUG);
	obs_property_list_add_int(list, "INFO", LOG_INFO);
	obs_property_list_add_int(list, "WARNING", LOG_WARNING);

	// add button to open filter and replace UI dialog
	obs_properties_add_button2(
		ppts, "open_filter_ui", MT_("open_filter_ui"),
		[](obs_properties_t *props, obs_property_t *property, void *data_) {
			UNUSED_PARAMETER(props);
			UNUSED_PARAMETER(property);
			struct transcription_filter_data *gf_ =
				static_cast<struct transcription_filter_data *>(data_);
			FilterReplaceDialog *filter_replace_dialog = new FilterReplaceDialog(
				(QWidget *)obs_frontend_get_main_window(), gf_);
			filter_replace_dialog->exec();
			// store the filter data on the source settings
			obs_data_t *settings = obs_source_get_settings(gf_->context);
			// serialize the filter data
			const std::string filter_data =
				serialize_filter_words_replace(gf_->filter_words_replace);
			obs_data_set_string(settings, "filter_words_replace", filter_data.c_str());
			obs_data_release(settings);
			return true;
		},
		gf);

	obs_properties_t *whisper_params_group = obs_properties_create();
	obs_properties_add_group(ppts, "whisper_params_group", MT_("whisper_parameters"),
				 OBS_GROUP_NORMAL, whisper_params_group);

	obs_property_t *whisper_sampling_method_list = obs_properties_add_list(
		whisper_params_group, "whisper_sampling_method", MT_("whisper_sampling_method"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(whisper_sampling_method_list, "Beam search",
				  WHISPER_SAMPLING_BEAM_SEARCH);
	obs_property_list_add_int(whisper_sampling_method_list, "Greedy", WHISPER_SAMPLING_GREEDY);

	// int n_threads;
	obs_properties_add_int_slider(whisper_params_group, "n_threads", MT_("n_threads"), 1, 8, 1);
	// int n_max_text_ctx;     // max tokens to use from past text as prompt for the decoder
	obs_properties_add_int_slider(whisper_params_group, "n_max_text_ctx", MT_("n_max_text_ctx"),
				      0, 16384, 100);
	// int offset_ms;          // start offset in ms
	// int duration_ms;        // audio duration to process in ms
	// bool translate;
	obs_properties_add_bool(whisper_params_group, "whisper_translate",
				MT_("whisper_translate"));
	// bool no_context;        // do not use past transcription (if any) as initial prompt for the decoder
	obs_properties_add_bool(whisper_params_group, "no_context", MT_("no_context"));
	// bool single_segment;    // force single segment output (useful for streaming)
	obs_properties_add_bool(whisper_params_group, "single_segment", MT_("single_segment"));
	// bool print_special;     // print special tokens (e.g. <SOT>, <EOT>, <BEG>, etc.)
	obs_properties_add_bool(whisper_params_group, "print_special", MT_("print_special"));
	// bool print_progress;    // print progress information
	obs_properties_add_bool(whisper_params_group, "print_progress", MT_("print_progress"));
	// bool print_realtime;    // print results from within whisper.cpp (avoid it, use callback instead)
	obs_properties_add_bool(whisper_params_group, "print_realtime", MT_("print_realtime"));
	// bool print_timestamps;  // print timestamps for each text segment when printing realtime
	obs_properties_add_bool(whisper_params_group, "print_timestamps", MT_("print_timestamps"));
	// bool  token_timestamps; // enable token-level timestamps
	obs_properties_add_bool(whisper_params_group, "token_timestamps", MT_("token_timestamps"));
	// enable DTW timestamps
	obs_properties_add_bool(whisper_params_group, "dtw_token_timestamps",
				MT_("dtw_token_timestamps"));
	// float thold_pt;         // timestamp token probability threshold (~0.01)
	obs_properties_add_float_slider(whisper_params_group, "thold_pt", MT_("thold_pt"), 0.0f,
					1.0f, 0.05f);
	// float thold_ptsum;      // timestamp token sum probability threshold (~0.01)
	obs_properties_add_float_slider(whisper_params_group, "thold_ptsum", MT_("thold_ptsum"),
					0.0f, 1.0f, 0.05f);
	// int   max_len;          // max segment length in characters
	obs_properties_add_int_slider(whisper_params_group, "max_len", MT_("max_len"), 0, 100, 1);
	// bool  split_on_word;    // split on word rather than on token (when used with max_len)
	obs_properties_add_bool(whisper_params_group, "split_on_word", MT_("split_on_word"));
	// int   max_tokens;       // max tokens per segment (0 = no limit)
	obs_properties_add_int_slider(whisper_params_group, "max_tokens", MT_("max_tokens"), 0, 100,
				      1);
	// bool speed_up;          // speed-up the audio by 2x using Phase Vocoder
	obs_properties_add_bool(whisper_params_group, "speed_up", MT_("speed_up"));
	// const char * initial_prompt;
	obs_properties_add_text(whisper_params_group, "initial_prompt", MT_("initial_prompt"),
				OBS_TEXT_DEFAULT);
	// bool suppress_blank
	obs_properties_add_bool(whisper_params_group, "suppress_blank", MT_("suppress_blank"));
	// bool suppress_non_speech_tokens
	obs_properties_add_bool(whisper_params_group, "suppress_non_speech_tokens",
				MT_("suppress_non_speech_tokens"));
	// float temperature
	obs_properties_add_float_slider(whisper_params_group, "temperature", MT_("temperature"),
					0.0f, 1.0f, 0.05f);
	// float max_initial_ts
	obs_properties_add_float_slider(whisper_params_group, "max_initial_ts",
					MT_("max_initial_ts"), 0.0f, 1.0f, 0.05f);
	// float length_penalty
	obs_properties_add_float_slider(whisper_params_group, "length_penalty",
					MT_("length_penalty"), -1.0f, 1.0f, 0.1f);

	// Add a informative text about the plugin
	obs_properties_add_text(
		ppts, "info",
		QString(PLUGIN_INFO_TEMPLATE).arg(PLUGIN_VERSION).toStdString().c_str(),
		OBS_TEXT_INFO);

	UNUSED_PARAMETER(data);
	return ppts;
}
