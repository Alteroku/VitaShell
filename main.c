/*
	VitaShell
	Copyright (C) 2015-2016, TheFloW

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
	TODO:
	- vita2dlib: Handle big images > 4096
	- vita2dlib: Add unicode support
	- NEARLY DONE: Terminate thread / free stack of previous VitaShell when reloading
	- Page skip for hex and text viewer
	- Hex editor byte group size and write ability
	- Moving destination folder to subfolder of source folder prevention
	- Moving a folder to a location where the folder does already exit causes error, so move its content.
	- Duplicate when same location or same name. /lol to /lol - Backup. or overwrite question.
	- Maybe switch to libarchive
	- Shortcuts
	- CPU changement
	- Media player
*/

#include "main.h"
#include "init.h"
#include "io_process.h"
#include "archive.h"
#include "photo.h"
#include "file.h"
#include "text.h"
#include "hex.h"
#include "message_dialog.h"
#include "ime_dialog.h"
#include "language.h"
#include "utils.h"
#include "package_installer.h"

int _newlib_heap_size_user = 32 * 1024 * 1024;

#define MAX_DIR_LEVELS 1024

// File lists
static FileList file_list, mark_list, copy_list;

// Paths
static char cur_file[MAX_PATH_LENGTH], archive_path[MAX_PATH_LENGTH];

// Mount point stat
static SceIoStat mount_point_stat;

// Position
static int base_pos = 0, rel_pos = 0;
static int base_pos_list[MAX_DIR_LEVELS];
static int rel_pos_list[MAX_DIR_LEVELS];
static int dir_level = 0;

// Copy mode
static int copy_mode = COPY_MODE_NORMAL;

// Archive
static int is_in_archive = 0;
static int dir_level_archive = -1;

// Context menu
static int ctx_menu_mode = CONTEXT_MENU_CLOSED;
static int ctx_menu_pos = -1;
static float ctx_menu_width = 0;
static float ctx_menu_max_width = 0;

// Net info
static SceNetEtherAddr mac;
static char ip[16];

// FTP
static char vita_ip[16];
static unsigned short int vita_port;

// Enter and cancel buttons
int SCE_CTRL_ENTER = SCE_CTRL_CROSS, SCE_CTRL_CANCEL = SCE_CTRL_CIRCLE;

// Dialog step
int dialog_step = DIALOG_STEP_NONE;

VitaShellShared *shared_memory = NULL;

void dirLevelUp() {
	base_pos_list[dir_level] = base_pos;
	rel_pos_list[dir_level] = rel_pos;
	dir_level++;
	base_pos_list[dir_level] = 0;
	rel_pos_list[dir_level] = 0;
	base_pos = 0;
	rel_pos = 0;
}

int isInArchive() {
	return is_in_archive;
}

void dirUpCloseArchive() {
	if (isInArchive() && dir_level_archive >= dir_level) {
		is_in_archive = 0;
		archiveClose();
		dir_level_archive = -1;
	}
}

void dirUp() {
	removeEndSlash(file_list.path);

	char *p;

	p = strrchr(file_list.path, '/');
	if (p) {
		p[1] = '\0';
		dir_level--;
		goto DIR_UP_RETURN;
	}

	p = strrchr(file_list.path, ':');
	if (p) {
		if (strlen(file_list.path) - ((p + 1) - file_list.path) > 0) {
			p[1] = '\0';
			dir_level--;
			goto DIR_UP_RETURN;
		}
	}

	strcpy(file_list.path, HOME_PATH);
	dir_level = 0;

DIR_UP_RETURN:
	base_pos = base_pos_list[dir_level];
	rel_pos = rel_pos_list[dir_level];
	dirUpCloseArchive();
}

void refreshFileList() {
	int res = 0;

	do {
		fileListEmpty(&file_list);

		res = fileListGetEntries(&file_list, file_list.path);

		if (res < 0)
			dirUp();
	} while (res < 0);

	while (!fileListGetNthEntry(&file_list, base_pos + rel_pos)) {
		if (rel_pos > 0) {
			rel_pos--;
		} else {
			if (base_pos > 0) {
				base_pos--;
			}
		}
	}
}

void refreshMarkList() {
	FileListEntry *entry = mark_list.head;

	int length = mark_list.length;

	int i;
	for (i = 0; i < length; i++) {
		// Get next entry already now to prevent crash after entry is removed
		FileListEntry *next = entry->next;

		char path[MAX_PATH_LENGTH];
		snprintf(path, MAX_PATH_LENGTH, "%s%s", file_list.path, entry->name);

		// Check if the entry still exits. If not, remove it from list
		SceIoStat stat;
		if (sceIoGetstat(path, &stat) < 0)
			fileListRemoveEntry(&mark_list, entry);

		// Next
		entry = next;
	}
}

void refreshCopyList() {
	FileListEntry *entry = copy_list.head;

	int length = copy_list.length;

	int i;
	for (i = 0; i < length; i++) {
		// Get next entry already now to prevent crash after entry is removed
		FileListEntry *next = entry->next;

		char path[MAX_PATH_LENGTH];
		snprintf(path, MAX_PATH_LENGTH, "%s%s", copy_list.path, entry->name);

		// Check if the entry still exits. If not, remove it from list
		SceIoStat stat;
		if (sceIoGetstat(path, &stat) < 0)
			fileListRemoveEntry(&copy_list, entry);

		// Next
		entry = next;
	}
}

void resetFileLists() {
	memset(&file_list, 0, sizeof(FileList));
	memset(&mark_list, 0, sizeof(FileList));
	memset(&copy_list, 0, sizeof(FileList));

	// Home
	strcpy(file_list.path, HOME_PATH);

	refreshFileList();
}

int handleFile(char *file, FileListEntry *entry) {
	int res = 0;

	int type = getFileType(file);
	switch (type) {
		case FILE_TYPE_CG:
		case FILE_TYPE_ELF:
		case FILE_TYPE_PBP:
		case FILE_TYPE_RAR:
		case FILE_TYPE_7ZIP:
		case FILE_TYPE_ZIP:
			if (isInArchive())
				type = FILE_TYPE_UNKNOWN;

			break;
	}

	switch (type) {
		case FILE_TYPE_UNKNOWN:
			res = textViewer(file);
			break;
			
		case FILE_TYPE_BMP:
		case FILE_TYPE_PNG:
		case FILE_TYPE_JPEG:
			res = photoViewer(file, type, &file_list, entry, &base_pos, &rel_pos);
			break;
			
		case FILE_TYPE_RAR:
		case FILE_TYPE_7ZIP:
		case FILE_TYPE_ZIP:
			res = archiveOpen(file);
			break;

		case FILE_TYPE_VPK:
			initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, "Install this package?");
			dialog_step = DIALOG_STEP_INSTALL_CONFIRM;
			break;
			
		default:
			errorDialog(type);
			break;
	}

	if (res < 0) {
		errorDialog(res);
		return res;
	}

	return type;
}

void drawScrollBar(int pos, int n) {
	if (n > MAX_POSITION) {
		vita2d_draw_rectangle(SCROLL_BAR_X, START_Y, SCROLL_BAR_WIDTH, MAX_ENTRIES * FONT_Y_SPACE, GRAY);

		float y = START_Y + ((pos * FONT_Y_SPACE) / (n * FONT_Y_SPACE)) * (MAX_ENTRIES * FONT_Y_SPACE);
		float height = ((MAX_POSITION * FONT_Y_SPACE) / (n * FONT_Y_SPACE)) * (MAX_ENTRIES * FONT_Y_SPACE);

		vita2d_draw_rectangle(SCROLL_BAR_X, MIN(y, (START_Y + MAX_ENTRIES * FONT_Y_SPACE - height)), SCROLL_BAR_WIDTH, MAX(height, SCROLL_BAR_MIN_HEIGHT), AZURE);
	}
}

void drawShellInfo(char *path) {
	// Title
	pgf_draw_textf(SHELL_MARGIN_X, SHELL_MARGIN_Y, VIOLET, FONT_SIZE, "molecularShell (based on VitaShell %d.%d)", VITASHELL_VERSION_MAJOR, VITASHELL_VERSION_MINOR);

	// Battery
	float battery_x = ALIGN_LEFT(SCREEN_WIDTH - SHELL_MARGIN_X, vita2d_texture_get_width(battery_image));
	vita2d_draw_texture(battery_image, battery_x, SHELL_MARGIN_Y + 3.0f);

	vita2d_texture *battery_bar_image = battery_bar_green_image;

	float percent = scePowerGetBatteryLifePercent() / 100.0f;

	if (percent <= 0.18f) // Like in Livearea
		battery_bar_image = battery_bar_red_image;

	float width = vita2d_texture_get_width(battery_bar_image);
	vita2d_draw_texture_part(battery_bar_image, battery_x + 3.0f + (1.0f - percent) * width, SHELL_MARGIN_Y + 5.0f, (1.0f - percent) * width, 0.0f, percent * width, vita2d_texture_get_height(battery_bar_image));

	// Date & time
	SceRtcTime time;
	sceRtcGetCurrentClockLocalTime(&time);

	char date_string[16];
	getDateString(date_string, date_format, &time);

	char time_string[24];
	getTimeString(time_string, time_format, &time);

	char string[64];
	sprintf(string, "%s  %s", date_string, time_string);
	pgf_draw_text(ALIGN_LEFT(battery_x - 12.0f, vita2d_pgf_text_width(font, FONT_SIZE, string)), SHELL_MARGIN_Y, WHITE, FONT_SIZE, string);

	// TODO: make this more elegant
	// Path
	int line_width = 0;

	int i;
	for (i = 0; i < strlen(path); i++) {
		char ch_width = font_size_cache[(int)path[i]];

		// Too long
		if ((line_width + ch_width) >= MAX_WIDTH)
			break;

		// Increase line width
		line_width += ch_width;
	}

	char path_first_line[256], path_second_line[256];

	strncpy(path_first_line, path, i);
	path_first_line[i] = '\0';

	strcpy(path_second_line, path + i);

	pgf_draw_text(SHELL_MARGIN_X, PATH_Y, LITEGRAY, FONT_SIZE, path_first_line);
	pgf_draw_text(SHELL_MARGIN_X, PATH_Y + FONT_Y_SPACE, LITEGRAY, FONT_SIZE, path_second_line);
	
	// TODO: Tabs
	//pgf_draw_textf(SHELL_MARGIN_X, SCREEN_HEIGHT - SHELL_MARGIN_Y - FONT_Y_SPACE - 2.0f, LITEGRAY, FONT_SIZE, "TABS");
}

enum MenuEntrys {
	MENU_ENTRY_MARK_UNMARK_ALL,
	MENU_ENTRY_EMPTY_1,
//	MENU_ENTRY_SPLIT,
//	MENU_ENTRY_JOIN,
//	MENU_ENTRY_EMPTY_2,
	MENU_ENTRY_MOVE,
	MENU_ENTRY_COPY,
	MENU_ENTRY_PASTE,
	MENU_ENTRY_EMPTY_3,
	MENU_ENTRY_DELETE,
	MENU_ENTRY_RENAME,
	MENU_ENTRY_EMPTY_4,
	MENU_ENTRY_NEW_FOLDER,
};

enum MenuVisibilities {
	VISIBILITY_UNUSED,
	VISIBILITY_INVISIBLE,
	VISIBILITY_VISIBLE,
};

typedef struct {
	int name;
	int visibility;
} MenuEntry;

MenuEntry menu_entries[] = {
	{ MARK_ALL, VISIBILITY_INVISIBLE },
	{ -1, VISIBILITY_UNUSED },
//	{ SPLIT, VISIBILITY_INVISIBLE },
//	{ JOIN, VISIBILITY_INVISIBLE },
//	{ -1, VISIBILITY_UNUSED },
	{ MOVE, VISIBILITY_INVISIBLE },
	{ COPY, VISIBILITY_INVISIBLE },
	{ PASTE, VISIBILITY_INVISIBLE },
	{ -1, VISIBILITY_UNUSED },
	{ DELETE, VISIBILITY_INVISIBLE },
	{ RENAME, VISIBILITY_INVISIBLE },
	{ -1, VISIBILITY_UNUSED },
	{ NEW_FOLDER, VISIBILITY_INVISIBLE },
};

#define N_MENU_ENTRIES (sizeof(menu_entries) / sizeof(MenuEntry))

void initContextMenu() {
	int i;

	// All visible
	for (i = 0; i < N_MENU_ENTRIES; i++) {
		if (menu_entries[i].visibility == VISIBILITY_INVISIBLE)
			menu_entries[i].visibility = VISIBILITY_VISIBLE;
	}

	FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

	// Invisble entries when on '..'
	if (strcmp(file_entry->name, DIR_UP) == 0) {
		menu_entries[MENU_ENTRY_MARK_UNMARK_ALL].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_MOVE].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_COPY].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_DELETE].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_RENAME].visibility = VISIBILITY_INVISIBLE;
		//menu_entries[MENU_ENTRY_SPLIT].visibility = VISIBILITY_INVISIBLE;
		//menu_entries[MENU_ENTRY_JOIN].visibility = VISIBILITY_INVISIBLE;
	}

	// Split/join
/*	if (file_entry->is_folder) {
		menu_entries[MENU_ENTRY_SPLIT].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_JOIN].visibility = VISIBILITY_INVISIBLE;

		char *p = strrchr(file_entry->name, '.');
		if (p) {
			if (strncmp(p, SPLIT_SUFFIX, sizeof(SPLIT_SUFFIX) - 1) == 0) {
				menu_entries[MENU_ENTRY_JOIN].visibility = VISIBILITY_VISIBLE;
			}
		}
	} else {
		menu_entries[MENU_ENTRY_JOIN].visibility = VISIBILITY_INVISIBLE;
	}*/

	// Invisible 'Paste' if nothing is copied yet
	if (copy_list.length == 0)
		menu_entries[MENU_ENTRY_PASTE].visibility = VISIBILITY_INVISIBLE;

	// Invisble write operations in archives or read-only mount points
	if (isInArchive()) {
		menu_entries[MENU_ENTRY_MOVE].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_PASTE].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_DELETE].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_RENAME].visibility = VISIBILITY_INVISIBLE;
		//menu_entries[MENU_ENTRY_SPLIT].visibility = VISIBILITY_INVISIBLE;
		//menu_entries[MENU_ENTRY_JOIN].visibility = VISIBILITY_INVISIBLE;
		menu_entries[MENU_ENTRY_NEW_FOLDER].visibility = VISIBILITY_INVISIBLE;
	}

	// TODO: Moving from one mount point to another is not possible

	// Mark/Unmark all text
	if (mark_list.length == (file_list.length - 1)) { // All marked
		menu_entries[MENU_ENTRY_MARK_UNMARK_ALL].name = UNMARK_ALL;
	} else { // Not all marked yet
		// On marked entry
		if (fileListFindEntry(&mark_list, file_entry->name)) {
			menu_entries[MENU_ENTRY_MARK_UNMARK_ALL].name = UNMARK_ALL;
		} else {
			menu_entries[MENU_ENTRY_MARK_UNMARK_ALL].name = MARK_ALL;
		}
	}

	// Go to first entry
	for (i = 0; i < N_MENU_ENTRIES; i++) {
		if (menu_entries[i].visibility == VISIBILITY_VISIBLE) {
			ctx_menu_pos = i;
			break;
		}
	}

	if (i == N_MENU_ENTRIES)
		ctx_menu_pos = -1;
}

float easeOut(float x0, float x1, float a) {
	float dx = (x1 - x0);
	return ((dx * a) > 0.5f) ? (dx * a) : dx;
}

void drawContextMenu() {
	// Easing out
	if (ctx_menu_mode == CONTEXT_MENU_CLOSING) {
		if (ctx_menu_width > 0.0f) {
			ctx_menu_width -= easeOut(0.0f, ctx_menu_width, 0.375f);
		} else {
			ctx_menu_mode = CONTEXT_MENU_CLOSED;
		}
	}

	if (ctx_menu_mode == CONTEXT_MENU_OPENING) {
		if (ctx_menu_width < ctx_menu_max_width) {
			ctx_menu_width += easeOut(ctx_menu_width, ctx_menu_max_width, 0.375f);
		} else {
			ctx_menu_mode = CONTEXT_MENU_OPENED;
		}
	}

	// Draw context menu
	if (ctx_menu_mode != CONTEXT_MENU_CLOSED) {
		vita2d_draw_rectangle(SCREEN_WIDTH - ctx_menu_width, 0.0f, ctx_menu_width, SCREEN_HEIGHT, COLOR_ALPHA(0xFF2F2F2F, 0xFA));

		int i;
		for (i = 0; i < N_MENU_ENTRIES; i++) {
			if (menu_entries[i].visibility == VISIBILITY_UNUSED)
				continue;

			float y = START_Y + (i * FONT_Y_SPACE);

			uint32_t color = WHITE;

			if (i == ctx_menu_pos)
				color = GREEN;

			if (menu_entries[i].visibility == VISIBILITY_INVISIBLE)
				color = DARKGRAY;

			pgf_draw_text(SCREEN_WIDTH - ctx_menu_width + CONTEXT_MENU_MARGIN, y, color, FONT_SIZE, language_container[menu_entries[i].name]);
		}
	}
}

void contextMenuCtrl() {
	if (hold_buttons & SCE_CTRL_UP || hold2_buttons & SCE_CTRL_LEFT_ANALOG_UP) {
		int i;
		for (i = N_MENU_ENTRIES - 1; i >= 0; i--) {
			if (menu_entries[i].visibility == VISIBILITY_VISIBLE) {
				if (i < ctx_menu_pos) {
					ctx_menu_pos = i;
					break;
				}
			}
		}
	} else if (hold_buttons & SCE_CTRL_DOWN || hold2_buttons & SCE_CTRL_LEFT_ANALOG_DOWN) {
		int i;
		for (i = 0; i < N_MENU_ENTRIES; i++) {
			if (menu_entries[i].visibility == VISIBILITY_VISIBLE) {
				if (i > ctx_menu_pos) {
					ctx_menu_pos = i;
					break;
				}
			}
		}
	}

	// Back
	if (pressed_buttons & SCE_CTRL_TRIANGLE || pressed_buttons & SCE_CTRL_CANCEL) {
		ctx_menu_mode = CONTEXT_MENU_CLOSING;
	}

	// Handle
	if (pressed_buttons & SCE_CTRL_ENTER) {
		switch (ctx_menu_pos) {
			case MENU_ENTRY_MARK_UNMARK_ALL:
			{
				int on_marked_entry = 0;
				int length = mark_list.length;

				FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);
				if (fileListFindEntry(&mark_list, file_entry->name))
					on_marked_entry = 1;

				// Empty mark list
				fileListEmpty(&mark_list);

				// Mark all if not all entries are marked yet and we are not focusing on a marked entry
				if (length != (file_list.length - 1) && !on_marked_entry) {
					FileListEntry *file_entry = file_list.head->next; // Ignore '..'

					int i;
					for (i = 0; i < file_list.length - 1; i++) {
						FileListEntry *mark_entry = malloc(sizeof(FileListEntry));
						memcpy(mark_entry, file_entry, sizeof(FileListEntry));
						fileListAddEntry(&mark_list, mark_entry, SORT_NONE);

						// Next
						file_entry = file_entry->next;
					}
				}

				break;
			}
			/*
			case MENU_ENTRY_SPLIT:
				initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, language_container[SPLIT_QUESTION]);
				dialog_step = DIALOG_STEP_SPLIT_QUESTION;
				break;
				
			case MENU_ENTRY_JOIN:
				initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, language_container[JOIN_QUESTION]);
				dialog_step = DIALOG_STEP_JOIN_QUESTION;
				break;
				*/
			case MENU_ENTRY_MOVE:
			case MENU_ENTRY_COPY:
			{
				// Mode
				if (ctx_menu_pos == MENU_ENTRY_MOVE) {
					copy_mode = COPY_MODE_MOVE;
				} else {
					copy_mode = isInArchive() ? COPY_MODE_EXTRACT : COPY_MODE_NORMAL;
				}

				// Empty copy list at first
				if (copy_list.length > 0)
					fileListEmpty(&copy_list);

				FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

				// Paths
				if (fileListFindEntry(&mark_list, file_entry->name)) { // On marked entry
					// Copy mark list to copy list
					FileListEntry *mark_entry = mark_list.head;

					int i;
					for (i = 0; i < mark_list.length; i++) {
						FileListEntry *copy_entry = malloc(sizeof(FileListEntry));
						memcpy(copy_entry, mark_entry, sizeof(FileListEntry));
						fileListAddEntry(&copy_list, copy_entry, SORT_NONE);

						// Next
						mark_entry = mark_entry->next;
					}
				} else {
					FileListEntry *copy_entry = malloc(sizeof(FileListEntry));
					memcpy(copy_entry, file_entry, sizeof(FileListEntry));
					fileListAddEntry(&copy_list, copy_entry, SORT_NONE);
				}

				strcpy(copy_list.path, file_list.path);

				char *message;

				// On marked entry
				if (fileListFindEntry(&copy_list, file_entry->name)) {
					if (copy_list.length == 1) {
						message = language_container[file_entry->is_folder ? COPIED_FOLDER : COPIED_FILE];
					} else {
						message = language_container[COPIED_FILES_FOLDERS];
					}
				} else {
					message = language_container[file_entry->is_folder ? COPIED_FOLDER : COPIED_FILE];
				}

				// Copy message
				infoDialog(message, copy_list.length);

				break;
			}

			case MENU_ENTRY_PASTE:
				initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, language_container[copy_mode == COPY_MODE_MOVE ? MOVING : COPYING]);
				dialog_step = DIALOG_STEP_PASTE;
				break;

			case MENU_ENTRY_DELETE:
			{
				char *message;

				FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

				// On marked entry
				if (fileListFindEntry(&mark_list, file_entry->name)) {
					if (mark_list.length == 1) {
						message = language_container[file_entry->is_folder ? DELETE_FOLDER_QUESTION : DELETE_FILE_QUESTION];
					} else {
						message = language_container[DELETE_FILES_FOLDERS_QUESTION];
					}
				} else {
					message = language_container[file_entry->is_folder ? DELETE_FOLDER_QUESTION : DELETE_FILE_QUESTION];
				}

				initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, message);
				dialog_step = DIALOG_STEP_DELETE_QUESTION;
				break;
			}
#if 0
			case MENU_ENTRY_RENAME:
			{
				FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

				char name[MAX_NAME_LENGTH];
				strcpy(name, file_entry->name);
				removeEndSlash(name);

				initImeDialog(language_container[RENAME], name, MAX_NAME_LENGTH);

				dialog_step = DIALOG_STEP_RENAME;
				break;
			}
			
			case MENU_ENTRY_NEW_FOLDER:
			{
				// Find a new folder name
				char path[MAX_PATH_LENGTH];

				int count = 1;
				while (1) {
					if (count == 1) {
						snprintf(path, MAX_PATH_LENGTH, "%s%s", file_list.path, language_container[NEW_FOLDER]);
					} else {
						snprintf(path, MAX_PATH_LENGTH, "%s%s (%d)", file_list.path, language_container[NEW_FOLDER], count);
					}

					SceIoStat stat;
					if (sceIoGetstat(path, &stat) < 0)
						break;

					count++;
				}

				initImeDialog(language_container[NEW_FOLDER], path + strlen(file_list.path), MAX_NAME_LENGTH);
				dialog_step = DIALOG_STEP_NEW_FOLDER;
				break;
			}
#endif
		}

		ctx_menu_mode = CONTEXT_MENU_CLOSING;
	}
}

int dialogSteps() {
	int refresh = 0;

	int msg_result = updateMessageDialog();
#if 0
	int ime_result = updateImeDialog();
#endif

	switch (dialog_step) {
		// Without refresh
		case DIALOG_STEP_ERROR:
		case DIALOG_STEP_INFO:
		case DIALOG_STEP_SYSTEM:
			if (msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		// With refresh
		case DIALOG_STEP_COPIED:
		case DIALOG_STEP_DELETED:
		case DIALOG_STEP_SPLITTED:
		case DIALOG_STEP_JOINED:
		case DIALOG_STEP_INSTALLED:
			if (msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
				refresh = 1;
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_MOVED:
			if (msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
				fileListEmpty(&copy_list);
				refresh = 1;
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_FTP:
			disableAutoSuspend();

			if (msg_result == MESSAGE_DIALOG_RESULT_FINISHED) {
				ftpvita_fini();
				refresh = 1;
				dialog_step = DIALOG_STEP_NONE;
			}

			break;

		case DIALOG_STEP_INSTALL_CONFIRM:
			if (msg_result == MESSAGE_DIALOG_RESULT_YES) {
				initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, "Installing...");
				dialog_step = DIALOG_STEP_INSTALL_CONFIRMED;
			} else if (msg_result == MESSAGE_DIALOG_RESULT_NO) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;

		case DIALOG_STEP_INSTALL_CONFIRMED:
			if (msg_result == MESSAGE_DIALOG_RESULT_RUNNING) {
				InstallArguments args = {0};
				args.file = cur_file;

				SceUID thid = sceKernelCreateThread("install_thread", (SceKernelThreadEntry)install_thread, 0x40, 0x10000, 0, 0, NULL);
				if (thid >= 0)
					sceKernelStartThread(thid, sizeof(args), &args);

				dialog_step = DIALOG_STEP_INSTALLING;
			}

			break;
			
		case DIALOG_STEP_PASTE:
			if (msg_result == MESSAGE_DIALOG_RESULT_RUNNING) {
				CopyArguments args;
				args.file_list = &file_list;
				args.copy_list = &copy_list;
				args.archive_path = archive_path;
				args.copy_mode = copy_mode;

				SceUID thid = sceKernelCreateThread("copy_thread", (SceKernelThreadEntry)copy_thread, 0x40, 0x10000, 0, 0, NULL);
				if (thid >= 0)
					sceKernelStartThread(thid, sizeof(CopyArguments), &args);

				dialog_step = DIALOG_STEP_COPYING;
			}

			break;
			
		case DIALOG_STEP_DELETE_QUESTION:
			if (msg_result == MESSAGE_DIALOG_RESULT_YES) {
				initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, language_container[DELETING]);
				dialog_step = DIALOG_STEP_DELETE_CONFIRMED;
			} else if (msg_result == MESSAGE_DIALOG_RESULT_NO) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_DELETE_CONFIRMED:
			if (msg_result == MESSAGE_DIALOG_RESULT_RUNNING) {
				DeleteArguments args;
				args.file_list = &file_list;
				args.mark_list = &mark_list;
				args.index = base_pos + rel_pos;

				SceUID thid = sceKernelCreateThread("delete_thread", (SceKernelThreadEntry)delete_thread, 0x40, 0x10000, 0, 0, NULL);
				if (thid >= 0)
					sceKernelStartThread(thid, sizeof(DeleteArguments), &args);

				dialog_step = DIALOG_STEP_DELETING;
			}

			break;
#if 0
		case DIALOG_STEP_RENAME:
			if (ime_result == IME_DIALOG_RESULT_FINISHED) {
				char *name = (char *)getImeDialogInputTextUTF8();
				if (strlen(name) > 0) {
					FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);

					char old_path[MAX_PATH_LENGTH];
					char new_path[MAX_PATH_LENGTH];

					snprintf(old_path, MAX_PATH_LENGTH, "%s%s", file_list.path, file_entry->name);
					snprintf(new_path, MAX_PATH_LENGTH, "%s%s", file_list.path, name);

					int res = sceIoRename(old_path, new_path);
					if (res < 0) {
						errorDialog(res);
					} else {
						refresh = 1;
						dialog_step = DIALOG_STEP_NONE;
					}
				}
			} else if (ime_result == IME_DIALOG_RESULT_CANCELED) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_NEW_FOLDER:
			if (ime_result == IME_DIALOG_RESULT_FINISHED) {
				char *name = (char *)getImeDialogInputTextUTF8();
				if (strlen(name) > 0) {
					char path[MAX_PATH_LENGTH];
					snprintf(path, MAX_PATH_LENGTH, "%s%s", file_list.path, name);

					int res = sceIoMkdir(path, 0777);
					if (res < 0) {
						errorDialog(res);
					} else {
						refresh = 1;
						dialog_step = DIALOG_STEP_NONE;
					}
				}
			} else if (ime_result == IME_DIALOG_RESULT_CANCELED) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
#endif
		case DIALOG_STEP_SPLIT_QUESTION:
			if (msg_result == MESSAGE_DIALOG_RESULT_YES) {
				initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, language_container[SPLITTING]);
				dialog_step = DIALOG_STEP_SPLIT_CONFIRMED;
			} else if (msg_result == MESSAGE_DIALOG_RESULT_NO) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_SPLIT_CONFIRMED:
			if (msg_result == MESSAGE_DIALOG_RESULT_RUNNING) {
				SplitArguments args;
				args.file_list = &file_list;
				args.index = base_pos + rel_pos;

				SceUID thid = sceKernelCreateThread("split_thread", (SceKernelThreadEntry)split_thread, 0x40, 0x10000, 0, 0, NULL);
				if (thid >= 0)
					sceKernelStartThread(thid, sizeof(DeleteArguments), &args);

				dialog_step = DIALOG_STEP_SPLITTING;
			}

			break;
			
		case DIALOG_STEP_JOIN_QUESTION:
			if (msg_result == MESSAGE_DIALOG_RESULT_YES) {
				initMessageDialog(MESSAGE_DIALOG_PROGRESS_BAR, language_container[JOINING]);
				dialog_step = DIALOG_STEP_JOIN_CONFIRMED;
			} else if (msg_result == MESSAGE_DIALOG_RESULT_NO) {
				dialog_step = DIALOG_STEP_NONE;
			}

			break;
			
		case DIALOG_STEP_JOIN_CONFIRMED:
			if (msg_result == MESSAGE_DIALOG_RESULT_RUNNING) {
				JoinArguments args;
				args.file_list = &file_list;
				args.index = base_pos + rel_pos;

				SceUID thid = sceKernelCreateThread("join_thread", (SceKernelThreadEntry)join_thread, 0x40, 0x10000, 0, 0, NULL);
				if (thid >= 0)
					sceKernelStartThread(thid, sizeof(DeleteArguments), &args);

				dialog_step = DIALOG_STEP_JOINING;
			}

			break;
	}

	return refresh;
}

void fileBrowserMenuCtrl() {
	// Hidden trigger
	if (current_buttons & SCE_CTRL_LTRIGGER && current_buttons & SCE_CTRL_RTRIGGER && current_buttons & SCE_CTRL_START) {
		SwVersionParam sw_ver_param;
		sw_ver_param.size = sizeof(SwVersionParam);
		sceKernelGetSystemSwVersion(&sw_ver_param);

		char mac_string[32];
		sprintf(mac_string, "%02X:%02X:%02X:%02X:%02X:%02X", mac.data[0], mac.data[1], mac.data[2], mac.data[3], mac.data[4], mac.data[5]);

		uint64_t free_size = 0, max_size = 0;
		sceAppMgrGetDevInfo("ux0:", &max_size, &free_size);

		char free_size_string[16], max_size_string[16];
		getSizeString(free_size_string, free_size);
		getSizeString(max_size_string, max_size);

		initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_OK, "System software: %s\nModel: 0x%08X\nMAC address: %s\nIP address: %s\nMemory card: %s/%s", sw_ver_param.version_string, sceKernelGetModelForCDialog(), mac_string, ip, free_size_string, max_size_string);
		dialog_step = DIALOG_STEP_SYSTEM;
	}

	if (pressed_buttons & SCE_CTRL_SELECT) {
		if (!ftpvita_is_initialized()) {
			int res = ftpvita_init(vita_ip, &vita_port);
			if (res < 0) {
				infoDialog(language_container[WIFI_ERROR]);
			} else {
				// Add all the current mountpoints to ftpvita
				int i;
				for (i = 0; i < getNumberMountPoints(); i++) {
					char **mount_points = getMountPoints();
					if (mount_points[i] && strcmp(mount_points[i], HOST0) != 0) {
						ftpvita_add_device(mount_points[i]);
					}
				}

				initMessageDialog(SCE_MSG_DIALOG_BUTTON_TYPE_CANCEL, language_container[FTP_SERVER], vita_ip, vita_port);
				dialog_step = DIALOG_STEP_FTP;
			}
		}
	}

	// Move
	if (hold_buttons & SCE_CTRL_UP || hold2_buttons & SCE_CTRL_LEFT_ANALOG_UP) {
		if (rel_pos > 0) {
			rel_pos--;
		} else {
			if (base_pos > 0) {
				base_pos--;
			}
		}
	} else if (hold_buttons & SCE_CTRL_DOWN || hold2_buttons & SCE_CTRL_LEFT_ANALOG_DOWN) {
		if ((rel_pos + 1) < file_list.length) {
			if ((rel_pos + 1) < MAX_POSITION) {
				rel_pos++;
			} else {
				if ((base_pos + rel_pos + 1) < file_list.length) {
					base_pos++;
				}
			}
		}
	}

	// Not at 'home'
	if (dir_level > 0) {
		// Context menu trigger
		if (pressed_buttons & SCE_CTRL_TRIANGLE) {
			if (ctx_menu_mode == CONTEXT_MENU_CLOSED) {
				initContextMenu();
				ctx_menu_mode = CONTEXT_MENU_OPENING;
			}
		}

		// Mark entry
		if (pressed_buttons & SCE_CTRL_SQUARE) {
			FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);
			if (strcmp(file_entry->name, DIR_UP) != 0) {
				if (!fileListFindEntry(&mark_list, file_entry->name)) {
					FileListEntry *mark_entry = malloc(sizeof(FileListEntry));
					memcpy(mark_entry, file_entry, sizeof(FileListEntry));
					fileListAddEntry(&mark_list, mark_entry, SORT_NONE);
				} else {
					fileListRemoveEntryByName(&mark_list, file_entry->name);
				}
			}
		}

		// Back
		if (pressed_buttons & SCE_CTRL_CANCEL) {
			fileListEmpty(&mark_list);
			dirUp();
			refreshFileList();
		}
	}

	// Handle
	if (pressed_buttons & SCE_CTRL_ENTER) {
		fileListEmpty(&mark_list);

		// Handle file or folder
		FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos + rel_pos);
		if (file_entry->is_folder) {
			if (strcmp(file_entry->name, DIR_UP) == 0) {
				dirUp();
			} else {
				if (dir_level == 0) {
					strcpy(file_list.path, file_entry->name);
					memset(&mount_point_stat, 0, sizeof(SceIoStat));
					sceIoGetstat(file_entry->name, &mount_point_stat);
				} else {
					if (dir_level > 1)
						addEndSlash(file_list.path);
					strcat(file_list.path, file_entry->name);
				}

				dirLevelUp();
			}

			refreshFileList();
		} else {
			snprintf(cur_file, MAX_PATH_LENGTH, "%s%s", file_list.path, file_entry->name);
			int type = handleFile(cur_file, file_entry);

			// Archive mode
			if (type == FILE_TYPE_7ZIP || type == FILE_TYPE_RAR || type == FILE_TYPE_ZIP) {
				is_in_archive = 1;
				dir_level_archive = dir_level;

				snprintf(archive_path, MAX_PATH_LENGTH, "%s%s", file_list.path, file_entry->name);

				strcat(file_list.path, file_entry->name);
				addEndSlash(file_list.path);

				dirLevelUp();
				refreshFileList();
			}
		}
	}
}

int shellMain() {
	// Position
	memset(base_pos_list, 0, sizeof(base_pos_list));
	memset(rel_pos_list, 0, sizeof(rel_pos_list));

	// Paths
	memset(cur_file, 0, sizeof(cur_file));
	memset(archive_path, 0, sizeof(archive_path));

	// Reset file lists
	resetFileLists();

	while (1) {
		readPad();

		int refresh = 0;

		// Control
		if (dialog_step == DIALOG_STEP_NONE) {
			if (ctx_menu_mode != CONTEXT_MENU_CLOSED) {
				contextMenuCtrl();
			} else {
				fileBrowserMenuCtrl();
			}
		} else {
			refresh = dialogSteps();
		}

		if (refresh) {
			// Refresh lists
			refreshFileList();
			refreshMarkList();
			refreshCopyList();
		}

		// Start drawing
		START_DRAWING();

		// Draw shell info
		drawShellInfo(file_list.path);

		// Draw scroll bar
		drawScrollBar(base_pos, file_list.length);

		// Draw
		FileListEntry *file_entry = fileListGetNthEntry(&file_list, base_pos);

		int i;
		for (i = 0; i < MAX_ENTRIES && (base_pos + i) < file_list.length; i++) {
			uint32_t color = WHITE;

			if (file_entry->is_folder)
				color = CYAN;
			if (getFileType(file_entry->name) == FILE_TYPE_VPK)
				color = 0xFFFF55FF;
/*
			if (file_entry->type == FILE_TYPE_RAR || file_entry->type == FILE_TYPE_7ZIP || file_entry->type == FILE_TYPE_ZIP) {
				color = YELLOW;
			}
*/
			if (i == rel_pos)
				color = GREEN;

			float y = START_Y + (i * FONT_Y_SPACE);

			// Marked
			if (fileListFindEntry(&mark_list, file_entry->name))
				vita2d_draw_rectangle(SHELL_MARGIN_X, y + 3.0f, MARK_WIDTH, FONT_Y_SPACE, COLOR_ALPHA(AZURE, 0x4F));

			// File name
			int length = strlen(file_entry->name);
			int line_width = 0;

			int j;
			for (j = 0; j < length; j++) {
				char ch_width = font_size_cache[(int)file_entry->name[j]];

				// Too long
				if ((line_width + ch_width) >= MAX_NAME_WIDTH)
					break;

				// Increase line width
				line_width += ch_width;
			}

			char ch = 0;

			if (j != length) {
				ch = file_entry->name[j];
				file_entry->name[j] = '\0';
			}

			// Draw shortened file name
			pgf_draw_text(SHELL_MARGIN_X, y, color, FONT_SIZE, file_entry->name);

			if (j != length)
				file_entry->name[j] = ch;

			// File information
			if (strcmp(file_entry->name, DIR_UP) != 0) {
				// Folder/size
				char size_string[16];
				getSizeString(size_string, file_entry->size);

				char *str = file_entry->is_folder ? language_container[FOLDER] : size_string;

				pgf_draw_text(ALIGN_LEFT(INFORMATION_X, vita2d_pgf_text_width(font, FONT_SIZE, str)), y, color, FONT_SIZE, str);

				// Date
				char date_string[16];
				getDateString(date_string, date_format, &file_entry->time);

				char time_string[24];
				getTimeString(time_string, time_format, &file_entry->time);

				char string[64];
				sprintf(string, "%s %s", date_string, time_string);

				pgf_draw_text(ALIGN_LEFT(SCREEN_WIDTH - SHELL_MARGIN_X, vita2d_pgf_text_width(font, FONT_SIZE, string)), y, color, FONT_SIZE, string);
			}

			// Next
			file_entry = file_entry->next;
		}

		// Draw context menu
		drawContextMenu();

		// End drawing
		END_DRAWING();
	}

	// Empty lists
	fileListEmpty(&copy_list);
	fileListEmpty(&mark_list);
	fileListEmpty(&file_list);

	return 0;
}

void initShell() {
	int i;
	for (i = 0; i < N_MENU_ENTRIES; i++) {
		if (menu_entries[i].visibility != VISIBILITY_UNUSED)
			ctx_menu_max_width = MAX(ctx_menu_max_width, vita2d_pgf_text_width(font, FONT_SIZE, language_container[menu_entries[i].name]));

		if (menu_entries[i].name == MARK_ALL) {
			menu_entries[i].name = UNMARK_ALL;
			i--;
		}
	}

	ctx_menu_max_width += 2.0f * CONTEXT_MENU_MARGIN;
	ctx_menu_max_width = MAX(ctx_menu_max_width, CONTEXT_MENU_MIN_WIDTH);
}

void getNetInfo() {
	static char memory[16 * 1024];

	SceNetInitParam param;
	param.memory = memory;
	param.size = sizeof(memory);
	param.flags = 0;

	int net_init = sceNetInit(&param);
	int netctl_init = sceNetCtlInit();

	// Get mac address
	sceNetGetMacAddress(&mac, 0);

	// Get IP
	SceNetCtlInfo info;
	if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0) {
		strcpy(ip, "-");
	} else {
		strcpy(ip, info.ip_address);
	}

	if (netctl_init >= 0)
		sceNetCtlTerm();

	if (net_init >= 0)
		sceNetTerm();
}

int initSharedMemory() {
	int res = 0;

	SceUID blockid = sceKernelOpenMemBlock("VitaShellShared", 0x10);
	if (blockid >= 0) {
		res = sceKernelGetMemBlockBase(blockid, (void *)&shared_memory);
		if (res >= 0) {
			res = sceKernelGetMemBlockBase(shared_memory->shared_blockid, (void *)&shared_memory);
		}

		sceKernelCloseMemBlock(blockid);
	} else {
		int i;
		for (i = sizeof(SceKernelAllocMemBlockOpt); i > 0; i -= 4) {
			SceKernelAllocMemBlockOpt option;
			memset(&option, 0, sizeof(SceKernelAllocMemBlockOpt));
			option.size = i;
			option.attr = 0x4020;
			option.flags = 0x10;

			SceUID blockid = sceKernelAllocMemBlock("VitaShellShared", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, ALIGN(sizeof(VitaShellShared), 0x1000), &option);
			if (blockid < 0 && blockid != 0x80020009) {
				return blockid;
			}

			res = sceKernelGetMemBlockBase(blockid, (void *)&shared_memory);
			if (res >= 0) {
				/* Init shared memory */
				memset((void *)shared_memory, 0, sizeof(VitaShellShared));
				shared_memory->shared_blockid = blockid;
				shared_memory->code_blockid = INVALID_UID;
				shared_memory->data_blockid = INVALID_UID;
			}

			if (blockid >= 0)
				break;
		}
	}

	return res;
}

int main(int argc, const char *argv[]) {
#ifndef RELEASE
	sceIoRemove(TEMP_BASE "vitashell_log.txt");
#endif

	// Init VitaShell
	initVitaShell();

	// Init shared memory
	initSharedMemory();

	// Free previous data
	if (shared_memory->data_blockid >= 0) {
		sceKernelFreeMemBlock(shared_memory->data_blockid);
	}

	// Get net info
	getNetInfo();

	// Load language
	loadLanguage(language);

	// Main
	initShell();
	shellMain();

	// Finish VitaShell
	finishVitaShell();

	return 0;
}
