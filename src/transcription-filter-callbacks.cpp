#ifdef _WIN32
#define NOMINMAX
#endif

#include <obs.h>
#include <obs-frontend-api.h>

#include <curl/curl.h>

#include <fstream>
#include <iomanip>
#include <regex>
#include <string>
#include <vector>

#include "transcription-filter-callbacks.h"
#include "transcription-utils.h"
#include "translation/translation.h"
#include "translation/translation-includes.h"
#include "whisper-utils/whisper-utils.h"
#include "whisper-utils/whisper-model-utils.h"

void send_caption_to_source(const std::string &target_source_name, const std::string &caption,
			    struct transcription_filter_data *gf)
{
	if (target_source_name.empty()) {
		return;
	}
	auto target = obs_get_source_by_name(target_source_name.c_str());
	if (!target) {
		obs_log(gf->log_level, "text_source target is null");
		return;
	}
	auto text_settings = obs_source_get_settings(target);
	obs_data_set_string(text_settings, "text", caption.c_str());
	obs_source_update(target, text_settings);
	obs_source_release(target);
}

void audio_chunk_callback(struct transcription_filter_data *gf, const float *pcm32f_data,
			  size_t frames, int vad_state, const DetectionResultWithText &result)
{
	UNUSED_PARAMETER(gf);
	UNUSED_PARAMETER(pcm32f_data);
	UNUSED_PARAMETER(frames);
	UNUSED_PARAMETER(vad_state);
	UNUSED_PARAMETER(result);
	// stub
}

std::string send_sentence_to_translation(const std::string &sentence,
					 struct transcription_filter_data *gf)
{
	const std::string last_text = gf->last_text;
	gf->last_text = sentence;
	if (gf->translate && !sentence.empty() && sentence != last_text) {
		obs_log(gf->log_level, "Translating text. %s -> %s", gf->source_lang.c_str(),
			gf->target_lang.c_str());
		std::string translated_text;
		if (translate(gf->translation_ctx, sentence, gf->source_lang, gf->target_lang,
			      translated_text) == OBS_POLYGLOT_TRANSLATION_SUCCESS) {
			if (gf->log_words) {
				obs_log(LOG_INFO, "Translation: '%s' -> '%s'", sentence.c_str(),
					translated_text.c_str());
			}
			if (gf->translation_output == "none") {
				// overwrite the original text with the translated text
				return translated_text;
			} else {
				// send the translation to the selected source
				send_caption_to_source(gf->translation_output, translated_text, gf);
			}
		} else {
			obs_log(gf->log_level, "Failed to translate text");
		}
	}
	return sentence;
}

void send_sentence_to_file(struct transcription_filter_data *gf,
			   const DetectionResultWithText &result, const std::string &str_copy)
{
	// Check if we should save the sentence
	if (gf->save_only_while_recording && !obs_frontend_recording_active()) {
		// We are not recording, do not save the sentence to file
		return;
	}
	// should the file be truncated?
	std::ios_base::openmode openmode = std::ios::out;
	if (gf->truncate_output_file) {
		openmode |= std::ios::trunc;
	} else {
		openmode |= std::ios::app;
	}
	if (!gf->save_srt) {
		// Write raw sentence to file
		std::ofstream output_file(gf->output_file_path, openmode);
		output_file << str_copy << std::endl;
		output_file.close();
	} else {
		obs_log(gf->log_level, "Saving sentence to file %s, sentence #%d",
			gf->output_file_path.c_str(), gf->sentence_number);
		// Append sentence to file in .srt format
		std::ofstream output_file(gf->output_file_path, openmode);
		output_file << gf->sentence_number << std::endl;
		// use the start and end timestamps to calculate the start and end time in srt format
		auto format_ts_for_srt = [&output_file](uint64_t ts) {
			uint64_t time_s = ts / 1000;
			uint64_t time_m = time_s / 60;
			uint64_t time_h = time_m / 60;
			uint64_t time_ms_rem = ts % 1000;
			uint64_t time_s_rem = time_s % 60;
			uint64_t time_m_rem = time_m % 60;
			uint64_t time_h_rem = time_h % 60;
			output_file << std::setfill('0') << std::setw(2) << time_h_rem << ":"
				    << std::setfill('0') << std::setw(2) << time_m_rem << ":"
				    << std::setfill('0') << std::setw(2) << time_s_rem << ","
				    << std::setfill('0') << std::setw(3) << time_ms_rem;
		};
		format_ts_for_srt(result.start_timestamp_ms);
		output_file << " --> ";
		format_ts_for_srt(result.end_timestamp_ms);
		output_file << std::endl;

		output_file << str_copy << std::endl;
		output_file << std::endl;
		output_file.close();
		gf->sentence_number++;
	}
}

void send_caption_to_stream(DetectionResultWithText result, const std::string &str_copy,
			    struct transcription_filter_data *gf)
{
	obs_output_t *streaming_output = obs_frontend_get_streaming_output();
	if (streaming_output) {
		// calculate the duration in seconds
		const uint64_t duration = result.end_timestamp_ms - result.start_timestamp_ms;
		obs_log(gf->log_level, "Sending caption to streaming output: %s", str_copy.c_str());
		obs_output_output_caption_text2(streaming_output, str_copy.c_str(),
						(double)duration / 1000.0);
		obs_output_release(streaming_output);
	}
}

void set_text_callback(struct transcription_filter_data *gf,
		       const DetectionResultWithText &resultIn)
{
	DetectionResultWithText result = resultIn;
	uint64_t now = now_ms();
	if (result.text.empty() || result.result != DETECTION_RESULT_SPEECH) {
		// check if we should clear the current sub depending on the minimum subtitle duration
		if ((now - gf->last_sub_render_time) > gf->min_sub_duration) {
			// clear the current sub, run an empty sub
			result.text = "";
		} else {
			// nothing to do, the incoming sub is empty
			return;
		}
	}
	gf->last_sub_render_time = now;

	std::string str_copy = result.text;

	// recondition the text - only if the output is not English
	if (gf->whisper_params.language != nullptr &&
	    strcmp(gf->whisper_params.language, "en") != 0) {
		str_copy = fix_utf8(str_copy);
	} else {
		// only remove leading and trailing non-alphanumeric characters if the output is English
		str_copy = remove_leading_trailing_nonalpha(str_copy);
	}

	// if suppression is enabled, check if the text is in the suppression list
	if (!gf->filter_words_replace.empty()) {
		const std::string original_str_copy = str_copy;
		// check if the text is in the suppression list
		for (const auto &filter_words : gf->filter_words_replace) {
			// if filter exists within str_copy, replace it with the replacement
			str_copy = std::regex_replace(str_copy,
						      std::regex(std::get<0>(filter_words)),
						      std::get<1>(filter_words));
		}
		// if the text was modified, log the original and modified text
		if (original_str_copy != str_copy) {
			obs_log(gf->log_level, "------ Suppressed text: '%s' -> '%s'",
				original_str_copy.c_str(), str_copy.c_str());
		}
		if (remove_leading_trailing_nonalpha(str_copy).empty()) {
			// if the text is empty after suppression, return
			return;
		}
	}

	if (gf->buffered_output) {
		gf->captions_monitor.addSentence(str_copy);
	} else {
		// non-buffered output
		if (gf->translate) {
			// send the sentence to translation (if enabled)
			str_copy = send_sentence_to_translation(str_copy, gf);
		} else {
			// send the sentence to the selected source
			send_caption_to_source(gf->text_source_name, str_copy, gf);
		}
	}

	if (gf->caption_to_stream) {
		send_caption_to_stream(result, str_copy, gf);
	}

	if (gf->output_file_path != "" && gf->text_source_name.empty()) {
		send_sentence_to_file(gf, result, str_copy);
	}
};

void recording_state_callback(enum obs_frontend_event event, void *data)
{
	struct transcription_filter_data *gf_ =
		static_cast<struct transcription_filter_data *>(data);
	if (event == OBS_FRONTEND_EVENT_RECORDING_STARTING) {
		if (gf_->save_srt && gf_->save_only_while_recording) {
			obs_log(gf_->log_level, "Recording started. Resetting srt file.");
			// truncate file if it exists
			std::ofstream output_file(gf_->output_file_path,
						  std::ios::out | std::ios::trunc);
			output_file.close();
			gf_->sentence_number = 1;
			gf_->start_timestamp_ms = now_ms();
		}
	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
		if (gf_->save_srt && gf_->save_only_while_recording &&
		    gf_->rename_file_to_match_recording) {
			obs_log(gf_->log_level, "Recording stopped. Rename srt file.");
			// rename file to match the recording file name with .srt extension
			// use obs_frontend_get_last_recording to get the last recording file name
			std::string recording_file_name = obs_frontend_get_last_recording();
			// remove the extension
			recording_file_name = recording_file_name.substr(
				0, recording_file_name.find_last_of("."));
			std::string srt_file_name = recording_file_name + ".srt";
			// rename the file
			std::rename(gf_->output_file_path.c_str(), srt_file_name.c_str());
		}
	}
}

void reset_caption_state(transcription_filter_data *gf_)
{
	if (gf_->captions_monitor.isEnabled()) {
		gf_->captions_monitor.clear();
	}
	send_caption_to_source(gf_->text_source_name, "", gf_);
	// flush the buffer
	{
		std::lock_guard<std::mutex> lock(gf_->whisper_buf_mutex);
		for (size_t c = 0; c < gf_->channels; c++) {
			if (gf_->input_buffers[c].data != nullptr) {
				circlebuf_free(&gf_->input_buffers[c]);
			}
		}
		if (gf_->info_buffer.data != nullptr) {
			circlebuf_free(&gf_->info_buffer);
		}
		if (gf_->whisper_buffer.data != nullptr) {
			circlebuf_free(&gf_->whisper_buffer);
		}
	}
}

void media_play_callback(void *data_, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	transcription_filter_data *gf_ = static_cast<struct transcription_filter_data *>(data_);
	obs_log(gf_->log_level, "media_play");
	gf_->active = true;
}

void media_started_callback(void *data_, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	transcription_filter_data *gf_ = static_cast<struct transcription_filter_data *>(data_);
	obs_log(gf_->log_level, "media_started");
	gf_->active = true;
	reset_caption_state(gf_);
}
void media_pause_callback(void *data_, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	transcription_filter_data *gf_ = static_cast<struct transcription_filter_data *>(data_);
	obs_log(gf_->log_level, "media_pause");
	gf_->active = false;
}
void media_restart_callback(void *data_, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	transcription_filter_data *gf_ = static_cast<struct transcription_filter_data *>(data_);
	obs_log(gf_->log_level, "media_restart");
	gf_->active = true;
	reset_caption_state(gf_);
}
void media_stopped_callback(void *data_, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	transcription_filter_data *gf_ = static_cast<struct transcription_filter_data *>(data_);
	obs_log(gf_->log_level, "media_stopped");
	gf_->active = false;
	reset_caption_state(gf_);
}

void enable_callback(void *data_, calldata_t *cd)
{
	transcription_filter_data *gf_ = static_cast<struct transcription_filter_data *>(data_);
	bool enable = calldata_bool(cd, "enabled");
	if (enable) {
		obs_log(gf_->log_level, "enable_callback: enable");
		gf_->active = true;
		reset_caption_state(gf_);
		update_whisper_model(gf_);
	} else {
		obs_log(gf_->log_level, "enable_callback: disable");
		gf_->active = false;
		reset_caption_state(gf_);
		shutdown_whisper_thread(gf_);
	}
}
