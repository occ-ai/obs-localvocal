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
#include "timed-metadata/timed-metadata-utils.h"
#include "translation/language_codes.h"

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
					 struct transcription_filter_data *gf,
					 const std::string &source_language)
{
	const std::string last_text = gf->last_text;
	gf->last_text = sentence;
	if (gf->translate && !sentence.empty() && sentence != last_text) {
		obs_log(gf->log_level, "Translating text. %s -> %s", source_language.c_str(),
			gf->target_lang.c_str());
		std::string translated_text;
		if (translate(gf->translation_ctx, sentence,
			      language_codes_from_whisper[source_language], gf->target_lang,
			      translated_text) == OBS_POLYGLOT_TRANSLATION_SUCCESS) {
			if (gf->log_words) {
				obs_log(LOG_INFO, "Translation: '%s' -> '%s'", sentence.c_str(),
					translated_text.c_str());
			}
			return translated_text;
		} else {
			obs_log(gf->log_level, "Failed to translate text");
		}
	}
	return "";
}

void send_sentence_to_file(struct transcription_filter_data *gf,
			   const DetectionResultWithText &result, const std::string &str_copy,
			   const std::string &translated_sentence)
{
	// Check if we should save the sentence
	if (gf->save_only_while_recording && !obs_frontend_recording_active()) {
		// We are not recording, do not save the sentence to file
		return;
	}

	std::string translated_file_path = "";
	bool write_translations = gf->translate && !translated_sentence.empty();

	// if translation is enabled, save the translated sentence to another file
	if (write_translations) {
		// add a postfix to the file name (without extension) with the translation target language
		std::string output_file_path = gf->output_file_path;
		std::string file_extension =
			output_file_path.substr(output_file_path.find_last_of(".") + 1);
		std::string file_name =
			output_file_path.substr(0, output_file_path.find_last_of("."));
		translated_file_path = file_name + "_" + gf->target_lang + "." + file_extension;
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
		if (write_translations) {
			std::ofstream translated_output_file(translated_file_path, openmode);
			translated_output_file << translated_sentence << std::endl;
			translated_output_file.close();
		}
	} else {
		if (result.start_timestamp_ms == 0 && result.end_timestamp_ms == 0) {
			// No timestamps, do not save the sentence to srt
			return;
		}

		obs_log(gf->log_level, "Saving sentence to file %s, sentence #%d",
			gf->output_file_path.c_str(), gf->sentence_number);
		// Append sentence to file in .srt format
		std::ofstream output_file(gf->output_file_path, openmode);
		output_file << gf->sentence_number << std::endl;
		// use the start and end timestamps to calculate the start and end time in srt format
		auto format_ts_for_srt = [](std::ofstream &output_stream, uint64_t ts) {
			uint64_t time_s = ts / 1000;
			uint64_t time_m = time_s / 60;
			uint64_t time_h = time_m / 60;
			uint64_t time_ms_rem = ts % 1000;
			uint64_t time_s_rem = time_s % 60;
			uint64_t time_m_rem = time_m % 60;
			uint64_t time_h_rem = time_h % 60;
			output_stream << std::setfill('0') << std::setw(2) << time_h_rem << ":"
				      << std::setfill('0') << std::setw(2) << time_m_rem << ":"
				      << std::setfill('0') << std::setw(2) << time_s_rem << ","
				      << std::setfill('0') << std::setw(3) << time_ms_rem;
		};
		format_ts_for_srt(output_file, result.start_timestamp_ms);
		output_file << " --> ";
		format_ts_for_srt(output_file, result.end_timestamp_ms);
		output_file << std::endl;

		output_file << str_copy << std::endl;
		output_file << std::endl;
		output_file.close();

		if (write_translations) {
			obs_log(gf->log_level, "Saving translation to file %s, sentence #%d",
				translated_file_path.c_str(), gf->sentence_number);

			// Append translated sentence to file in .srt format
			std::ofstream translated_output_file(translated_file_path, openmode);
			translated_output_file << gf->sentence_number << std::endl;
			format_ts_for_srt(translated_output_file, result.start_timestamp_ms);
			translated_output_file << " --> ";
			format_ts_for_srt(translated_output_file, result.end_timestamp_ms);
			translated_output_file << std::endl;

			translated_output_file << translated_sentence << std::endl;
			translated_output_file << std::endl;
			translated_output_file.close();
		}

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
	if (!result.text.empty() && result.result == DETECTION_RESULT_SPEECH) {
		// this sub should be rendered - update the last sub render time
		gf->last_sub_render_time = now_ms();
		gf->cleared_last_sub = false;
	}

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
	}

	// time the translation
	uint64_t start_time = now_ms();

	// send the sentence to translation (if enabled)
	std::string translated_sentence =
		send_sentence_to_translation(str_copy, gf, result.language);

	if (gf->translate) {
		// log the translation time
		obs_log(gf->log_level, "Translation time: %llu ms", now_ms() - start_time);

		// send the translated sentence to the server
		send_timed_metadata_to_server(gf, NON_WHISPER_TRANSLATE, str_copy, result.language,
					      translated_sentence, gf->target_lang);

		// send the translated sentence to the selected output
		if (gf->translation_output == "none") {
			// overwrite the original text with the translated text
			str_copy = translated_sentence;
		} else {
			if (gf->buffered_output) {
				gf->translation_monitor.addSentence(translated_sentence);
			} else {
				// non-buffered output - send the sentence to the selected source
				send_caption_to_source(gf->translation_output, translated_sentence,
						       gf);
			}
		}
	} else {
		send_timed_metadata_to_server(gf, TRANSCRIBE, str_copy, result.language, "", "");
	}

	if (gf->buffered_output) {
		gf->captions_monitor.addSentence(str_copy);
	} else {
		// non-buffered output - send the sentence to the selected source
		send_caption_to_source(gf->text_source_name, str_copy, gf);
	}

	if (gf->caption_to_stream) {
		send_caption_to_stream(result, str_copy, gf);
	}

	if (gf->save_to_file && gf->output_file_path != "") {
		send_sentence_to_file(gf, result, str_copy, translated_sentence);
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
