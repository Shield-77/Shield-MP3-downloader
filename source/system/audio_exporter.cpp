#include "headers.hpp"
#include "audio_exporter.hpp"

#ifdef MP3_DOWNLOADER_APP
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <layer3.h>
}

#include <atomic>
#include <cctype>
#include <cstdio>
#include <sys/stat.h>
#include <curl/curl.h>

namespace {
std::atomic<int> state((int)AudioExportState::IDLE);
std::atomic<int> progress(0);
std::atomic<int> selected_bitrate(128);
std::atomic<bool> selected_artwork(true);
std::atomic<bool> cancel_requested(false);
std::atomic<bool> stop_requested(false);
std::atomic<bool> request_pending(false);
std::atomic<bool> player_stopped(false);
std::atomic<bool> wait_for_player(false);
std::atomic<bool> duration_limit_rejected(false);
std::atomic<bool> resumable_partial(false);
std::atomic<bool> conversion_failed(false);
std::atomic<bool> artwork_embedded(false);
std::atomic<int> conversion_error_code(0);
// These counters describe the file download, not playback or MP3 encoding.
std::atomic<unsigned int> download_received_bytes(0);
std::atomic<unsigned int> download_written_bytes(0);
std::atomic<unsigned int> download_attempt_start_ms(0);
std::atomic<unsigned int> download_first_data_ms(0);
std::atomic<unsigned int> download_last_data_ms(0);
std::atomic<int> download_http_status(0);
std::atomic<int> download_curl_code(0);
std::atomic<int> download_phase(0); // 0: preparing, 1: connecting, 2: receiving, 3: retrying
std::atomic<bool> download_total_known(false);
std::atomic<bool> download_write_failed(false);
std::atomic<bool> download_size_exceeded(false);
std::atomic<unsigned int> transfer_elapsed_ms(0);
std::atomic<unsigned int> encode_elapsed_ms(0);
std::atomic<unsigned int> encode_started_ms(0);
std::string last_error_detail;
static const int MP3_OUTPUT_SAMPLE_RATE = 44100;
static const long long MAX_SOURCE_BYTES = 192LL * 1024 * 1024;
std::string requested_url;
std::string requested_title;
std::string requested_id;
long long requested_duration_ms = 0;
long long requested_size = 0;
long long download_offset = 0;
Thread worker_thread = 0;
LightEvent worker_wake_event;
LightEvent player_stop_event;

struct DownloadContext {
	FILE *file;
};

size_t curl_write_file(char *data, size_t size, size_t count, void *user_data) {
	DownloadContext *context = (DownloadContext *)user_data;
	if (size && count > SIZE_MAX / size) return 0;
	size_t bytes = size * count;
	if (cancel_requested.load()) return 0;
	const long long saved = download_offset + download_written_bytes.load();
	if (saved > MAX_SOURCE_BYTES || bytes > (size_t)(MAX_SOURCE_BYTES - saved)) {
		download_size_exceeded.store(true);
		return 0;
	}
	download_received_bytes.fetch_add((unsigned int)bytes);
	size_t written = fwrite(data, 1, bytes, context->file);
	download_written_bytes.fetch_add((unsigned int)written);
	if (written != bytes) download_write_failed.store(true);
	if (written) {
		unsigned int now_ms = (unsigned int)osGetTime();
		unsigned int first_ms = 0;
		download_first_data_ms.compare_exchange_strong(first_ms, now_ms);
		download_last_data_ms.store(now_ms);
		download_phase.store(2);
	}
	return written;
}

int curl_update_progress(void *, curl_off_t total, curl_off_t, curl_off_t, curl_off_t) {
	if (cancel_requested.load()) return 1;
	long long total_size = requested_size > 0 ? requested_size : (total > 0 ? download_offset + total : 0);
	download_total_known.store(total_size > 0);
	if (total_size > 0) {
		// Count bytes successfully written to the SD, not bytes merely offered by curl.
		long long saved = download_offset + download_written_bytes.load();
		int value = (int)std::min<long long>(99, saved * 100 / total_size);
		progress.store(value);
	}
	return 0;
}

size_t curl_write_art(char *data, size_t size, size_t count, void *user_data) {
	std::vector<unsigned char> *image = (std::vector<unsigned char> *)user_data;
	if (cancel_requested.load() || (size && count > SIZE_MAX / size)) return 0;
	size_t bytes = size * count;
	if (bytes > 128 * 1024 - image->size()) return 0;
	image->insert(image->end(), (unsigned char *)data, (unsigned char *)data + bytes);
	return bytes;
}

int curl_art_progress(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	return cancel_requested.load() ? 1 : 0;
}

bool fetch_album_art(CURL *curl, const std::string &id, std::vector<unsigned char> &image) {
	if (!curl) return false;
	if (id.size() != 11) return false;
	for (unsigned char c : id)
		if (!std::isalnum(c) && c != '-' && c != '_') return false;
	std::string url = "https://i.ytimg.com/vi/" + id + "/mqdefault.jpg";
	curl_easy_reset(curl);
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 7L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 2L);
	curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Shield MP3 Downloader/3DS");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_art);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &image);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_art_progress);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	CURLcode result = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_reset(curl);
	bool valid = result == CURLE_OK && status == 200 && image.size() >= 4 &&
	             image[0] == 0xff && image[1] == 0xd8 &&
	             image[image.size() - 2] == 0xff && image.back() == 0xd9;
	if (!valid) image.clear();
	return valid;
}

void syncsafe32(unsigned char *out, uint32_t value) {
	out[0] = (value >> 21) & 0x7f;
	out[1] = (value >> 14) & 0x7f;
	out[2] = (value >> 7) & 0x7f;
	out[3] = value & 0x7f;
}

bool write_id3_frame_header(FILE *output, const char *name, uint32_t size) {
	unsigned char header[10] = {};
	memcpy(header, name, 4);
	syncsafe32(header + 4, size);
	return fwrite(header, 1, sizeof(header), output) == sizeof(header);
}

bool write_id3_tag(FILE *output, const std::string &title,
	               const std::vector<unsigned char> &image) {
	const size_t title_bytes = std::min<size_t>(title.size(), 1024);
	const uint32_t title_frame_size = (uint32_t)title_bytes + 1;
	const uint32_t art_frame_size = image.empty() ? 0 : (uint32_t)image.size() + 14;
	const uint32_t body_size = 10 + title_frame_size + (art_frame_size ? 10 + art_frame_size : 0);
	unsigned char header[10] = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0};
	syncsafe32(header + 6, body_size);
	const unsigned char utf8 = 3;
	if (fwrite(header, 1, sizeof(header), output) != sizeof(header) ||
	    !write_id3_frame_header(output, "TIT2", title_frame_size) ||
	    fwrite(&utf8, 1, 1, output) != 1 ||
	    fwrite(title.data(), 1, title_bytes, output) != title_bytes) return false;
	if (!art_frame_size) return true;
	const unsigned char art_prefix[] = {3, 'i', 'm', 'a', 'g', 'e', '/', 'j', 'p', 'e', 'g', 0, 3, 0};
	return write_id3_frame_header(output, "APIC", art_frame_size) &&
	       fwrite(art_prefix, 1, sizeof(art_prefix), output) == sizeof(art_prefix) &&
	       fwrite(image.data(), 1, image.size(), output) == image.size();
}

std::string audio_source_key(const std::string &url, long long expected_size) {
	std::string key;
	size_t itag = url.find("itag=");
	if (itag != std::string::npos) {
		itag += 5;
		size_t end = url.find_first_not_of("0123456789", itag);
		key = url.substr(itag, end == std::string::npos ? end : end - itag);
	}
	if (key.empty()) key = "unknown";
	if (expected_size > 0) key += "_" + std::to_string(expected_size);
	return key;
}

std::string make_safe_name(const std::string &title, const std::string &id) {
	std::string result;
	result.reserve(64);
	for (unsigned char c : title) {
		if (result.size() >= 48) break;
		if (std::isalnum(c) || c == ' ' || c == '-' || c == '_') result.push_back((char)c);
		else if (c >= 0x80) result.push_back('_');
	}
	while (!result.empty() && result.back() == ' ') result.pop_back();
	if (result.empty()) result = "YouTube audio";
	// The page supplies this ID. Never copy an unvalidated ID into an SD path.
	bool safe_id = id.size() == 11;
	for (unsigned char c : id)
		if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
		      (c >= 'a' && c <= 'z') || c == '-' || c == '_')) safe_id = false;
	if (safe_id) result += "_" + id;
	return result;
}

// Stable codes shown on the 3DS screen so conversion failures can be reported
// without needing to read the console log.
int get_conversion_error_code(const std::string &error) {
	if (error.find("Cannot open downloaded audio") != std::string::npos) return 1;
	if (error.find("Cannot read audio stream") != std::string::npos) return 2;
	if (error.find("No supported audio stream") != std::string::npos) return 3;
	if (error.find("audio decoder") != std::string::npos || error.find("AAC decoder") != std::string::npos ||
	    error.find("AAC packet") != std::string::npos) return 4;
	if (error.find("audio format") != std::string::npos || error.find("resampling") != std::string::npos) return 5;
	if (error.find("bitrate") != std::string::npos) return 6;
	if (error.find("MP3 encoder") != std::string::npos || error.find("MP3 encoding") != std::string::npos) return 7;
	if (error.find("create MP3") != std::string::npos) return 8;
	if (error.find("memory") != std::string::npos) return 9;
	if (error.find("write") != std::string::npos || error.find("SD card") != std::string::npos) return 10;
	if (error.find("close MP3 file") != std::string::npos) return 10;
	if (error.find("finalize MP3 file") != std::string::npos) return 10;
	if (error.find("incomplete or unreadable") != std::string::npos) return 11;
	return 99;
}

bool write_encoded_samples(shine_t encoder, FILE *output, std::vector<int16_t> &buffer, size_t &buffered_samples,
                           const int16_t *samples, size_t sample_count) {
	const size_t frame_samples = shine_samples_per_pass(encoder);
	while (sample_count) {
		size_t available = frame_samples - buffered_samples;
		size_t copy_count = std::min(available, sample_count);
		memcpy(buffer.data() + buffered_samples * 2, samples, copy_count * 2 * sizeof(int16_t));
		buffered_samples += copy_count;
		samples += copy_count * 2;
		sample_count -= copy_count;
		if (buffered_samples == frame_samples) {
			int written = 0;
			unsigned char *encoded = shine_encode_buffer_interleaved(encoder, buffer.data(), &written);
			if (written < 0 || (written && fwrite(encoded, 1, written, output) != (size_t)written)) return false;
			buffered_samples = 0;
		}
	}
	return true;
}

bool encode_file(const std::string &input_path, const std::string &output_path, long long duration_ms,
                 const std::string &title, const std::vector<unsigned char> &image, std::string &error) {
	AVFormatContext *format = NULL;
	AVCodecContext *decoder = NULL;
	const AVCodec *codec = NULL;
	AVPacket *packet = NULL;
	AVFrame *frame = NULL;
	SwrContext *resampler = NULL;
	shine_t encoder = NULL;
	FILE *output = NULL;
	uint8_t *converted = NULL;
	bool ok = false;
	int audio_stream_index = -1;
	bool audio_parameters_ready = false;

	// FFmpeg otherwise parses "sdmc:" as an unsupported URL protocol instead
	// of opening the libctru-mounted SD path as a local file.
	std::string input_url = "file:" + input_path;
	int result = avformat_open_input(&format, input_url.c_str(), NULL, NULL);
	if (result < 0) {
		char error_buffer[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(result, error_buffer, sizeof(error_buffer));
		error = std::string("Cannot open downloaded audio: ") + error_buffer;
		goto cleanup;
	}
	// AAC in an MP4 container normally supplies complete codec parameters in
	// its sample description. Avoid the extra stream probe when it does.
	for (unsigned i = 0; i < format->nb_streams; ++i) {
		const AVCodecParameters *params = format->streams[i]->codecpar;
		if (params && params->codec_type == AVMEDIA_TYPE_AUDIO && params->codec_id == AV_CODEC_ID_AAC &&
		    params->sample_rate > 0 && params->channels > 0 && params->extradata_size > 0) {
			audio_parameters_ready = true;
			break;
		}
	}
	if (!audio_parameters_ready) {
		result = avformat_find_stream_info(format, NULL);
		if (result < 0) { error = "Cannot read audio stream"; goto cleanup; }
	}
	{
		audio_stream_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
		if (audio_stream_index < 0 || !codec) { error = "No supported audio stream"; goto cleanup; }
		decoder = avcodec_alloc_context3(codec);
		if (!decoder) { error = "Not enough memory for audio decoder"; goto cleanup; }
		// Force a single decoder thread before opening the codec to keep Old 3DS RAM use low.
		decoder->thread_count = 1;
		if (avcodec_parameters_to_context(decoder, format->streams[audio_stream_index]->codecpar) < 0 ||
		    avcodec_open2(decoder, codec, NULL) < 0) {
			error = "Could not start audio decoder";
			goto cleanup;
		}
		resampler = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, MP3_OUTPUT_SAMPLE_RATE,
		                               av_get_default_channel_layout(decoder->channels), decoder->sample_fmt,
		                               decoder->sample_rate, 0, NULL);
		if (!resampler || swr_init(resampler) < 0) { error = "Could not convert audio format"; goto cleanup; }
	}

	{
		shine_config_t config;
		shine_set_config_mpeg_defaults(&config.mpeg);
		config.mpeg.bitr = selected_bitrate.load();
		config.mpeg.mode = STEREO;
		config.wave.samplerate = MP3_OUTPUT_SAMPLE_RATE;
		config.wave.channels = PCM_STEREO;
		if (shine_check_config(config.wave.samplerate, config.mpeg.bitr) < 0) {
			error = "Selected MP3 bitrate is unsupported";
			goto cleanup;
		}
		encoder = shine_initialise(&config);
		if (!encoder) { error = "Not enough memory for MP3 encoder"; goto cleanup; }
	}

	output = fopen(output_path.c_str(), "wb");
	if (!output) { error = "Cannot create MP3 file on SD card"; goto cleanup; }
	setvbuf(output, NULL, _IOFBF, 32768);
	if (!write_id3_tag(output, title, image)) { error = "Could not write MP3 cover or title"; goto cleanup; }
	packet = av_packet_alloc();
	frame = av_frame_alloc();
	if (!packet || !frame) { error = "Not enough memory to convert audio"; goto cleanup; }

	{
		std::vector<int16_t> sample_buffer((size_t)shine_samples_per_pass(encoder) * 2);
		size_t buffered_samples = 0;
		int converted_capacity = 4096;
		if (av_samples_alloc(&converted, NULL, 2, converted_capacity, AV_SAMPLE_FMT_S16, 0) < 0) {
			error = "Not enough memory for audio conversion";
			goto cleanup;
		}
		auto receive_frames = [&]() -> bool {
			int receive_result;
			while ((receive_result = avcodec_receive_frame(decoder, frame)) == 0) {
				if (cancel_requested.load()) return false;
				int sample_capacity = swr_get_out_samples(resampler, frame->nb_samples);
				if (sample_capacity > converted_capacity) {
					av_freep(&converted);
					converted_capacity = sample_capacity;
					if (av_samples_alloc(&converted, NULL, 2, converted_capacity, AV_SAMPLE_FMT_S16, 0) < 0)
						return false;
				}
				uint8_t *out_planes[1] = {converted};
				int count = swr_convert(resampler, out_planes, converted_capacity,
				                        (const uint8_t **)frame->extended_data, frame->nb_samples);
				if (count < 0) {
					error = "Audio resampling failed";
					return false;
				}
				if (!write_encoded_samples(encoder, output, sample_buffer, buffered_samples,
				                           (const int16_t *)converted, count)) {
					error = "MP3 encoding or SD write failed";
					return false;
				}
				if (duration_ms > 0 && frame->pts != AV_NOPTS_VALUE) {
					int64_t timestamp_ms = av_rescale_q(frame->pts, format->streams[audio_stream_index]->time_base,
					                                   (AVRational){1, 1000});
					progress.store((int)std::min<int64_t>(99, std::max<int64_t>(0, timestamp_ms * 100 / duration_ms)));
				}
				av_frame_unref(frame);
			}
			if (cancel_requested.load()) return false;
			if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) return true;
			char error_buffer[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(receive_result, error_buffer, sizeof(error_buffer));
			error = std::string("AAC decoder failed: ") + error_buffer;
			return false;
		};

		int read_result = 0;
		while (!cancel_requested.load() && (read_result = av_read_frame(format, packet)) >= 0) {
			if (packet->stream_index == audio_stream_index) {
				result = avcodec_send_packet(decoder, packet);
				if (result < 0) {
					av_packet_unref(packet);
					char error_buffer[AV_ERROR_MAX_STRING_SIZE];
					av_strerror(result, error_buffer, sizeof(error_buffer));
					error = std::string("AAC packet rejected: ") + error_buffer;
					goto cleanup;
				}
				if (!receive_frames()) { av_packet_unref(packet); goto cleanup; }
			}
			av_packet_unref(packet);
		}
		if (cancel_requested.load()) { error = "Cancelled"; goto cleanup; }
		if (read_result != AVERROR_EOF) { error = "Downloaded audio is incomplete or unreadable"; goto cleanup; }
		avcodec_send_packet(decoder, NULL);
		if (!receive_frames()) {
			if (error.empty()) error = "Audio conversion failed";
			goto cleanup;
		}
		if (buffered_samples) {
			memset(sample_buffer.data() + buffered_samples * 2, 0,
			       (sample_buffer.size() / 2 - buffered_samples) * 2 * sizeof(int16_t));
			int written = 0;
			unsigned char *encoded = shine_encode_buffer_interleaved(encoder, sample_buffer.data(), &written);
			if (written < 0 || (written && fwrite(encoded, 1, written, output) != (size_t)written)) {
				error = "Could not write MP3 data";
				goto cleanup;
			}
		}
		int written = 0;
		unsigned char *encoded = shine_flush(encoder, &written);
		if (written < 0 || (written && fwrite(encoded, 1, written, output) != (size_t)written)) {
			error = "Could not finish MP3 file";
			goto cleanup;
		}
	}
	ok = true;

cleanup:
	if (output) {
		if (fflush(output) != 0) { ok = false; error = "Could not finish writing to SD card"; }
		if (fclose(output) != 0) { ok = false; error = "Could not close MP3 file on SD card"; }
	}
	if (converted) av_freep(&converted);
	if (frame) av_frame_free(&frame);
	if (packet) av_packet_free(&packet);
	if (resampler) swr_free(&resampler);
	if (decoder) avcodec_free_context(&decoder);
	if (format) avformat_close_input(&format);
	if (encoder) shine_close(encoder);
	return ok;
}

bool download_and_convert(CURL *curl, const std::string &url, const std::string &title, const std::string &id,
                          long long duration_ms, long long expected_size, bool include_artwork, std::string &saved_name,
                          std::string &error) {
	const char *directory = "sdmc:/download audio";
	if (mkdir(directory, 0777) != 0 && errno != EEXIST) { error = "Could not create download audio folder"; return false; }
	std::string base = make_safe_name(title, id);
	std::string output_path = std::string(directory) + "/" + base + ".mp3";
	for (int suffix = 1; Path(output_path).is_file(); suffix++) {
		output_path = std::string(directory) + "/" + base + " (" + std::to_string(suffix) + ").mp3";
	}
	const std::string pending_output_path = output_path + ".part";
	std::string input_path = output_path + ".source_" + audio_source_key(url, expected_size) + ".tmp";
	if (expected_size > MAX_SOURCE_BYTES) {
		error = "Audio source exceeds 192 MiB safety limit";
		return false;
	}
	long long offset = 0;
	struct stat partial_stat;
	if (stat(input_path.c_str(), &partial_stat) == 0 && partial_stat.st_size > 0 &&
	    (expected_size <= 0 || partial_stat.st_size <= expected_size)) {
		offset = partial_stat.st_size;
	} else {
		std::remove(input_path.c_str());
	}
	resumable_partial.store(offset > 0);
	if (offset > MAX_SOURCE_BYTES) {
		error = "Temporary audio exceeds 192 MiB safety limit";
		return false;
	}
	const u64 transfer_started_ms = osGetTime();
	bool downloaded = expected_size > 0 && offset == expected_size;
	if (expected_size > 0 && offset == expected_size) {
		progress.store(99);
	} else {
		DownloadContext context = {NULL};
	if (!curl) { error = "Could not start network download"; return false; }
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Accept: */*");
	headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Nintendo 3DS; FourthTube MP3 Downloader)");
	bool write_failed = false;
	CURLcode curl_result = CURLE_OK;
	long status_code = 0;
	// A single budget covers all retries. A short clip cannot trickle for hours.
	const long long estimated_size = expected_size > 0 ? expected_size :
	                                 std::max<long long>(0, duration_ms / 1000) * 16000;
	const long long budget_seconds = std::min<long long>(5400,
	                                std::max<long long>(180, 120 + estimated_size / 8192));
	const u64 download_started_ms = osGetTime();
	for (int attempt = 0; attempt < 3 && !cancel_requested.load(); attempt++) {
		const long long elapsed_seconds = (osGetTime() - download_started_ms) / 1000;
		const long remaining_seconds = (long)std::max<long long>(0, budget_seconds - elapsed_seconds);
		if (remaining_seconds == 0) { error = "Download took too long"; break; }
		download_received_bytes.store(0);
		download_written_bytes.store(0);
		download_first_data_ms.store(0);
		download_last_data_ms.store(0);
		download_attempt_start_ms.store((unsigned int)osGetTime());
		download_http_status.store(0);
		download_curl_code.store(0);
		download_write_failed.store(false);
		download_size_exceeded.store(false);
		download_total_known.store(expected_size > 0);
		download_phase.store(attempt ? 3 : 1);
		FILE *file = fopen(input_path.c_str(), offset > 0 ? "ab" : "wb");
		if (!file) { error = "Could not open temporary audio file on SD"; break; }
        // Larger sequential-I/O buffers reduce callback and SD write overhead
        // without a meaningful Old 3DS memory cost (32 KiB each).
        setvbuf(file, NULL, _IOFBF, 32768);
		context.file = file;
		curl_easy_reset(curl);
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
		curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
		curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
		curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 2L);
		curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, remaining_seconds);
		// Abort a connection that stops delivering data instead of showing 0% forever.
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 64L);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
		curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 65536L);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_file);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_update_progress);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
		if (offset > 0) curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)offset);
		download_offset = offset;
		curl_result = curl_easy_perform(curl);
		status_code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
		download_http_status.store((int)status_code);
		download_curl_code.store((int)curl_result);
		write_failed = ferror(file) != 0 || fflush(file) != 0;
		if (fclose(file) != 0) write_failed = true;
		if (write_failed) download_write_failed.store(true);
		if (cancel_requested.load()) break;
		if (download_size_exceeded.load()) { error = "Audio source exceeds 192 MiB safety limit"; break; }
		// Another network attempt cannot repair a failed SD write. Leave the
		// partial file untouched and report the storage error immediately.
		if (download_write_failed.load()) { error = "SD write failed"; break; }
		// Some CDNs ignore Range. Drop the partial file and retry from byte zero.
		if (offset > 0 && status_code == 200) {
			std::remove(input_path.c_str());
			offset = 0;
			resumable_partial.store(false);
			continue;
		}
		bool expected_size_matches = true;
		if (expected_size > 0) {
			struct stat downloaded_stat;
			expected_size_matches = stat(input_path.c_str(), &downloaded_stat) == 0 &&
			                        downloaded_stat.st_size == expected_size;
		}
		if (curl_result == CURLE_OK && status_code >= 200 && status_code < 300 &&
		    download_written_bytes.load() > 0 && !write_failed &&
		    expected_size_matches) {
			downloaded = true;
			break;
		}
		// A rejected or missing source will not become playable by retrying the
		// same URL. Fail promptly and let the caller refresh the video metadata.
		if (status_code == 401 || status_code == 403 || status_code == 404 || status_code == 410) {
			error = "Audio unavailable at source (HTTP " + std::to_string(status_code) + ")";
			break;
		}
		if (stat(input_path.c_str(), &partial_stat) == 0) offset = partial_stat.st_size;
		if (expected_size > 0 && offset >= expected_size) {
			if (offset == expected_size) { downloaded = true; break; }
			std::remove(input_path.c_str());
			offset = 0;
		}
		resumable_partial.store(offset > 0);
		logger.error("mp3/download", "Attempt " + std::to_string(attempt + 1) +
		             ": curl=" + std::to_string((int)curl_result) +
		             " HTTP=" + std::to_string(status_code) +
		             " received=" + std::to_string(download_received_bytes.load()) +
		             " written=" + std::to_string(download_written_bytes.load()) +
		             " SD_error=" + std::to_string((int)download_write_failed.load()));
		if (attempt < 2) svcSleepThread(500000000LL);
	}
	curl_easy_reset(curl);
	curl_slist_free_all(headers);
	}
	if (!downloaded || cancel_requested.load()) {
		if (cancel_requested.load()) {
			std::remove(input_path.c_str());
			resumable_partial.store(false);
			error = "Cancelled";
		} else if (!error.empty()) {
			// Keep the specific filesystem/network setup error.
		} else if (download_write_failed.load()) {
			error = "SD write failed";
		} else if (download_http_status.load() >= 400) {
			error = "Audio source HTTP " + std::to_string(download_http_status.load());
		} else if (download_curl_code.load() != CURLE_OK) {
			int code = download_curl_code.load();
			error = "Network C" + std::to_string(code) + ": " + curl_easy_strerror((CURLcode)code);
		} else if (download_written_bytes.load() == 0) {
			error = "No audio data received";
		} else if (expected_size > 0) {
			error = "Audio size differs from source metadata";
		} else if (resumable_partial.load()) {
			error = "Download interrupted; partial audio kept for resume";
		} else {
			error = "Audio download failed (network or SD card)";
		}
		return false;
	}
	transfer_elapsed_ms.store((unsigned int)(osGetTime() - transfer_started_ms));

	std::vector<unsigned char> cover_image;
	if (include_artwork) {
		state.store((int)AudioExportState::FETCHING_ART);
		(void)fetch_album_art(curl, id, cover_image);
	}
	if (cancel_requested.load()) {
		std::remove(input_path.c_str());
		error = "Cancelled";
		return false;
	}
	state.store((int)AudioExportState::ENCODING);
	progress.store(0);
	encode_started_ms.store((unsigned int)osGetTime());
	if (!encode_file(input_path, pending_output_path, duration_ms, title, cover_image, error)) {
		encode_elapsed_ms.store((unsigned int)osGetTime() - encode_started_ms.load());
		struct stat source_stat;
		bool keep_complete_source = expected_size > 0 && stat(input_path.c_str(), &source_stat) == 0 &&
		                            source_stat.st_size == expected_size;
		if (!keep_complete_source) std::remove(input_path.c_str());
		std::remove(pending_output_path.c_str());
		resumable_partial.store(keep_complete_source);
		conversion_failed.store(true);
		conversion_error_code.store(get_conversion_error_code(error));
		return false;
	}
	encode_elapsed_ms.store((unsigned int)osGetTime() - encode_started_ms.load());
	if (rename(pending_output_path.c_str(), output_path.c_str()) != 0) {
		error = "Could not finalize MP3 file on SD";
		std::remove(pending_output_path.c_str());
		if (expected_size <= 0) std::remove(input_path.c_str());
		resumable_partial.store(expected_size > 0);
		conversion_failed.store(true);
		conversion_error_code.store(get_conversion_error_code(error));
		return false;
	}
	conversion_failed.store(false);
	conversion_error_code.store(0);
	artwork_embedded.store(!cover_image.empty());
	std::remove(input_path.c_str());
	resumable_partial.store(false);
	saved_name = output_path;
	return true;
}

void worker(void *) {
	CURL *curl = curl_easy_init();
	while (true) {
		LightEvent_Wait(&worker_wake_event);
		if (stop_requested.load()) break;
		if (!request_pending.exchange(false)) continue;
		if (wait_for_player.load()) {
			while (!player_stopped.load() && !cancel_requested.load() && !stop_requested.load())
				LightEvent_Wait(&player_stop_event);
		}
		if (cancel_requested.load() || stop_requested.load()) {
			state.store((int)AudioExportState::CANCELLED);
			continue;
		}
		state.store((int)AudioExportState::DOWNLOADING);
		progress.store(0);
		std::string url = requested_url, title = requested_title, id = requested_id;
		std::string saved_name, error;
		bool success = download_and_convert(curl, url, title, id, requested_duration_ms, requested_size,
		                                    selected_artwork.load(), saved_name, error);
		last_error_detail = error;
		// A completed MP3 wins over a late cancel request. The queue must not
		// retain an item whose final file was already saved.
		if (success) state.store((int)AudioExportState::DONE);
		else state.store((int)(cancel_requested.load() ? AudioExportState::CANCELLED : AudioExportState::FAILED));
		if (success) logger.info("mp3", "Saved audio file: " + saved_name);
		else if (!cancel_requested.load()) logger.error("mp3", error);
	}
	if (curl) curl_easy_cleanup(curl);
	threadExit(0);
}
} // namespace

void AudioExport_init() {
	LightEvent_Init(&worker_wake_event, RESET_ONESHOT);
	LightEvent_Init(&player_stop_event, RESET_ONESHOT);
	stop_requested.store(false);
	cancel_requested.store(false);
	request_pending.store(false);
	player_stopped.store(true);
	state.store((int)AudioExportState::IDLE);
	const bool old_model = var_model == CFG_MODEL_3DS || var_model == CFG_MODEL_3DSXL ||
	                       var_model == CFG_MODEL_2DS;
	// Core 1 has limited app time on old models. A low-priority worker on
	// core 0 can use the time left by the UI during an export.
	worker_thread = threadCreate(worker, NULL, DEF_STACKSIZE,
	                             old_model ? DEF_THREAD_PRIORITY_IDLE : DEF_THREAD_PRIORITY_NORMAL,
	                             old_model ? 0 : 1, false);
	if (!worker_thread && old_model)
		worker_thread = threadCreate(worker, NULL, DEF_STACKSIZE, DEF_THREAD_PRIORITY_NORMAL, 1, false);
	if (!worker_thread) state.store((int)AudioExportState::FAILED);
}

void AudioExport_exit() {
	if (!worker_thread) return;
	cancel_requested.store(true);
	stop_requested.store(true);
	LightEvent_Signal(&worker_wake_event);
	LightEvent_Signal(&player_stop_event);
	// The worker owns curl/FFmpeg and its files; never free its handle while it
	// can still be running. Its cancellation checks make this join bounded by I/O.
	Result join_result = threadJoin(worker_thread, UINT64_MAX);
	if (R_FAILED(join_result)) {
		logger.error("mp3", "Could not join audio export worker: " + std::to_string(join_result));
		return;
	}
	threadFree(worker_thread);
	worker_thread = 0;
}

void AudioExport_begin(const std::string &url, const std::string &title, const std::string &video_id,
                       int64_t duration_ms, long long expected_size) {
	if (state.load() != (int)AudioExportState::IDLE) return;
	duration_limit_rejected.store(duration_ms <= 0 || duration_ms > 60 * 60 * 1000);
	if (duration_limit_rejected.load()) {
		state.store((int)AudioExportState::FAILED);
		return;
	}
	requested_url = url;
	requested_title = title;
	requested_id = video_id;
	requested_duration_ms = duration_ms;
	requested_size = expected_size;
	selected_bitrate.store(128);
	selected_artwork.store(true);
	progress.store(0);
	cancel_requested.store(false);
	resumable_partial.store(false);
	conversion_failed.store(false);
	artwork_embedded.store(false);
	conversion_error_code.store(0);
	last_error_detail.clear();
	transfer_elapsed_ms.store(0);
	encode_elapsed_ms.store(0);
	encode_started_ms.store(0);
	player_stopped.store(false);
	state.store((int)AudioExportState::SELECT_BITRATE);
}

void AudioExport_choose_bitrate(int bitrate_kbps) {
	if (state.load() != (int)AudioExportState::SELECT_BITRATE || (bitrate_kbps != 128 && bitrate_kbps != 256)) return;
	selected_bitrate.store(bitrate_kbps);
	state.store((int)AudioExportState::SELECT_ARTWORK);
}

void AudioExport_choose_artwork(bool include_artwork) {
	if (state.load() != (int)AudioExportState::SELECT_ARTWORK) return;
	selected_artwork.store(include_artwork);
	state.store((int)AudioExportState::CONFIRM);
}

void AudioExport_confirm(bool wait_for_player_stop) {
	if (state.load() != (int)AudioExportState::CONFIRM) return;
	if (wait_for_player_stop) LightEvent_Clear(&player_stop_event);
	wait_for_player.store(wait_for_player_stop);
	player_stopped.store(!wait_for_player_stop);
	state.store((int)AudioExportState::WAITING_FOR_PLAYER);
	request_pending.store(true);
	LightEvent_Signal(&worker_wake_event);
}

void AudioExport_notify_player_stopped() {
	player_stopped.store(true);
	LightEvent_Signal(&player_stop_event);
}
void AudioExport_cancel_prompt() {
	if (state.load() == (int)AudioExportState::SELECT_BITRATE ||
	    state.load() == (int)AudioExportState::SELECT_ARTWORK ||
	    state.load() == (int)AudioExportState::CONFIRM)
		state.store((int)AudioExportState::IDLE);
}
void AudioExport_cancel_operation() {
	cancel_requested.store(true);
	LightEvent_Signal(&player_stop_event);
}
void AudioExport_dismiss() {
	if (state.load() == (int)AudioExportState::DONE || state.load() == (int)AudioExportState::FAILED ||
	    state.load() == (int)AudioExportState::CANCELLED)
		state.store((int)AudioExportState::IDLE);
}
AudioExportState AudioExport_get_state() { return (AudioExportState)state.load(); }
int AudioExport_get_progress() { return progress.load(); }
int AudioExport_get_bitrate() { return selected_bitrate.load(); }
std::string AudioExport_get_error_detail() { return last_error_detail; }

void AudioExport_draw_overlay() {
	AudioExportState current = AudioExport_get_state();
	if (current == AudioExportState::IDLE) return;
	Draw_texture(var_square_image[0], DEF_DRAW_BLACK, 18, 78, 364, 86);
	bool show_progress = current == AudioExportState::ENCODING ||
	                     (current == AudioExportState::DOWNLOADING && download_total_known.load());
	if (show_progress) {
		Draw_texture(var_square_image[0], 0xFF555555, 30, 143, 340, 8);
		Draw_texture(var_square_image[0], 0xFF30A0E0, 30, 143, 340 * AudioExport_get_progress() / 100.0f, 8);
	}
	const bool french = var_lang == "fr";
	std::string line1, line2, line3;
	switch (current) {
		case AudioExportState::SELECT_BITRATE:
			line1 = french ? "Debit du MP3" : "MP3 output bitrate";
			line2 = french ? "A:128 kbps  X:256 kbps  B:Annuler" : "A:128 kbps  X:256 kbps  B:Cancel";
			line3 = french ? "La source peut etre de moins bonne qualite" : "Source audio quality may be lower";
			break;
		case AudioExportState::SELECT_ARTWORK:
			line1 = french ? "Inclure la miniature dans le MP3 ?" : "Include video thumbnail in MP3?";
			line2 = french ? "A: Avec pochette  B: Sans" : "A: With cover   B: Without";
			line3 = french ? "X: Annuler" : "X: Cancel";
			break;
		case AudioExportState::CONFIRM:
			line1 = (french ? "Creer un MP3 a " : "Create MP3 at ") +
			        std::to_string(AudioExport_get_bitrate()) + " kbps?";
			line2 = french ? "A: Oui    B: Annuler" : "A: Yes    B: Cancel";
			line3 = selected_artwork.load()
			        ? (french ? "Pochette si disponible" : "Cover if available")
			        : (french ? "Sans pochette" : "No cover");
			break;
		case AudioExportState::WAITING_FOR_PLAYER:
			line1 = french ? "Preparation du MP3..." : "Preparing audio export...";
			line2 = french ? "Arret du lecteur pour liberer la RAM" : "Stopping playback to free memory";
			break;
		case AudioExportState::DOWNLOADING:
			line1 = download_total_known.load()
			            ? (french ? "Telechargement audio : " : "Downloading audio: ") +
			              std::to_string(AudioExport_get_progress()) + "%"
			            : (french ? "Telechargement : taille inconnue" : "Downloading: size unknown");
			line2 = download_phase.load() == 3
			        ? (french ? "Nouvelle tentative...  B: Annuler" : "Retrying connection...  B: Cancel")
			        : download_phase.load() == 2
			        ? (french ? "Reception audio  B: Annuler" : "Receiving audio  B: Cancel")
			        : (french ? "Connexion audio...  B: Annuler" : "Connecting to audio...  B: Cancel");
			if (download_phase.load() == 2 && download_first_data_ms.load()) {
				unsigned int elapsed_ms = (unsigned int)osGetTime() - download_first_data_ms.load();
				if (elapsed_ms >= 1000) {
					unsigned int kb_per_s = (unsigned int)((uint64_t)download_written_bytes.load() * 1000 /
					                                  ((uint64_t)elapsed_ms * 1024));
					line2 = std::to_string(kb_per_s) + " KB/s  B: " + (french ? "Annuler" : "Cancel");
				}
			}
			line3 = (french ? "Recu " : "Received ") +
			        std::to_string(download_received_bytes.load() / 1024) +
			        " KB / SD " + std::to_string(download_written_bytes.load() / 1024) + " KB";
			if (download_last_data_ms.load()) {
				line3 += (french ? "  attente " : "  idle ") +
				         std::to_string(((unsigned int)osGetTime() -
				                                   download_last_data_ms.load()) / 1000) + " s";
			} else if (download_attempt_start_ms.load()) {
				line3 += (french ? "  attente " : "  waiting ") +
				         std::to_string(((unsigned int)osGetTime() -
				                                      download_attempt_start_ms.load()) / 1000) + " s";
			}
			break;
		case AudioExportState::FETCHING_ART:
			line1 = french ? "Preparation de la pochette..." : "Preparing MP3 cover art...";
			line2 = french ? "B: Annuler" : "B: Cancel";
			line3 = french ? "Le son sera sauve meme sans pochette" : "Audio saves even without artwork";
			break;
		case AudioExportState::ENCODING:
			line1 = (french ? "Conversion MP3 : " : "Converting to MP3: ") +
			        std::to_string(AudioExport_get_progress()) + "%";
			line2 = std::to_string(encode_started_ms.load()
			    ? ((unsigned int)osGetTime() - encode_started_ms.load()) / 1000 : 0) +
			    (french ? " s ecoulees  B: Annuler" : " s elapsed  B: Cancel");
			break;
		case AudioExportState::DONE:
			line1 = french ? "MP3 sauve : SD:/download audio" : "MP3 saved to SD:/download audio";
			line2 = "DL " + std::to_string(transfer_elapsed_ms.load() / 1000) +
			        " s  MP3 " + std::to_string(encode_elapsed_ms.load() / 1000) +
			        (french ? " s  A/B: fermer" : " s  A/B: close");
			line3 = artwork_embedded.load()
			        ? (french ? "Pochette incluse" : "Cover included in MP3")
			        : (french ? "Son sauve sans pochette" : "Audio saved without cover");
			break;
		case AudioExportState::FAILED:
			line1 = duration_limit_rejected.load()
			        ? (french ? "Duree inconnue ou >1 h" : "Unknown or >1h audio blocked")
			        : conversion_failed.load()
			        ? (french ? "Echec conversion MP3" : "MP3 conversion failed")
			        : resumable_partial.load()
			        ? (french ? "Telechargement interrompu" : "Download interrupted; partial kept")
			        : (french ? "Echec export MP3" : "MP3 export failed");
			line2 = conversion_failed.load() ? std::string("E") +
			                                       (conversion_error_code.load() < 10 ? "0" : "") +
			                                       std::to_string(conversion_error_code.load()) +
			                                       (french ? "  A/B: fermer" : "  A/B: close")
			                                  : resumable_partial.load()
			                                    ? (french ? "Source gardee  A/B: fermer" : "Source kept  A/B: close")
			                                    : (french ? "Reseau/SD  A: fermer" : "Network/SD  A: close");
			if (!last_error_detail.empty()) {
				size_t detail_start = last_error_detail.find(": ");
				line3 = last_error_detail.substr(detail_start == std::string::npos ? 0 : detail_start + 2, 46);
				if (last_error_detail.size() - (detail_start == std::string::npos ? 0 : detail_start + 2) > 46)
					line3 += "...";
			}
			break;
		case AudioExportState::CANCELLED:
			line1 = french ? "Export MP3 annule" : "MP3 export cancelled";
			line2 = french ? "A ou B: fermer" : "A or B: close";
			break;
		default: return;
	}
	Draw_xy_centered(line1, 24, 376, 91, 116, 0.65, 0.65, DEFAULT_TEXT_COLOR);
	Draw_xy_centered(line2, 24, 376, 116, 137, 0.45, 0.45, DEFAULT_TEXT_COLOR);
	if (!line3.empty()) Draw_xy_centered(line3, 24, 376, 138, 159, 0.4, 0.4, DEFAULT_TEXT_COLOR);
}

#else
void AudioExport_init() {}
void AudioExport_exit() {}
void AudioExport_begin(const std::string &, const std::string &, const std::string &, int64_t, long long) {}
void AudioExport_choose_bitrate(int) {}
void AudioExport_choose_artwork(bool) {}
void AudioExport_confirm(bool) {}
void AudioExport_notify_player_stopped() {}
void AudioExport_cancel_prompt() {}
void AudioExport_cancel_operation() {}
void AudioExport_dismiss() {}
AudioExportState AudioExport_get_state() { return AudioExportState::IDLE; }
int AudioExport_get_progress() { return 0; }
int AudioExport_get_bitrate() { return 128; }
std::string AudioExport_get_error_detail() { return ""; }
void AudioExport_draw_overlay() {}
#endif
