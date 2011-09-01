/*
 * Fetchmail notification IMAP plugin for Dovecot
 *
 * Copyright (C) 2007 Guillaume Chazarain <guichaz@yahoo.fr>
 * - original version named wake_up_fetchmail.c
 *
 * Copyright (C) 2009-2011 Peter Marschall <peter@adpm.de>
 * - adaptions to dovecot 1.1, 1.2 & 2.0
 * - rename to fetchmail_wakeup.c
 * - configuration via dovecot.config
 *
 * License: LGPL v2.1
 *
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include "lib.h"
#include "imap-client.h"


#define FETCHMAIL_PIDFILE	"/var/run/fetchmail/fetchmail.pid"
#define FETCHMAIL_INTERVAL	60

#define FETCHMAIL_IMAPCMD_LEN	10


/* data structure for commands to be overridden */
struct overrides {
	const char *name;		/* the IMAPv4 command name */
	char *interval_name;		/* "fetchmail_<cmd>_interval" variable name */
	struct command orig_cmd;	/* copy of the original command's data structure */
};


/* commands that can be overridden */
static struct overrides cmds[] = {
	{ "IDLE",   NULL, {} },
	{ "NOOP",   NULL, {} },
	{ "STATUS", NULL, {} },
	{ NULL,     NULL, {} }
};


/*
 * Get a interval value from config and parse it into a number (with fallback for failures)
 */
static long getenv_interval(struct mail_user *user, const char *name, long fallback)
{
	if (name != NULL) {
		const char *value_as_str = mail_user_plugin_getenv(user, name);

		if (value_as_str != NULL) {
			long value;

			if ((str_to_long(value_as_str, &value) < 0) || (value <= 0)) {
				i_warning("fetchmail_wakeup: %s must be a positive number", name);
				return fallback;
			}
			else
				return value;
		}
	}
	return fallback;
}


/*
 * Don't bother waking up fetchmail too often
 */
static bool ratelimit(long interval)
{
	static struct timeval last_one;
	struct timeval now;
	long long millisec_delta;

	if (gettimeofday(&now, NULL))
		return FALSE;

	millisec_delta = ((now.tv_sec - last_one.tv_sec) * 1000000LL +
	                  now.tv_usec - last_one.tv_usec) / 1000LL;
	if (millisec_delta > interval * 1000LL) {
		last_one = now;
		return FALSE;
	}

	return TRUE;
}


/*
 * Send a signal to fetchmail or call a helper to awaken fetchmail
 */
static void fetchmail_wakeup(struct client_command_context *cmd, const char *interval_name)
{
	struct client *client = cmd->client;
	long fetchmail_interval = FETCHMAIL_INTERVAL;

	/* read config variables depending on the session */
	const char *fetchmail_helper = mail_user_plugin_getenv(client->user, "fetchmail_helper");
	const char *fetchmail_pidfile = mail_user_plugin_getenv(client->user, "fetchmail_pidfile");

	/* convert config variable "fetchmail_interval" into a number */
	fetchmail_interval = getenv_interval(client->user, "fetchmail_interval", FETCHMAIL_INTERVAL);

	/* if a command-specific fetchmail_<CMD>_interval was passed, evaluate it */
	fetchmail_interval = getenv_interval(client->user, interval_name, fetchmail_interval);

	if (ratelimit(fetchmail_interval))
		return;

	/* if a helper application is defined, then call it */
	if ((fetchmail_helper != NULL) && (*fetchmail_helper != '\0')) {
		pid_t pid;
		int status;
		char *const *argv;

		switch (pid = fork()) {
			case -1:	// fork failed
				i_warning("fetchmail_wakeup: fork() failed");
				return;
			case 0:		// child
				argv = (char *const *) t_strsplit_spaces(fetchmail_helper, " ");
				if ((argv != NULL) && (*argv != NULL)) {
					execv(argv[0], argv);
					i_warning("fetchmail_wakeup: execv(%s) failed: %s",
						argv[0], strerror(errno));
					exit(1);
				}
				else {
					i_warning("fetchmail_wakeup: illegal fetchmail_helper");
					exit(1);
				}
			default:	// parent
				waitpid(pid, &status, 0);
		}
	}
	/* otherwise if a pid file name is given, signal fetchmail with that pid */
	else if ((fetchmail_pidfile != NULL) && (*fetchmail_pidfile != '\0')) {
		FILE *pidfile = fopen(fetchmail_pidfile, "r");
		if (pidfile) {
			pid_t pid = 0;

			if ((fscanf(pidfile, "%d", &pid) == 1) && (pid > 1))
				kill(pid, SIGUSR1);
			else
				i_warning("fetchmail_wakeup: error reading valid pid from %s",
					fetchmail_pidfile);
			fclose(pidfile);
		}
		else {
			i_warning("fetchmail_wakeup: error opening %s",
				 fetchmail_pidfile);
		}
	}
	/* otherwise warn on missing configuration */
	else {
		i_warning("fetchmail_wakeup: neither fetchmail_pidfile nor fetchmail_helper given");
	}
}


/*
 * Our IMAPv4 command wrapper that calls fetchmail_wakeup
 */
static bool cmd_with_fetchmail(struct client_command_context *cmd)
{
	if (cmd != NULL) {
		int i;

		for (i = 0; cmds[i].name != NULL; i++) {
			if (strcasecmp(cmds[i].name, cmd->name) == 0) {

				/* try to wake up fetchmail */
				fetchmail_wakeup(cmd, cmds[i].interval_name);

				/* daisy chaining: call original IMAPv4 command handler */
				return ((cmds[i].orig_cmd.func != NULL)
					? cmds[i].orig_cmd.func(cmd)
					: FALSE);
			}
		}
	}
	return FALSE;
}


/*
 * Plugin init: remember dovecot's original IMAPv4 handler functions and add our own
 * in place
 */
void fetchmail_wakeup_plugin_init(struct module *module)
{
	int i;

	/* replace IMAPv4 command handlers by our own */
	for (i = 0; cmds[i].name != NULL; i++) {
		struct command *orig_cmd_ptr = command_find(cmds[i].name);

		if (orig_cmd_ptr != NULL) {
			memcpy(&cmds[i].orig_cmd, orig_cmd_ptr, sizeof(struct command));

			/* build 'fetchmail_<CMD>_interval' variable name & save it */
			if (strnlen(cmds[i].name, FETCHMAIL_IMAPCMD_LEN) < FETCHMAIL_IMAPCMD_LEN) {
				char interval_name[sizeof("fetchmail_%s_interval")+FETCHMAIL_IMAPCMD_LEN];

				/* build variable name */
				i_snprintf(interval_name, sizeof(interval_name),
					"fetchmail_%s_interval", cmds[i].name);
				/* convert it to lowercase */
				str_lcase(interval_name);

				/* store it */
				cmds[i].interval_name = i_strdup(interval_name);
			}

			command_unregister(cmds[i].name);
			command_register(cmds[i].name, cmd_with_fetchmail, cmds[i].orig_cmd.flags);
		}
	}
}

/*
 * Plugin deinit: restore dovecot's original IMAPv4 handler functions
 */
void fetchmail_wakeup_plugin_deinit(void)
{
	int i;

	/* restore previous IMAPv4 command handlers */
	for (i = 0; cmds[i].name != NULL; i++) {
		command_unregister(cmds[i].name);
		command_register(cmds[i].orig_cmd.name, cmds[i].orig_cmd.func, cmds[i].orig_cmd.flags);

		/* free pre-built 'fetchmail_<CMD>_interval' variable name */
		if (cmds[i].interval_name != NULL)
			i_free_and_null(cmds[i].interval_name);
	}
}


/*
 * declare dependency on IMAP
 */
const char fetchmail_wakeup_plugin_binary_dependency[] = "imap";

/* EOF */
