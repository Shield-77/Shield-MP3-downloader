#include "headers.hpp"
#include "network_io.hpp"
#include <deque>

#include <fcntl.h>

#include <sys/types.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static volatile bool exiting = false;

static std::vector<NetworkSessionList *> deinit_list;
static Mutex deinit_list_lock;

void NetworkSessionList::init() {
	// Different worker threads initialize their own lists concurrently.
	deinit_list_lock.lock();
	if (!inited) {
		deinit_list.push_back(this);
		inited = true;
	}
	deinit_list_lock.unlock();
}
void NetworkSessionList::deinit() {
	inited = false;

	// curl cleanup
	if (curl_multi) {
		curl_multi_cleanup(curl_multi);
		curl_multi = NULL;
	}
}
void NetworkSessionList::exit_request() { exiting = true; }
void NetworkSessionList::at_exit() {
	deinit_list_lock.lock();
	for (auto session_list : deinit_list) {
		session_list->deinit();
	}
	deinit_list.clear();
	deinit_list_lock.unlock();
}

static std::string remove_leading_whitespaces(std::string str) {
	size_t i = 0;
	while (i < str.size() && str[i] == ' ') {
		i++;
	}
	return str.substr(i, str.size() - i);
}

// libcurl callback functions
static size_t curl_receive_data_callback_func(char *in_ptr, size_t size, size_t count, void *user_data) {
	NetworkResult *out = (NetworkResult *)user_data;
	if (size && count > SIZE_MAX / size) {
		return 0;
	}
	const size_t len = size * count;
	if (out->response_size_limit &&
	    (out->data.size() > out->response_size_limit || len > out->response_size_limit - out->data.size())) {
		out->fail = true;
		out->error = "Response exceeds requested media range";
		return 0;
	}
	out->data.insert(out->data.end(), in_ptr, in_ptr + len);

	// Util_log_save("curl", "received : " + std::to_string(len));
	return len;
}
static size_t curl_receive_headers_callback_func(char *in_ptr, size_t, size_t len, void *user_data) {
	std::map<std::string, std::string> *out = (std::map<std::string, std::string> *)user_data;

	std::string cur_line = std::string(in_ptr, in_ptr + len);
	if (cur_line.size() && cur_line.back() == '\n') {
		cur_line.pop_back();
	}
	if (cur_line.size() && cur_line.back() == '\r') {
		cur_line.pop_back();
	}
	auto colon = std::find(cur_line.begin(), cur_line.end(), ':');
	if (colon == cur_line.end()) {
		// Util_log_save("curl", "unknown header line : " + cur_line);
	} else {
		std::string header_name = remove_leading_whitespaces(std::string(cur_line.begin(), colon));
		std::string header_content = remove_leading_whitespaces(std::string(colon + 1, cur_line.end()));
		// Util_log_save("curl", "header line : " + header_name + " : " + header_content);
		for (auto &c : header_name) {
			c = tolower(c);
		}
		(*out)[header_name] = header_content;
	}
	return len;
}
static int curl_set_socket_options(void *, curl_socket_t sockfd, curlsocktype purpose) {
	static const int SOCKET_BUFFER_MAX_SIZE = 0x8000;

	// expand socket buffer size
	setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &SOCKET_BUFFER_MAX_SIZE, sizeof(int));

	return CURL_SOCKOPT_OK;
}
static int curl_progress_callback_func(void *data, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                                       curl_off_t ulnow) {
	const HttpRequest *request = (const HttpRequest *)data;
	if (exiting || (request->cancel_func && request->cancel_func())) {
		return 1;
	}
	if (request->progress_func) {
		request->progress_func(dlnow, dltotal);
	}
	return 0;
}
static int curl_debug_callback_func(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr) {
	std::string prefix;
	if (type == CURLINFO_HEADER_OUT) {
		prefix = "h>";
	}
	if (type == CURLINFO_HEADER_IN) {
		prefix = "h<";
	}
	if (type == CURLINFO_DATA_OUT) {
		prefix = "d>";
	}
	if (type == CURLINFO_SSL_DATA_OUT) {
		prefix = "D>";
	}
	if (type == CURLINFO_DATA_IN) {
		prefix = "d<";
	}
	if (type == CURLINFO_SSL_DATA_IN) {
		prefix = "D<";
	}
	logger.info("curl", prefix + std::string(data, data + size));
	return 0;
}

void NetworkSessionList::curl_add_request(const HttpRequest &request, NetworkResult *res) {
	if (!curl_multi) {
		curl_multi = curl_multi_init();
		curl_multi_setopt(curl_multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
	}
	CURL *curl;
	curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 102400L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "br");
	curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_receive_data_callback_func);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_receive_headers_callback_func);
	curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, curl_set_socket_options);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &request);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_callback_func);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, (long)0);
	// curl_easy_setopt(curl, CURLOPT_VERBOSE, (long) 1);
	// curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_debug_callback_func);
	char *curl_errbuf = (char *)malloc(CURL_ERROR_SIZE);
	memset(curl_errbuf, 0, CURL_ERROR_SIZE);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_errbuf);
	curl_easy_setopt(curl, CURLOPT_PIPEWAIT, 1L);
	// Fail stalled connections so the media downloader can use its bounded
	// retry path instead of waiting indefinitely after Wi-Fi disappears.
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 64L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);

	res->response_size_limit = request.response_size_limit;
	if (request.response_size_limit) {
		// Range sizes are known. One allocation avoids repeatedly growing and
		// copying the receive vector on the memory-constrained Old 3DS.
		res->data.reserve(std::min<size_t>(request.response_size_limit, 512 * 1024));
	}
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, res);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &res->response_headers);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, (long)request.follow_redirect);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
	curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());

	struct curl_slist *request_headers_list = NULL;
	for (auto i : request.headers) {
		request_headers_list = curl_slist_append(request_headers_list, (i.first + ": " + i.second).c_str());
	}
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, request_headers_list);

	curl_multi_add_handle(curl_multi, curl);
	curl_requests.push_back({curl, res, curl_errbuf, request_headers_list, request.url, request.on_finish});
}
CURLMcode NetworkSessionList::curl_perform_requests() {
	auto read_multi_info = [this]() {
		CURLMsg *msg;
		int msg_left;
		while ((msg = curl_multi_info_read(curl_multi, &msg_left))) {
			if (msg->msg == CURLMSG_DONE) {
				CURL *curl = msg->easy_handle;

				int request_index = -1;
				for (size_t i = 0; i < curl_requests.size(); i++) {
					if (curl_requests[i].curl == msg->easy_handle) {
						request_index = i;
						break;
					}
				}
				// Util_log_save("bench", "finished #" + std::to_string(request_index));

				if (request_index == -1) {
					logger.error("curl", "unexpected : while processing multi message corresponding request not found");
					continue;
				}
				RequestInternal &req = curl_requests[request_index];
				NetworkResult &res = *req.res;

				CURLcode each_result = msg->data.result;
				if (each_result == CURLE_OK) {
					long status_code;
					curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
					res.status_code = status_code;

					char *redirected_url;
					curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &redirected_url);
					res.redirected_url = redirected_url;
					if (res.redirected_url != req.orig_url) {
						logger.info("curl", "redir : " + res.redirected_url);
					}
				} else {
					logger.error("curl",
					             std::string("deep fail : ") + curl_easy_strerror(each_result) + " / " + req.errbuf);
					res.fail = true;
					if (res.error.empty()) {
						res.error = req.errbuf[0] ? req.errbuf : curl_easy_strerror(each_result);
					}
				}
				if (req.on_finish) {
					req.on_finish(res, request_index);
				}
			}
		}
	};

	int running_request_num;
	do {
		CURLMcode res = curl_multi_perform(curl_multi, &running_request_num);
		if (res) {
			std::string err = curl_multi_strerror(res);
			logger.error("curl", "curl multi deep fail : " + err);
			for (auto &i : curl_requests) {
				i.res->fail = true;
				i.res->error = err;
			}
			return res;
		}
		if (running_request_num) {
			curl_multi_poll(curl_multi, NULL, 0, 250, NULL);
		}
		if (exiting) {
			for (auto &i : curl_requests) {
				i.res->fail = true;
				i.res->error = "The app is exiting";
			}
			return CURLM_OK;
		}
		read_multi_info();
	} while (running_request_num > 0);

	return CURLM_OK;
}
void NetworkSessionList::curl_clear_requests() {
	for (auto &i : curl_requests) {
		free(i.errbuf);
		curl_multi_remove_handle(curl_multi, i.curl);
		curl_easy_cleanup(i.curl);
		curl_slist_free_all(i.request_headers);
	}
	curl_requests.clear();
}

NetworkResult NetworkSessionList::perform(const HttpRequest &request) {
	NetworkResult result;

	if (!this->inited) {
		result.fail = true;
		result.error = "invalid session list";
		return result;
	}

	this->curl_add_request(request, &result);
	this->curl_perform_requests();
	this->curl_clear_requests();
	return result;
}
std::vector<NetworkResult> NetworkSessionList::perform(const std::vector<HttpRequest> &requests) {
	std::vector<NetworkResult> results(requests.size());

	if (!this->inited) {
		for (auto &result : results) {
			result.fail = true;
			result.error = "invalid session list";
		}
		return results;
	}

	for (size_t i = 0; i < requests.size(); i++) {
		this->curl_add_request(requests[i], &results[i]);
	}
	this->curl_perform_requests();
	this->curl_clear_requests();
	return results;
}

std::string NetworkResult::get_header(std::string key) {
	for (auto &c : key) {
		c = tolower(c);
	}
	return response_headers.count(key) ? response_headers[key] : "";
}

static bool exclusive_state_entered = false;
void lock_network_state() {
	int res = 0;
	if (!exclusive_state_entered) {
		res = ndmuInit();
		if (res != 0) {
			logger.error("init", "ndmuInit(): " + std::to_string(res));
		}
		res = NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
		if (R_SUCCEEDED(res)) {
			res = NDMU_LockState(); // prevents ndm from switching to StreetPass when the lid is closed
		}
		exclusive_state_entered = R_SUCCEEDED(res);
	}
}
void unlock_network_state() {
	int res = 0;
	if (exclusive_state_entered) {
		res = NDMU_UnlockState();
		if (R_SUCCEEDED(res)) {
			res = NDMU_LeaveExclusiveState();
		}
		ndmuExit();
		exclusive_state_entered = R_FAILED(res);
	}
}
