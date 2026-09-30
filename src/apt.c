/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 APPIT Adam Skowroński
 */
#include "apt.h"
#include "dialog.h"
#include "proc.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * Output capture                                                       *
 * ------------------------------------------------------------------ */

typedef struct {
	Buf out;
	Buf err;
} Cap;

/* Function: Initialize the standard-output and standard-error capture buffers.
 * Parameters:
 *   c (Cap *): Caller-owned capture object; must be non-NULL and uninitialized.
 * Return (void): No return value.
 */
static void cap_init(Cap *c)
{
	buf_init(&c->out);
	buf_init(&c->err);
}

/* Function: Release both capture buffers and leave c reusable.
 * Parameters:
 *   c (Cap *): Non-NULL capture initialized by cap_init().
 * Return (void): No return value.
 */
static void cap_free(Cap *c)
{
	buf_free(&c->out);
	buf_free(&c->err);
}

/* Function: Append one child-output line, including its newline, to the proper buffer.
 * Parameters:
 *   ud (void *): Cast to a Cap *, supplied unchanged by proc_run().
 *   line (const char *): NUL-terminated line valid only for the duration of this call.
 *   is_err (int): Zero for standard output; nonzero for standard error.
 * Return (void): No return value.
 */
static void cap_cb(void *ud, const char *line, int is_err)
{
	Cap *c = ud;
	Buf *b = is_err ? &c->err : &c->out;

	buf_puts(b, line);
	buf_putc(b, '\n');
}

/* Function: Run a command while capturing both output streams into c.
 * Parameters:
 *   argv (char *const []): Non-NULL, NULL-terminated argument vector; strings are borrowed.
 *   c (Cap *): Uninitialized caller-owned capture; release it with cap_free().
 * Return (int): The proc_run() exit status, 128 plus a terminating signal, or -1 if the child could not be started.
 */
static int cap_run(char *const argv[], Cap *c)
{
	cap_init(c);
	return proc_run(argv, NULL, cap_cb, NULL, c);
}

/* ------------------------------------------------------------------ *
 * Privilege handling                                                   *
 * ------------------------------------------------------------------ */

/* Function: Determine whether package-changing commands can use sudo.
 * Parameters: None.
 * Return (int): 0 when already root, 1 when cached sudo works, 2 when a password is required, or -1 when sudo could not be started.
 */
static int sudo_mode(void)
{
	char *a[] = { "sudo", "-n", "true", NULL };
	int st;

	if (geteuid() == 0)
		return 0;
	st = proc_run(a, NULL, NULL, NULL, NULL);
	if (st == 0)
		return 1;
	if (st == 127 || st < 0)
		return -1;
	return 2;
}

/* Function: Prompt for the sudo password and validate it without running a package action.
 * Parameters: None.
 * Return (int): 1 when sudo credentials are refreshed, otherwise 0 after user cancellation or a rejected password.
 */
static int authenticate(void)
{
	char pw[128];
	char *stdin_data;
	size_t len;
	char *a[] = { "sudo", "-S", "-v", NULL };
	Cap cap;
	int st;

	memset(pw, 0, sizeof(pw));
	/* Use the dedicated hidden field so authentication is visually
	 * distinct from ordinary search and marking inputs. */
	if (dlg_password("Authentication required",
			 "Root password (used for this operation):",
			 pw, sizeof(pw)) != 0)
		return 0; /* cancelled */

	len = strlen(pw);
	stdin_data = xmalloc(len + 2);
	memcpy(stdin_data, pw, len);
	stdin_data[len] = '\n';
	stdin_data[len + 1] = '\0';
	memset(pw, 0, sizeof(pw));

	cap_init(&cap); /* cap_cb() appends sudo's output to this */
	st = proc_run(a, stdin_data, cap_cb, NULL, &cap);
	free(stdin_data);
	cap_free(&cap);
	if (st != 0) {
		dlg_alert("Authentication failed",
			  "The password you entered is not correct.", 1);
		return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------ *
 * Running apt-get                                                      *
 * ------------------------------------------------------------------ */

/* Function: Run an apt argument vector with the minimum required privilege.
 * Parameters:
 *   title (const char *): Non-NULL display-only command-window title.
 *   args (const Argv *): Initialized non-NULL command with at least one element; its element strings are borrowed.
 * Return (int): dlg_command()'s status, or -1 if sudo is unavailable or authentication is cancelled.
 */
static int apt_run(const char *title, const Argv *args)
{
	Argv av;
	int mode = sudo_mode();
	int status;
	size_t i;

	if (mode == -1) {
		dlg_alert("sudo not found",
			  "mapt needs sudo to change packages.\n"
			  "Run it as root instead.", 1);
		return -1;
	}
	if (mode == 2 && !authenticate())
		return -1;

	argv_init(&av);
	if (mode != 0) {
		argv_push(&av, "sudo");
		argv_push(&av, "-n");
		argv_push(&av, "--");
	}
	for (i = 0; i < args->n; i++)
		argv_push(&av, args->v[i]);

	status = dlg_command(title, av.v, NULL);
	argv_free(&av);
	return status;
}

/* Function: Run an apt-get package subcommand with automatic confirmation and progress.
 * Parameters:
 *   title (const char *): Non-NULL display-only command-window title.
 *   subcmd (const char *): Non-NULL apt-get subcommand token.
 *   names (char *const *): Array of n borrowed package-name strings; must be non-NULL.
 *   n (size_t): Number of entries in names; must be greater than zero.
 *   extra (const char *): Optional borrowed argument placed before the common options.
 * Return (int): apt_run()'s status, or -1 when names is NULL or n is zero.
 */
static int run_names(const char *title, const char *subcmd,
		     char *const *names, size_t n, const char *extra)
{
	Argv a;
	size_t i;
	int st;

	if (!names || !n)
		return -1;
	argv_init(&a);
	argv_push(&a, "apt-get");
	argv_push(&a, (char *)subcmd);
	if (extra)
		argv_push(&a, (char *)extra);
	argv_push(&a, "-y");
	/* dpkg phase percentages drive the progress bar of the dialog */
	argv_push(&a, "--show-progress");
	argv_push(&a, "--");
	for (i = 0; i < n; i++)
		argv_push(&a, names[i]);
	st = apt_run(title, &a);
	argv_free(&a);
	return st;
}

/* Function: Refresh the local apt package lists.
 * Parameters: None.
 * Return (int): apt_run()'s command status; a negative value denotes cancellation, unavailable sudo, or failure to start the child.
 */
int apt_update(void)
{
	Argv a;
	int st;

	argv_init(&a);
	argv_push(&a, "apt-get");
	argv_push(&a, "update");
	st = apt_run("Updating package lists", &a);
	argv_free(&a);
	return st;
}

/* Function: Run "apt-get <sub> -y --show-progress" without a package list.
 * Parameters:
 *   sub (const char *): Non-NULL apt-get subcommand token, such as "upgrade".
 *   title (const char *): Non-NULL display-only command-window title.
 * Return (int): apt_run()'s command status.
 */
static int apt_get(const char *sub, const char *title)
{
	Argv a;
	int st;

	argv_init(&a);
	argv_push(&a, "apt-get");
	argv_push(&a, (char *)sub);
	argv_push(&a, "-y");
	argv_push(&a, "--show-progress");
	st = apt_run(title, &a);
	argv_free(&a);
	return st;
}

/* Function: Upgrade all installed packages.
 * Parameters: None.
 * Return (int): apt_get()'s command status.
 */
int apt_upgrade_all(void)
{
	return apt_get("upgrade", "Upgrading all packages");
}

/* Function: Refresh package lists and then upgrade all packages in one command window.
 * Parameters: None.
 * Return (int): apt_run()'s command status.
 */
int apt_update_upgrade(void)
{
	Argv a;
	int st;

	argv_init(&a);
	argv_push(&a, "sh");
	argv_push(&a, "-c");
	/* "apt update; apt upgrade" as one command window: the update
	 * phase runs on the spinner, the upgrade phase drives the bar. */
	argv_push(&a,
		  "apt-get update; apt-get upgrade -y --show-progress");
	st = apt_run("Updating and upgrading", &a);
	argv_free(&a);
	return st;
}

/* Function: Run apt-get's distribution-wide upgrade operation.
 * Parameters: None.
 * Return (int): apt_get()'s command status.
 */
int apt_dist_upgrade(void)
{
	return apt_get("dist-upgrade", "Dist-upgrading all packages");
}

/* Function: Remove packages that were installed automatically and are no longer needed.
 * Parameters: None.
 * Return (int): apt_get()'s command status.
 */
int apt_autoremove(void)
{
	return apt_get("autoremove", "Removing unused packages");
}

/* Function: Install the named packages with apt-get.
 * Parameters:
 *   title (const char *): Non-NULL display-only command-window title.
 *   names (char *const *): Array of n borrowed, non-NULL package-name strings.
 *   n (size_t): Number of names; zero or a NULL names is rejected.
 * Return (int): run_names()'s command status, or -1 for invalid input or cancellation.
 */
int apt_install(const char *title, char *const *names, size_t n)
{
	return run_names(title, "install", names, n, NULL);
}

/* Function: Reinstall the named packages with apt-get.
 * Parameters:
 *   title (const char *): Non-NULL display-only command-window title.
 *   names (char *const *): Array of n borrowed, non-NULL package-name strings.
 *   n (size_t): Number of names; zero or a NULL names is rejected.
 * Return (int): run_names()'s command status, or -1 for invalid input or cancellation.
 */
int apt_reinstall(const char *title, char *const *names, size_t n)
{
	return run_names(title, "install", names, n, "--reinstall");
}

/* Function: Remove or purge the named packages with apt-get.
 * Parameters:
 *   title (const char *): Non-NULL display-only command-window title.
 *   names (char *const *): Array of n borrowed, non-NULL package-name strings.
 *   n (size_t): Number of names; zero or a NULL names is rejected.
 *   purge (int): Nonzero selects "apt-get purge"; zero selects "apt-get remove".
 * Return (int): run_names()'s command status, or -1 for invalid input or cancellation.
 */
int apt_remove(const char *title, char *const *names, size_t n, int purge)
{
	return run_names(title, purge ? "purge" : "remove", names, n, NULL);
}

/* ------------------------------------------------------------------ *
 * Package details                                                      *
 * ------------------------------------------------------------------ */

typedef struct {
	char *desc;
	char *maint;
	char *home;
	char *section;
	char *debsize;
	char *filename;
	char *priority;
} Stanza;

/* Function: Append a byte span as one line in a NUL-terminated stanza field.
 * Parameters:
 *   dst (char **): Address of a NULL or heap-owned field pointer; the resulting allocation remains owned by the caller.
 *   value (const char *): Borrowed byte span that need not be NUL-terminated; may be NULL only when vlen is zero.
 *   vlen (size_t): Number of bytes in value; zero is valid.
 * Return (void): No return value.
 */
static void stanza_append(char **dst, const char *value, size_t vlen)
{
	size_t l;

	if (!value)
		vlen = 0;
	if (!*dst) {
		*dst = xmalloc(vlen + 1);
		if (vlen)
			memcpy(*dst, value, vlen);
		(*dst)[vlen] = '\0';
		return;
	}
	if (!vlen)
		return;
	l = strlen(*dst);
	*dst = xrealloc(*dst, l + vlen + 2);
	if (l)
		(*dst)[l++] = '\n';
	memcpy(*dst + l, value, vlen);
	(*dst)[l + vlen] = '\0';
}

/* Function: Replace a stanza field with a NUL-terminated copy of a byte span.
 * Parameters:
 *   dst (char **): Address of a NULL or heap-owned field; any old allocation is freed.
 *   value (const char *): Borrowed byte span that need not be NUL-terminated; may be NULL only when vlen is zero.
 *   vlen (size_t): Number of bytes to copy; zero produces an empty string.
 * Return (void): No return value.
 */
static void stanza_field(char **dst, const char *value, size_t vlen)
{
	char *s;

	if (!value)
		vlen = 0;
	s = xmalloc(vlen + 1);
	if (vlen)
		memcpy(s, value, vlen);
	s[vlen] = '\0';
	free(*dst);
	*dst = s;
}

/* Function: Parse the first package stanza in "apt-cache show" output.
 * Parameters:
 *   text (const char *): NUL-terminated command output; NULL is treated as empty.
 *   s (Stanza *): Uninitialized or zeroed result storage; prior allocations are not released and any populated fields must later use stanza_free().
 * Return (void): No return value.
 */
static void parse_show(const char *text, Stanza *s)
{
	const char *p = text;
	char **last = NULL;

	memset(s, 0, sizeof(*s));
	if (!text)
		return;

	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		const char *line = p;
		const char *colon;
		size_t klen, vlen;
		char key[64];
		const char *val;

		if (len == 0)
			break; /* end of the first stanza */

		if (line[0] == ' ' || line[0] == '\t') {
			if (last == &s->desc)
				stanza_append(&s->desc, line + 1,
					      len - 1);
			if (!nl)
				break;
			p = nl + 1;
			continue;
		}

		colon = memchr(line, ':', len);
		if (!colon)
			goto next;
		klen = (size_t)(colon - line);
		if (klen == 0 || klen >= sizeof(key))
			goto next;
		memcpy(key, line, klen);
		key[klen] = '\0';
		val = colon + 1;
		while (val < line + len && (*val == ' ' || *val == '\t'))
			val++;
		vlen = (size_t)(line + len - val);

		last = NULL;
		if (strcmp(key, "Description") == 0) {
			stanza_field(&s->desc, "", 0);
			stanza_append(&s->desc, val, vlen);
			last = &s->desc;
		} else if (strcmp(key, "Description-en") == 0 && !s->desc) {
			stanza_field(&s->desc, "", 0);
			stanza_append(&s->desc, val, vlen);
			last = &s->desc;
		} else if (strcmp(key, "Maintainer") == 0) {
			stanza_field(&s->maint, val, vlen);
		} else if (strcmp(key, "Homepage") == 0) {
			stanza_field(&s->home, val, vlen);
		} else if (strcmp(key, "Section") == 0) {
			stanza_field(&s->section, val, vlen);
		} else if (strcmp(key, "Size") == 0) {
			stanza_field(&s->debsize, val, vlen);
		} else if (strcmp(key, "Filename") == 0) {
			stanza_field(&s->filename, val, vlen);
		} else if (strcmp(key, "Priority") == 0) {
			stanza_field(&s->priority, val, vlen);
		}
next:
		if (!nl)
			break;
		p = nl + 1;
	}
}

/* Function: Release every field in a stanza and reset it to empty.
 * Parameters:
 *   s (Stanza *): Non-NULL stanza; may be zeroed already.
 * Return (void): No return value.
 */
static void stanza_free(Stanza *s)
{
	free(s->desc);
	free(s->maint);
	free(s->home);
	free(s->section);
	free(s->debsize);
	free(s->filename);
	free(s->priority);
	memset(s, 0, sizeof(*s));
}

/* Function: Extract "Depends:" values from apt-cache output and append them to a buffer. Each input line is considered only up to 511 bytes.
 * Parameters:
 *   b (Buf *): Initialized caller-owned buffer; existing text is preserved.
 *   text (const char *): NUL-terminated command output; NULL is accepted and does nothing.
 * Return (void): No return value.
 */
static void append_depends(Buf *b, const char *text)
{
	const char *p = text;

	while (p && *p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		char line[512];
		char *hit;

		if (len >= sizeof(line))
			len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = '\0';

		hit = strstr(line, "Depends:");
		if (hit) {
			char *val = hit + strlen("Depends:");

			while (*val == ' ' || *val == '\t')
				val++;
			str_trim(val);
			if (*val)
				buf_printf(b, "  %s\n", val);
		}
		p = nl ? nl + 1 : NULL;
	}
}

/* Function: Return a display value for a package's installed version.
 * Parameters:
 *   p (const Package *): Non-NULL package description.
 * Return (const char *): Borrowed p->installed when present, otherwise the static string "-". The result is valid while p remains unchanged.
 */
static const char *installed_or_none(const Package *p)
{
	return p->installed ? p->installed : "-";
}

/* Function: Return a display value for a package's candidate version.
 * Parameters:
 *   p (const Package *): Non-NULL package description.
 * Return (const char *): Borrowed p->candidate when present, otherwise the static string "-". The result is valid while p remains unchanged.
 */
static const char *candidate_or_none(const Package *p)
{
	return p->candidate ? p->candidate : "-";
}

/* Function: Return a compact display value for a package's repository.
 * Parameters:
 *   p (const Package *): Non-NULL package description.
 * Return (const char *): Borrowed p->repo when present, otherwise static "(local)" for an installed package or static "-" otherwise. The result is valid while p remains unchanged.
 */
static const char *repo_or_local(const Package *p)
{
	if (p->repo)
		return p->repo;
	return p->installed ? "(local)" : "-";
}

/* Function: Query apt-cache for one package and parse its first metadata stanza.
 * Parameters:
 *   name (const char *): Non-NULL borrowed package name.
 *   st (Stanza *): Uninitialized or zeroed result storage; release it with stanza_free() after use.
 *   status (int *): Receives the command exit status, 128 plus a signal, or -1 if the child could not start.
 * Return (void): No return value.
 */
static void show_stanza(const char *name, Stanza *st, int *status)
{
	char *show_argv[] = { "apt-cache", "show", NULL, NULL };
	Cap cap;

	show_argv[2] = (char *)name;
	*status = cap_run(show_argv, &cap);
	if (*status >= 0)
		parse_show(cap.out.data ? cap.out.data : "", st);
	else
		memset(st, 0, sizeof(*st));
	cap_free(&cap);
}

/* Function: Build human-readable package details from database and apt-cache data.
 * Parameters:
 *   p (const Package *): Borrowed package description whose fields remain valid for this call; NULL is accepted and produces an empty result.
 * Return (char *): Newly allocated NUL-terminated text owned by the caller; returns an allocated empty string when p is NULL.
 */
char *apt_pkg_info(const Package *p)
{
	char *dep_argv[] = { "apt-cache", "depends", NULL, NULL };
	Cap cap;
	Stanza st;
	Buf info, deps;
	char sizebuf[32], kibbuf[32];
	int show_st;

	if (!p)
		return xstrdup("");

	show_stanza(p->name, &st, &show_st);

	buf_init(&deps);
	if (p->flags & PKGF_DEPS) {
		/* pkgdb_fetch_deps() already asked "apt-cache depends"
		 * for the "[+]" markers of this row, so reuse its answer
		 * instead of paying for a second run (~0.3s). */
		size_t i;

		for (i = 0; i < p->n_deps; i++)
			buf_printf(&deps, "  %s\n", p->deps[i]);
		if (p->n_deps == 0)
			buf_puts(&deps, "  (none reported)\n");
	} else {
		dep_argv[2] = (char *)p->name;
		if (cap_run(dep_argv, &cap) >= 0)
			append_depends(&deps, cap.out.data ? cap.out.data : "");
		cap_free(&cap);
	}

	buf_init(&info);
	buf_printf(&info, "Package:       %s\n", p->name);
	buf_printf(&info, "Status:        %s\n",
		   p->installed ? "installed" : "not installed");
	buf_printf(&info, "Installed:     %s\n", installed_or_none(p));
	if (p->installed && p->candidate && (p->flags & PKGF_UPGRADEABLE))
		buf_printf(&info, "Candidate:     %s  (upgrade available)\n",
			   candidate_or_none(p));
	else
		buf_printf(&info, "Candidate:     %s\n", candidate_or_none(p));
	buf_printf(&info, "Origin:        %s\n", repo_or_local(p));
	buf_printf(&info, "Architecture:  %s\n", p->arch ? p->arch : "-");
	if (st.section)
		buf_printf(&info, "Section:       %s\n", st.section);
	if (st.priority)
		buf_printf(&info, "Priority:      %s\n", st.priority);
	if (p->installed && p->size_kib) {
		human_kib(p->size_kib, kibbuf, sizeof(kibbuf));
		buf_printf(&info, "Installed sz:  %s\n", kibbuf);
	}
	if (st.debsize) {
		unsigned long long sz = strtoull(st.debsize, NULL, 10);

		human_kib((unsigned long)((sz + 1023) / 1024), sizebuf,
			  sizeof(sizebuf));
		buf_printf(&info, "Package size:  %s\n", sizebuf);
	}
	if (st.maint)
		buf_printf(&info, "Maintainer:    %s\n", st.maint);
	if (st.home)
		buf_printf(&info, "Homepage:      %s\n", st.home);
	if (st.filename)
		buf_printf(&info, "Archive file:  %s\n", st.filename);

	/* Full version table: where every known version comes from. */
	{
		char *pol_argv[] = { "apt-cache", "policy", NULL, NULL };
		Buf pol;

		buf_init(&pol);
		pol_argv[2] = (char *)p->name;
		if (cap_run(pol_argv, &cap) >= 0 && cap.out.len)
			buf_puts(&pol, cap.out.data);
		cap_free(&cap);

		buf_puts(&info, "\nAvailability:\n");
		if (pol.len)
			buf_puts(&info, pol.data);
		else
			buf_puts(&info, "  (unknown)\n");
		buf_free(&pol);
	}

	if (st.desc && st.desc[0]) {
		buf_puts(&info, "\nDescription:\n");
		buf_puts(&info, st.desc);
		buf_putc(&info, '\n');
	}
	buf_puts(&info, "\nDepends on:\n");
	if (deps.len)
		buf_puts(&info, deps.data);
	else
		buf_puts(&info, show_st >= 0 ? "  (none reported)\n" :
						"  (unknown)\n");

	buf_free(&deps);
	stanza_free(&st);
	return buf_steal(&info);
}

/* Function: Build a file list for an installed package, or archive details otherwise.
 * Parameters:
 *   p (const Package *): Borrowed package description whose fields remain valid for this call; NULL is accepted and produces an empty result.
 * Return (char *): Newly allocated NUL-terminated text owned by the caller; returns an allocated empty string when p is NULL.
 */
char *apt_pkg_files(const Package *p)
{
	char *lst_argv[] = { "dpkg", "-L", NULL, NULL };
	Cap cap;
	Stanza st;
	Buf b;
	int st_status;

	if (!p)
		return xstrdup("");

	buf_init(&b);
	if (p->installed && p->installed[0]) {
		lst_argv[2] = (char *)p->name;
		if (cap_run(lst_argv, &cap) == 0 && cap.out.len) {
			buf_printf(&b, "Files owned by %s (%s):\n\n", p->name,
				   p->installed);
			buf_puts(&b, cap.out.data);
		} else {
			buf_printf(&b, "Could not list the files of %s.\n",
				   p->name);
			if (cap.err.len)
				buf_puts(&b, cap.err.data);
		}
		cap_free(&cap);
	} else {
		show_stanza(p->name, &st, &st_status);
		buf_printf(&b, "%s is not installed.\n", p->name);
		if (st.filename)
			buf_printf(&b, "\nArchive file:\n  %s\n", st.filename);
		else
			buf_puts(&b, "\nArchive file is unknown.\n");
		stanza_free(&st);
	}
	return buf_steal(&b);
}
