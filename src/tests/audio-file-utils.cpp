
#include "audio-file-utils.h"
#include "plugin-support.h"

#include <obs-module.h>

#include <vector>
#include <functional>

#if defined(_WIN32) || defined(__APPLE__)

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libavutil/log.h>
}

std::vector<std::vector<uint8_t>>
read_audio_file(const char *filename, std::function<void(int, int)> initialization_callback)
{
	av_log_set_level(AV_LOG_QUIET);

	obs_log(LOG_INFO, "Reading audio file %s", filename);

	AVFormatContext *formatContext = nullptr;
	int ret = avformat_open_input(&formatContext, filename, nullptr, nullptr);
	if (ret != 0) {
		char errbuf[AV_ERROR_MAX_STRING_SIZE];
		av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
		obs_log(LOG_ERROR, "Error opening file: %s", errbuf);
		return {};
	}

	if (avformat_find_stream_info(formatContext, nullptr) < 0) {
		obs_log(LOG_ERROR, "Error finding stream information");
		return {};
	}

	int audioStreamIndex = -1;
	for (unsigned int i = 0; i < formatContext->nb_streams; i++) {
		if (formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			audioStreamIndex = i;
			break;
		}
	}

	if (audioStreamIndex == -1) {
		obs_log(LOG_ERROR, "No audio stream found");
		return {};
	}

	// print information about the file
	av_dump_format(formatContext, 0, filename, 0);

	// if the sample format is not float, return
	if (formatContext->streams[audioStreamIndex]->codecpar->format != AV_SAMPLE_FMT_FLTP) {
		obs_log(LOG_ERROR,
			"Sample format is not float (it is %s). Encode the audio file with float planar sample format."
			" For example, use the command 'ffmpeg -i input.mp3 -f f32le -acodec pcm_f32le output.f32le'",
			"convert the audio file to float format.",
			av_get_sample_fmt_name(
				(AVSampleFormat)formatContext->streams[audioStreamIndex]
					->codecpar->format));
		return {};
	}

	initialization_callback(formatContext->streams[audioStreamIndex]->codecpar->sample_rate,
				formatContext->streams[audioStreamIndex]->codecpar->channels);

	AVCodecParameters *codecParams = formatContext->streams[audioStreamIndex]->codecpar;
	const AVCodec *codec = avcodec_find_decoder(codecParams->codec_id);
	if (!codec) {
		obs_log(LOG_ERROR, "Decoder not found");
		return {};
	}

	AVCodecContext *codecContext = avcodec_alloc_context3(codec);
	if (!codecContext) {
		obs_log(LOG_ERROR, "Failed to allocate codec context");
		return {};
	}

	if (avcodec_parameters_to_context(codecContext, codecParams) < 0) {
		obs_log(LOG_ERROR, "Failed to copy codec parameters to codec context");
		return {};
	}

	if (avcodec_open2(codecContext, codec, nullptr) < 0) {
		obs_log(LOG_ERROR, "Failed to open codec");
		return {};
	}

	AVFrame *frame = av_frame_alloc();
	AVPacket packet;

	std::vector<std::vector<uint8_t>> buffer(
		formatContext->streams[audioStreamIndex]->codecpar->channels);

	while (av_read_frame(formatContext, &packet) >= 0) {
		if (packet.stream_index == audioStreamIndex) {
			if (avcodec_send_packet(codecContext, &packet) == 0) {
				while (avcodec_receive_frame(codecContext, frame) == 0) {
					// push data to the buffer
					for (int j = 0; j < codecContext->channels; j++) {
						buffer[j].insert(buffer[j].end(), frame->data[j],
								 frame->data[j] +
									 frame->nb_samples *
										 sizeof(float));
					}
				}
			}
		}
		av_packet_unref(&packet);
	}

	av_frame_free(&frame);
	avcodec_free_context(&codecContext);
	avformat_close_input(&formatContext);

	return buffer;
}

void write_audio_wav_file(const std::string &filename, const float *pcm32f_data,
			  const size_t frames)
{
	av_log_set_level(AV_LOG_QUIET);

	AVFormatContext *formatContext = nullptr;
	AVCodecContext *codecContext = nullptr;
	AVStream *stream = nullptr;
	AVFrame *frame = nullptr;
	AVPacket packet;
	int ret = 0;

	avformat_alloc_output_context2(&formatContext, nullptr, nullptr, filename.c_str());
	if (!formatContext) {
		obs_log(LOG_ERROR, "Failed to allocate output context");
		return;
	}

	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PCM_F32LE);
	if (!codec) {
		obs_log(LOG_ERROR, "Failed to find encoder");
		return;
	}

	stream = avformat_new_stream(formatContext, codec);
	if (!stream) {
		obs_log(LOG_ERROR, "Failed to create new stream");
		return;
	}

	codecContext = avcodec_alloc_context3(codec);
	if (!codecContext) {
		obs_log(LOG_ERROR, "Failed to allocate codec context");
		return;
	}

	codecContext->sample_fmt = AV_SAMPLE_FMT_FLTP;
	codecContext->sample_rate = 16000;
	codecContext->channels = 1;
	codecContext->channel_layout = AV_CH_LAYOUT_MONO;
	codecContext->bit_rate = 64000;
	codecContext->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

	if (avcodec_open2(codecContext, codec, nullptr) < 0) {
		obs_log(LOG_ERROR, "Failed to open codec");
		return;
	}

	if (avcodec_parameters_from_context(stream->codecpar, codecContext) < 0) {
		obs_log(LOG_ERROR, "Failed to copy codec parameters to stream");
		return;
	}

	if (avio_open(&formatContext->pb, filename.c_str(), AVIO_FLAG_WRITE) < 0) {
		obs_log(LOG_ERROR, "Failed to open file");
		return;
	}

	if (avformat_write_header(formatContext, nullptr) < 0) {
		obs_log(LOG_ERROR, "Failed to write header");
		return;
	}

	const int frame_size = 1024;
	const int frame_size_in_bytes = frame_size * sizeof(float);
	frame = av_frame_alloc();
	frame->nb_samples = frame_size;
	frame->format = codecContext->sample_fmt;
	frame->ch_layout = codecContext->ch_layout;

	ret = av_frame_get_buffer(frame, 0);
	if (ret < 0) {
		char errbuf[AV_ERROR_MAX_STRING_SIZE];
		av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
		obs_log(LOG_ERROR, "Failed to allocate frame buffer: %s", errbuf);
		return;
	}

	for (size_t i = 0; i < frames; i += frame_size) {
		av_init_packet(&packet);
		packet.data = nullptr;
		packet.size = 0;

		for (int k = 0; k < codecContext->channels; k++) {
			if (i + frame_size < frames) {
				memcpy(frame->data[k], pcm32f_data + i, frame_size_in_bytes);
			} else {
				// zero pad the last frame
				memset(frame->data[k], 0, frame_size_in_bytes);
				memcpy(frame->data[k], pcm32f_data + i,
				       (frames - i) * sizeof(float));
			}
		}

		ret = avcodec_send_frame(codecContext, frame);
		if (ret < 0) {
			obs_log(LOG_ERROR, "Failed to send frame");
			break;
		}

		ret = avcodec_receive_packet(codecContext, &packet);
		if (ret < 0) {
			obs_log(LOG_ERROR, "Failed to receive packet");
			break;
		}

		av_packet_rescale_ts(&packet, codecContext->time_base, stream->time_base);
		packet.stream_index = stream->index;

		ret = av_interleaved_write_frame(formatContext, &packet);
		if (ret < 0) {
			obs_log(LOG_ERROR, "Failed to write frame");
			break;
		}

		av_packet_unref(&packet);
	}

	if (ret >= 0) {
		av_write_trailer(formatContext);
	}

	av_frame_free(&frame);
	avcodec_free_context(&codecContext);
	avformat_free_context(formatContext);

	if (ret < 0) {
		obs_log(LOG_ERROR, "Failed to write audio file %s", filename.c_str());
	}
}

#else

std::vector<std::vector<uint8_t>>
read_audio_file(const char *filename, std::function<void(int, int)> initialization_callback)
{
	obs_log(LOG_ERROR, "Reading audio files is not supported on this platform");
	return {};
}

void write_audio_wav_file(const std::string &filename, const float *pcm32f_data,
			  const size_t frames)
{
	obs_log(LOG_ERROR, "Writing audio files is not supported on this platform");
}

#endif
