#ifndef MAPT_MENU_H
#define MAPT_MENU_H

#include "cmd.h"
#include "panel.h"

typedef struct {
	const char *label;
	int cmd; /* CMD_SEP draws a separator line */
} MenuEntry;

typedef struct {
	const char *title;
	const MenuEntry *items; /* terminated by a NULL label */
} Menu;

typedef struct {
	int active;                     /* focused panel, 0 or 1 */
	int sort[2];                    /* SortKey of both panels */
	int desc[2];
	int left_only_upgradeable;
} MenuState;

/* Paint row 0; open_idx is the highlighted menu index or -1. */
void menubar_draw(int open_idx);

/* Return the menu title containing screen column x, or -1. */
int menubar_hit(int x);

/* Run the menu using the current state.  initial selects the first visible
 * top-level menu, and the return value is a Command or CMD_NONE. */
int menubar_run(const MenuState *st, int initial);

#endif
