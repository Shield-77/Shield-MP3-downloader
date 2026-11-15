#include "headers.hpp"
#include "network_downloader.hpp"
#include "network_io.hpp"
#include "cache_policy.hpp"
#include <cerrno>

#define MAX_CACHE_BLOCKS NetworkStream::OLD3DS_MAX_CACHE_BLOCKS

static u64 forward_cache_blocks() {
	return std::min<int>(MAX_CACHE_BLOCKS - 1, std::max<int>(2, (MAX_CACHE_BLOCKS - 1) * var_forward_buffer_ratio));
}

// --------------------------------
// NetworkStream implementation
// --------------------------------

bool NetworkStream::is_data_available(u64 start, u64 size) {
	if (!ready || !size || start >= len || size > len - start) {
		return false;
	}
	u64 end = start + size - 1;
	u64 start_block = start / BLOCK_SIZE;
	u64 end_block = end / BLOCK_SIZE;

	bool res = true;
	downloaded_data_lock.lock();
	for (u64 block = start_block; block <= end_block; block++) {
		if (!downloaded_data.count(block)) {
			res = false;
			break;
		}
	}
	downloaded_data_lock.unlock();
	return res;
}
bool NetworkStream::has_block(u64 block) {
	downloaded_data_lock.lock();
	const bool available = downloaded_data.count(block) != 0;
	downloaded_data_lock.unlock();
	return available;
}
size_t NetworkStream::read_data(u64 start, u8 *destination, size_t size) {
	if (!ready || !destination || !size || start >= len) {
		return 0;
	}
	size = (size_t)std::min<u64>(size, len - start);
	u64 end = start + size - 1;
	u64 start_block = start / BLOCK_SIZE;
	u64 end_block = end / BLOCK_SIZE;
	size_t copied = 0;

	downloaded_data_lock.lock();
	for (u64 block = start_block; block <= end_block; block++) {
		auto itr = downloaded_data.find(block);
		if (itr == downloaded_data.end()) {
			downloaded_data_lock.unlock();
			return 0;
		}
		u64 block_start = block * BLOCK_SIZE;
		u64 cur_l = std::max(start, block_start) - block_start;
		u64 cur_r = std::min(end + 1, block_start + itr->second.size()) - block_start;
		if (cur_r <= cur_l) {
			downloaded_data_lock.unlock();
			return 0;
		}
		size_t chunk_size = (size_t)(cur_r - cur_l);
		memcpy(destination + copied, itr->second.data() + cur_l, chunk_size);
		copied += chunk_size;
	}
	downloaded_data_lock.unlock();
	return copied;
}

size_t NetworkStream::read_next_data(u8 *destination, size_t size, bool &eof) {
	eof = false;
	if (!destination || !size) {
		return 0;
	}

	downloaded_data_lock.lock();
	if (!ready) {
		downloaded_data_lock.unlock();
		return 0;
	}
	if (read_head >= len) {
		eof = true;
		downloaded_data_lock.unlock();
		return 0;
	}

	const u64 start = read_head;
	const size_t read_size = (size_t)std::min<u64>(size, len - start);
	size_t copied = 0;
	// AVIO accepts short reads. Return the contiguous bytes already cached
	// instead of withholding the end of one block until the next arrives.
	while (copied < read_size) {
		const u64 position = start + copied;
		auto block = downloaded_data.find(position / BLOCK_SIZE);
		if (block == downloaded_data.end()) {
			break;
		}
		const size_t offset = (size_t)(position % BLOCK_SIZE);
		if (offset >= block->second.size()) {
			break;
		}
		const size_t chunk_size = std::min(read_size - copied, block->second.size() - offset);
		memcpy(destination + copied, block->second.data() + offset, chunk_size);
		copied += chunk_size;
	}
	read_head += copied;
	downloaded_data_lock.unlock();
	return copied;
}

u64 NetworkStream::get_read_head() {
	downloaded_data_lock.lock();
	const u64 position = read_head;
	downloaded_data_lock.unlock();
	return position;
}

void NetworkStream::set_read_head(u64 position) {
	downloaded_data_lock.lock();
	read_head = position;
	downloaded_data_lock.unlock();
}

void NetworkStream::set_data(u64 block, std::vector<u8> data) {
	downloaded_data_lock.lock();

	// if you move it, it doesn't perform a copy which means you will never get a std::bad_alloc which used to happen before
	downloaded_data[block] = std::move(data);

	const fourthtube::CacheEvictionPolicy policy(read_head / BLOCK_SIZE, MAX_CACHE_BLOCKS, forward_cache_blocks());
	while (downloaded_data.size() > MAX_CACHE_BLOCKS) {
		auto victim = downloaded_data.end();
		for (auto itr = downloaded_data.begin(); itr != downloaded_data.end(); ++itr) {
			// Preserve the same forward window that the downloader fills. Choosing
			// only by distance could discard every newly prefetched block, then
			// download that very block again forever while old blocks remained.
			if (policy.prefer_eviction(itr->first, victim == downloaded_data.end() ? 0 : victim->first,
			                           victim != downloaded_data.end())) {
				victim = itr;
			}
		}
		if (victim == downloaded_data.end()) {
			break;
		}
		downloaded_data.erase(victim);
	}
	downloaded_data_lock.unlock();
}
double NetworkStream::get_download_percentage() {
	downloaded_data_lock.lock();
	double res = len ? std::min(100.0, (double)downloaded_data.size() * BLOCK_SIZE / len * 100) : 0;
	downloaded_data_lock.unlock();
	return res;
}
std::vector<double> NetworkStream::get_buffering_progress_bar(int res_len) {
	if (res_len <= 0) {
		return {};
	}
	downloaded_data_lock.lock();
	std::vector<double> res(res_len);
	if (!len) {
		downloaded_data_lock.unlock();
		return res;
	}
	auto itr = downloaded_data.begin();
	for (int i = 0; i < res_len; i++) {
		u64 l = (u64)len * i / res_len;
		u64 r = std::min<u64>(len, len * (i + 1) / res_len);
		if (r <= l) {
			continue;
		}
		while (itr != downloaded_data.end()) {
			u64 il = itr->first * BLOCK_SIZE;
			u64 ir = std::min((itr->first + 1) * BLOCK_SIZE, len);
			if (ir <= l) {
				itr++;
			} else if (il >= r) {
				break;
			} else {
				res[i] += std::min(ir, r) - std::max(il, l);
				if (ir >= r) {
					break;
				} else {
					itr++;
				}
			}
		}
		res[i] /= r - l;
		res[i] *= 100;
	}
	downloaded_data_lock.unlock();
	return res;
}

// --------------------------------
// NetworkStreamDownloader implementation
// --------------------------------

void NetworkStreamDownloader::add_stream(NetworkStream *stream) {
	streams_lock.lock();
	size_t index = (size_t)-1;
	for (size_t i = 0; i < streams.size(); i++) {
		if (!streams[i]) {
			streams[i] = stream;
			index = i;
			break;
		}
	}
	if (index == (size_t)-1) {
		index = streams.size();
		streams.push_back(stream);
	}
	streams_lock.unlock();
}

static bool thread_network_session_list_inited = false;
static NetworkSessionList thread_network_session_list;
static void confirm_thread_network_session_list_inited() {
	if (!thread_network_session_list_inited) {
		thread_network_session_list_inited = true;
		thread_network_session_list.init();
	}
}

static std::string remove_url_parameter(const std::string &url, const std::string &param) {
	std::string res;
	for (size_t i = 0; i < url.size();) {
		if (url[i] == '&' || url[i] == '?') {
			if (url.substr(i + 1, param.size() + 1) == param + "=") {
				i = std::find(url.begin() + i + 1, url.end(), '&') - url.begin();
			} else {
				res.push_back(url[i++]);
			}
		} else {
			res.push_back(url[i++]);
		}
	}
	return res;
}

#define LOG_THREAD_STR "net/dl"
void NetworkStreamDownloader::downloader_thread() {
	while (!thread_exit_requested) {
		size_t cur_stream_index =
		    (size_t)-1; // the index of the stream on which we will perform a download in this loop
		streams_lock.lock();
		// back up 'read_head's as those can be changed from another thread
		std::vector<u64> read_heads(streams.size());
		for (size_t i = 0; i < streams.size(); i++) {
			if (streams[i]) {
				read_heads[i] = streams[i]->get_read_head();
			}
		}

		// find the stream to download next
		double margin_percentage_min = 1000;
		for (size_t i = 0; i < streams.size(); i++) {
			if (!streams[i]) {
				continue;
			}
			if (streams[i]->quit_request) {
				delete streams[i];
				streams[i] = NULL;
				continue;
			}
			if (streams[i]->error) {
				continue;
			}
			if (streams[i]->suspend_request) {
				continue;
			}
			if (!streams[i]->ready) {
				cur_stream_index = i;
				break;
			}
			if (streams[i]->whole_download) {
				continue; // its entire content should already be downloaded
			}

			u64 forward_buffer_block_num = forward_cache_blocks(); // block #0 is always kept
			u64 read_head_block = read_heads[i] / BLOCK_SIZE;
			u64 first_not_downloaded_block = read_head_block;
			while (first_not_downloaded_block < streams[i]->block_num &&
			       streams[i]->has_block(first_not_downloaded_block)) {
				first_not_downloaded_block++;
				if (first_not_downloaded_block == read_head_block + forward_buffer_block_num) {
					break;
				}
			}
			if (first_not_downloaded_block == streams[i]->block_num) {
				continue;
			}
			if (first_not_downloaded_block == read_head_block + forward_buffer_block_num) {
				continue; // no need to download this stream for now
			}

			double margin_percentage;
			if (first_not_downloaded_block == read_head_block) {
				margin_percentage = 0;
			} else {
				margin_percentage =
				    (double)(first_not_downloaded_block * BLOCK_SIZE - read_heads[i]) / streams[i]->len * 100;
			}
			if (margin_percentage_min > margin_percentage) {
				margin_percentage_min = margin_percentage;
				cur_stream_index = i;
			}
		}

		if (cur_stream_index == (size_t)-1) {
			streams_lock.unlock();
			usleep(20000);
			continue;
		}
		NetworkStream *cur_stream = streams[cur_stream_index];
		streams_lock.unlock();

		confirm_thread_network_session_list_inited();
		// whole download
		if (cur_stream->whole_download) {
			auto &session_list = cur_stream->session_list ? *cur_stream->session_list : thread_network_session_list;
			auto request = HttpRequest::GET(cur_stream->url, {})
			                   .with_response_size_limit(NetworkStream::OLD3DS_MAX_CACHE_BYTES)
			                   .with_cancel_func([this, cur_stream]() {
				                   return thread_exit_requested || cur_stream->quit_request;
			                   });
			auto result = session_list.perform(request);
			if (thread_exit_requested || cur_stream->quit_request) {
				continue;
			}
			if (result.redirected_url != "") {
				cur_stream->url = result.redirected_url;
			}

			if (!result.fail && result.status_code_is_success() && result.data.size()) {
				{ // acquire necessary headers
					char *end;
					auto value = result.get_header("x-head-seqnum");
					cur_stream->seq_head = strtoll(value.c_str(), &end, 10);
					if (*end || !value.size()) {
						logger.error("net/dl", "failed to acquire x-head-seqnum");
						cur_stream->seq_head = -1;
						cur_stream->error = true;
					}
					value = result.get_header("x-sequence-num");
					cur_stream->seq_id = strtoll(value.c_str(), &end, 10);
					if (*end || !value.size()) {
						logger.error("net/dl", "failed to acquire x-sequence-num");
						cur_stream->seq_id = -1;
						cur_stream->error = true;
					}
				}
				if (!cur_stream->error) {
					cur_stream->len = result.data.size();
					cur_stream->block_num = (cur_stream->len + BLOCK_SIZE - 1) / BLOCK_SIZE;
					for (size_t i = 0; i < result.data.size(); i += BLOCK_SIZE) {
						size_t left = i;
						size_t right = std::min<size_t>(i + BLOCK_SIZE, result.data.size());
						cur_stream->set_data(i / BLOCK_SIZE,
						                     std::vector<u8>(result.data.begin() + left, result.data.begin() + right));
					}
					cur_stream->ready = true;
				}
			} else {
				logger.error("net/dl", "failed accessing : " + result.error);
				cur_stream->error = true;
				switch (result.status_code) {
				// these codes are returned when trying to read beyond the end of the livestream
				case HTTP_STATUS_CODE_NO_CONTENT:
				case HTTP_STATUS_CODE_NOT_FOUND:
					cur_stream->livestream_eof = true;
					break;
				// this code is returned when trying to read an ended livestream without archive
				case HTTP_STATUS_CODE_FORBIDDEN:
					cur_stream->livestream_private = true;
					break;
				}
			}
		} else {
			u64 block_reading = read_heads[cur_stream_index] / BLOCK_SIZE;
			if (cur_stream->ready) {
				while (block_reading < cur_stream->block_num && cur_stream->has_block(block_reading)) {
					block_reading++;
				}
				if (block_reading == cur_stream->block_num) { // something unexpected happened
					logger.error(LOG_THREAD_STR, "unexpected error (trying to read beyond the end of the stream)");
					cur_stream->error = true;
					continue;
				}
			}
			// Util_log_save("net/dl", "dl next : " + std::to_string(cur_stream_index) + " " +
			// std::to_string(block_reading));

			u64 start = block_reading * BLOCK_SIZE;
			u64 end = cur_stream->len ? std::min((block_reading + 1) * BLOCK_SIZE, cur_stream->len)
			                          : (block_reading + 1) * BLOCK_SIZE;
			u64 expected_len = end - start;

			auto &session_list = cur_stream->session_list ? *cur_stream->session_list : thread_network_session_list;
			// length not sure -> use Range header to get the size (slower)
			auto request =
			    cur_stream->len == 0
			        ? HttpRequest::GET(
			              cur_stream->url,
			              {{"Range", "bytes=" + std::to_string(start) + "-" + std::to_string(end - 1)}})
			        : HttpRequest::GET(
			              cur_stream->url + "&range=" + std::to_string(start) + "-" + std::to_string(end - 1), {});
			request = request.with_response_size_limit((size_t)expected_len).with_cancel_func([this, cur_stream]() {
				return thread_exit_requested || cur_stream->quit_request;
			});
			auto result = session_list.perform(request);
			if (thread_exit_requested || cur_stream->quit_request) {
				continue;
			}
			if (result.redirected_url != "") {
				cur_stream->url = remove_url_parameter(result.redirected_url, "range");
			}

			if (!result.fail && result.status_code_is_success()) {
				if (cur_stream->len == 0) {
					auto content_range_str = result.get_header("Content-Range");
					char *slash = strchr(content_range_str.c_str(), '/');
					bool ok = false;
					if (slash) {
						char *end;
						errno = 0;
						const u64 total_len = strtoull(slash + 1, &end, 10);
						if (end != slash + 1 && !*end && slash[1] != '-' && !errno && total_len > start) {
							ok = true;
							cur_stream->len = total_len;
							cur_stream->block_num = NetworkStream::get_block_num(cur_stream->len);
						} else {
							logger.error(LOG_THREAD_STR, "failed to parse Content-Range : " + std::string(slash + 1));
						}
					} else {
						logger.error(LOG_THREAD_STR,
						             "no slash in Content-Range response header : " + content_range_str);
					}
					if (!ok) {
						cur_stream->error = true;
						continue;
					}
				}
				// The first range must be validated too. Publishing a short initial
				// block leaves permanent holes that the reader can never get past.
				expected_len = std::min<u64>(BLOCK_SIZE, cur_stream->len - start);
				if (result.data.size() != expected_len) {
					logger.error(LOG_THREAD_STR, "size discrepancy : " + std::to_string(expected_len) + " -> " +
					                                 std::to_string(result.data.size()));
					if (cur_stream->retry_cnt_left) {
						cur_stream->retry_cnt_left--;
					} else {
						cur_stream->error = true;
					}
					continue;
				}
				cur_stream->retry_cnt_left = NetworkStream::RETRY_CNT_MAX;
				cur_stream->set_data(block_reading, std::move(result.data)); // data isn't accessed further down so we can move it
				cur_stream->ready = true;
			} else if (!result.fail) {
				logger.error("net/dl", "stream returned: " + std::to_string(result.status_code));
				// Recover from temporary server throttling/outages, but fail fast
				// for permanent URL and range errors such as 403 or 404.
				const bool transient_status = result.status_code == 408 || result.status_code == 429 ||
				                              result.status_code >= 500;
				if (transient_status && cur_stream->retry_cnt_left > 0) {
					const int retry_number = NetworkStream::RETRY_CNT_MAX - cur_stream->retry_cnt_left;
					cur_stream->retry_cnt_left--;
					usleep(250000 * (retry_number + 1));
				} else {
					cur_stream->error = true;
				}
			} else {
				logger.error("net/dl", "access failed : " + result.error);
				if (cur_stream->retry_cnt_left) {
					const int retry_number = NetworkStream::RETRY_CNT_MAX - cur_stream->retry_cnt_left;
					cur_stream->retry_cnt_left--;
					usleep(250000 * (retry_number + 1));
				} else {
					cur_stream->error = true;
				}
			}
		}
	}
	logger.info(LOG_THREAD_STR, "Exit, deiniting...");
	for (auto stream : streams) {
		if (stream) {
			stream->quit_request = true;
		}
	}
}
void NetworkStreamDownloader::delete_all() {
	for (auto &stream : streams) {
		delete stream;
		stream = NULL;
	}
}

// --------------------------------
// other functions implementation
// --------------------------------

void network_downloader_thread(void *downloader_) {
	NetworkStreamDownloader *downloader = (NetworkStreamDownloader *)downloader_;

	downloader->downloader_thread();

	threadExit(0);
}
