#include "headers.hpp"
#include <vector>
#include <string>
#include <set>
#include <map>
#include <numeric>

#include "scenes/search.hpp"
#include "scenes/video_player.hpp"
#include "youtube_parser/parser.hpp"
#include "ui/overlay.hpp"
#include "ui/colors.hpp"
#include "ui/ui.hpp"
#include "network_decoder/thumbnail_loader.hpp"
#include "network_decoder/network_io.hpp"
#include "util/async_task.hpp"
#include "system/download_queue.hpp"
#include "system/audio_exporter.hpp"

#define SEARCH_BOX_MARGIN 4

#define URL_BUTTON_WIDTH 60
#define QUEUE_BUTTON_WIDTH 52

#define MAX_THUMBNAIL_LOAD_REQUEST 12

namespace Search {
bool thread_suspend = false;
bool already_init = false;
bool exiting = false;

Mutex resource_lock;
std::string cur_search_word = "";

static size_t get_next_utf8_char_pos(const std::string &text, size_t pos) {
	if (pos >= text.length()) {
		return text.length();
	}

	unsigned char c = text[pos];
	if ((c & 0x80) == 0) {
		return pos + 1;
	} else if ((c & 0xE0) == 0xC0) {
		return pos + 2;
	} else if ((c & 0xF0) == 0xE0) {
		return pos + 3;
	} else if ((c & 0xF8) == 0xF0) {
		return pos + 4;
	}
	return pos + 1;
}

std::string truncate_search_text_for_display(const std::string &text, double max_width, double font_size) {
	float text_width = Draw_get_width(text, font_size);

	if (text_width <= max_width - SMALL_MARGIN * 2) {
		return text;
	}

	std::string ellipsis = "...";
	float ellipsis_width = Draw_get_width(ellipsis, font_size);
	float available_width = max_width - SMALL_MARGIN * 2 - ellipsis_width;

	size_t pos = 0;
	size_t last_valid_pos = 0;

	while (pos < text.length()) {
		size_t next_pos = get_next_utf8_char_pos(text, pos);
		if (next_pos > text.length()) {
			break;
		}

		std::string substr = text.substr(0, next_pos);
		float substr_width = Draw_get_width(substr, font_size);

		if (substr_width <= available_width) {
			last_valid_pos = next_pos;
			pos = next_pos;
		} else {
			break;
		}
	}

	if (last_valid_pos > 0 && last_valid_pos < text.length()) {
		return text.substr(0, last_valid_pos) + ellipsis;
	}

	return text;
}
YouTubeSearchResult search_result;
bool search_done = false;

std::string last_url_input = "https://m.youtube.com/watch?v=";

bool clicked_is_channel;
std::string clicked_url;
bool queue_open = false;
size_t selected_result = 0;
std::string pending_queue_remove;
std::string pending_queue_retry;
bool pending_queue_undo = false;
int pending_queue_start_bitrate = 0;
bool pending_queue_toggle_cover = false;
// A cover costs a separate network request for every item. Batch mode defaults
// to audio only; the user can enable covers in the queue header.
bool queue_include_cover = false;
bool queue_processing = false;
bool queue_stop_requested = false;
std::string queue_stop_reason;
std::string queue_current_id;
enum class QueuePhase { IDLE, LOOKUP, EXPORT };
QueuePhase queue_phase = QueuePhase::IDLE;
int queue_bitrate = 128;
size_t queue_batch_total = 0;
YouTubeVideoDetail queue_lookup_detail;
bool queue_lookup_ready = false;
std::set<std::string> queue_attempted;

TextView *toast_view = (new TextView((320 - 150) / 2, 190, 150, DEFAULT_FONT_INTERVAL + SMALL_MARGIN));
int toast_view_visible_frames_left = 0;

const int RESULT_Y_LOW = 25;
int RESULT_Y_HIGH = 240; // changes according to whether the video playing bar is drawn or not

VerticalListView *main_view;
VerticalListView *top_bar_view = new VerticalListView(0, 0, 320);
// annonymous HorizontalListView
TextView *search_box_view;
TextView *url_button_view;
TextView *queue_button_view;
// annonymous RuleView : height 1
ScrollView *result_view;
ScrollView *queue_scroll_view;
VerticalListView *queue_list_view;
VerticalListView *result_list_view =
    (new VerticalListView(0, 0, 320))
        ->set_margin(SMALL_MARGIN)
        ->enable_thumbnail_request_update(MAX_THUMBNAIL_LOAD_REQUEST, SceneType::SEARCH);
View *result_bottom_view = new EmptyView(0, 0, 320, 0);
}; // namespace Search
using namespace Search;

TextView *Search_get_toast_view() { return toast_view; }
View *Search_get_search_bar_view() { return top_bar_view; }

static void load_search_results(void *);
static void load_more_search_results(void *);
static void refresh_queue_view();
static void show_search_toast(const std::string &message);
static void load_queue_video(void *);
static void queue_tick();

void Search_init(void) {
	logger.info("search/init", "Initializing...");
	Result_with_string result;
	DownloadQueue_load();
	queue_open = false;
	queue_processing = false;
	queue_stop_requested = false;
	queue_stop_reason.clear();
	queue_phase = QueuePhase::IDLE;
	queue_current_id.clear();
	queue_attempted.clear();

	search_box_view = (new TextView(0, SEARCH_BOX_MARGIN, 320 - SEARCH_BOX_MARGIN * 4 - URL_BUTTON_WIDTH - QUEUE_BUTTON_WIDTH,
	                                RESULT_Y_LOW - SEARCH_BOX_MARGIN * 2 - 1));
	search_box_view->set_text_offset(0, -1);
	search_box_view->set_text((std::function<std::string()>)[]() { return LOCALIZED(SEARCH_HINT); });
	search_box_view->set_get_background_color([](const View &) { return LIGHT0_BACK_COLOR; });
	search_box_view->set_get_text_color([]() { return LIGHT1_TEXT_COLOR; });
	search_box_view->set_on_view_released([](View &) {
		if (Search_show_search_keyboard()) {
			global_intent.next_scene = SceneType::SEARCH;
		}
	});

	url_button_view = (new TextView(0, SEARCH_BOX_MARGIN, URL_BUTTON_WIDTH, RESULT_Y_LOW - SEARCH_BOX_MARGIN * 2 - 1));
	url_button_view->set_text_offset(0, -1);
	url_button_view->set_x_alignment(TextView::XAlign::CENTER);
	url_button_view->set_text((std::function<std::string()>)[]() { return LOCALIZED(URL); });
	url_button_view->set_get_background_color([](const View &) { return LIGHT1_BACK_COLOR; });
	url_button_view->set_on_view_released([](View &) {
		if (Search_show_url_input_keyboard()) {
			global_intent.next_scene = SceneType::SEARCH;
		}
	});
	queue_button_view = (new TextView(0, SEARCH_BOX_MARGIN, QUEUE_BUTTON_WIDTH, RESULT_Y_LOW - SEARCH_BOX_MARGIN * 2 - 1));
	queue_button_view->set_text((std::function<std::string()>)[]() {
		return (var_lang == "fr" ? "File " : "Queue ") + std::to_string(DownloadQueue_entries().size());
	});
	queue_button_view->set_x_alignment(TextView::XAlign::CENTER);
	queue_button_view->set_get_background_color([](const View &) { return LIGHT1_BACK_COLOR; });
	queue_button_view->set_on_view_released([](View &) {
		if (queue_processing) return;
		queue_open = !queue_open;
		if (queue_open) refresh_queue_view();
		var_need_refresh = true;
	});

	toast_view->set_is_visible(false);
	toast_view->set_x_alignment(TextView::XAlign::CENTER);
	toast_view->set_text_offset(0, -1);
	toast_view->set_get_background_color([](const View &) { return 0x50000000; });
	toast_view->set_get_text_color([]() { return (u32)-1; });

	top_bar_view->set_views({(new HorizontalListView(0, 0, RESULT_Y_LOW - 1))
	                             ->set_views({new EmptyView(0, 0, SEARCH_BOX_MARGIN, RESULT_Y_LOW - 1), search_box_view,
	                                          new EmptyView(0, 0, SEARCH_BOX_MARGIN, RESULT_Y_LOW - 1), queue_button_view,
	                                          new EmptyView(0, 0, SEARCH_BOX_MARGIN, RESULT_Y_LOW - 1), url_button_view,
	                                          new EmptyView(0, 0, SEARCH_BOX_MARGIN, RESULT_Y_LOW - 1)})
	                             ->set_get_background_color([](const View &) { return DEFAULT_BACK_COLOR; }),
	                         (new RuleView(0, 0, 320, 1))
	                             ->set_get_color([]() { return 0xFF000000; }) // always black
	                             ->set_margin(0)});

	result_view =
	    (new ScrollView(0, 0, 320, RESULT_Y_HIGH - RESULT_Y_LOW))->set_views({result_list_view, result_bottom_view});
	queue_list_view = new VerticalListView(0, 0, 320);
	queue_scroll_view = (new ScrollView(0, RESULT_Y_LOW, 320, RESULT_Y_HIGH - RESULT_Y_LOW))->set_views({queue_list_view});
	refresh_queue_view();
	main_view = (new VerticalListView(0, 0, 320))->set_views({top_bar_view, result_view})->set_draw_order({1, 0});

	Search_resume("");
	already_init = true;
}

void Search_exit(void) {
	already_init = false;
	thread_suspend = false;
	exiting = true;

	resource_lock.lock();

	main_view->recursive_delete_subviews();
	delete main_view;
	queue_scroll_view->recursive_delete_subviews();
	delete queue_scroll_view;
	main_view = NULL;
	result_view = NULL;
	result_list_view = NULL;
	result_bottom_view = NULL;
	top_bar_view = NULL;
	search_box_view = NULL;
	url_button_view = NULL;
	queue_button_view = NULL;
	queue_scroll_view = NULL;
	queue_list_view = NULL;

	toast_view->recursive_delete_subviews();
	delete toast_view;
	toast_view = NULL;

	resource_lock.unlock();

	logger.info("search/exit", "Exited.");
}

void Search_suspend(void) { thread_suspend = true; }

void Search_resume(std::string arg) {
	(void)arg;
	overlay_menu_on_resume();

	thread_suspend = false;
	var_need_refresh = true;
}

// async functions
static void set_loading_bottom_view() {
	delete result_bottom_view;
	result_bottom_view = (new TextView(0, 0, 320, 30))
	                         ->set_text((std::function<std::string()>)[]() { return LOCALIZED(LOADING); })
	                         ->set_x_alignment(TextView::XAlign::CENTER);
	result_view->views[1] = result_bottom_view;
}
static void update_result_bottom_view() {
	delete result_bottom_view;
	if (search_result.error != "" || search_result.has_more_results()) {
		TextView *cur_view =
		    (new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL * 2))->set_x_alignment(TextView::XAlign::CENTER);
		if (search_result.error != "") {
			cur_view->set_text(search_result.error);
		} else {
			cur_view->set_text((std::function<std::string()>)[]() { return LOCALIZED(LOADING); });
		}
		result_bottom_view = cur_view;
	} else {
		result_bottom_view = new EmptyView(0, 0, 320, 0);
	}
	result_view->views[1] = result_bottom_view;
	result_view->set_on_child_drawn(1, [](const ScrollView &, int) {
		if (search_result.has_more_results() && search_result.error == "") {
			if (!is_async_task_running(load_search_results) && !is_async_task_running(load_more_search_results)) {
				queue_async_task(load_more_search_results, NULL);
			}
		}
	});
}
static void show_search_toast(const std::string &message) {
	toast_view->set_text(message);
	toast_view->set_is_visible(true);
	toast_view_visible_frames_left = 120;
	var_need_refresh = true;
}

static void refresh_queue_view() {
	queue_list_view->recursive_delete_subviews();
	std::vector<View *> views;
	TextView *heading = (new TextView(0, 0, DownloadQueue_has_undo() ? 260 : 320, 28))
	    ->set_text((std::function<std::string()>)[]() {
		return var_lang == "fr" ? "File de telechargement" : "Download queue";
	});
	std::vector<View *> heading_parts = {heading};
	if (DownloadQueue_has_undo()) {
		TextView *undo = (new TextView(0, 0, 60, 28))
		    ->set_text((std::function<std::string()>)[]() { return var_lang == "fr" ? "Annuler" : "Undo"; });
		undo->set_get_background_color([](const View &) { return LIGHT1_BACK_COLOR; });
		undo->set_on_view_released([](View &) { pending_queue_undo = true; });
		heading_parts.push_back(undo);
	}
	views.push_back((new HorizontalListView(0, 0, 28))->set_views(heading_parts));
	TextView *start_128 = (new TextView(0, 0, 92, 28))->set_text("128 kbps");
	TextView *start_256 = (new TextView(0, 0, 92, 28))->set_text("256 kbps");
	TextView *cover = (new TextView(0, 0, 136, 28))->set_text((std::function<std::string()>)[]() {
		return (var_lang == "fr" ? "Pochette: " : "Cover: ") +
		       std::string(queue_include_cover ? "ON" : "OFF");
	});
	for (TextView *button : {start_128, start_256, cover}) {
		button->set_x_alignment(TextView::XAlign::CENTER);
		button->set_get_background_color([](const View &) { return LIGHT1_BACK_COLOR; });
	}
	start_128->set_on_view_released([](View &) { pending_queue_start_bitrate = 128; });
	start_256->set_on_view_released([](View &) { pending_queue_start_bitrate = 256; });
	cover->set_on_view_released([](View &) { pending_queue_toggle_cover = true; });
	views.push_back((new HorizontalListView(0, 0, 28))->set_views({start_128, start_256, cover}));
	views.push_back((new TextView(0, 0, 320, 22))->set_text((std::function<std::string()>)[]() {
		return var_lang == "fr" ? "A/X: demarrer  Y: pochette  B: retour" : "A/X: start  Y: cover  B: back";
	}));
	if (DownloadQueue_storage_error()) {
		views.push_back((new TextView(0, 0, 320, 44))->set_text((std::function<std::string()>)[]() {
			return var_lang == "fr" ? "File illisible : sauvegarde protegee" : "Queue unreadable: save protected";
		}));
	}
	if (DownloadQueue_entries().empty()) {
		views.push_back((new TextView(0, 0, 320, 44))->set_text((std::function<std::string()>)[]() {
			return var_lang == "fr" ? "File vide. X sur une video de recherche." : "Empty. Press X on a search result.";
		}));
	}
	for (const auto &entry : DownloadQueue_entries()) {
		const bool retryable = entry.status == DownloadQueueStatus::BLOCKED ||
		                       entry.status == DownloadQueueStatus::FAILED;
		std::string label = entry.title;
		if (entry.status == DownloadQueueStatus::BLOCKED) label += (var_lang == "fr" ? " [bloquee]" : " [blocked]");
		else if (entry.status == DownloadQueueStatus::FAILED) label += (var_lang == "fr" ? " [echec]" : " [failed]");
		else if (entry.status == DownloadQueueStatus::DONE) label += (var_lang == "fr" ? " [termine]" : " [done]");
		else if (entry.status == DownloadQueueStatus::PREPARING) label += (var_lang == "fr" ? " [controle]" : " [checking]");
		else if (entry.status == DownloadQueueStatus::DOWNLOADING) label += (var_lang == "fr" ? " [en cours]" : " [running]");
		std::string detail = entry.reason.empty()
		    ? (retryable ? (var_lang == "fr" ? "Toucher pour reessayer" : "Tap to retry") : "")
		    : entry.reason + (retryable ? (var_lang == "fr" ? " - toucher pour reessayer" : " - tap to retry") : "");
		TextView *title = (new TextView(0, 0, 296, 42))
		    ->set_text_lines(std::vector<std::string>{truncate_search_text_for_display(label, 292, 0.5),
		                                                truncate_search_text_for_display(detail, 292, 0.42)});
		title->set_font_size(0.42, 18);
		title->set_get_background_color([](const View &) { return LIGHT0_BACK_COLOR; });
		if (retryable) title->set_on_view_released([id = entry.id](View &) { pending_queue_retry = id; });
		TextView *remove = (new TextView(0, 0, 24, 42))->set_text("x");
		remove->set_x_alignment(TextView::XAlign::CENTER);
		remove->set_font_size(0.48, DEFAULT_FONT_INTERVAL);
		remove->set_get_background_color([](const View &) { return LIGHT1_BACK_COLOR; });
		remove->set_on_view_released([id = entry.id](View &) { pending_queue_remove = id; });
		views.push_back((new HorizontalListView(0, 0, 42))->set_views({title, remove}));
	}
	queue_list_view->views = std::move(views);
	var_need_refresh = true;
}

static void load_queue_video(void *) {
	resource_lock.lock();
	const std::string id = queue_current_id;
	resource_lock.unlock();
	if (id.empty()) return;
	YouTubeVideoDetail detail = youtube_load_video_page(youtube_get_video_url_by_id(id), false);
	resource_lock.lock();
	if (!exiting && queue_current_id == id && queue_phase == QueuePhase::LOOKUP) {
		queue_lookup_detail = std::move(detail);
		queue_lookup_ready = true;
		var_need_refresh = true;
	}
	resource_lock.unlock();
}

// Called under resource_lock on the UI thread. The worker only fetches fresh
// metadata; the exporter owns its own worker and accepts one item at a time.
static void queue_tick() {
	if (!queue_processing) return;
	if (queue_phase == QueuePhase::LOOKUP) {
		if (!queue_lookup_ready) return;
		queue_lookup_ready = false;
		bool status_saved = true;
		if (queue_stop_requested) {
			status_saved = DownloadQueue_set_status(queue_current_id, DownloadQueueStatus::PENDING);
			queue_lookup_detail = YouTubeVideoDetail();
			queue_phase = QueuePhase::IDLE;
			queue_processing = false;
			queue_current_id.clear();
			refresh_queue_view();
			return;
		}
		const YouTubeVideoDetail &detail = queue_lookup_detail;
		if (!detail.error.empty()) {
			status_saved = DownloadQueue_set_status(queue_current_id, DownloadQueueStatus::FAILED, detail.error);
			if (detail.error.find("Network request failed") != std::string::npos ||
			    detail.error.find("Empty response data") != std::string::npos) {
				queue_stop_requested = true;
				queue_stop_reason = var_lang == "fr" ? "Connexion perdue; file gardee" : "Connection lost; queue saved";
			}
		} else if (detail.id != queue_current_id) {
			status_saved = DownloadQueue_set_status(queue_current_id, DownloadQueueStatus::FAILED,
			                         "Identifiant video inattendu");
			queue_stop_requested = true;
		} else if (detail.playability_status != "OK") {
			status_saved = DownloadQueue_set_status(queue_current_id, DownloadQueueStatus::BLOCKED,
			                         detail.playability_reason.empty() ? "Source indisponible" : detail.playability_reason);
		} else if (detail.audio_stream_url.empty() || detail.duration_ms <= 0 || detail.duration_ms > 3600000) {
			status_saved = DownloadQueue_set_status(queue_current_id, DownloadQueueStatus::FAILED,
			                         "Source audio ou duree indisponible");
		} else if (AudioExport_get_state() == AudioExportState::IDLE) {
			AudioExport_begin(detail.audio_stream_url, detail.title, detail.id,
			                  detail.duration_ms, detail.audio_stream_size);
			AudioExport_choose_bitrate(queue_bitrate);
			AudioExport_choose_artwork(queue_include_cover);
			AudioExport_confirm(false);
			// The exporter copies the few fields it needs. Release the full page,
			// suggestions and comments before MP3 encoding on Old 3DS.
			queue_lookup_detail = YouTubeVideoDetail();
			// The current export remains recoverable even if a status write fails.
			if (!DownloadQueue_set_status(queue_current_id, DownloadQueueStatus::DOWNLOADING))
				queue_stop_requested = true;
			queue_phase = QueuePhase::EXPORT;
			refresh_queue_view();
			return;
		} else {
			status_saved = DownloadQueue_set_status(queue_current_id, DownloadQueueStatus::FAILED, "Exporteur occupe");
		}
		if (!status_saved) queue_stop_requested = true;
		queue_lookup_detail = YouTubeVideoDetail();
		queue_phase = QueuePhase::IDLE;
		queue_current_id.clear();
		refresh_queue_view();
	}
	if (queue_phase == QueuePhase::EXPORT) {
		const AudioExportState state = AudioExport_get_state();
		if (state != AudioExportState::DONE && state != AudioExportState::FAILED &&
		    state != AudioExportState::CANCELLED) return;
		const std::string export_error = AudioExport_get_error_detail();
		const bool source_unavailable = export_error.find("Audio unavailable at source") != std::string::npos;
		if (export_error.find("Network C") != std::string::npos ||
		    export_error.find("Download took too long") != std::string::npos ||
		    export_error.find("Audio source HTTP 429") != std::string::npos ||
		    export_error.find("SD write failed") != std::string::npos ||
		    export_error.find("finalize MP3 file") != std::string::npos) {
			queue_stop_requested = true;
			queue_stop_reason = var_lang == "fr" ? "Reseau/SD : file gardee" : "Network/SD: queue saved";
		}
		const bool status_saved = DownloadQueue_set_status(queue_current_id,
		    state == AudioExportState::DONE ? DownloadQueueStatus::DONE :
		    source_unavailable ? DownloadQueueStatus::BLOCKED : DownloadQueueStatus::FAILED,
		    state == AudioExportState::DONE ? "" :
		    state == AudioExportState::CANCELLED ? "Interrompu" : export_error);
		AudioExport_dismiss();
		queue_phase = QueuePhase::IDLE;
		queue_current_id.clear();
		refresh_queue_view();
		if (!status_saved) queue_stop_requested = true;
	}
	if (queue_phase != QueuePhase::IDLE) return;
	if (queue_stop_requested) {
		queue_processing = false;
		queue_stop_requested = false;
		show_search_toast(queue_stop_reason.empty()
		    ? (var_lang == "fr" ? "File interrompue" : "Queue stopped") : queue_stop_reason);
		return;
	}
	for (const auto &entry : DownloadQueue_entries()) {
		if (entry.status != DownloadQueueStatus::PENDING && entry.status != DownloadQueueStatus::FAILED) continue;
		if (queue_attempted.count(entry.id)) continue;
		const std::string next_id = entry.id;
		if (!DownloadQueue_set_status(next_id, DownloadQueueStatus::PREPARING)) {
			queue_processing = false;
			show_search_toast(var_lang == "fr" ? "Erreur sauvegarde SD" : "SD save error");
			return;
		}
		queue_current_id = next_id;
		queue_attempted.insert(next_id);
		queue_lookup_ready = false;
		queue_phase = QueuePhase::LOOKUP;
		queue_async_task(load_queue_video, NULL);
		refresh_queue_view();
		return;
	}
	queue_processing = false;
	show_search_toast(var_lang == "fr" ? "File terminee" : "Queue finished");
}

static View *result_item_to_view(const YouTubeSuccinctItem &item, size_t index) {
	View *res_view;
	if (item.type == YouTubeSuccinctItem::CHANNEL) {
		SuccinctChannelView *cur_view = new SuccinctChannelView(0, 0, 320, VIDEO_LIST_THUMBNAIL_HEIGHT);
		cur_view->set_name(item.channel.name);
		cur_view->set_auxiliary_lines({item.channel.subscribers, item.channel.video_num});
		cur_view->set_thumbnail_url(item.channel.icon_url);
		res_view = cur_view;
	} else {
		SuccinctVideoView *cur_view = new SuccinctVideoView(0, 0, 320, VIDEO_LIST_THUMBNAIL_HEIGHT);
		cur_view->set_title_lines(truncate_str(item.get_name(), 320 - (VIDEO_LIST_THUMBNAIL_WIDTH + 3), 2, 0.5, 0.5));
		if (item.type == YouTubeSuccinctItem::VIDEO) {
			cur_view->set_auxiliary_lines({item.video.views_str, item.video.publish_date});
			cur_view->set_bottom_right_overlay(item.video.duration_text);
		} else if (item.type == YouTubeSuccinctItem::PLAYLIST) {
			cur_view->set_auxiliary_lines({item.playlist.video_count_str});
		}
		cur_view->set_thumbnail_url(item.get_thumbnail_url());
		cur_view->set_is_playlist(item.type == YouTubeSuccinctItem::PLAYLIST);
		res_view = cur_view;
	}
	res_view->set_get_background_color([index](const View &view) {
		return index == selected_result ? LIGHT1_BACK_COLOR : View::STANDARD_BACKGROUND(view);
	});
	res_view->set_on_view_released([item, index](View &view) {
		selected_result = index;
		clicked_url = item.get_url();
		clicked_is_channel = item.type == YouTubeSuccinctItem::CHANNEL;
	});
	return res_view;
}
static std::pair<int *, std::string *> get_thumbnail_info_from_view(View *view) {
	SuccinctVideoView *cur_view0 = dynamic_cast<SuccinctVideoView *>(view);
	if (cur_view0) {
		return {&cur_view0->thumbnail_handle, &cur_view0->thumbnail_url};
	}
	SuccinctChannelView *cur_view1 = dynamic_cast<SuccinctChannelView *>(view);
	if (cur_view1) {
		return {&cur_view1->thumbnail_handle, &cur_view1->thumbnail_url};
	}
	usleep(1000000);
	return {NULL, NULL};
}
static void load_search_results(void *) {
	// pre-access processing
	resource_lock.lock();
	std::string search_word = cur_search_word;
	search_done = false;

	result_list_view->recursive_delete_subviews();
	set_loading_bottom_view();
	search_result = YouTubeSearchResult();
	var_need_refresh = true;
	resource_lock.unlock();

	// actual loading
	std::string search_url = "https://m.youtube.com/results?search_query=";
	for (auto c : search_word) {
		search_url.push_back('%');
		search_url.push_back("0123456789ABCDEF"[(u8)c / 16]);
		search_url.push_back("0123456789ABCDEF"[(u8)c % 16]);
	}
	add_cpu_limit(ADDITIONAL_CPU_LIMIT);
	YouTubeSearchResult new_result = youtube_load_search(search_url);
	remove_cpu_limit(ADDITIONAL_CPU_LIMIT);

	// wrap and truncate here
	logger.info("search", "truncate/view creation start");
	std::vector<View *> new_result_views;
	for (size_t i = 0; i < new_result.results.size(); i++) {
		new_result_views.push_back(result_item_to_view(new_result.results[i], i));
	}
	logger.info("search", "truncate/view creation end");

	resource_lock.lock();
	if (exiting) { // app shut down while loading
		resource_lock.unlock();
		return;
	}
	search_result = new_result;
	selected_result = 0;
	result_list_view->views = new_result_views;
	update_result_bottom_view();

	search_done = true;
	var_need_refresh = true;
	resource_lock.unlock();
}
static void load_more_search_results(void *) {
	auto new_result = search_result;
	new_result.load_more_results();

	logger.info("search-c", "truncate/view creation start");
	std::vector<View *> new_result_views;
	for (size_t i = search_result.results.size(); i < new_result.results.size(); i++) {
		new_result_views.push_back(result_item_to_view(new_result.results[i], i));
	}
	logger.info("search-c", "truncate/view creation end");

	resource_lock.lock();
	if (exiting) { // app shut down while loading
		resource_lock.unlock();
		return;
	}
	search_result = new_result;
	if (new_result.error == "") {
		result_list_view->views.insert(result_list_view->views.end(), new_result_views.begin(), new_result_views.end());
	}
	update_result_bottom_view();
	var_need_refresh = true;
	resource_lock.unlock();
}
static void access_input_url(void *) {
	resource_lock.lock();
	auto url = last_url_input;
	resource_lock.unlock();

	YouTubePageType page_type = youtube_get_page_type(url);

	if (page_type == YouTubePageType::INVALID) {
		static NetworkSessionList session_list;
		if (!session_list.inited) {
			session_list.init();
		}

		auto result = session_list.perform(HttpRequest::GET(url, {}));
		page_type = youtube_get_page_type(result.redirected_url);
		url = result.redirected_url;
	}

	resource_lock.lock();
	if (exiting) { // app shut down while loading
		resource_lock.unlock();
		return;
	}
	if (page_type != YouTubePageType::INVALID) {
		if (page_type == YouTubePageType::VIDEO) {
			global_intent.next_scene = SceneType::VIDEO_PLAYER;
		} else if (page_type == YouTubePageType::CHANNEL) {
			global_intent.next_scene = SceneType::CHANNEL;
		} else if (page_type == YouTubePageType::SEARCH) {
			global_intent.next_scene = SceneType::SEARCH;
		}
		global_intent.arg = url;
	} else {
		toast_view->set_text((std::function<std::string()>)[]() { return LOCALIZED(NOT_A_YOUTUBE_URL); });
		toast_view->set_is_visible(true);
		toast_view_visible_frames_left = 100;
	}
	resource_lock.unlock();
}

bool Search_show_search_keyboard() {
	if (!is_async_task_running(load_search_results)) {
		if (global_current_scene != SceneType::SEARCH) {
			resource_lock.lock();
		}
		SwkbdState keyboard;
		swkbdInit(&keyboard, SWKBD_TYPE_NORMAL, 2, 256);
		swkbdSetFeatures(&keyboard, SWKBD_DEFAULT_QWERTY | SWKBD_PREDICTIVE_INPUT);
		swkbdSetValidation(&keyboard, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
		swkbdSetButton(&keyboard, SWKBD_BUTTON_LEFT, LOCALIZED(CANCEL).c_str(), false);
		swkbdSetButton(&keyboard, SWKBD_BUTTON_RIGHT, LOCALIZED(OK).c_str(), true);
		swkbdSetInitialText(&keyboard, cur_search_word.c_str());
		char search_word[257];
		add_cpu_limit(40);
		video_set_skip_drawing(true);
		auto button_pressed = swkbdInputText(&keyboard, search_word, 256);
		video_set_skip_drawing(false);
		remove_cpu_limit(40);

		if (button_pressed == SWKBD_BUTTON_RIGHT) {
			cur_search_word = search_word;
			// Truncate text to fit in search box and display
			double search_box_width = 320 - SEARCH_BOX_MARGIN * 4 - URL_BUTTON_WIDTH - QUEUE_BUTTON_WIDTH;
			std::string display_text = truncate_search_text_for_display(search_word, search_box_width, 0.5);
			search_box_view->set_text(display_text);
			search_box_view->set_get_text_color([]() { return DEFAULT_TEXT_COLOR; });

			remove_all_async_tasks_with_type(load_search_results);
			remove_all_async_tasks_with_type(load_more_search_results);
			queue_async_task(load_search_results, NULL);

			global_intent.next_scene = SceneType::SEARCH;
		}
		if (global_current_scene != SceneType::SEARCH) {
			resource_lock.unlock();
		}
		return button_pressed == SWKBD_BUTTON_RIGHT;
	} else {
		return false;
	}
}
bool Search_show_url_input_keyboard() {
	if (!is_async_task_running(access_input_url)) {
		if (global_current_scene != SceneType::SEARCH) {
			resource_lock.lock();
		}
		SwkbdState keyboard;
		swkbdInit(&keyboard, SWKBD_TYPE_NORMAL, 2, 255);
		swkbdSetFeatures(&keyboard, SWKBD_DEFAULT_QWERTY | SWKBD_PREDICTIVE_INPUT);
		swkbdSetValidation(&keyboard, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
		swkbdSetButton(&keyboard, SWKBD_BUTTON_LEFT, LOCALIZED(CANCEL).c_str(), false);
		swkbdSetButton(&keyboard, SWKBD_BUTTON_RIGHT, LOCALIZED(OK).c_str(), true);
		swkbdSetInitialText(&keyboard, last_url_input.c_str());
		char url[256];
		add_cpu_limit(40);
		video_set_skip_drawing(true);
		auto button_pressed = swkbdInputText(&keyboard, url, 256 - 1);
		video_set_skip_drawing(false);
		remove_cpu_limit(40);

		if (button_pressed == SWKBD_BUTTON_RIGHT) {
			last_url_input = url;

			remove_all_async_tasks_with_type(access_input_url);
			queue_async_task(access_input_url, NULL);
		}
		if (global_current_scene != SceneType::SEARCH) {
			resource_lock.unlock();
		}
		return button_pressed == SWKBD_BUTTON_RIGHT;
	} else {
		return false;
	}
}

void Search_draw(void) {
	Hid_info key;
	Util_hid_query_key_state(&key);
	static int last_export_state = -1;
	static int last_export_progress = -1;
	static u64 last_export_refresh_ms = 0;
	const int export_state = (int)AudioExport_get_state();
	const int export_progress = AudioExport_get_progress();
	const u64 now_ms = osGetTime();
	if (queue_processing && (export_state != last_export_state || export_progress != last_export_progress ||
	    now_ms - last_export_refresh_ms >= 1000)) {
		var_need_refresh = true;
		last_export_refresh_ms = now_ms;
	}
	last_export_state = export_state;
	last_export_progress = export_progress;

	thumbnail_set_active_scene(SceneType::SEARCH);

	bool video_playing_bar_show = video_is_playing();
	RESULT_Y_HIGH = video_playing_bar_show ? 240 - VIDEO_PLAYING_BAR_HEIGHT : 240;
	result_view->update_y_range(0, RESULT_Y_HIGH - RESULT_Y_LOW);
	queue_scroll_view->update_y_range(RESULT_Y_LOW, RESULT_Y_HIGH);

	if (toast_view_visible_frames_left > 0) {
		toast_view_visible_frames_left--;
		if (!toast_view_visible_frames_left) {
			toast_view->set_is_visible(false);
		}
	}
	if (var_need_refresh || !var_eco_mode) {
		var_need_refresh = false;
		Draw_frame_ready();
		video_draw_top_screen();
		if (queue_processing && queue_phase == QueuePhase::LOOKUP) {
			Draw_texture(var_square_image[0], 0xC0000000, 25, 170, 350, 38);
			const std::string label = (var_lang == "fr" ? "Controle de la video " : "Checking video ") +
			    std::to_string(queue_attempted.size()) + "/" + std::to_string(queue_batch_total);
			Draw_xy_centered(label, 30, 370, 175, 202, 0.5, 0.5, DEF_DRAW_WHITE);
		}

		Draw_screen_ready(2, DEFAULT_BACK_COLOR);

		// (!) : I don't know how to draw textures truncated, so I will just fill the margin with white again
		resource_lock.lock();
		if (queue_open) {
			top_bar_view->draw();
			queue_scroll_view->draw();
		} else {
			main_view->draw();
		}
		if (toast_view_visible_frames_left > 0) {
			toast_view->draw();
		}
		resource_lock.unlock();

		if (video_playing_bar_show) {
			video_draw_playing_bar();
		}
		if (!queue_processing)
			draw_overlay_menu(video_playing_bar_show ? 240 - OVERLAY_MENU_ICON_SIZE - VIDEO_PLAYING_BAR_HEIGHT
			                                         : 240 - OVERLAY_MENU_ICON_SIZE);

		if (Util_expl_query_show_flag()) {
			Util_expl_draw();
		}

		if (Util_err_query_error_show_flag()) {
			Util_err_draw();
		}

		Draw_touch_pos();

		Draw_apply_draw();
	} else {
		gspWaitForVBlank();
	}

	if (Util_err_query_error_show_flag()) {
		Util_err_main(key);
	} else if (Util_expl_query_show_flag()) {
		Util_expl_main(key);
	} else {
		if (!queue_processing) update_overlay_menu(&key);

		resource_lock.lock();
		if (queue_open) {
			if (!queue_processing) top_bar_view->update(key);
			queue_scroll_view->update(key);
			if (!pending_queue_remove.empty()) {
				std::string id = pending_queue_remove;
				pending_queue_remove.clear();
				if (DownloadQueue_remove(id)) {
					refresh_queue_view();
					show_search_toast(var_lang == "fr" ? "Retire. Annuler disponible." : "Removed. Undo available.");
				} else show_search_toast(var_lang == "fr" ? "Impossible de retirer" : "Could not remove");
			}
			if (!pending_queue_retry.empty()) {
				const std::string id = pending_queue_retry;
				pending_queue_retry.clear();
				if (DownloadQueue_set_status(id, DownloadQueueStatus::PENDING)) {
					refresh_queue_view();
					show_search_toast(var_lang == "fr" ? "Remise en attente" : "Queued for retry");
				}
			}
			if (pending_queue_undo) {
				pending_queue_undo = false;
				if (DownloadQueue_undo_remove()) {
					refresh_queue_view();
					show_search_toast(var_lang == "fr" ? "Video retablie" : "Video restored");
				}
			}
			if (pending_queue_toggle_cover || (key.p_y && !queue_processing)) {
				pending_queue_toggle_cover = false;
				if (!queue_processing) queue_include_cover = !queue_include_cover;
				var_need_refresh = true;
			}
			int start_bitrate = pending_queue_start_bitrate;
			pending_queue_start_bitrate = 0;
			if (!start_bitrate && !queue_processing && key.p_a) start_bitrate = 128;
			if (!start_bitrate && !queue_processing && key.p_x) start_bitrate = 256;
			if (start_bitrate && !queue_processing) {
				if (is_async_task_running(access_input_url)) {
					show_search_toast(var_lang == "fr" ? "Attends la fin du lien" : "Wait for URL lookup");
				} else if (video_is_playing()) {
					show_search_toast(var_lang == "fr" ? "Arrete la lecture avant la file" : "Stop playback before queue");
				} else if (video_is_media_busy()) {
					show_search_toast(var_lang == "fr" ? "Attends la fermeture du lecteur" : "Wait for player cleanup");
				} else if (AudioExport_get_state() != AudioExportState::IDLE) {
					show_search_toast(var_lang == "fr" ? "Exporteur occupe" : "Exporter busy");
				} else if (DownloadQueue_storage_error()) {
					show_search_toast(var_lang == "fr" ? "File illisible sur SD" : "Queue unreadable on SD");
				} else if (DownloadQueue_entries().empty()) {
					show_search_toast(var_lang == "fr" ? "File vide" : "Queue empty");
				} else {
					queue_bitrate = start_bitrate;
					// Page loading shares the metadata worker. Do not queue more
					// search pages ahead of the selected audio items.
					remove_all_async_tasks_with_type(load_more_search_results);
					queue_attempted.clear();
					queue_batch_total = 0;
					for (const auto &entry : DownloadQueue_entries())
						if (entry.status == DownloadQueueStatus::PENDING || entry.status == DownloadQueueStatus::FAILED)
							++queue_batch_total;
					queue_stop_requested = false;
					queue_stop_reason.clear();
					queue_processing = queue_batch_total > 0;
					show_search_toast(queue_processing
					    ? (var_lang == "fr" ? "File demarree" : "Queue started")
					    : (var_lang == "fr" ? "Aucune video en attente" : "No pending videos"));
				}
			}
		} else {
			const int old_scroll_offset = result_view->get_offset();
			main_view->update(key);
			if (!search_result.results.empty() && result_view->get_offset() != old_scroll_offset &&
			    !key.p_d_up && !key.p_d_down) {
				const int pitch = VIDEO_LIST_THUMBNAIL_HEIGHT + SMALL_MARGIN;
				const int center = result_view->get_offset() + (RESULT_Y_HIGH - RESULT_Y_LOW) / 2;
				selected_result = std::min<size_t>(search_result.results.size() - 1, center / pitch);
				var_need_refresh = true;
			}
			if (key.p_d_up && selected_result > 0) {
				--selected_result;
				result_view->scroll(-(VIDEO_LIST_THUMBNAIL_HEIGHT + SMALL_MARGIN));
				var_need_refresh = true;
			} else if (key.p_d_down && selected_result + 1 < search_result.results.size()) {
				++selected_result;
				result_view->scroll(VIDEO_LIST_THUMBNAIL_HEIGHT + SMALL_MARGIN);
				var_need_refresh = true;
			}
			if (key.p_x && selected_result < search_result.results.size()) {
				const auto &item = search_result.results[selected_result];
				if (item.type == YouTubeSuccinctItem::VIDEO) {
					std::string id = youtube_get_video_id_by_url(item.video.url);
					if (id.empty())
						show_search_toast(var_lang == "fr" ? "Lien video non reconnu" : "Unrecognized video URL");
					else if (DownloadQueue_add(id, item.video.title))
						show_search_toast(var_lang == "fr" ? "Ajoute a la file" : "Added to queue");
					else show_search_toast(var_lang == "fr" ? "Deja ajoute ou SD indisponible" : "Already queued or SD unavailable");
				} else show_search_toast(var_lang == "fr" ? "Choisis une video" : "Select a video");
			}
		}
		if (clicked_url != "") {
			global_intent.next_scene = clicked_is_channel ? SceneType::CHANNEL : SceneType::VIDEO_PLAYER;
			global_intent.arg = clicked_url;
			clicked_url = "";
		}
		resource_lock.unlock();

		if (video_playing_bar_show) {
			video_update_playing_bar(key);
		}

		if (!queue_open && key.p_a) {
			Search_show_search_keyboard();
		}

		if (key.p_b) {
			if (queue_processing) {
				queue_stop_requested = true;
				if (queue_phase == QueuePhase::EXPORT) AudioExport_cancel_operation();
				show_search_toast(var_lang == "fr" ? "Arret en cours..." : "Stopping...");
			} else if (queue_open) { queue_open = false; var_need_refresh = true; }
			else global_intent.next_scene = SceneType::BACK;
		}
		queue_tick();
	}
}
