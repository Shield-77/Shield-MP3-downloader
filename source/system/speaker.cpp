#include "headers.hpp"

#define BUFFER_SIZE 180

ndspWaveBuf util_ndsp_buffer[24][BUFFER_SIZE];
double util_ndsp_buffer_timestamp[24][BUFFER_SIZE]; // {pts, sample rate}
static size_t util_ndsp_buffer_capacity[24][BUFFER_SIZE];

void Util_speaker_init(int play_ch, int music_ch, int sample_rate) {
	float mix[12] = {
	    1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	};

	ndspChnReset(play_ch);
	ndspChnWaveBufClear(play_ch);
	ndspChnSetMix(play_ch, mix);
	if (music_ch == 2) {
		ndspChnSetFormat(play_ch, NDSP_FORMAT_STEREO_PCM16);
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	} else {
		ndspChnSetFormat(play_ch, NDSP_FORMAT_MONO_PCM16);
		ndspSetOutputMode(NDSP_OUTPUT_MONO);
	}

	ndspChnSetInterp(play_ch, NDSP_INTERP_LINEAR);
	ndspChnSetRate(play_ch, sample_rate);
	memset(util_ndsp_buffer[play_ch], 0, sizeof(util_ndsp_buffer[play_ch]));
	for (int i = 0; i < BUFFER_SIZE; i++) {
		util_ndsp_buffer[play_ch][i].data_vaddr = NULL;
	}
}

Result_with_string Util_speaker_add_buffer(int play_ch, int music_ch, u8 *buffer, int size, double pts) {
	Result_with_string result;
	if (size <= 0 || music_ch <= 0 || play_ch < 0 || play_ch >= 24 || buffer == NULL) {
		result.code = DEF_ERR_OTHER;
		result.string = "[Error] Invalid audio buffer dimensions ";
		return result;
	}
	int free_queue = -1;

	for (int i = 0; i < BUFFER_SIZE; i++) {
		if (util_ndsp_buffer[play_ch][i].status == NDSP_WBUF_FREE ||
		    util_ndsp_buffer[play_ch][i].status == NDSP_WBUF_DONE) {
			free_queue = i;
			break;
		}
	}

	if (free_queue == -1) {
		result.code = DEF_ERR_NEED_MORE_OUTPUT;
		result.string = "[Error] Queues are full ";
		return result;
	}

	const size_t required_size = (size_t)size * (size_t)music_ch;
	if (required_size / (size_t)music_ch != (size_t)size) {
		result.code = DEF_ERR_OTHER;
		result.string = "[Error] Audio buffer size overflow ";
		return result;
	}
	if (required_size > util_ndsp_buffer_capacity[play_ch][free_queue]) {
		// Reuse each completed NDSP slot's linear-memory allocation. Audio frame
		// sizes are usually stable, so this avoids allocating/freeing linear RAM
		// for every packet and reduces allocator churn during long playback.
		linearFree_concurrent((void *)util_ndsp_buffer[play_ch][free_queue].data_vaddr);
		util_ndsp_buffer[play_ch][free_queue].data_vaddr = NULL;
		util_ndsp_buffer_capacity[play_ch][free_queue] = 0;
		util_ndsp_buffer[play_ch][free_queue].data_vaddr = (u8 *)linearAlloc_concurrent(required_size);
		if (util_ndsp_buffer[play_ch][free_queue].data_vaddr == NULL) {
			result.code = DEF_ERR_OUT_OF_LINEAR_MEMORY;
			result.string = DEF_ERR_OUT_OF_LINEAR_MEMORY_STR;
			return result;
		}
		util_ndsp_buffer_capacity[play_ch][free_queue] = required_size;
	}

	memcpy((void *)util_ndsp_buffer[play_ch][free_queue].data_vaddr, buffer, required_size);
	// The DSP reads linear RAM directly, so make the CPU's newly written samples
	// visible before publishing the wave buffer (as in libctru's audio examples).
	const Result flush_result = DSP_FlushDataCache(util_ndsp_buffer[play_ch][free_queue].data_vaddr, required_size);
	if (R_FAILED(flush_result)) {
		result.code = flush_result;
		result.string = "[Error] Audio cache flush failed ";
		return result;
	}

	util_ndsp_buffer[play_ch][free_queue].nsamples = size / 2;
	// Publish the timestamp before the DSP can start playing this slot.
	util_ndsp_buffer_timestamp[play_ch][free_queue] = pts;
	ndspChnWaveBufAdd(play_ch, &util_ndsp_buffer[play_ch][free_queue]);
	return result;
}

double Util_speaker_get_current_timestamp(int play_ch, int sample_rate) {
	if (play_ch < 0 || play_ch >= 24 || sample_rate <= 0) {
		return -1;
	}
	if (!Util_speaker_is_playing(play_ch)) {
		return -1;
	}
	double queued_min = INFINITY;
	for (int i = 0; i < BUFFER_SIZE; i++) {
		if (util_ndsp_buffer[play_ch][i].status == NDSP_WBUF_PLAYING) {
			return util_ndsp_buffer_timestamp[play_ch][i] + (double)ndspChnGetSamplePos(play_ch) / sample_rate;
		}
		if (util_ndsp_buffer[play_ch][i].status == NDSP_WBUF_QUEUED) {
			queued_min = std::min(queued_min, util_ndsp_buffer_timestamp[play_ch][i]);
		}
	}
	if (queued_min != INFINITY) {
		return queued_min;
	}
	// weired...
	if (!Util_speaker_is_playing(play_ch)) {
		return -1;
	}
	// really weired...
	logger.error("speaker", "unexpected behavior in get_current_timestamp");
	return -1;
}

double Util_speaker_get_buffered_duration(int play_ch, int sample_rate) {
	if (play_ch < 0 || play_ch >= 24 || sample_rate <= 0) {
		return 0.0;
	}

	u64 remaining_samples = 0;
	for (int i = 0; i < BUFFER_SIZE; i++) {
		const ndspWaveBuf &wave_buffer = util_ndsp_buffer[play_ch][i];
		if (wave_buffer.status == NDSP_WBUF_QUEUED) {
			remaining_samples += wave_buffer.nsamples;
		} else if (wave_buffer.status == NDSP_WBUF_PLAYING) {
			const u32 played_samples = ndspChnGetSamplePos(play_ch);
			remaining_samples += wave_buffer.nsamples - std::min<u32>(played_samples, wave_buffer.nsamples);
		}
	}

	return (double)remaining_samples / sample_rate;
}

void Util_speaker_clear_buffer(int play_ch) {
	ndspChnWaveBufClear(play_ch);
	while (Util_speaker_is_playing(play_ch)) {
		usleep(10000);
	}
	for (int i = 0; i < BUFFER_SIZE; i++) {
		util_ndsp_buffer[play_ch][i].status = NDSP_WBUF_FREE;
		util_ndsp_buffer_timestamp[play_ch][i] = 0.0;
	}
}

void Util_speaker_pause(int play_ch) { ndspChnSetPaused(play_ch, true); }

void Util_speaker_resume(int play_ch) { ndspChnSetPaused(play_ch, false); }

bool Util_speaker_is_paused(int play_ch) { return ndspChnIsPaused(play_ch); }

bool Util_speaker_is_playing(int play_ch) { return ndspChnIsPlaying(play_ch); }

void Util_speaker_exit(int play_ch) {
	ndspChnWaveBufClear(play_ch);
	ndspChnSetPaused(play_ch, false);
	for (int i = 0; i < BUFFER_SIZE; i++) {
		linearFree_concurrent((void *)util_ndsp_buffer[play_ch][i].data_vaddr);
		util_ndsp_buffer[play_ch][i].data_vaddr = NULL;
		util_ndsp_buffer_capacity[play_ch][i] = 0;
	}
}

bool Util_speaker_is_buffer_empty(int play_ch) {
	for (int i = 0; i < BUFFER_SIZE; i++) {
		if (util_ndsp_buffer[play_ch][i].status == NDSP_WBUF_QUEUED ||
		    util_ndsp_buffer[play_ch][i].status == NDSP_WBUF_PLAYING) {
			return false;
		}
	}
	return true;
}
