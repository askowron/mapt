#ifndef MAPT_DIALOG_H
#define MAPT_DIALOG_H

#include <stddef.h>

/* Return codes of dlg_menu(). */
#define DLG_MENU_CANCEL (-1)
#define DLG_MENU_LEFT   (-2)
#define DLG_MENU_RIGHT  (-3)
#define DLG_MENU_TOP    (-4) /* a top-level menu was clicked */

/* Show a message.  title and body are display text; is_error selects red
 * error styling. */
void dlg_alert(const char *title, const char *body, int is_error);

/* Ask a yes/no question.  default_yes selects the initially highlighted
 * button; the return value is 1 for Yes and 0 for No/cancel. */
int dlg_confirm(const char *title, const char *body, int default_yes);

/* Edit a line.  buf/cap describe the caller-owned buffer, prompt is the
 * optional label, and hidden masks the characters when non-zero. */
int dlg_input(const char *title, const char *prompt, char *buf, size_t cap,
	      int hidden);

/* Edit a hidden password line with the red authentication style. */
int dlg_password(const char *title, const char *prompt, char *buf, size_t cap);

/* Show scrollable text.  title and text are display-only strings. */
void dlg_text(const char *title, const char *text);

/* Show a dropdown.  items contains n labels, initial is the highlighted
 * item, y/x are the requested screen position, and top_x receives the
 * screen column when the menu bar itself is clicked. */
int dlg_menu(const char *const *items, int n, int initial, int y, int x,
	     int *top_x);

/* Run argv in a window with live output.  stdin_data is optional input
 * for the child; the return value is its exit status or a negative error. */
int dlg_command(const char *title, char *const argv[],
		const char *stdin_data);

/* Display the built-in keyboard and mouse help. */
void dlg_help(void);

#endif
