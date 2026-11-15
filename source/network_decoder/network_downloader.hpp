#pragma once
#include <vector>
#include <map>
#include <string>
#include <3ds.h>
#include "system/libctru_wrapper.hpp"
#include "network_io.hpp"

// one instance per one url (once constructed, the url is not changeable)
struct NetworkStream {
	// Larger ranges reduce request/round-trip overhead on the High RAM build.
	// Keep the standard Old 3DS build at 256 KiB to limit its per-request memory.
#ifdef FOURTHTUBE_HIGH_RAM_BUILD
	static constexpr u64 BLOCK_SIZE = 0x80000; // 512 KiB
#else
	static constexpr u64 BLOCK_SIZE = 0x40000; // 256 KiB
#endif
#ifdef FOURTHTUBE_HIGH_RAM_BUILD
	// Bound cache memory while buffering longer Wi-Fi stalls.
	static constexpr u64 OLD3DS_MAX_CACHE_BYTES = 8 * 1024 * 1024;
#else
	static constexpr u64 OLD3DS_MAX_CACHE_BYTES = 2 * 1000 * 1000;
#endif
	static constexpr u64 OLD3DS_MAX_CACHE_BLOCKS = OLD3DS_MAX_CACHE_BYTES / BLOCK_SIZE;
	// A couple of bounded retries help recover from brief Wi-Fi stalls without
	// letting a broken stream retry forever.
	static constexpr int RETRY_CNT_MAX = 2;
	static u64 get_block_num(u64 size) { return (size + BLOCK_SIZE - 1) / BLOCK_SIZE; }

	std::string url;
	Mutex downloaded_data_lock; // std::map needs locking when searching and inserting at the same time
	u64 len = 0;
	u64 block_num = 0;
	std::map<u64, std::vector<u8>> downloaded_data;
	bool whole_download = false;
	NetworkSessionList *session_list = NULL;

	// anything above here is not supposed to be used from outside network_downloader.cpp and network_downloader.hpp
	volatile bool ready = false;
	volatile bool suspend_request = false;
	volatile bool quit_request = false;
	volatile bool error = false;
	volatile int retry_cnt_left = RETRY_CNT_MAX;
	u64 read_head = 0; // Accessed under downloaded_data_lock (u64 isn't atomic on Old 3DS).
	const char *volatile network_waiting_status = NULL;
	bool disable_interrupt = false;
	// used for livestreams
	int seq_head = -1;
	int seq_id = -1;
	bool livestream_eof = false;
	bool livestream_private = false;
	bool read_dead_tried = false;

	// if `whole_download` is true, it will not use Range request but download the whole content at once (used for
	// livestreams)
	NetworkStream(std::string url, int64_t len, bool whole_download, NetworkSessionList *session_list)
	    : url(url), len(len < 0 ? 0 : len), block_num(get_block_num(this->len)), whole_download(whole_download),
	      session_list(session_list) {}

	double get_download_percentage();
	std::vector<double> get_buffering_progress_bar(int res_len);

	// check if the data of the current stream of range [start, start + size) is already downloaded and available
	bool is_data_available(u64 start, u64 size);
	bool has_block(u64 block);
	size_t read_next_data(u8 *destination, size_t size, bool &eof);
	u64 get_read_head();
	void set_read_head(u64 position);

	// this function must only be called when is_data_available(start, size) returns true
	// copies the stream range directly into the caller's buffer to avoid a
	// temporary vector allocation on every FFmpeg read.
	size_t read_data(u64 start, u8 *destination, size_t size);

	// this function is supposed to be called from NetworkStreamDownloader::*
	void set_data(u64 block, std::vector<u8> data);
};

// each instance of this class is paired with one downloader thread
// it owns NetworkStream instances, and the one with the least margin (as in proportion to the length of the entire
// stream) is the target of next downloading
class NetworkStreamDownloader {
  private:
	static constexpr u64 BLOCK_SIZE = NetworkStream::BLOCK_SIZE;
	static constexpr const char *USER_AGENT = "Mozilla/5.0 (Linux; Android 11; Pixel 3a) AppleWebKit/537.36 (KHTML, "
	                                          "like Gecko) Chrome/83.0.4103.101 Mobile Safari/537.36";

	Mutex streams_lock;
	std::vector<NetworkStream *> streams;

	bool thread_exit_requested = false;

  public:
	NetworkStreamDownloader() = default;

	// the pointer must be one that has been new-ed : it will be deleted once quit_request is made
	void add_stream(NetworkStream *stream);

	void request_thread_exit() { thread_exit_requested = true; }
	void delete_all();

	void downloader_thread();
};
// 'arg' should be a pointer to an instance of NetworkStreamDownloader
void network_downloader_thread(void *arg);
