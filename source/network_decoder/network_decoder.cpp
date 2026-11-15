#include "headers.hpp"
#include "network_decoder.hpp"
#include "network_downloader.hpp"
#include <numeric>

extern "C" void memcpy_asm(u8 *, u8 *, int);

/* --------------------------------------------------------- */
/*                  NetworkDecoderFFmpegIOData               */
/* --------------------------------------------------------- */
void NetworkDecoderFFmpegIOData::deinit_(int type, bool deinit_stream) {
	// Close the demuxer while its custom I/O and callback data still exist.
	avformat_close_input(&format_context[type]);
	if (io_context[type]) {
		av_freep(&io_context[type]->buffer);
	}
	avio_context_free(&io_context[type]);
	delete opaque[type];
	opaque[type] = NULL;
	if (deinit_stream && network_stream[type]) {
		network_stream[type]->quit_request = true;
		network_stream[type] = NULL;
	}
}
void NetworkDecoderFFmpegIOData::deinit(bool deinit_stream) {
	// Muxed playback stores the same stream in both slots. The downloader may
	// delete it as soon as quit_request is set, so never dereference it twice.
	NetworkStream *video_stream = network_stream[VIDEO];
	NetworkStream *audio_stream = network_stream[AUDIO];
	for (int type = 0; type < 2; type++) {
		deinit_(type, false);
	}
	if (deinit_stream) {
		network_stream[VIDEO] = network_stream[AUDIO] = NULL;
		if (video_stream) {
			video_stream->quit_request = true;
		}
		if (audio_stream && audio_stream != video_stream) {
			audio_stream->quit_request = true;
		}
	}
}
static int read_network_stream(void *opaque, u8 *buf, int buf_size_) { // size or AVERROR_EOF
	NetworkDecoder *decoder = ((std::pair<NetworkDecoder *, NetworkStream *> *)opaque)->first;
	NetworkStream *stream = ((std::pair<NetworkDecoder *, NetworkStream *> *)opaque)->second;
	size_t buf_size = buf_size_;

	// Util_log_save("dec", "read " + std::to_string(stream->get_read_head()) + " " + std::to_string(buf_size_) + " " +
	// std::to_string(stream->len));
	bool cpu_limited = false;
	int failure_code = AVERROR(EIO);
	while (true) {
		bool eof = false;
		size_t read_size = stream->read_next_data(buf, buf_size, eof);
		if (read_size) {
			if (cpu_limited) {
				remove_cpu_limit(ADDITIONAL_CPU_LIMIT);
			}
			stream->network_waiting_status = NULL;
			return (int)read_size;
		}
		if (eof) {
			if (cpu_limited) {
				remove_cpu_limit(ADDITIONAL_CPU_LIMIT);
			}
			stream->network_waiting_status = NULL;
			return AVERROR_EOF;
		}
		if (!stream->disable_interrupt && decoder->interrupt) {
			logger.info("dec", "read interrupt");
			decoder->need_reinit = true;
			failure_code = AVERROR_EXIT;
			goto fail;
		}
		stream->network_waiting_status = "Reading stream";
		if (!cpu_limited) {
			cpu_limited = true;
			add_cpu_limit(ADDITIONAL_CPU_LIMIT);
		}
		usleep(20000);
		if (stream->error || stream->quit_request) {
			// show the error message only once per stream
			if (!stream->read_dead_tried) {
				stream->read_dead_tried = true,
				logger.error("dec", "read dead stream : " + std::string(stream->error ? "error" : "quitted"));
			}
			goto fail;
		}
	}

fail:
	if (cpu_limited) {
		remove_cpu_limit(ADDITIONAL_CPU_LIMIT);
	}
	stream->network_waiting_status = NULL;
	return failure_code;
}
static int64_t seek_network_stream(void *opaque, s64 offset, int whence) { // size or AVERROR_EOF
	NetworkDecoder *decoder = ((std::pair<NetworkDecoder *, NetworkStream *> *)opaque)->first;
	NetworkStream *stream = ((std::pair<NetworkDecoder *, NetworkStream *> *)opaque)->second;

	while (!stream->ready) {
		if (!stream->disable_interrupt && decoder->interrupt) {
			stream->network_waiting_status = NULL;
			return AVERROR_EXIT;
		}
		stream->network_waiting_status = "Reading stream (init, seek)";
		usleep(20000);
		if (stream->error || stream->quit_request) {
			stream->network_waiting_status = NULL;
			return -1;
		}
	}
	stream->network_waiting_status = NULL;

	if (whence == AVSEEK_SIZE) {
		// Util_log_save("dec", "inquire size : " + std::to_string(stream->len));
		return stream->len;
	}

	whence &= ~AVSEEK_FORCE;
	int64_t base = 0;
	if (whence == SEEK_CUR) {
		base = stream->get_read_head();
	} else if (whence == SEEK_END) {
		base = stream->len;
	} else if (whence != SEEK_SET) {
		return AVERROR(EINVAL);
	}
	if (offset < -base || offset > (int64_t)stream->len - base) {
		return AVERROR(EINVAL);
	}
	u64 new_pos = base + offset;

	// Util_log_save("dec", "seek " + std::to_string(new_pos) + " " + std::to_string(stream->len));

	if (new_pos > stream->len) {
		return -1;
	}

	stream->set_read_head(new_pos);
	return new_pos;
}

void ffmpeg_log_callback(void *, int level, const char *fmt, va_list vargs) {
	char buf[256] = {0};
	vsnprintf(buf, 256, fmt, vargs);
	if (level <= AV_LOG_WARNING) {
		logger.info("ffmpeg", buf);
	}
}

#ifdef FOURTHTUBE_HIGH_RAM_BUILD
#define NETWORK_BUFFER_SIZE 0x20000
#else
#define NETWORK_BUFFER_SIZE 0x10000
#endif
Result_with_string NetworkDecoderFFmpegIOData::init_(int type, NetworkDecoder *parent_decoder) {
	Result_with_string result;
	int ffmpeg_result;

	network_stream[type]->set_read_head(0);

	opaque[type] = new std::pair<NetworkDecoder *, NetworkStream *>(parent_decoder, network_stream[type]);
	unsigned char *buffer = (unsigned char *)av_malloc(NETWORK_BUFFER_SIZE);
	if (!buffer) {
		result.error_description = "network buffer allocation failed";
		result.code = DEF_ERR_OUT_OF_MEMORY;
		result.string = DEF_ERR_OUT_OF_MEMORY_STR;
		return result;
	}
	io_context[type] = avio_alloc_context(buffer, NETWORK_BUFFER_SIZE, 0, opaque[type], read_network_stream, NULL,
	                                      seek_network_stream);
	if (!io_context[type]) {
		av_free(buffer);
		result.error_description = "IO context allocation failed";
		result.code = DEF_ERR_OUT_OF_MEMORY;
		result.string = DEF_ERR_OUT_OF_MEMORY_STR;
		return result;
	}
	format_context[type] = avformat_alloc_context();
	if (!format_context[type]) {
		result.error_description = "format context allocation failed";
		result.code = DEF_ERR_OUT_OF_MEMORY;
		result.string = DEF_ERR_OUT_OF_MEMORY_STR;
		return result;
	}
	format_context[type]->pb = io_context[type];
	// FourthTube opens known, seekable YouTube media streams (MP4/WebM), not
	// arbitrary local files. Keep format and stream probing bounded so startup
	// does not download/analyze megabytes before playback can begin. The MP4
	// initialization segment contains codec parameters; retain enough probing
	// headroom for unusual streams while avoiding FFmpeg's generic 5 MiB default.
	format_context[type]->format_probesize = 64 * 1024;
	format_context[type]->probesize = 1024 * 1024;
	format_context[type]->max_analyze_duration = 2 * AV_TIME_BASE;
	ffmpeg_result = avformat_open_input(&format_context[type], "yay", NULL, NULL);
	if (ffmpeg_result != 0) {
		result.error_description = "avformat_open_input() failed " + std::to_string(ffmpeg_result);
		goto fail;
	}
	ffmpeg_result = avformat_find_stream_info(format_context[type], NULL);
	if (ffmpeg_result < 0 || !format_context[type]) {
		result.error_description = "avformat_find_stream_info() failed " + std::to_string(ffmpeg_result);
		goto fail;
	}
	if (video_audio_separate) {
		if (format_context[type]->nb_streams != 1) {
			result.error_description = "nb_streams != 1 : " + std::to_string(format_context[type]->nb_streams);
			goto fail;
		} else if (format_context[type]->streams[0]->codecpar->codec_type !=
		           (type == VIDEO ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO)) {
			result.error_description =
			    "stream type wrong : " + std::to_string(format_context[type]->streams[0]->codecpar->codec_type);
			goto fail;
		}
		stream_index[type] = 0;
	} else {
		stream_index[VIDEO] = stream_index[AUDIO] = -1;
		for (size_t i = 0; i < format_context[BOTH]->nb_streams; i++) {
			if (format_context[BOTH]->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
				stream_index[VIDEO] = i;
			}
			if (format_context[BOTH]->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
				stream_index[AUDIO] = i;
			}
		}
	}
	return result;

fail:
	result.code = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS;
	result.string = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS_STR;
	return result;
}
#define RETURN_WITH_PREFIX_ON_ERROR(exp, prefix)                                                                       \
	do {                                                                                                               \
		if ((result = exp).code != 0) {                                                                                \
			result.error_description = prefix + result.error_description;                                              \
			return result;                                                                                             \
		}                                                                                                              \
	} while (0)
Result_with_string NetworkDecoderFFmpegIOData::init(NetworkStream *video_stream, NetworkStream *audio_stream,
                                                    NetworkDecoder *parent_decoder) {
	Result_with_string result;

	video_audio_separate = video_stream != audio_stream;
	audio_only = false;
	stream_index[VIDEO] = stream_index[AUDIO] = -1;
	network_stream[VIDEO] = video_stream;
	network_stream[AUDIO] = audio_stream;
	this->parent_decoder = parent_decoder;

	// init io
	if (video_audio_separate) {
		RETURN_WITH_PREFIX_ON_ERROR(init_(VIDEO, parent_decoder), "[v] ");
		RETURN_WITH_PREFIX_ON_ERROR(init_(AUDIO, parent_decoder), "[a] ");
	} else {
		RETURN_WITH_PREFIX_ON_ERROR(init_(VIDEO, parent_decoder), "[v+a] ");
	}

	if (stream_index[VIDEO] == -1) {
		audio_only = true;
	}
	if (stream_index[AUDIO] == -1) {
		result.error_description = "audio stream not found";
		result.code = -1;
		return result;
	}

	return result;
}
Result_with_string NetworkDecoderFFmpegIOData::init(NetworkStream *both_stream, NetworkDecoder *parent_decoder) {
	return init(both_stream, both_stream, parent_decoder);
}
Result_with_string NetworkDecoderFFmpegIOData::reinit() {
	deinit(false);
	return init(network_stream[VIDEO], network_stream[AUDIO], parent_decoder);
}
Result_with_string NetworkDecoderFFmpegIOData::reinit_stream(int type, int64_t seek_timestamp) {
	Result_with_string result;
	int ffmpeg_result;
	AVPacket *test_packet = NULL;

	logger.info("debug", "avformat reinit #" + std::to_string(type) + "...");
	deinit_(type, false);
	RETURN_WITH_PREFIX_ON_ERROR(init_(type, parent_decoder), video_audio_separate ? "[v+a]"
	                                                         : type == VIDEO      ? "[v]"
	                                                                              : "[a]");

	ffmpeg_result = av_seek_frame(format_context[type], stream_index[type], seek_timestamp, 0);
	if (ffmpeg_result != 0) {
		result.error_description = "av_seek_frame() failed : " + std::to_string(ffmpeg_result);
		goto ffmpeg_fail;
	}

	// the dts of the first packet read from the new format_context should be equal to seek_timestamp
	test_packet = av_packet_alloc();
	if (!test_packet) {
		result.code = DEF_ERR_OUT_OF_MEMORY;
		result.string = DEF_ERR_OUT_OF_MEMORY_STR;
		result.error_description = "av_packet_alloc() failed while reinitializing stream";
		return result;
	}
	ffmpeg_result = av_read_frame(format_context[type], test_packet);
	if (ffmpeg_result != 0) {
		result.error_description = "av_read_frame() failed : " + std::to_string(ffmpeg_result);
		goto ffmpeg_fail;
	}
	if (test_packet->dts != seek_timestamp) {
		logger.warning("debug",
		               "warning : " + std::to_string(seek_timestamp) + " -> " + std::to_string(test_packet->dts));
	} else {
		logger.info("debug", "ok");
	}
	goto end;

ffmpeg_fail:
	result.code = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS;
	result.string = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS_STR;

end:
	av_packet_free(&test_packet);
	return result;
}
double NetworkDecoderFFmpegIOData::get_duration() {
	return (double)format_context[video_audio_separate ? AUDIO : BOTH]->duration / AV_TIME_BASE;
}

/* ********************************************************* */
/*                  NetworkDecoderFilterData                 */
/* ********************************************************* */

void NetworkDecoderFilterData::deinit() {
	avfilter_graph_free(&audio_filter_graph);
	av_frame_free(&output_frame);
	audio_filter_src = audio_filter_sink = NULL;
}
Result_with_string NetworkDecoderFilterData::init(AVCodecContext *audio_context) {
	Result_with_string result;
	int ffmpeg_result = 0;

	first_frame = true;
	initial_timestamp = -1;
	received_frame_num = 0;
	input_time_base = av_q2d(audio_context->time_base);

	volume = volume_request;
	tempo = tempo_request;
	pitch = pitch_request;
	memcpy(equalizer_values, (void *)equalizer_values_request, sizeof(equalizer_values));

	{
		av_log_set_level(AV_LOG_ERROR);
		av_log_set_callback(ffmpeg_log_callback);

		output_frame = av_frame_alloc();
		if (!output_frame) {
			result.error_description = "av_frame_alloc() failed";
			goto fail;
		}

		audio_filter_graph = avfilter_graph_alloc();
		if (!audio_filter_graph) {
			result.error_description = "avfilter_graph_alloc() failed ";
			goto fail;
		}
		const AVFilter *abuffer = avfilter_get_by_name("abuffer");
		const AVFilter *atempo = avfilter_get_by_name("atempo");
		const AVFilter *asetrate = avfilter_get_by_name("asetrate");
		const AVFilter *asuperequalizer = avfilter_get_by_name("superequalizer");
		const AVFilter *aecho = avfilter_get_by_name("aecho");
		const AVFilter *volume_filter = avfilter_get_by_name("volume");
		const AVFilter *aformat = avfilter_get_by_name("aformat");
		const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");

#define ERROR_IF_NULL(val)                                                                                             \
	if (!val) {                                                                                                        \
		result.error_description = "filter " + std::string(#val) + " not found";                                       \
		goto fail;                                                                                                     \
	}
		ERROR_IF_NULL(abuffer);
		ERROR_IF_NULL(atempo);
		ERROR_IF_NULL(asetrate);
		ERROR_IF_NULL(asuperequalizer);
		ERROR_IF_NULL(aecho);
		ERROR_IF_NULL(volume_filter);
		ERROR_IF_NULL(aformat);
		ERROR_IF_NULL(abuffersink);

		std::vector<AVFilterContext *> filter_sequence;
		char option_buffer[256] = {0};
		// abuffer (source)
		snprintf(option_buffer, 256, "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
		         audio_context->time_base.num, audio_context->time_base.den, audio_context->sample_rate,
		         av_get_sample_fmt_name(audio_context->sample_fmt), audio_context->channel_layout);
		ffmpeg_result =
		    avfilter_graph_create_filter(&audio_filter_src, abuffer, NULL, option_buffer, NULL, audio_filter_graph);
		if (ffmpeg_result < 0) {
			result.error_description = "abuffer creation failed";
			goto fail;
		}
		filter_sequence.push_back(audio_filter_src);
		double atempo_value = tempo;
		// asetrate (simultaneous tempo/pitch shift)
		if (std::abs(1.0 - pitch) >= 0.01) {
			atempo_value /= pitch;
			AVFilterContext *audio_pitch_filter;
			snprintf(option_buffer, 256, "sample_rate=%f", audio_context->sample_rate * pitch);
			ffmpeg_result = avfilter_graph_create_filter(&audio_pitch_filter, asetrate, NULL, option_buffer, NULL,
			                                             audio_filter_graph);
			if (ffmpeg_result < 0) {
				result.error_description = "asetrate creation failed";
				goto fail;
			}
			filter_sequence.push_back(audio_pitch_filter);
		}

		// atempo (tempo filter)
		// atempo does not support a tempo lower than 0.5, so chain multiple atempo filters in that case
		while (std::abs(1.0 - atempo_value) >= 0.01) {
			double cur_tempo = std::max(0.5, atempo_value);
			atempo_value /= cur_tempo;
			AVFilterContext *audio_tempo_filter;
			snprintf(option_buffer, 256, "tempo=%f", cur_tempo);
			ffmpeg_result = avfilter_graph_create_filter(&audio_tempo_filter, atempo, NULL, option_buffer, NULL,
			                                             audio_filter_graph);
			if (ffmpeg_result < 0) {
				result.error_description = "atempo creation failed";
				goto fail;
			}
			filter_sequence.push_back(audio_tempo_filter);
		}
		// aecho (echo filter)
		/*
		snprintf(option_buffer, 256, "");
		Util_log_save("decoder", std::string("aecho : ") + option_buffer);
		ffmpeg_result = avfilter_graph_create_filter(&audio_echo_filter, aecho, NULL, option_buffer, NULL,
		audio_filter_graph); if (ffmpeg_result < 0) { result.error_description = "aecho creation failed"; goto fail;
		}*/

		// volume
		if (std::abs(1.0 - volume) >= 0.01) {
			AVFilterContext *audio_volume_filter;
			snprintf(option_buffer, 256, "volume=%f", volume);
			ffmpeg_result = avfilter_graph_create_filter(&audio_volume_filter, volume_filter, NULL, option_buffer, NULL,
			                                             audio_filter_graph);
			if (ffmpeg_result < 0) {
				result.error_description = "volume creation failed";
				goto fail;
			}
			filter_sequence.push_back(audio_volume_filter);
		}
		// superequalizer
		bool use_equalizer = false;
		for (int i = 0; i < 18; i++) {
			if (std::abs(1.0 - equalizer_values[i]) >= 0.01) {
				use_equalizer = true;
			}
		}
		if (use_equalizer) {
			AVFilterContext *superequalizer_filter;
			char *head = option_buffer;
			for (int i = 0; i < 18; i++) {
				if (i) {
					if (head < option_buffer + 256 - 1) {
						*head++ = ':';
					}
				}
				size_t remaining = option_buffer + 256 - head;
				int n = snprintf(head, remaining, "%db=%.3f", i + 1, equalizer_values[i]);
				// If error or output truncated, stop writing
				if (n < 0 || (size_t)n >= remaining) {
					// Make sure buffer is null terminated
					if (option_buffer[255] != '\0') {
						option_buffer[255] = '\0';
					}
					break;
				}
				head += n;
			}
			ffmpeg_result = avfilter_graph_create_filter(&superequalizer_filter, asuperequalizer, NULL, option_buffer,
			                                             NULL, audio_filter_graph);
			if (ffmpeg_result < 0) {
				result.error_description = "asuperequalizer creation failed";
				goto fail;
			}
			filter_sequence.push_back(superequalizer_filter);
		}

		// aformat
		AVFilterContext *audio_format_filter;
		snprintf(option_buffer, 256, "sample_fmts=%s:sample_rates=%d:channel_layouts=0x%" PRIx64,
		         av_get_sample_fmt_name(audio_context->sample_fmt), 44100, (uint64_t)AV_CH_LAYOUT_STEREO);
		ffmpeg_result =
		    avfilter_graph_create_filter(&audio_format_filter, aformat, NULL, option_buffer, NULL, audio_filter_graph);
		if (ffmpeg_result < 0) {
			result.error_description = "aformat creation failed";
			goto fail;
		}
		filter_sequence.push_back(audio_format_filter);
		// abuffersink (sink)
		ffmpeg_result =
		    avfilter_graph_create_filter(&audio_filter_sink, abuffersink, NULL, NULL, NULL, audio_filter_graph);
		if (ffmpeg_result < 0) {
			result.error_description = "abuffersink creation failed";
			goto fail;
		}
		filter_sequence.push_back(audio_filter_sink);

		// linking & configuring
		for (size_t i = 0; i + 1 < filter_sequence.size() && ffmpeg_result >= 0; i++) {
			ffmpeg_result = avfilter_link(filter_sequence[i], 0, filter_sequence[i + 1], 0);
		}
		if (ffmpeg_result < 0) {
			result.error_description = "filter graph linking failed";
			goto fail;
		}
		ffmpeg_result = avfilter_graph_config(audio_filter_graph, NULL);
		if (ffmpeg_result < 0) {
			result.error_description = "avfilter_graph_config() failed : " + std::to_string(ffmpeg_result);
			goto fail;
		}
	}
	return result;

fail:
	result.code = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS;
	result.string = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS_STR;
	return result;
}
Result_with_string NetworkDecoderFilterData::process_audio_frame(AVFrame *input, double *out_pts) {
	Result_with_string result;
	int ffmpeg_result = 0;

	if (first_frame) {
		first_frame = false, initial_timestamp = input->pts * input_time_base;
	}

	ffmpeg_result = av_buffersrc_write_frame(audio_filter_src, input);
	if (ffmpeg_result < 0) {
		result.error_description = "av_buffersrc_write_frame() failed : " + std::to_string(ffmpeg_result);
		goto fail;
	}

	av_frame_unref(output_frame);
	ffmpeg_result = av_buffersink_get_frame(audio_filter_sink, output_frame);
	if (ffmpeg_result == AVERROR(EAGAIN)) {
		result.code = DEF_ERR_NEED_MORE_INPUT;
		return result;
	}
	if (ffmpeg_result < 0) {
		result.error_description = "av_buffersink_get_frame() failed : " + std::to_string(ffmpeg_result);
		goto fail;
	}

	*out_pts = initial_timestamp + (double)received_frame_num / 44100 * tempo;
	received_frame_num += output_frame->nb_samples;

	return result;

fail:
	result.code = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS;
	result.string = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS_STR;
	return result;
}

/* ********************************************************* */
/*                       NetworkDecoder                      */
/* ********************************************************* */

void NetworkDecoder::deinit() {
	ready = false;
	// ensure the UI thread will see ready == false after locking the mutex durning deiniting
	critical_op_lock.lock();
	critical_op_lock.unlock();

	for (int type = 0; type < 2; type++) {
		for (auto i : packet_buffer[type]) {
			av_packet_free(&i);
		}
		packet_buffer[type].clear();
	}
	// for HW decoder
	for (auto i : video_mvd_tmp_frames.deinit()) {
		free(i);
	}
	linearFree_concurrent(mvd_frame);
	mvd_frame = NULL;
	buffered_pts_list.clear();
	// for SW decoder
	for (auto i : video_tmp_frames.deinit()) {
		av_frame_free(&i);
	}
	free(sw_video_output_tmp);
	sw_video_output_tmp = NULL;

	// its members should be freed by NetworkMultipleDecoder, not here
	// just to prevent use-after-free, we set the pointer to NULL
	io = NULL;

	// deinit decoder
	for (int type = 0; type < 2; type++) {
		avcodec_free_context(&decoder_context[type]);
	}
	swr_free(&swr_context);
}

Result_with_string NetworkDecoder::init_output_buffer(bool is_mvd) {
	Result_with_string result;
	int width, height;
	std::vector<u8 *> mvd_frames;
	std::vector<AVFrame *> sw_frames;

	// init output buffer
	width = decoder_context[VIDEO]->width;
	height = decoder_context[VIDEO]->height;
	if (width <= 0 || height <= 0) {
		result.code = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS;
		result.string = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS_STR;
		result.error_description = "invalid decoded video dimensions";
		return result;
	}
	if (width % 16) {
		width += 16 - width % 16;
	}
	if (height % 16) {
		height += 16 - height % 16;
	}
	const size_t MAX_RAW_BUFFER_SIZE = OLD_MAX_RAW_BUFFER_SIZE;
	if (is_mvd) {
		const size_t frame_size = (size_t)width * (size_t)height * 2;
		int buffer_size = (int)std::max<size_t>(2, MAX_RAW_BUFFER_SIZE / frame_size);
		if (buffer_size <= 10) {
			logger.warning("decoder", "mvd buffer size too low:" + std::to_string(buffer_size));
		}

		mvd_frames.resize(buffer_size, NULL);
		for (auto &i : mvd_frames) {
			i = (u8 *)malloc(width * height * 2);
			if (!i) {
				result.error_description = "malloc() failed while preallocating ";
				goto fail;
			}
		}

		mvd_frame = (u8 *)linearAlloc_concurrent(width * height * 2);
		if (!mvd_frame) {
			result.error_description = "malloc() failed while preallocating ";
			goto fail;
		}
		video_mvd_tmp_frames.init(mvd_frames);
	} else {
		const size_t frame_size = (size_t)width * (size_t)height * 3 / 2;
		int buffer_size = (int)std::max<size_t>(2, MAX_RAW_BUFFER_SIZE / frame_size);
		if (buffer_size <= 10) {
			logger.warning("decoder", "buffer size too low:" + std::to_string(buffer_size));
		}

		sw_frames.resize(buffer_size, NULL);
		for (auto &i : sw_frames) {
			i = av_frame_alloc();
			if (!i) {
				result.error_description = "av_frame_alloc() failed while preallocating ";
				goto fail;
			}
		}
		sw_video_output_tmp = (u8 *)malloc(width * height * 1.5);
		if (!sw_video_output_tmp) {
			result.error_description = "malloc() failed while preallocating ";
			goto fail;
		}
		video_tmp_frames.init(sw_frames);
	}

	return result;
fail:
	for (auto i : mvd_frames) {
		free(i);
	}
	for (auto i : sw_frames) {
		av_frame_free(&i);
	}
	linearFree_concurrent(mvd_frame);
	mvd_frame = NULL;
	free(sw_video_output_tmp);
	sw_video_output_tmp = NULL;
	result.code = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS;
	result.string = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS_STR;
	return result;
}
Result_with_string NetworkDecoder::init_decoder(int type) {
	Result_with_string result;
	int ffmpeg_result;

	// initialize decoder
	codec[type] = avcodec_find_decoder(get_stream(type)->codecpar->codec_id);
	if (!codec[type]) {
		result.error_description = "avcodec_find_decoder() failed";
		goto fail;
	}

	decoder_context[type] = avcodec_alloc_context3(codec[type]);
	if (!decoder_context[type]) {
		result.error_description = "avcodec_alloc_context3() failed";
		goto fail;
	}

	ffmpeg_result = avcodec_parameters_to_context(decoder_context[type], get_stream(type)->codecpar);
	if (ffmpeg_result != 0) {
		result.error_description = "avcodec_parameters_to_context() failed " + std::to_string(ffmpeg_result);
		goto fail;
	}

	if ((is_av_separate() ? (type == VIDEO) : (type == BOTH))) {
		decoder_context[type]->lowres = 0;
		decoder_context[type]->flags = AV_CODEC_FLAG_OUTPUT_CORRUPT;

		decoder_context[type]->skip_frame = AVDISCARD_NONREF;
		decoder_context[type]->skip_loop_filter = AVDISCARD_ALL;

		if (codec[type]->capabilities & AV_CODEC_CAP_FRAME_THREADS) {
			decoder_context[type]->thread_type = FF_THREAD_FRAME;
		} else if (codec[type]->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
			decoder_context[type]->thread_type = FF_THREAD_SLICE;
		} else {
			decoder_context[type]->thread_type = 0;
		}

		if (decoder_context[type]->thread_type == FF_THREAD_FRAME) {
			Util_fake_pthread_set_enabled_core(frame_cores_enabled);
			decoder_context[type]->thread_count =
			    std::accumulate(std::begin(frame_cores_enabled), std::end(frame_cores_enabled), 0);
		} else if (decoder_context[type]->thread_type == FF_THREAD_SLICE) {
			Util_fake_pthread_set_enabled_core(slice_cores_enabled);
			decoder_context[type]->thread_count =
			    std::accumulate(std::begin(slice_cores_enabled), std::end(slice_cores_enabled), 0);
		} else {
			decoder_context[type]->thread_count = 1;
		}
	}
	ffmpeg_result = avcodec_open2(decoder_context[type], codec[type], NULL);
	if (ffmpeg_result != 0) {
		result.error_description = "avcodec_open2() failed " + std::to_string(ffmpeg_result);
		goto fail;
	}

	if (type == AUDIO) {
		swr_context = swr_alloc();
		if (!swr_context) {
			result.error_description = "swr_alloc() failed ";
			goto fail;
		}
		// The filter graph already outputs 44.1 kHz stereo. Match that format
		// here and in NDSP instead of reusing the source codec's rate/channels.
		if (!swr_alloc_set_opts(swr_context, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 44100,
		                        AV_CH_LAYOUT_STEREO, decoder_context[AUDIO]->sample_fmt, 44100, 0, NULL)) {
			result.error_description = "swr_alloc_set_opts() failed ";
			goto fail;
		}

		ffmpeg_result = swr_init(swr_context);
		if (ffmpeg_result != 0) {
			result.error_description = "swr_init() failed " + std::to_string(ffmpeg_result);
			goto fail;
		}
	}
	return result;

fail:
	result.code = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS;
	result.string = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS_STR;
	return result;
}

Result_with_string NetworkDecoder::init(bool request_hw_decoder) {
	Result_with_string result;

	hw_decoder_enabled = request_hw_decoder;
	interrupt = false;

	// init decoder
	if (!is_audio_only()) {
		RETURN_WITH_PREFIX_ON_ERROR(init_decoder(VIDEO), "[v] ");
	}
	RETURN_WITH_PREFIX_ON_ERROR(init_decoder(AUDIO), "[a] ");

	// init buffers based on decoder info
	if (!is_audio_only()) {
		result = init_output_buffer(request_hw_decoder);
		if (result.code != 0) {
			result.error_description = "[out buf] " + result.error_description;
			return result;
		}
	}

	mvd_first = true;
	ready = true;
	return result;
}
void NetworkDecoder::clear_buffer() {
	reset_read_state();
	for (int type = 0; type < 2; type++) {
		for (auto i : packet_buffer[type]) {
			av_packet_free(&i);
		}
		packet_buffer[type].clear();
	}
	video_mvd_tmp_frames.clear();
	video_tmp_frames.clear();
	buffered_pts_list.clear();
}

NetworkDecoder::VideoFormatInfo NetworkDecoder::get_video_info() {
	VideoFormatInfo res;
	if (is_audio_only()) {
		res.width = res.height = res.framerate = res.duration = 0;
		res.format_name = "N/A";
	} else {
		res.width = decoder_context[VIDEO]->width;
		res.height = decoder_context[VIDEO]->height;
		res.framerate = av_q2d(get_stream(VIDEO)->avg_frame_rate);
		res.format_name = codec[VIDEO]->long_name;
		res.duration = (double)io->format_context[is_av_separate() ? VIDEO : BOTH]->duration / AV_TIME_BASE;
	}
	return res;
}
NetworkDecoder::AudioFormatInfo NetworkDecoder::get_audio_info() {
	AudioFormatInfo res;
	res.bitrate = decoder_context[AUDIO]->bit_rate;
	res.sample_rate = 44100;
	res.ch = 2;
	res.format_name = codec[AUDIO]->long_name;
	res.duration = (double)io->format_context[is_av_separate() ? AUDIO : BOTH]->duration / AV_TIME_BASE;
	return res;
}
std::vector<std::pair<double, std::vector<double>>> NetworkDecoder::get_buffering_progress_bars(int bar_len) {
	std::vector<std::pair<double, std::vector<double>>> res;
	critical_op_lock.lock();
	if (ready) {
		if (is_av_separate()) {
			if (io->network_stream[VIDEO] && !io->network_stream[VIDEO]->quit_request) {
				res.push_back({(double)io->network_stream[VIDEO]->get_read_head() / io->network_stream[VIDEO]->len,
				               io->network_stream[VIDEO]->get_buffering_progress_bar(bar_len)});
			} else {
				res.push_back({0, {}});
			}
			if (io->network_stream[AUDIO] && !io->network_stream[AUDIO]->quit_request) {
				res.push_back({(double)io->network_stream[AUDIO]->get_read_head() / io->network_stream[AUDIO]->len,
				               io->network_stream[AUDIO]->get_buffering_progress_bar(bar_len)});
			} else {
				res.push_back({0, {}});
			}
		} else {
			if (io->network_stream[BOTH] && !io->network_stream[BOTH]->quit_request) {
				res.push_back({(double)io->network_stream[BOTH]->get_read_head() / io->network_stream[BOTH]->len,
				               io->network_stream[BOTH]->get_buffering_progress_bar(bar_len)});
			} else {
				res.push_back({0, {}});
			}
		}
	}
	critical_op_lock.unlock();
	return res;
}

Result_with_string NetworkDecoder::read_packet(int type) {
	Result_with_string result;
	int ffmpeg_result;

	AVPacket *tmp_packet = av_packet_alloc();
	if (!tmp_packet) {
		result.code = DEF_ERR_OUT_OF_MEMORY;
		result.string = DEF_ERR_OUT_OF_MEMORY_STR;
		result.error_description = "av_packet_alloc() failed";
		read_error = result.error_description;
		return result;
	}

	ffmpeg_result = av_read_frame(io->format_context[type], tmp_packet);
	if (ffmpeg_result != 0) {
		if (ffmpeg_result == AVERROR_EOF) {
			demux_eof[type] = true;
			if (!is_av_separate()) {
				demux_eof[AUDIO] = true;
			}
		} else if (!(ffmpeg_result == AVERROR_EXIT && interrupt)) {
			read_error = "Lecture du flux impossible (" + std::to_string(ffmpeg_result) + ")";
		}
		result.error_description = "av_read_frame() failed " + std::to_string(ffmpeg_result);
		goto ffmpeg_fail;
	}
	my_assert(tmp_packet->buf);

	// Keep the demuxer alive for the whole stream. Periodic reinitialization by
	// packet count can reset timestamps mid-playback, causing A/V desync and
	// apparent fast-forward on long videos. Explicit recovery/seek paths can
	// still request reinitialization when necessary.
	if (is_av_separate() && avformat_reinit_request[type] && tmp_packet->flags & AV_PKT_FLAG_KEY) {
		result = io->reinit_stream(type, tmp_packet->dts);
		avformat_reinit_request[type] = false;
	}
	if (!is_av_separate() && avformat_reinit_request[VIDEO] && tmp_packet->stream_index == io->stream_index[VIDEO] &&
	    tmp_packet->flags & AV_PKT_FLAG_KEY) {
		result = io->reinit_stream(VIDEO, tmp_packet->dts);
		avformat_reinit_request[VIDEO] = avformat_reinit_request[AUDIO] = false;
	}
	if (result.code != 0) {
		goto reinit_fail;
	}
	{
		int packet_type =
		    is_av_separate() ? type : (tmp_packet->stream_index == io->stream_index[VIDEO] ? VIDEO : AUDIO);
		packet_buffer[packet_type].push_back(tmp_packet);
		return result;
	}

ffmpeg_fail:
	av_packet_free(&tmp_packet);
	result.code = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS;
	result.string = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS_STR;
	return result;
reinit_fail:
	av_packet_free(&tmp_packet);
	read_error = result.error_description;
	logger.error("debug", "avformat reinit fail : " + result.error_description);
	return result;
}
NetworkDecoder::PacketType NetworkDecoder::next_decode_type() {
	if (interrupt) {
		return PacketType::INTERRUPTED;
	}
	if (is_av_separate()) {
		for (int type = 0; type < 2; type++) {
			if (packet_buffer[type].empty() && !demux_eof[type] && read_error.empty()) {
				read_packet(type);
			}
		}
	} else {
		while (!demux_eof[BOTH] && read_error.empty() &&
		       ((!is_audio_only() && packet_buffer[VIDEO].empty()) || packet_buffer[AUDIO].empty())) {
			Result_with_string result = read_packet(BOTH);
			if (result.code != 0) {
				break;
			}
		}
	}

	if (interrupt) {
		return PacketType::INTERRUPTED;
	}
	if (!read_error.empty()) {
		return PacketType::ERROR;
	}
	bool video_empty = packet_buffer[VIDEO].empty();
	bool audio_empty = packet_buffer[AUDIO].empty();
	bool video_decoded_empty = video_tmp_frames.empty() && video_mvd_tmp_frames.empty();

	if (video_empty && audio_empty) {
		// A network read head can reach the end while FFmpeg still holds packets
		// in its AVIO buffer. Only the demuxer can establish the actual EOF.
		if (!is_audio_only() && demux_eof[VIDEO] && !codec_drained[VIDEO]) {
			if (hw_decoder_enabled) {
				codec_drained[VIDEO] = true;
			} else {
				return PacketType::VIDEO;
			}
		}
		if (demux_eof[AUDIO] && !codec_drained[AUDIO]) {
			return PacketType::AUDIO;
		}
		if (demux_eof[AUDIO] && (is_audio_only() || demux_eof[VIDEO]) && video_decoded_empty) {
			return PacketType::EoF;
		}
		return PacketType::None;
	}

	if (!packet_buffer[AUDIO].size()) {
		return PacketType::VIDEO;
	}
	if (!packet_buffer[VIDEO].size()) {
		return PacketType::AUDIO;
	}
	double video_dts = packet_buffer[VIDEO][0]->dts * av_q2d(get_stream(VIDEO)->time_base);
	double audio_dts = packet_buffer[AUDIO][0]->dts * av_q2d(get_stream(AUDIO)->time_base);
	return video_dts <= audio_dts ? PacketType::VIDEO : PacketType::AUDIO;
}
Result_with_string NetworkDecoder::mvd_decode(int *width, int *height) {
	Result_with_string result;

	*width = decoder_context[VIDEO]->width;
	*height = decoder_context[VIDEO]->height;
	if (*width % 16 != 0) {
		*width += 16 - *width % 16;
	}
	if (*height % 16 != 0) {
		*height += 16 - *height % 16;
	}

	MVDSTD_Config config;
	mvdstdGenerateDefaultConfig(&config, *width, *height, *width, *height, NULL, NULL, NULL);

	int offset = 0;
	int source_offset = 0;

	AVPacket *packet_read = packet_buffer[VIDEO][0];
	u8 *mvd_packet = (u8 *)linearAlloc_concurrent(packet_read->size);
	if (mvd_first) {
		// set extra data

		offset = 0;
		memset(mvd_packet, 0x0, 0x2);
		offset += 2;
		memset(mvd_packet + offset, 0x1, 0x1);
		offset += 1;
		memcpy(mvd_packet + offset, decoder_context[VIDEO]->extradata + 8, *(decoder_context[VIDEO]->extradata + 7));
		offset += *(decoder_context[VIDEO]->extradata + 7);

		result.code = mvdstdProcessVideoFrame(mvd_packet, offset, 0, NULL);
		if (!MVD_CHECKNALUPROC_SUCCESS(result.code)) {
			logger.caution("mvd", "0 : mvdstdProcessVideoFrame() : " + std::to_string(result.code));
		}

		offset = 0;
		memset(mvd_packet, 0x0, 0x2);
		offset += 2;
		memset(mvd_packet + offset, 0x1, 0x1);
		offset += 1;
		memcpy(mvd_packet + offset, decoder_context[VIDEO]->extradata + 11 + *(decoder_context[VIDEO]->extradata + 7),
		       *(decoder_context[VIDEO]->extradata + 10 + *(decoder_context[VIDEO]->extradata + 7)));
		offset += *(decoder_context[VIDEO]->extradata + 10 + *(decoder_context[VIDEO]->extradata + 7));

		result.code = mvdstdProcessVideoFrame(mvd_packet, offset, 0, NULL);
		if (!MVD_CHECKNALUPROC_SUCCESS(result.code)) {
			logger.caution("mvd", "1 : mvdstdProcessVideoFrame() : " + std::to_string(result.code));
		}
	}

	offset = 0;
	source_offset = 0;

	while (source_offset + 4 < packet_read->size) {
		// get nal size
		int size = *((int *)(packet_read->data + source_offset));
		size = __builtin_bswap32(size);
		source_offset += 4;

		// set nal prefix 0x0 0x0 0x1
		memset(mvd_packet + offset, 0x0, 0x2);
		offset += 2;
		memset(mvd_packet + offset, 0x1, 0x1);
		offset += 1;

		// copy raw nal data
		memcpy(mvd_packet + offset, (packet_read->data + source_offset), size);
		offset += size;
		source_offset += size;
	}

	config.physaddr_outdata0 = osConvertVirtToPhys(mvd_frame);

	result.code = mvdstdProcessVideoFrame(mvd_packet, offset, 0, NULL);

	if (mvd_first) {
		// Do I need to send same nal data at first frame?
		result.code = mvdstdProcessVideoFrame(mvd_packet, offset, 0, NULL);
		if (!MVD_CHECKNALUPROC_SUCCESS(result.code)) {
			logger.caution("mvd", "1 : mvdstdProcessVideoFrame() : " + std::to_string(result.code));
		}
	}

	if (MVD_CHECKNALUPROC_SUCCESS(result.code)) {
		double cur_pos;
		double time_base = av_q2d(get_stream(VIDEO)->time_base);
		if (packet_read->pts != AV_NOPTS_VALUE) {
			cur_pos = packet_read->pts * time_base;
		} else {
			cur_pos = packet_read->dts * time_base;
		}

		buffered_pts_list_lock.lock();
		buffered_pts_list.insert(cur_pos + timestamp_offset);
		buffered_pts_list_lock.unlock();
	}
	if (result.code == MVD_STATUS_FRAMEREADY) {
		result.code = 0;
		mvdstdRenderVideoFrame(&config, true);

		if (!mvd_first) { // when changing video, it somehow outputs a frame of previous video, so ignore the first one
			memcpy_asm(video_mvd_tmp_frames.get_next_pushed(), mvd_frame, (*width * *height * 2) / 32 * 32);
			video_mvd_tmp_frames.push();
		}
	} else {
		logger.caution("", "mvdstdProcessVideoFrame()...", result.code);
	}

	mvd_first = false;
	linearFree_concurrent(mvd_packet);
	mvd_packet = NULL;
	av_packet_free(&packet_read);
	packet_buffer[VIDEO].pop_front();
	// refill the packet buffer
	while (!packet_buffer[VIDEO].size() && read_packet(is_av_separate() ? VIDEO : BOTH).code == 0)
		;

	return result;
}
Result_with_string NetworkDecoder::decode_video(int *width, int *height, bool *key_frame) {
	Result_with_string result;
	int ffmpeg_result = 0;

	if (packet_buffer[VIDEO].empty() && !demux_eof[VIDEO]) {
		result.code = DEF_ERR_NEED_MORE_INPUT;
		return result;
	}

	AVPacket *packet_read = packet_buffer[VIDEO].empty() ? NULL : packet_buffer[VIDEO][0];
	bool packet_consumed = false;
	*key_frame = packet_read && (packet_read->flags & AV_PKT_FLAG_KEY);

	if (hw_decoder_enabled) {
		if (video_mvd_tmp_frames.full()) {
			result.code = DEF_ERR_NEED_MORE_OUTPUT;
			return result;
		}
		auto tmp = mvd_decode(width, height);
		return tmp;
	}

	if (video_tmp_frames.full()) {
		result.code = DEF_ERR_NEED_MORE_OUTPUT;
		return result;
	}

	*width = 0;
	*height = 0;

	AVFrame *cur_frame = video_tmp_frames.get_next_pushed();

	if (packet_read || !codec_flush_sent[VIDEO]) {
		ffmpeg_result = avcodec_send_packet(decoder_context[VIDEO], packet_read);
		if (ffmpeg_result == 0) {
			packet_consumed = packet_read != NULL;
			if (!packet_read) {
				codec_flush_sent[VIDEO] = true;
			}
		}
	}
	if (ffmpeg_result == 0 || ffmpeg_result == AVERROR(EAGAIN)) {
		ffmpeg_result = avcodec_receive_frame(decoder_context[VIDEO], cur_frame);
		if (ffmpeg_result == 0) {
			*width = cur_frame->width;
			*height = cur_frame->height;

			double time_base = av_q2d(get_stream(VIDEO)->time_base);
			double cur_pos;
			if (cur_frame->pts != AV_NOPTS_VALUE) {
				cur_pos = cur_frame->pts * time_base;
			} else {
				cur_pos = cur_frame->pkt_dts * time_base;
			}
			cur_pos += timestamp_offset;

			buffered_pts_list_lock.lock();
			buffered_pts_list.insert(cur_pos);
			buffered_pts_list_lock.unlock();

			video_tmp_frames.push();
		} else if (ffmpeg_result == AVERROR(EAGAIN)) {
			result.code = DEF_ERR_NEED_MORE_INPUT;
		} else if (ffmpeg_result == AVERROR_EOF) {
			codec_drained[VIDEO] = true;
			result.code = DEF_ERR_NEED_MORE_INPUT;
		} else {
			result.code = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS;
			result.string = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS_STR;
			result.error_description = "avcodec_receive_frame() failed " + std::to_string(ffmpeg_result);
			read_error = result.error_description;
		}
	} else if (ffmpeg_result == AVERROR_EOF) {
		codec_drained[VIDEO] = true;
		if (packet_read) {
			packet_consumed = true;
			read_error = "Video decoder reached EOF before the end of the stream";
		}
		result.code = DEF_ERR_NEED_MORE_INPUT;
	} else {
		packet_consumed = packet_read != NULL;
		result.code = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS;
		result.string = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS_STR;
		result.error_description = "avcodec_send_packet() failed " + std::to_string(ffmpeg_result);
		read_error = result.error_description;
	}

	if (packet_consumed) {
		av_packet_free(&packet_read);
		packet_buffer[VIDEO].pop_front();
	}
	// refill the packet buffer
	while (!demux_eof[VIDEO] && !interrupt && read_error.empty() && packet_buffer[VIDEO].empty() &&
	       read_packet(is_av_separate() ? VIDEO : BOTH).code == 0)
		;

	return result;
}
Result_with_string NetworkDecoder::decode_audio(int *size, u8 **data, double *cur_pos) {
	int ffmpeg_result = 0;
	Result_with_string result;
	*size = 0;
	*data = NULL;

	if (packet_buffer[AUDIO].empty() && !demux_eof[AUDIO]) {
		result.code = DEF_ERR_NEED_MORE_INPUT;
		return result;
	}

	AVPacket *packet_read = packet_buffer[AUDIO].empty() ? NULL : packet_buffer[AUDIO][0];
	bool packet_consumed = false;

	AVFrame *cur_frame = av_frame_alloc();
	if (!cur_frame) {
		result.error_description = "av_frame_alloc() failed";
		read_error = result.error_description;
		goto ffmpeg_fail;
	}

	if (packet_read || !codec_flush_sent[AUDIO]) {
		ffmpeg_result = avcodec_send_packet(decoder_context[AUDIO], packet_read);
		if (ffmpeg_result == 0) {
			packet_consumed = packet_read != NULL;
			if (!packet_read) {
				codec_flush_sent[AUDIO] = true;
			}
		}
	}
	if (ffmpeg_result == 0 || ffmpeg_result == AVERROR(EAGAIN)) {
		ffmpeg_result = avcodec_receive_frame(decoder_context[AUDIO], cur_frame);
		if (ffmpeg_result == 0) {
			result = filter.process_audio_frame(cur_frame, cur_pos);
			if (result.code != 0) {
				goto cleanup;
			}
			auto out_frame = filter.output_frame;
			*data = (u8 *)malloc((size_t)out_frame->nb_samples * 2 * 2);
			if (!*data) {
				result.code = DEF_ERR_OUT_OF_MEMORY;
				result.string = DEF_ERR_OUT_OF_MEMORY_STR;
				result.error_description = "malloc() failed for decoded audio";
				read_error = result.error_description;
				goto cleanup;
			}
			ffmpeg_result = swr_convert(swr_context, data, out_frame->nb_samples, (const u8 **)out_frame->data,
			                            out_frame->nb_samples);
			if (ffmpeg_result < 0) {
				free(*data);
				*data = NULL;
				result.error_description = "swr_convert() failed " + std::to_string(ffmpeg_result);
				goto ffmpeg_fail;
			}
			*size = 2 * ffmpeg_result;
			*cur_pos += timestamp_offset;
		} else if (ffmpeg_result == AVERROR(EAGAIN)) {
			result.code = DEF_ERR_NEED_MORE_INPUT;
		} else if (ffmpeg_result == AVERROR_EOF) {
			codec_drained[AUDIO] = true;
			result.code = DEF_ERR_NEED_MORE_INPUT;
		} else {
			result.error_description = "avcodec_receive_frame() failed " + std::to_string(ffmpeg_result);
			goto ffmpeg_fail;
		}
	} else if (ffmpeg_result == AVERROR_EOF) {
		codec_drained[AUDIO] = true;
		if (packet_read) {
			packet_consumed = true;
			read_error = "Audio decoder reached EOF before the end of the stream";
		}
		result.code = DEF_ERR_NEED_MORE_INPUT;
	} else {
		packet_consumed = packet_read != NULL;
		result.error_description =
		    "avcodec_send_packet() failed " + std::to_string(ffmpeg_result) + " " + std::to_string(AVERROR(EAGAIN));
		goto ffmpeg_fail;
	}
	goto cleanup;

ffmpeg_fail:
	result.code = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS;
	result.string = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS_STR;
	read_error = result.error_description;

cleanup:
	if (packet_consumed) {
		av_packet_free(&packet_read);
		packet_buffer[AUDIO].pop_front();
	}
	while (!demux_eof[AUDIO] && !interrupt && read_error.empty() && packet_buffer[AUDIO].empty() &&
	       read_packet(is_av_separate() ? AUDIO : BOTH).code == 0)
		;
	av_frame_free(&cur_frame);
	return result;
}

Result_with_string NetworkDecoder::get_decoded_video_frame(int width, int height, u8 **data, double *cur_pos) {
	Result_with_string result;

	if (hw_decoder_enabled) {
		if (video_mvd_tmp_frames.empty()) {
			result.code = DEF_ERR_NEED_MORE_INPUT;
			return result;
		}
		*data = video_mvd_tmp_frames.get_next_poped(); // it's valid until the next pop() is called
		video_mvd_tmp_frames.pop();
	} else {
		if (video_tmp_frames.empty()) {
			result.code = DEF_ERR_NEED_MORE_INPUT;
			return result;
		}
		AVFrame *cur_frame = video_tmp_frames.get_next_poped();

		int cpy_size[2] = {
		    0,
		    0,
		};

		cpy_size[0] = (width * height);
		cpy_size[1] = cpy_size[0] / 4;
		cpy_size[0] -= cpy_size[0] % 32;
		cpy_size[1] -= cpy_size[1] % 32;

		memcpy_asm(sw_video_output_tmp, cur_frame->data[0], cpy_size[0]);
		memcpy_asm(sw_video_output_tmp + (width * height), cur_frame->data[1], cpy_size[1]);
		memcpy_asm(sw_video_output_tmp + (width * height) + (width * height / 4), cur_frame->data[2], cpy_size[1]);

		*data = sw_video_output_tmp;

		av_frame_unref(cur_frame);
		video_tmp_frames.pop();
	}

	buffered_pts_list_lock.lock();
	my_assert(buffered_pts_list.size());
	*cur_pos = *buffered_pts_list.begin();
	buffered_pts_list.erase(buffered_pts_list.begin());
	buffered_pts_list_lock.unlock();

	return result;
}

Result_with_string NetworkDecoder::seek(s64 microseconds) {
	Result_with_string result;

	clear_buffer();

	s64 min_ts = std::max<s64>(0, microseconds - 1000000);
	s64 max_ts = microseconds + 500000;

	if (is_av_separate()) {
		int ffmpeg_result = avformat_seek_file(io->format_context[VIDEO], -1, min_ts, microseconds, max_ts,
		                                       AVSEEK_FLAG_FRAME); // AVSEEK_FLAG_FRAME <- ?
		for (int i = 2; i <= 3 && ffmpeg_result < 0; i++) {        // retry with wider range backward
			min_ts = std::max<s64>(0, microseconds - i * 1000000);
			ffmpeg_result =
			    avformat_seek_file(io->format_context[VIDEO], -1, min_ts, microseconds, max_ts, AVSEEK_FLAG_FRAME);
			if (ffmpeg_result >= 0) {
				logger.info("seek", "succeeded at " + std::to_string(ffmpeg_result));
			}
		}
		if (ffmpeg_result < 0) {
			result.code = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS;
			result.string = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS_STR;
			result.error_description = "avformat_seek_file() for video failed " + std::to_string(ffmpeg_result);
			return result;
		}
		avcodec_flush_buffers(decoder_context[VIDEO]);
		// refill the next packets
		result = read_packet(VIDEO);
		if (result.code != 0) {
			return result;
		}

		// decode and discard video frames until we reach the target timestamp
		int width, height;
		bool key_frame;
		while (true) {
			result = decode_video(&width, &height, &key_frame);
			if (result.code == DEF_ERR_NEED_MORE_INPUT) {
				result = read_packet(VIDEO);
				if (result.code != 0) {
					break;
				}
				continue;
			} else if (result.code == DEF_ERR_NEED_MORE_OUTPUT) {
				// buffer is full
				u8 *buf;
				double pos;
				get_decoded_video_frame(width, height, &buf, &pos);
				continue;
			} else if (result.code != 0) {
				break;
			}

			// check timestamp
			double cur_pos;
			buffered_pts_list_lock.lock();
			if (!buffered_pts_list.empty()) {
				cur_pos = *std::prev(buffered_pts_list.end());
			} else {
				cur_pos = -1;
			}
			buffered_pts_list_lock.unlock();

			if (cur_pos != -1 && cur_pos * 1000000 >= microseconds - 1000) {
				break;
			}

			u8 *buf;
			double pos;
			get_decoded_video_frame(width, height, &buf, &pos);
		}

		// once successfully sought on video, perform an exact seek on audio
		double time_base = av_q2d(get_stream(VIDEO)->time_base);

		buffered_pts_list_lock.lock();
		double video_current_pts = 0;
		if (!buffered_pts_list.empty()) {
			video_current_pts = *buffered_pts_list.begin(); // The next frame to be shown
		} else {
			// fallback if something went wrong and we have no frames (eof?)
			if (!packet_buffer[VIDEO].empty()) {
				if (packet_buffer[VIDEO][0]->pts != AV_NOPTS_VALUE) {
					video_current_pts = packet_buffer[VIDEO][0]->pts * time_base;
				} else {
					video_current_pts = packet_buffer[VIDEO][0]->dts * time_base;
				}
			} else {
				video_current_pts = microseconds / 1000000.0;
			}
		}
		buffered_pts_list_lock.unlock();

		microseconds = video_current_pts * 1000000;

		s64 audio_seek_min = std::max<s64>(0, microseconds - 2000000);
		s64 audio_seek_max = microseconds + 2000000;
		ffmpeg_result = avformat_seek_file(io->format_context[AUDIO], -1, audio_seek_min, microseconds, audio_seek_max,
		                                   AVSEEK_FLAG_FRAME);
		if (ffmpeg_result < 0) {
			result.code = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS;
			result.string = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS_STR;
			result.error_description = "avformat_seek_file() for audio failed " + std::to_string(ffmpeg_result);
			return result;
		}
		avcodec_flush_buffers(decoder_context[AUDIO]);
		result = read_packet(AUDIO);
		if (result.code != 0) {
			return result;
		}

		// Discard audio packets until close to target
		double audio_time_base = av_q2d(get_stream(AUDIO)->time_base);
		while (!packet_buffer[AUDIO].empty()) {
			AVPacket *pkt = packet_buffer[AUDIO].front();
			double pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
			pts *= audio_time_base;

			if (pts * 1000000 < microseconds - 50000) { // Keep if within 50ms of target
				av_packet_free(&pkt);
				packet_buffer[AUDIO].pop_front();
				if (packet_buffer[AUDIO].empty()) {
					read_packet(AUDIO);
				}
			} else {
				break;
			}
		}
		return result;
	} else {
		int ffmpeg_result = avformat_seek_file(io->format_context[BOTH], -1, min_ts, microseconds, max_ts,
		                                       AVSEEK_FLAG_FRAME); // AVSEEK_FLAG_FRAME <- ?
		for (int i = 2; i <= 3 && ffmpeg_result < 0; i++) {        // retry with wider range backward
			min_ts = std::max<s64>(0, microseconds - i * 1000000);
			ffmpeg_result =
			    avformat_seek_file(io->format_context[BOTH], -1, min_ts, microseconds, max_ts, AVSEEK_FLAG_FRAME);
			if (ffmpeg_result >= 0) {
				logger.info("seek", "succeeded at " + std::to_string(ffmpeg_result));
			}
		}
		if (ffmpeg_result < 0) {
			result.code = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS;
			result.string = DEF_ERR_FFMPEG_RETURNED_NOT_SUCCESS_STR;
			result.error_description = "avformat_seek_file() failed " + std::to_string(ffmpeg_result);
			return result;
		}
		if (!is_audio_only()) {
			avcodec_flush_buffers(decoder_context[VIDEO]);
		}
		avcodec_flush_buffers(decoder_context[AUDIO]);
		while ((!is_audio_only() && !packet_buffer[VIDEO].size()) || !packet_buffer[AUDIO].size()) {
			result = read_packet(BOTH);
			if (result.code != 0) {
				return result;
			}
		}
	}

	// now, try to decode frames until the current position reaches the required timestamp
	if (!is_audio_only()) {
		int width, height;
		bool key_frame;
		while (true) {
			result = decode_video(&width, &height, &key_frame);
			if (result.code == DEF_ERR_NEED_MORE_INPUT) {
				result = read_packet(is_av_separate() ? VIDEO : BOTH);
				if (result.code != 0) {
					break;
				}
				continue;
			} else if (result.code == DEF_ERR_NEED_MORE_OUTPUT) {
				// buffer is full, but we haven't reached the target timestamp yet
				// so pop the frame and discard it
				u8 *buf;
				double pos;
				get_decoded_video_frame(width, height, &buf, &pos);
				continue;
			} else if (result.code != 0) {
				break;
			}

			// check if the decoded frame is close enough to the target timestamp
			double time_base = av_q2d(get_stream(VIDEO)->time_base);
			double cur_pos;
			buffered_pts_list_lock.lock();
			// the last inserted one is the current frame
			if (!buffered_pts_list.empty()) {
				cur_pos = *std::prev(buffered_pts_list.end());
			} else {
				cur_pos = -1;
			}
			buffered_pts_list_lock.unlock();

			if (cur_pos != -1 && cur_pos * 1000000 >= microseconds - 1000) {
				break;
			}

			// discard the frame
			u8 *buf;
			double pos;
			get_decoded_video_frame(width, height, &buf, &pos);
		}
	}

	// Retrying with Packet-based discard for Audio to avoid losing decoded data
	if (!is_av_separate()) {
		double time_base = av_q2d(get_stream(AUDIO)->time_base);
		while (!packet_buffer[AUDIO].empty()) {
			AVPacket *pkt = packet_buffer[AUDIO].front();
			double pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
			pts *= time_base;

			// If packet ends before target?
			// We don't know duration exactly without decoding or checking next.
			// But if pts is significantly before target, drop it.
			// Be conservative.
			if (pts * 1000000 < microseconds - 100000) { // 100ms grace period
				av_packet_free(&pkt);
				packet_buffer[AUDIO].pop_front();
			} else {
				break;
			}
		}
	}

	return result;
}
