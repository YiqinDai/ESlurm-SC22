/*****************************************************************************\
 *  scrontab.c
 *****************************************************************************
 *  Copyright (C) 2020 SchedMD LLC.
 *  Written by Tim Wickberg <tim@schedmd.com>
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>


#include "src/common/bitstring.h"
#include "src/common/cli_filter.h"
#include "src/common/cron.h"
#include "src/common/env.h"
#include "src/common/fetch_config.h"
#include "src/common/log.h"
#include "src/common/plugstack.h"
#include "src/common/ref.h"
#include "src/common/read_config.h"
#include "src/common/slurm_opt.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "scrontab.h"

static void _usage(void);

decl_static_data(default_crontab_txt);
decl_static_data(usage_txt);

static uid_t uid;
static gid_t gid;
static bool list_only = false;
static bool remove_only = false;

scron_opt_t scopt;
slurm_opt_t opt = {
	.scron_opt = &scopt,
	.help_func = _usage,
	.usage_func = _usage
};

static void _usage(void)
{
	char *txt;
	static_ref_to_cstring(txt, usage_txt);
	fprintf(stderr, "%s", txt);
	xfree(txt);
}

static void _parse_args(int argc, char **argv)
{
	log_options_t logopt = LOG_OPTS_STDERR_ONLY;
	int c = 0;

	log_init(xbasename(argv[0]), logopt, 0, NULL);
	uid = getuid();
	gid = getgid();

	opterr = 0;
	while ((c = getopt(argc, argv, "elru:v")) != -1) {
		switch (c) {
		case 'e':
			break;
		case 'l':
			list_only = true;
			break;
		case 'r':
			remove_only = true;
			break;
		case 'u':
			if (uid_from_string(optarg, &uid))
				fatal("Could not find user %s", optarg);
			gid = gid_from_uid(uid);
			break;
		case 'v':
			logopt.stderr_level++;
			log_alter(logopt, 0, NULL);
			break;
		default:
			_usage();
			exit(1);
		}
	}
}

static void _update_crontab_with_disabled_lines(char **crontab,
						char *disabled_lines,
						char *prepend)
{
	char **lines;
	int line_count = 0;
	bitstr_t *disabled;
	char *new_crontab = NULL;

	if (!*crontab || !disabled_lines || disabled_lines[0] == '\0')
		return;

	lines = convert_file_to_line_array(*crontab, &line_count);
	disabled = bit_alloc(line_count);

	bit_unfmt(disabled, disabled_lines);

	for (int i = 0; lines[i]; i++) {
		if (bit_test(disabled, i))
			xstrfmtcat(new_crontab, "%s%s\n", prepend, lines[i]);
		else
			xstrfmtcat(new_crontab, "%s\n", lines[i]);
	}
	xfree(*crontab);
	*crontab = new_crontab;
	bit_free(disabled);
	xfree(lines);
}

static void _reset_options(void)
{
        slurm_reset_all_options(&opt, true);
        /* cli_filter plugins can change the defaults */
        if (cli_filter_g_setup_defaults(&opt, false)) {
                error("cli_filter plugin terminated with error");
		exit(1);
	}

	opt.job_flags |= CRON_JOB;
}

static char *_job_script_header(void)
{
	return xstrdup("#!/bin/sh\n"
		       "# This job was submitted through scrontab\n");
}

char *_load_script_from_fd(int fd)
{
	char *buf, *ptr;
	int buf_size = 4096, buf_left, script_size = 0, tmp_size;
        buf = ptr = xmalloc(buf_size);
        buf_left = buf_size;

	if (lseek(fd, 0, SEEK_SET) < 0)
		fatal("%s: lseek(0): %m", __func__);

        while((tmp_size = read(fd, ptr, buf_left)) > 0) {
                buf_left -= tmp_size;
                script_size += tmp_size;
                if (buf_left == 0) {
                        buf_size += BUFSIZ;
                        xrealloc(buf, buf_size);
                }
                ptr = buf + script_size;
                buf_left = buf_size - script_size;
        }

	return buf;
}

/*
 * Replace crontab with an edited version after running an editor.
 */
static void _edit_crontab(char **crontab)
{
	int fd, wstatus;
	pid_t pid;
	char *editor, *filename = NULL;

	if (!*crontab)
		static_ref_to_cstring(*crontab, default_crontab_txt);

	xstrfmtcat(filename, "%s/scrontab-XXXXXX", slurm_conf.tmp_fs);

	/* protect against weak file permissions in old glibc */
	umask(0077);
	fd = mkstemp(filename);
	if (fd < 0 )
		fatal("could not create temp file");
	safe_write(fd, *crontab, strlen(*crontab));

	xfree(*crontab);

	if (!(editor = getenv("VISUAL")) || (editor[0] == '\0'))
		if (!(editor = getenv("EDITOR")) || (editor[0] == '\0'))
			editor = "vi";

	if ((pid = fork()) == -1)
		fatal("cannot fork");

	if (!pid) {
		/* child */
		char *argv[3];
		argv[0] = editor;
		argv[1] = filename;
		argv[2] = NULL;

		execvp(editor, argv);
		exit(127);
	}

	waitpid(pid, &wstatus, 0);

	if (wstatus) {
		close(fd);
		unlink(filename);
		xfree(filename);
		fatal("editor returned non-zero exit code");
	}

	*crontab = _load_script_from_fd(fd);
	close(fd);
	unlink(filename);
	xfree(filename);
	return;

rwfail:
	close(fd);
	unlink(filename);
	xfree(filename);
	fatal("failed to write to temp crontab file");
}

static job_desc_msg_t *_entry_to_job(cron_entry_t *entry, char *script)
{
	job_desc_msg_t *job = xmalloc(sizeof(*job));

	slurm_init_job_desc_msg(job);
	fill_job_desc_from_opts(job);

	job->crontab_entry = entry;

	/* finish building the batch script */
	xstrfmtcat(script, "# crontab time request was: '%s'\n%s\n",
		   entry->cronspec, entry->command);
	job->script = script;

	job->environment = env_array_create();
	env_array_overwrite(&job->environment, "SLURM_GET_USER_ENV", "1");
	job->env_size = envcount(job->environment);

	job->argc = 1;
	job->argv = xmalloc(sizeof(char *));
	job->argv[0] = xstrdup(entry->command);

	if (!job->name) {
		char *pos;
		job->name = xstrdup(entry->command);
		if ((pos = xstrstr(job->name, " ")))
			*pos = '\0';
	}

	if (!job->work_dir)
		job->work_dir = xstrdup(getenv("HOME"));

	return job;
}

static void _edit_and_update_crontab(char *crontab)
{
	char **lines;
	char *badline = NULL;
	int lineno, line_start;
	char *line;
	List jobs;
	int line_count;
	bool setup_next_entry = true;
	char *script;
	crontab_update_response_msg_t *response;

edit:
	_edit_crontab(&crontab);

	jobs = list_create((ListDelF) slurm_free_job_desc_msg);
	lines = convert_file_to_line_array(xstrdup(crontab), &line_count);

	lineno = 0;
	line_start = -1;
	setup_next_entry = true;
	while ((line = lines[lineno])) {
		char *pos = line;
		cron_entry_t *entry;

		if (setup_next_entry) {
			_reset_options();
			script = _job_script_header();
			setup_next_entry = false;
		}

		/* advance to the first non-whitespace character */
		while (*pos == ' ' || *pos == '\t')
			pos++;

		if (*pos == '\0' || *pos == '\n') {
			/* line was only whitespace */
			lineno++;
			continue;
		} else if (!xstrncmp(pos, "#SCRON", 6)) {
			xstrfmtcat(script, "%s\n", line);
			if (line_start == -1)
				line_start = lineno;
			if (parse_scron_line(line + 6, lineno)) {
				badline = xstrdup_printf("%d", lineno);
				break;
			}
			lineno++;
			continue;
		} else if (*pos == '#') {
			/* boring comment line */
			lineno++;
			continue;
		}

		if (!(entry = cronspec_to_bitstring(pos))) {
			badline = xstrdup_printf("%d", lineno);
			break;
		}

		if (cli_filter_g_pre_submit(&opt, 0)) {
			badline = xstrdup_printf("%d-%d", line_start, lineno);
			printf("cli_filter plugin terminated with error\n");
			break;
		}

		/*
		 * track lines associated with this job submission, starting
		 * starting at the first SCRON directive and completing here
		 */
		if (line_start != -1)
			entry->line_start = line_start;
		else
			entry->line_start = lineno;
		line_start = -1;

		entry->line_end = lineno;

		list_append(jobs, _entry_to_job(entry, script));
		script = NULL;
		setup_next_entry = true;
		lineno++;
	}

	xfree(*lines);
	xfree(lines);
	xfree(script);

	if (badline) {
		char c = '\0';

		list_destroy(jobs);

		while (tolower(c) != 'y' && tolower(c) != 'n') {
			printf("There are errors in your crontab.\n"
			       "The failed line(s) is commented out with #BAD:\n"
			       "Do you want to retry the edit? (y/n) ");
			c = (char) getchar();
		}

		if (c == 'n')
			exit(0);

		_update_crontab_with_disabled_lines(&crontab, badline,
						    "#BAD: ");
		xfree(badline);

		goto edit;
	}

	response = slurm_update_crontab(uid, gid, crontab, jobs);

	if (response->return_code) {
		char c = '\0';
		list_destroy(jobs);
		while (tolower(c) != 'y' && tolower(c) != 'n') {
			printf("There was an issue with the job submission on lines %s\n"
			       "The error code return was: %s\n"
			       "The error message was: %s\n"
			       "The failed lines are commented out with #BAD:\n"
			       "Do you want to retry the edit? (y/n) ",
			       response->failed_lines,
			       slurm_strerror(response->return_code),
			       response->err_msg);
			c = (char) getchar();
		}

		if (c == 'n')
			exit(0);

		_update_crontab_with_disabled_lines(&crontab,
						    response->failed_lines,
						    "#BAD: ");
		slurm_free_crontab_update_response_msg(response);
		goto edit;
	}

	for (int i = 0; i < response->jobids_count; i++)
		cli_filter_g_post_submit(0, response->jobids[i], NO_VAL);

	slurm_free_crontab_update_response_msg(response);
	xfree(crontab);
	list_destroy(jobs);
}

extern int main(int argc, char **argv)
{
	int rc;
	char *crontab = NULL, *disabled_lines = NULL;

	slurm_conf_init(NULL);
	_parse_args(argc, argv);

	if (!xstrcasestr(slurm_conf.scron_params, "enable"))
		fatal("scrontab is disabled on this cluster");

	if (remove_only) {
		if ((rc = slurm_remove_crontab(uid, gid)))
			fatal("slurm_remove_crontab failed: %s",
			      slurm_strerror(rc));
		exit(0);
	}

	if ((rc = slurm_request_crontab(uid, &crontab, &disabled_lines)) &&
	    (rc != ESLURM_JOB_SCRIPT_MISSING)) {
		fatal("slurm_request_crontab failed: %s",
		      slurm_strerror(rc));
	}

	_update_crontab_with_disabled_lines(&crontab, disabled_lines,
					    "#DISABLED: ");
	xfree(disabled_lines);

	if (list_only) {
		if (!crontab) {
			printf("no crontab for %s\n", uid_to_string(uid));
			exit(1);
		}

		printf("%s", crontab);
		xfree(crontab);
		exit(0);
	}

	/* needed otherwise slurm_option_table_create() always returns NULL */
	if (spank_init_allocator() < 0)
		fatal("failed to initialize plugin stack");

	_edit_and_update_crontab(crontab);

	spank_fini(NULL);

	return 0;
}
