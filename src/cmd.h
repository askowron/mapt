/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 APPIT Adam Skowroński
 */
#ifndef MAPT_CMD_H
#define MAPT_CMD_H

/* Commands produced by the menu bar and by the key dispatcher.
 * CMD_NONE doubles as "key already consumed, do nothing". */
typedef enum {
	CMD_NONE = 0,
	CMD_QUIT,
	CMD_HELP,
	CMD_ABOUT,
	CMD_MENU,
	CMD_INFO,
	CMD_CONTENTS,
	CMD_FIND,
	CMD_RELOAD,
	CMD_INSTALL,
	CMD_REINSTALL,
	CMD_UPGRADE,
	CMD_UPGRADE_ALL,
	CMD_UPDATE_UPGRADE,
	CMD_DIST_UPGRADE,
	CMD_REMOVE,
	CMD_PURGE,
	CMD_AUTOREMOVE,
	CMD_UPDATE,
	CMD_MARK_TOGGLE,
	CMD_MARK_PATTERN,
	CMD_MARK_CLEAR,
	CMD_SORT_NAME,
	CMD_SORT_VERSION,
	CMD_SORT_SIZE,
	CMD_SORT_REVERSE,
	CMD_PANEL_MODE
} Command;

/* Pseudo command used by menus to draw a horizontal separator line. */
#define CMD_SEP (-9999)

#endif
