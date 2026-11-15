#pragma once

#include <cstdint>
#include <string>

enum class AudioExportState {
	IDLE,
	SELECT_BITRATE,
	SELECT_ARTWORK,
	CONFIRM,
	WAITING_FOR_PLAYER,
	DOWNLOADING,
	FETCHING_ART,
	ENCODING,
	DONE,
	FAILED,
	CANCELLED,
};

void AudioExport_init();
void AudioExport_exit();
void AudioExport_begin(const std::string &url, const std::string &title, const std::string &video_id,
                       int64_t duration_ms, long long expected_size);
void AudioExport_choose_bitrate(int bitrate_kbps);
void AudioExport_choose_artwork(bool include_artwork);
void AudioExport_confirm(bool wait_for_player_stop);
void AudioExport_notify_player_stopped();
void AudioExport_cancel_prompt();
void AudioExport_cancel_operation();
void AudioExport_dismiss();
AudioExportState AudioExport_get_state();
int AudioExport_get_progress();
int AudioExport_get_bitrate();
void AudioExport_draw_overlay();
