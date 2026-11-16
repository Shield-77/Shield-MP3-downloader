#pragma once

#include <string>
#include <vector>

enum class DownloadQueueStatus { PENDING, PREPARING, DOWNLOADING, BLOCKED, FAILED, DONE };

struct DownloadQueueEntry {
	std::string id;
	std::string title;
	DownloadQueueStatus status = DownloadQueueStatus::PENDING;
	std::string reason;
};

// Main-thread API. No media files are touched by these operations.
bool DownloadQueue_load();
const std::vector<DownloadQueueEntry> &DownloadQueue_entries();
bool DownloadQueue_add(const std::string &id, const std::string &title);
bool DownloadQueue_remove(const std::string &id);
// Removes only a completed entry, without adding it to the manual Undo slot.
bool DownloadQueue_remove_completed(const std::string &id);
bool DownloadQueue_undo_remove();
bool DownloadQueue_set_status(const std::string &id, DownloadQueueStatus status, const std::string &reason = "");
bool DownloadQueue_has_undo();
bool DownloadQueue_storage_error();
