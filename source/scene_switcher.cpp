#include "headers.hpp"

#include "scenes/video_player.hpp"
#include "scenes/search.hpp"
#include "scenes/channel.hpp"
#include "scenes/about.hpp"
#include "scenes/setting_menu.hpp"
#include "scenes/watch_history.hpp"
#include "scenes/home.hpp"
#include "network_decoder/network_io.hpp"
#include "network_decoder/thumbnail_loader.hpp"
#include "util/async_task.hpp"
#include "util/misc_tasks.hpp"
#include "ui/ui.hpp"
#include "system/audio_exporter.hpp"
#ifdef MP3_DOWNLOADER_APP
#include "data_io/settings.hpp"
#include "youtube_parser/parser.hpp"
#include <cstdio>
#include <sys/stat.h>
#endif
// add here

SceneType global_current_scene;
Intent global_intent;

namespace SceneSwitcher {
static bool menu_thread_run = false;
static bool menu_check_exit_request = false;
static bool home_allowed_before_launch = true;
static Thread menu_worker_thread, thumbnail_downloader_thread, async_task_thread, misc_tasks_thread;


bool manual_screen_off = false;

static Result sound_init_result;
}; // namespace SceneSwitcher
using namespace SceneSwitcher;

#ifdef MP3_DOWNLOADER_APP
namespace {
bool mp3_guide_pending = false;
bool mp3_language_pending = false;
bool mp3_language_drawn = false;
unsigned mp3_guide_page = 0;
int mp3_guide_drawn_page = -1;

bool open_mp3_guide_save(FS_Archive *archive) {
	FS_Path root = fsMakePath(PATH_EMPTY, "");
	if (R_SUCCEEDED(FSUSER_OpenArchive(archive, ARCHIVE_SAVEDATA, root))) return true;
	if (R_FAILED(FSUSER_FormatSaveData(ARCHIVE_SAVEDATA, root, 1024, 8, 8, 8, 8, false))) return false;
	return R_SUCCEEDED(FSUSER_OpenArchive(archive, ARCHIVE_SAVEDATA, root));
}

bool mp3_guide_saved(bool write_marker) {
	FS_Archive archive;
	if (!open_mp3_guide_save(&archive)) return false;
	FS_Path path = fsMakePath(PATH_ASCII, "/guide-v2.dat");
	if (write_marker) (void)FSUSER_CreateFile(archive, path, 0, 1);
	Handle file;
	bool okay = false;
	if (R_SUCCEEDED(FSUSER_OpenFile(&file, archive, path,
	                                  write_marker ? FS_OPEN_WRITE : FS_OPEN_READ, 0))) {
		char marker = '1';
		u32 count = 0;
		Result result = write_marker
		    ? FSFILE_Write(file, &count, 0, &marker, 1, FS_WRITE_FLUSH)
		    : FSFILE_Read(file, &count, 0, &marker, 1);
		FSFILE_Close(file);
		okay = R_SUCCEEDED(result) && count == 1 && marker == '1';
		if (okay && write_marker)
			okay = R_SUCCEEDED(FSUSER_ControlArchive(archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA,
			                                          NULL, 0, NULL, 0));
	}
	FSUSER_CloseArchive(archive);
	return okay;
}

bool mp3_guide_sd_marker(bool write_marker) {
	const char *path = "sdmc:/3ds/MP3Downloader/guide-v2.dat";
	if (write_marker) {
		(void)mkdir("sdmc:/3ds", 0777);
		(void)mkdir("sdmc:/3ds/MP3Downloader", 0777);
	}
	FILE *file = fopen(path, write_marker ? "wb" : "rb");
	if (!file) return false;
	char marker = '1';
	bool okay = write_marker ? fwrite(&marker, 1, 1, file) == 1 :
	                          fread(&marker, 1, 1, file) == 1 && marker == '1';
	if (fclose(file) != 0) okay = false;
	return okay;
}

bool mp3_language_marker(bool write_marker) {
	const char *path = "sdmc:/3ds/MP3Downloader/language-v1.dat";
	if (write_marker) {
		(void)mkdir("sdmc:/3ds", 0777);
		(void)mkdir("sdmc:/3ds/MP3Downloader", 0777);
	}
	FILE *file = fopen(path, write_marker ? "wb" : "rb");
	if (!file) return false;
	char marker = '1';
	bool okay = write_marker ? fwrite(&marker, 1, 1, file) == 1 :
	                          fread(&marker, 1, 1, file) == 1 && marker == '1';
	if (fclose(file) != 0) okay = false;
	return okay;
}

void mp3_guide_init() {
	hidScanInput();
	bool replay = (hidKeysHeld() & KEY_SELECT) != 0;
	mp3_guide_pending = replay || (!mp3_guide_saved(false) && !mp3_guide_sd_marker(false));
	mp3_language_pending = !mp3_language_marker(false);
	mp3_language_drawn = false;
	mp3_guide_page = 0;
	mp3_guide_drawn_page = -1;
}

void mp3_language_draw() {
	if (mp3_language_drawn) { gspWaitForVBlank(); return; }
	Draw_frame_ready();
	for (int eye = 0; eye < 2; ++eye) {
		Draw_screen_ready(eye, 0xFF1B1008);
		Draw_xy_centered("MP3 DOWNLOADER", 12, 388, 28, 62, 0.75f, 0.75f, DEF_DRAW_AQUA);
		Draw_xy_centered("Choisis ta langue / Choose your language", 20, 380, 87, 124,
		                 0.52f, 0.52f, DEF_DRAW_WHITE);
		Draw_xy_centered("A : FRANCAIS       B : ENGLISH", 20, 380, 150, 184,
		                 0.55f, 0.55f, DEF_DRAW_YELLOW);
	}
	Draw_screen_ready(2, DEF_DRAW_BLACK);
	Draw_xy_centered("Premier lancement / First launch", 10, 310, 75, 105,
	                 0.48f, 0.48f, DEF_DRAW_WHITE);
	Draw_apply_draw();
	mp3_language_drawn = true;
}

void mp3_guide_draw() {
	if (mp3_guide_drawn_page == (int)mp3_guide_page) {
		gspWaitForVBlank();
		return;
	}
	static const char *const titles_fr[] = {"BIENVENUE", "TELECHARGER", "FILE D'ATTENTE", "FORMAT ET AIDE"};
	static const char *const lines_fr[4][6] = {
		{"Parcours les videos YouTube en audio seul.", "Les miniatures restent visibles.",
		 "A : ouvrir une video ou mettre en pause", "B : revenir a l'ecran precedent",
		 "START : quitter (hors telechargement)", "SELECT au demarrage : revoir ce guide"},
		{"Ouvre une video puis mets-la en pause.", "Y : preparer le MP3.",
		 "A : 128 kbps   X : 256 kbps", "Pochette : A oui, B non, X annule.",
		 "Ensuite A confirme, B annule.", "SD:/download audio  (max. 1 heure)"},
		{"Recherche : pad haut/bas, X ajoute la video.", "X ajoute aussi la video ouverte ou choisie.",
		 "File : bouton en haut, pres de la recherche.", "A/128 ou X/256 lance; pochette OFF par defaut.",
		 "Petite croix retire; Annuler remet. Bloque : touche.", "B : retour; si ca telecharge, B arrete d'abord."},
		 {"Le resultat est un MP3 stereo 44,1 kHz.", "128 kbps : plus petit; 256 : plus lourd.",
		 "La qualite depend de la source audio.", "Sa miniature peut devenir la pochette MP3.",
		 "Une video bloquee ne peut pas etre exportee.", "Succes : retire de la file. Echec : conserve."}
	};
	static const char *const titles_en[] = {"WELCOME", "DOWNLOAD", "DOWNLOAD QUEUE", "FORMAT AND HELP"};
	static const char *const lines_en[4][6] = {
		{"Browse YouTube videos in audio-only mode.", "Thumbnails remain visible.",
		 "A: open a video or pause playback", "B: return to previous screen",
		 "START: exit (outside a download)", "Hold SELECT at startup: reopen this guide"},
		{"Open a video, then pause playback.", "Y: prepare the MP3 download.",
		 "A: 128 kbps   X: 256 kbps", "Cover: A yes, B no, X cancel.",
		 "Then A confirms, B cancels.", "SD:/download audio  (max. 1 hour)"},
		{"Search: up/down, X queues a video.", "X also queues the opened or chosen video.",
		 "Queue: button beside search at the top.", "A/128 or X/256 starts; cover OFF by default.",
		 "Small x removes; Undo restores. Tap blocked to retry.", "B: back; during download B stops it first."},
		 {"Output is stereo MP3 at 44.1 kHz.", "128 kbps: smaller; 256: larger.",
		 "Quality depends on the source audio.", "The thumbnail can become MP3 cover art.",
		 "A blocked video cannot be exported.", "Success: removed from queue. Failure: kept."}
	};
	const bool french = var_lang == "fr";
	const char *const *titles = french ? titles_fr : titles_en;
	const char *const (*lines)[6] = french ? lines_fr : lines_en;
	Draw_frame_ready();
	for (int eye = 0; eye < 2; ++eye) {
		Draw_screen_ready(eye, 0xFF1B1008);
		Draw_xy_centered("MP3 DOWNLOADER", 12, 388, 17, 42, 0.75f, 0.75f, DEF_DRAW_AQUA);
		Draw_xy_centered(std::string(titles[mp3_guide_page]) + "  " +
		                 std::to_string(mp3_guide_page + 1) + "/4",
		                 18, 382, 50, 72, 0.55f, 0.55f, DEF_DRAW_YELLOW);
		for (int i = 0; i < 6; ++i)
			Draw(lines[mp3_guide_page][i], 27, 82 + i * 20, 0.47f, 0.47f, DEF_DRAW_WHITE);
		Draw_xy_centered(french ? (mp3_guide_page == 3 ? "B : RETOUR       A : J'AI COMPRIS" :
		                           "B : RETOUR       A : PAGE SUIVANTE") :
		                 (mp3_guide_page == 3 ? "B: BACK       A: I UNDERSTAND" :
		                           "B: BACK       A: NEXT PAGE"),
		                 16, 384, 211, 233, 0.48f, 0.48f, DEF_DRAW_AQUA);
	}
	Draw_screen_ready(2, DEF_DRAW_BLACK);
	Draw_xy_centered(french ? "Guide de premiere utilisation" : "First-use guide",
	                 10, 310, 72, 102, 0.55f, 0.55f, DEF_DRAW_WHITE);
	Draw_xy_centered(french ? "A : continuer    B : retour" : "A: continue    B: back",
	                 10, 310, 124, 152, 0.5f, 0.5f, DEF_DRAW_AQUA);
	Draw_apply_draw();
	mp3_guide_drawn_page = (int)mp3_guide_page;
}
} // namespace
#endif

void Menu_worker_thread(void *arg);
#define LOG_IF_ERROR(expr)                                                                                             \
	do {                                                                                                               \
		auto res = expr;                                                                                               \
		if (res != 0)                                                                                                  \
			logger.error(DEF_MENU_INIT_STR, #expr ": " + std::to_string(res));                                         \
	} while (0)

void Menu_init(void) {
	Result_with_string result;

	logger.init();

	logger.info(DEF_MENU_INIT_STR, "Initializing...");
	logger.info(DEF_MENU_INIT_STR, "Version is \"" + DEF_CURRENT_APP_VER + "\"");

	osSetSpeedupEnable(true);
	svcSetThreadPriority(CUR_THREAD_HANDLE, DEF_THREAD_PRIORITY_HIGH - 1);

	logger.info(DEF_MENU_INIT_STR, "Initializing services...");
	LOG_IF_ERROR(fsInit());
	LOG_IF_ERROR(acInit());
	LOG_IF_ERROR(aptInit());
	LOG_IF_ERROR(mcuHwcInit());
	LOG_IF_ERROR(ptmuInit());
	{
		constexpr int SOC_BUFFERSIZE = 0x100000;
		u32 *soc_buffer = (u32 *)memalign(0x1000, SOC_BUFFERSIZE);
		if (!soc_buffer) {
			logger.error(DEF_MENU_INIT_STR, "soc buffer out of memory");
		} else {
			LOG_IF_ERROR(socInit(soc_buffer, SOC_BUFFERSIZE));
		}
	}
	LOG_IF_ERROR(sslcInit(0));
	LOG_IF_ERROR(httpcInit(0x200000));
	LOG_IF_ERROR(romfsInit());
	LOG_IF_ERROR(cfguInit());
	LOG_IF_ERROR(amInit());
	home_allowed_before_launch = aptIsHomeAllowed();
	aptSetHomeAllowed(false);
	LOG_IF_ERROR((sound_init_result = ndspInit())); // 0xd880A7FA if ndsp firmware is lacking
	LOG_IF_ERROR(APT_SetAppCpuTimeLimit(30));
	lock_network_state();

	aptSetSleepAllowed(false);
	set_apt_callback();

	logger.info(DEF_MENU_INIT_STR, "Services initialized.");

	CFGU_GetSystemModel(&var_model);

	LOG_IF_ERROR(Draw_init(var_model != CFG_MODEL_2DS).code);
#ifdef MP3_DOWNLOADER_APP
	mp3_guide_init();
#endif
	Draw_frame_ready();
	Draw_screen_ready(0, DEF_DRAW_BLACK); // Black prevents flashing.
	Draw_screen_ready(1, DEF_DRAW_BLACK); // Same here
	Draw_screen_ready(2, DEF_DRAW_BLACK); // Same here
	Draw_apply_draw();

	Util_expl_init();
	Extfont_init();
	for (int i = 0; i < FONT_BLOCK_NUM; i++) {
		Extfont_request_extfont_status(i, true);
	}
	for (int i = 0; i < SYSTEM_FONT_NUM; i++) {
		Extfont_request_sysfont_status(i, true);
	}

	menu_thread_run = true;
	menu_worker_thread =
	    threadCreate(Menu_worker_thread, (void *)(""), DEF_STACKSIZE, DEF_THREAD_PRIORITY_REALTIME, 1, false);

	// Get Wi-Fi state from shared memory #0x1FF81067
	var_wifi_state = *(u8 *)0x1FF81067;
	// Wait for the 3DS's Wi-Fi to (hopefully) hurry up.
	int tries = 0;
	while (var_wifi_state != 2) {
		tries++;
		if (tries > 20) {
			break;
		}
		// Enable Wi-Fi in case the user disabled it for whatever reason.
		if (tries < 2) {
			Util_cset_set_wifi_state(true);
		}
		// Sleep for one second. This will keep running until the console finally connects, or if we exceed 20 tries.
		usleep(1000000);
		// Fetch the updated Wi-Fi state
		var_wifi_state = *(u8 *)0x1FF81067;
		if (var_wifi_state == 2) {
			// Sometimes takes a bit longer to actually have connectivity for some reason.
			usleep(2500000);
		}
	}

	Sem_init();
	Sem_suspend();
	VideoPlayer_init();
	VideoPlayer_suspend();
	Channel_init();
	Channel_suspend();
	About_init();
	About_suspend();
	History_init();
	History_suspend();
	Search_init();
	Search_suspend();
	// add here
	Home_init(); // first running
	global_current_scene = SceneType::HOME;

	thumbnail_downloader_thread = threadCreate(thumbnail_downloader_thread_func, (void *)(""), DEF_STACKSIZE,
	                                           DEF_THREAD_PRIORITY_NORMAL, 0, false);
	async_task_thread = threadCreate(async_task_thread_func, NULL, DEF_STACKSIZE, DEF_THREAD_PRIORITY_NORMAL, 0, false);
	misc_tasks_thread = threadCreate(misc_tasks_thread_func, NULL, DEF_STACKSIZE, DEF_THREAD_PRIORITY_NORMAL, 0, false);

	Menu_get_system_info();

	logger.info(DEF_MENU_INIT_STR, "Initialized.");
}

void Menu_exit(void) {
	logger.info(DEF_MENU_EXIT_STR, "Exiting...");
	u64 time_out = 10000000000;
	Result_with_string result;

	menu_thread_run = false;

	VideoPlayer_exit();
	Channel_exit();
	Search_exit();
	Sem_exit();
	About_exit();
	History_exit();
	Home_exit();
	// add here

	Util_expl_exit();
	Extfont_exit();

	thumbnail_downloader_thread_exit_request();
	async_task_thread_exit_request();
	misc_tasks_thread_exit_request();
	NetworkSessionList::exit_request();
	unlock_network_state();

	remove_apt_callback();
	aptSetHomeAllowed(home_allowed_before_launch);

	logger.info(DEF_MENU_EXIT_STR, "threadJoin()...", threadJoin(menu_worker_thread, time_out));
	logger.info(DEF_MENU_EXIT_STR, "threadJoin()...", threadJoin(thumbnail_downloader_thread, time_out));
	logger.info(DEF_MENU_EXIT_STR, "threadJoin()...", threadJoin(async_task_thread, time_out));
	logger.info(DEF_MENU_EXIT_STR, "threadJoin()...", threadJoin(misc_tasks_thread, time_out));
	threadFree(menu_worker_thread);
	threadFree(thumbnail_downloader_thread);
	threadFree(async_task_thread);
	threadFree(misc_tasks_thread);

	NetworkSessionList::at_exit();

	fsExit();
	acExit();
	aptExit();
	mcuHwcExit();
	ptmuExit();
	httpcExit();
	romfsExit();
	cfguExit();
	amExit();
	ndspExit();
	sslcExit();
	socExit();
	Draw_exit();

	logger.info(DEF_MENU_EXIT_STR, "Exited.");
}

static std::vector<Intent> scene_stack = {{SceneType::HOME, ""}};

bool Menu_main(void) {
	Util_hid_update_key_state();

	Hid_info key;
	Util_hid_query_key_state(&key);
	// START requests a full app exit. An MP3 export must finish first.
	bool request_start_exit = false;
	if (key.p_start) {
		request_start_exit = true;
#ifdef MP3_DOWNLOADER_APP
		AudioExportState export_state = AudioExport_get_state();
		bool export_busy = export_state == AudioExportState::WAITING_FOR_PLAYER ||
		                   export_state == AudioExportState::DOWNLOADING ||
		                   export_state == AudioExportState::FETCHING_ART ||
		                   export_state == AudioExportState::ENCODING;
		request_start_exit = !export_busy;
#endif
	}

	if (sound_init_result != 0) {
		std::string error_msg = "Could not initialize NDSP (sound service)\n"
		                        "This is probably because you haven't run DSP1.\n"
		                        "You can download it from the link below:\n\n"
		                        "https://github.com/zoogie/DSP1/releases/\n\n"
		                        "If you are on Luma3DS v11.0 or later, you\n"
		                        "can open Rosalina menu, go to Misc. options\n"
		                        " -> Dump DSP firmware instead.\n";
		error_msg += "\n\nPress A to close the app";

		Draw_frame_ready();
		Draw_screen_ready(0, DEF_DRAW_WHITE);
		Draw_screen_ready(1, DEF_DRAW_WHITE);

		Draw_xy_centered(error_msg, 0, 400, 0, 240, 0.5, 0.5, DEF_DRAW_BLACK);
		Draw_top_ui();

		Draw_screen_ready(2, DEF_DRAW_WHITE);

		Draw_apply_draw();

		static int frame_cnt = 0;
		if (++frame_cnt >= 30) {
			frame_cnt = 0;
			logger.info(DEF_MENU_INIT_STR, "ndspInit() retry...", (sound_init_result = ndspInit())); // 0xd880A7FA
		}

		if (key.h_a) {
			return false;
		}
		return true;
	}

#ifdef MP3_DOWNLOADER_APP
	if (mp3_language_pending) {
		if (key.p_start) return false;
		if (key.p_a || key.p_b) {
			var_lang = key.p_a ? "fr" : "en";
			var_lang_content = var_lang;
			load_string_resources(var_lang);
			youtube_change_content_language(var_lang_content);
			save_settings();
			if (!mp3_language_marker(true)) logger.error("mp3/language", "Could not save first-use choice");
			mp3_language_pending = false;
			mp3_guide_drawn_page = -1;
			var_need_refresh = true;
			return true;
		}
		mp3_language_draw();
		return true;
	}
	if (mp3_guide_pending) {
		if (key.p_start) return false;
		if (key.p_a) {
			if (mp3_guide_page == 3) {
				if (!mp3_guide_saved(true)) (void)mp3_guide_sd_marker(true);
				mp3_guide_pending = false;
				var_need_refresh = true;
				return true;
			}
			++mp3_guide_page;
		} else if (key.p_b && mp3_guide_page > 0) {
			--mp3_guide_page;
		}
		mp3_guide_draw();
		return true;
	}
#endif

	if (var_show_fps) {
		snprintf(var_status, sizeof(var_status), "%02dFPS ", (int)Draw_query_fps());
	} else {
		var_status[0] = '\0';
	}

	if (var_debug_mode) {
		var_need_refresh = true;
	}

	global_intent = Intent();
	if (global_current_scene == SceneType::VIDEO_PLAYER) {
		VideoPlayer_draw();
	} else if (global_current_scene == SceneType::SEARCH) {
		Search_draw();
	} else if (global_current_scene == SceneType::CHANNEL) {
		Channel_draw();
	} else if (global_current_scene == SceneType::SETTINGS) {
		Sem_draw();
	} else if (global_current_scene == SceneType::ABOUT) {
		About_draw();
	} else if (global_current_scene == SceneType::HISTORY) {
		History_draw();
	} else if (global_current_scene == SceneType::HOME) {
		Home_draw();
	}
	// add here
	if (request_start_exit) global_intent.next_scene = SceneType::EXIT;

	if (global_intent.next_scene != SceneType::NO_CHANGE) {
		if (global_current_scene == SceneType::VIDEO_PLAYER) {
			VideoPlayer_suspend();
		} else if (global_current_scene == SceneType::SEARCH) {
			Search_suspend();
		} else if (global_current_scene == SceneType::CHANNEL) {
			Channel_suspend();
		} else if (global_current_scene == SceneType::SETTINGS) {
			Sem_suspend();
		} else if (global_current_scene == SceneType::ABOUT) {
			About_suspend();
		} else if (global_current_scene == SceneType::HISTORY) {
			History_suspend();
		} else if (global_current_scene == SceneType::HOME) {
			Home_suspend();
		}
		// add here
	}

	// common updates
	if (key.h_select && key.p_y) {
		var_debug_mode = !var_debug_mode;
	}
	if (key.h_select && key.h_r && key.p_a) {
		var_show_fps = !var_show_fps;
	}
	if (key.h_select && key.p_x) {
		logger.draw_enabled ^= 1, var_need_refresh = true; // toggle log drawing
	}
	logger.update(key);
	if (key.h_touch || key.p_touch) {
		var_need_refresh = true;
	}
#ifndef MP3_DOWNLOADER_APP
	if (((key.h_select && key.p_start) || (key.h_start && key.p_select)) && var_model != CFG_MODEL_2DS) {
		manual_screen_off = !manual_screen_off;
	}
#endif

	if (global_intent.next_scene == SceneType::EXIT) {
		return false;
	} else if (global_intent.next_scene != SceneType::NO_CHANGE && global_intent != scene_stack.back()) {
		if (scene_stack.size() >= 2 && global_intent == scene_stack[scene_stack.size() - 2]) {
			global_intent.next_scene = SceneType::BACK;
		}
		if (global_intent.next_scene == SceneType::BACK) {
			if (scene_stack.size() >= 2) {
				scene_stack.pop_back();
			}
		} else {
			scene_stack.push_back(global_intent);
		}

		global_current_scene = scene_stack.back().next_scene;
		std::string arg = scene_stack.back().arg;

		if (global_current_scene == SceneType::VIDEO_PLAYER) {
			VideoPlayer_resume(arg);
		} else if (global_current_scene == SceneType::SEARCH) {
			Search_resume(arg);
		} else if (global_current_scene == SceneType::CHANNEL) {
			Channel_resume(arg);
		} else if (global_current_scene == SceneType::SETTINGS) {
			Sem_resume(arg);
		} else if (global_current_scene == SceneType::ABOUT) {
			About_resume(arg);
		} else if (global_current_scene == SceneType::HISTORY) {
			History_resume(arg);
		} else if (global_current_scene == SceneType::HOME) {
			Home_resume(arg);
		}
		// add here
	}

	return true;
}

void Menu_get_system_info(void) {
	var_wifi_signal = osGetWifiStrength();
	var_wifi_state = *(u8 *)0x1FF81067;
	if (var_wifi_state != 2) {
		var_wifi_signal = 8;
	}

	// Get time
	time_t unixTime = time(NULL);
	struct tm *timeStruct = gmtime((const time_t *)&unixTime);
	var_years = timeStruct->tm_year + 1900;
	var_months = timeStruct->tm_mon + 1;
	var_days = timeStruct->tm_mday;
	var_hours = timeStruct->tm_hour;
	var_minutes = timeStruct->tm_min;
	var_seconds = timeStruct->tm_sec;

	if (var_debug_mode) {
		// check free RAM
		// Do not probe the general heap by allocating and freeing large blocks here:
		// repeated probes fragment the heap and can destabilize long video sessions.
		var_free_linear_ram = linearSpaceFree();
	}
}

void Menu_worker_thread(void *arg) {
	logger.info(DEF_MENU_WORKER_THREAD_STR, "Thread started.");
	int count = 0;
	Result_with_string result;

	while (menu_thread_run) {
		usleep(49000);
		count++;

		if (count >= 100) {
			u8 previous_wifi_signal = var_wifi_signal;
			u8 previous_wifi_state = var_wifi_state;
			Menu_get_system_info();
			if (var_wifi_signal != previous_wifi_signal || var_wifi_state != previous_wifi_state) {
				var_need_refresh = true;
			}
			count = 0;
		}

		if (var_time_to_turn_off_lcd != 0) {
			var_afk_time += 0.05;
		}

		static bool cur_screen_on = true;
		static bool cur_screen_dimmed = false;
		// track manual off state for top and bottom separately to detect changes
		static bool cur_top_manual_off = false;
		static bool cur_bot_manual_off = false;

		bool next_screen_dimmed = cur_screen_dimmed;

		// Determine which screens should be turned off based on mode
		// Mode 0: Bottom, Mode 1: Top, Mode 2: Both
		bool allowed_top_off = (var_screen_off_mode == 1 || var_screen_off_mode == 2);
		bool allowed_bot_off = (var_screen_off_mode == 0 || var_screen_off_mode == 2);

		// Manual off logic
		bool top_off_manual = manual_screen_off && (var_screen_off_mode == 1 || var_screen_off_mode == 2);
		bool bot_off_manual = manual_screen_off && (var_screen_off_mode == 0 || var_screen_off_mode == 2);

		// System handles sleep if app is suspended
		bool next_top_manual_off = top_off_manual && !var_app_suspended;
		bool next_bot_manual_off = bot_off_manual && !var_app_suspended;

		bool idle_off_requested = false;
		if (var_time_to_turn_off_lcd != 0 && var_afk_time > var_time_to_turn_off_lcd) {
			idle_off_requested = true;
			next_screen_dimmed = false;
		} else if (var_time_to_turn_off_lcd != 0 &&
		           var_afk_time > std::max<float>(var_time_to_turn_off_lcd * 0.5, (var_time_to_turn_off_lcd - 10))) {
			idle_off_requested = false;
			next_screen_dimmed = true;
		} else {
			idle_off_requested = false;
			next_screen_dimmed = false;
		}

		// Idle off logic (respects screen off mode)
		bool next_top_idle_off = idle_off_requested && allowed_top_off;
		bool next_bot_idle_off = idle_off_requested && allowed_bot_off;

		// Final screen state: ON if neither manually off nor idle off
		bool next_top_on = !next_top_manual_off && !next_top_idle_off;
		bool next_bot_on = !next_bot_manual_off && !next_bot_idle_off;

		// We track the applied state to detect changes
		static bool cur_top_applied_on = true;
		static bool cur_bot_applied_on = true;

		if (cur_top_applied_on != next_top_on || cur_bot_applied_on != next_bot_on ||
		    cur_screen_dimmed != next_screen_dimmed || cur_top_manual_off != next_top_manual_off ||
		    cur_bot_manual_off != next_bot_manual_off) {

			// Handle screen power
			if (cur_top_applied_on == cur_bot_applied_on && cur_top_applied_on != next_top_on &&
			    cur_bot_applied_on != next_bot_on) {
				Util_cset_set_screen_state(true, true, next_top_on);
			} else {
				if (cur_top_applied_on != next_top_on) {
					Util_cset_set_screen_state(true, false, next_top_on);
				}
				if (cur_bot_applied_on != next_bot_on) {
					Util_cset_set_screen_state(false, true, next_bot_on);
				}
			}

			// Handle dimming
			if (cur_screen_dimmed != next_screen_dimmed) {
				Util_cset_set_screen_brightness(allowed_top_off, allowed_bot_off,
				                                next_screen_dimmed ? 10 : var_lcd_brightness);
			}

			cur_top_applied_on = next_top_on;
			cur_bot_applied_on = next_bot_on;
			cur_screen_dimmed = next_screen_dimmed;
			cur_top_manual_off = next_top_manual_off;
			cur_bot_manual_off = next_bot_manual_off;
		}
	}
	logger.info(DEF_MENU_WORKER_THREAD_STR, "Thread exit.");
	threadExit(0);
}
