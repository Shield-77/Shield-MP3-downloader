#include "download_queue.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>

namespace {
const char *const PATH = "sdmc:/3ds/MP3Downloader/download-queue-v1.tsv";
const size_t MAX_ENTRIES = 128;
std::vector<DownloadQueueEntry> entries;
DownloadQueueEntry last_removed;
size_t last_removed_index = 0;
bool undo_available = false;
bool storage_error = false;

bool valid_id(const std::string &id) {
	if (id.size() != 11) return false;
	for (unsigned char c : id)
		if (!std::isalnum(c) && c != '-' && c != '_') return false;
	return true;
}

std::string clean_field(const std::string &value, size_t max_bytes) {
	std::string out;
	for (size_t i = 0; i < value.size();) {
		const unsigned char c = (unsigned char)value[i];
		const size_t bytes = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
		if (!bytes || i + bytes > value.size() || out.size() + bytes > max_bytes) break;
		bool valid = true;
		for (size_t j = 1; j < bytes; ++j)
			if (((unsigned char)value[i + j] & 0xC0) != 0x80) valid = false;
		if (!valid) break;
		out += bytes == 1 && (c == '\t' || c == '\r' || c == '\n' || c < 0x20)
		    ? std::string(" ") : value.substr(i, bytes);
		i += bytes;
	}
	return out;
}

bool read_file(const std::string &path, std::vector<DownloadQueueEntry> &out) {
	FILE *file = fopen(path.c_str(), "rb");
	if (!file) return false;
	char line[384];
	bool ok = fgets(line, sizeof(line), file) && std::string(line) == "SHIELD_QUEUE_1\n";
	while (ok && fgets(line, sizeof(line), file)) {
		std::string record(line);
		if (record.empty() || record.back() != '\n' || out.size() >= MAX_ENTRIES) { ok = false; break; }
		record.pop_back();
		size_t first = record.find('\t');
		size_t second = record.find('\t', first == std::string::npos ? first : first + 1);
		size_t third = record.find('\t', second == std::string::npos ? second : second + 1);
		if (first != 11 || second == std::string::npos || third == std::string::npos || second != first + 2) {
			ok = false; break;
		}
		DownloadQueueEntry entry;
		entry.id = record.substr(0, first);
		if (!valid_id(entry.id)) { ok = false; break; }
		const char status = record[first + 1];
		entry.status = status == 'B' ? DownloadQueueStatus::BLOCKED :
		               status == 'F' ? DownloadQueueStatus::FAILED :
		               status == 'D' ? DownloadQueueStatus::DONE : DownloadQueueStatus::PENDING;
		entry.title = record.substr(second + 1, third - second - 1);
		entry.reason = record.substr(third + 1);
		if (entry.title.size() > 160 || entry.reason.size() > 160) { ok = false; break; }
		for (const auto &existing : out) if (existing.id == entry.id) ok = false;
		if (!ok) break;
		out.push_back(entry);
	}
	if (ferror(file)) ok = false;
	fclose(file);
	if (!ok) out.clear();
	return ok;
}

bool save() {
	if (storage_error) return false;
	if (mkdir("sdmc:/3ds", 0777) != 0 && errno != EEXIST) return false;
	if (mkdir("sdmc:/3ds/MP3Downloader", 0777) != 0 && errno != EEXIST) return false;
	const std::string temp = std::string(PATH) + ".tmp";
	const std::string backup = std::string(PATH) + ".bak";
	FILE *file = fopen(temp.c_str(), "wb");
	if (!file) return false;
	bool ok = fputs("SHIELD_QUEUE_1\n", file) >= 0;
	for (const auto &entry : entries) {
		const char status = entry.status == DownloadQueueStatus::BLOCKED ? 'B' :
		                    entry.status == DownloadQueueStatus::FAILED ? 'F' :
		                    entry.status == DownloadQueueStatus::DONE ? 'D' : 'P';
		if (fprintf(file, "%s\t%c\t%s\t%s\n", entry.id.c_str(), status,
		            clean_field(entry.title, 160).c_str(), clean_field(entry.reason, 160).c_str()) < 0) ok = false;
	}
	if (fflush(file) != 0) ok = false;
	if (fclose(file) != 0) ok = false;
	if (!ok) { std::remove(temp.c_str()); return false; }
	struct stat old_stat;
	const bool had_old = stat(PATH, &old_stat) == 0;
	if (had_old) {
		std::remove(backup.c_str());
		if (rename(PATH, backup.c_str()) != 0) { std::remove(temp.c_str()); return false; }
	}
	if (rename(temp.c_str(), PATH) != 0) {
		if (had_old) rename(backup.c_str(), PATH);
		return false;
	}
	return true;
}
} // namespace

bool DownloadQueue_load() {
	std::vector<DownloadQueueEntry> loaded;
	if (read_file(PATH, loaded)) {
		entries.swap(loaded);
		undo_available = false;
		storage_error = false;
		return true;
	}
	const std::string backup = std::string(PATH) + ".bak";
	if (read_file(backup, loaded)) {
		// Restore the valid backup before any future save can rotate it away.
		struct stat stat_main;
		if ((stat(PATH, &stat_main) == 0 && std::remove(PATH) != 0) ||
		    rename(backup.c_str(), PATH) != 0) {
			storage_error = true;
			return false;
		}
		entries.swap(loaded);
		undo_available = false;
		storage_error = false;
		return true;
	}
	struct stat stat_file;
	storage_error = stat(PATH, &stat_file) == 0 || stat(backup.c_str(), &stat_file) == 0;
	if (!storage_error) entries.clear();
	return false;
}

const std::vector<DownloadQueueEntry> &DownloadQueue_entries() { return entries; }

bool DownloadQueue_add(const std::string &id, const std::string &title) {
	if (!valid_id(id) || entries.size() >= MAX_ENTRIES) return false;
	for (const auto &entry : entries) if (entry.id == id) return false;
	std::string safe_title = clean_field(title, 160);
	if (safe_title.empty()) safe_title = id;
	entries.push_back({id, safe_title, DownloadQueueStatus::PENDING, ""});
	if (!save()) { entries.pop_back(); return false; }
	if (undo_available && last_removed.id == id) undo_available = false;
	return true;
}

bool DownloadQueue_remove(const std::string &id) {
	for (size_t i = 0; i < entries.size(); ++i) {
		if (entries[i].id != id || entries[i].status == DownloadQueueStatus::DOWNLOADING ||
		    entries[i].status == DownloadQueueStatus::PREPARING) continue;
		DownloadQueueEntry removed = entries[i];
		entries.erase(entries.begin() + i);
		if (!save()) { entries.insert(entries.begin() + i, removed); return false; }
		last_removed = removed;
		last_removed_index = i;
		undo_available = true;
		return true;
	}
	return false;
}

bool DownloadQueue_undo_remove() {
	if (!undo_available || entries.size() >= MAX_ENTRIES) return false;
	for (const auto &entry : entries) if (entry.id == last_removed.id) return false;
	const size_t index = std::min(last_removed_index, entries.size());
	entries.insert(entries.begin() + index, last_removed);
	if (!save()) { entries.erase(entries.begin() + index); return false; }
	undo_available = false;
	return true;
}

bool DownloadQueue_set_status(const std::string &id, DownloadQueueStatus status, const std::string &reason) {
	for (auto &entry : entries) {
		if (entry.id != id) continue;
		DownloadQueueEntry old = entry;
		entry.status = status;
		entry.reason = clean_field(reason, 160);
		if (!save()) { entry = old; return false; }
		return true;
	}
	return false;
}

bool DownloadQueue_has_undo() { return undo_available; }
bool DownloadQueue_storage_error() { return storage_error; }
