#ifndef TIMED_METADATA_UTILS_H
#define TIMED_METADATA_UTILS_H

#include <string>
#include <vector>

#include "transcription-filter-data.h"

enum Translation_Mode { WHISPER_TRANSLATE, NON_WHISPER_TRANSLATE, TRANSCRIBE };

void send_timed_metadata_to_server(struct transcription_filter_data *gf, Translation_Mode mode,
				   const std::string &source_text, const std::string &source_lang,
				   const std::string &target_text, const std::string &target_lang);

#endif // TIMED_METADATA_UTILS_H
