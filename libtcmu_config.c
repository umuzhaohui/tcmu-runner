/*
 * Copyright 2016-2017 China Mobile, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <stdbool.h>

#include "darray.h"
#include "libtcmu_config.h"
#include "libtcmu_log.h"

/*
 * System config for TCMU, for now there are only 3 option types supported:
 * 1, The "int type" option, for example:
 *	log_level = 2
 *
 * 2, The "string type" option, for example:
 *	tcmu_str = "Tom"  --> Tom
 *    or
 *	tcmu_str = 'Tom'  --> Tom
 *    or
 *	tcmu_str = 'Tom is a "boy"' ---> Tom is a "boy"
 *    or
 *	tcmu_str = "'T' is short for Tom" --> 'T' is short for Tom
 *
 * 3, The "boolean type" option, for example:
 *	tcmu_bool
 *
 * ========================
 * How to add new options ?
 *
 * Using "log_level" as an example:
 *
 * 1, Add log_level member in:
 *	struct tcmu_config {
 *		int log_level;
 *	};
 *    in file libtcmu_config.h.
 *
 * 2, Add the following option in "tcmu.conf" file as default:
 *	log_level = 2
 *    or
 *	# log_level = 2
 *
 *    Note: the option name in config file must be the same as in
 *    tcmu_config.
 *
 * 3, You should add your own set method in:
 *	static void tcmu_conf_set_options(struct tcmu_config *cfg)
 *	{
 *		TCMU_PARSE_CFG_INT(cfg, log_level);
 *		TCMU_CONF_CHECK_LOG_LEVEL(log_level);
 *	}
 * 4, Then add your own free method in if it's a STR KEY:
 *	static void tcmu_conf_free_str_keys(struct tcmu_config *cfg)
 *	{
 *		TCMU_FREE_CFG_STR_KEY(cfg, 'STR KEY');
 *	}
 *
 * Note: For now, if the options have been changed in config file, the
 * system config reload thread daemon will try to update them for all the
 * tcmu-runner, consumer and tcmu-synthesizer daemons.
 */
#include "ccan/list/list.h"

static LIST_HEAD(tcmu_options);

static struct tcmu_conf_option * tcmu_get_option(const char *key)
{
	struct tcmu_conf_option *option;

	list_for_each(&tcmu_options, option, list) {
		if (!strcmp(option->key, key))
			return option;
	}

	return NULL;
}

#define TCMU_PARSE_CFG_INT(cfg, key) \
do { \
	struct tcmu_conf_option *option; \
	option = tcmu_get_option(#key); \
	if (option) { \
		cfg->key = option->opt_int; \
	} \
} while (0)

#define TCMU_PARSE_CFG_BOOL(cfg, key) \
do { \
	struct tcmu_conf_option *option; \
	option = tcmu_get_option(#key); \
	if (option) { \
		cfg->key = option->opt_bool; \
	} \
} while (0)

#define TCMU_PARSE_CFG_STR(cfg, key) \
do { \
	struct tcmu_conf_option *option; \
	option = tcmu_get_option(#key); \
	if (option) { \
		cfg->key = strdup(option->opt_str); } \
} while (0);

#define TCMU_FREE_CFG_STR_KEY(cfg, key) \
do { \
	free(cfg->key); \
} while (0);

static void tcmu_conf_set_options(struct tcmu_config *cfg, bool reloading)
{
	/* set log_level option */
	TCMU_PARSE_CFG_INT(cfg, log_level);
	if (cfg->log_level) {
		tcmu_set_log_level(cfg->log_level);
	}

	if (!reloading) {
		/* set log_dir path option */
		TCMU_PARSE_CFG_STR(cfg, log_dir_path);
		/*
		 * The priority of the logdir setting is:
		 * 1, --tcmu_log_dir/-l LOG_DIR_PATH
		 * 2, export TCMU_LOGDIR="/var/log/mychoice/"
		 * 3, tcmu.conf
		 * 4, default /var/log/
		 */
		if (!tcmu_get_logdir())
			tcmu_logdir_create(cfg->log_dir_path);
		else
			tcmu_warn("The logdir option from the tcmu.conf will be ignored\n");
	} else {
		tcmu_warn("The logdir option is not supported by dynamic reloading for now!\n");
	}

	/* add your new config options */
}

static void tcmu_conf_free_str_keys(struct tcmu_config *cfg)
{
	/* add your str type config options
	 *
	 * For example:
	 * TCMU_FREE_CFG_STR_KEY(cfg, 'STR KEY');
	 */
}

#define TCMU_MAX_CFG_FILE_SIZE (2 * 1024 * 1024)
static int tcmu_read_config(int fd, char *buf, int count)
{
	ssize_t len;
	int save = errno;

	do {
		len = read(fd, buf, count);
	} while (errno == EAGAIN);

	errno = save;
	return len;
}

/* end of line */
#define __EOL(c) (((c) == '\n') || ((c) == '\r'))

#define TCMU_TO_LINE_END(x, y) \
	do { while ((x) < (y) && !__EOL(*(x))) { \
		(x)++; } \
	} while (0);

/* skip blank lines */
#define TCMU_SKIP_BLANK_LINES(x, y) \
	do { while ((x) < (y) && (isblank(*(x)) || __EOL(*(x)))) { \
		(x)++; } \
	} while (0);

/* skip comment line with '#' */
#define TCMU_SKIP_COMMENT_LINE(x, y) \
	do { while ((x) < (y) && !__EOL(*x)) { \
		(x)++; } \
	     (x)++; \
	} while (0);

/* skip comment lines with '#' */
#define TCMU_SKIP_COMMENT_LINES(x, y) \
	do { while ((x) < (y) && *(x) == '#') { \
		TCMU_SKIP_COMMENT_LINE((x), (y)); } \
	} while (0);

#define MAX_KEY_LEN 64
#define MAX_VAL_STR_LEN 256

static struct tcmu_conf_option *
tcmu_register_option(char *key, tcmu_option_type type)
{
	struct tcmu_conf_option *option;

	option = calloc(1, sizeof(*option));
	if (!option)
		return NULL;

	option->key = strdup(key);
	if (!option->key)
		goto free_option;
	option->type = type;
	list_node_init(&option->list);

	list_add_tail(&tcmu_options, &option->list);
	return option;

free_option:
	free(option);
	return NULL;
}

static void tcmu_parse_option(char **cur, const char *end)
{
	struct tcmu_conf_option *option;
	tcmu_option_type type;
	char *p = *cur, *q = *cur, *r, *s;

	while (isblank(*p))
		p++;

	TCMU_TO_LINE_END(q, end);
	*q = '\0';
	*cur = q + 1;

	/* parse the boolean type option */
	s = r = strchr(p, '=');
	if (!r) {
		/* boolean type option at file end or line end */
		r = p;
		while (!isblank(*r) && r < q)
			r++;
		*r = '\0';
		option = tcmu_get_option(p);
		if (!option)
			option = tcmu_register_option(p, TCMU_OPT_BOOL);

		if (option)
			option->opt_bool = true;

		return;
	}
	/* skip character '='  */
	s++;
	while (isblank(*r) || *r == '=')
		r--;
	r++;
	*r = '\0';

	option = tcmu_get_option(p);
	if (!option) {
		r = s;
		while (isblank(*r) || *r == '=')
			r++;

		if (*r == '"' || *r == '\'') {
			type = TCMU_OPT_STR;
		} else if (isdigit(*r)) {
			type = TCMU_OPT_INT;
		} else {
			tcmu_err("option type not supported!\n");
			return;
		}

		option = tcmu_register_option(p, type);
		if (!option)
			return;
	}

	/* parse the int/string type options */
	switch (option->type) {
	case TCMU_OPT_INT:
		while (!isdigit(*s))
			s++;
		r = s;
		while (isdigit(*r))
			r++;
		*r= '\0';

		option->opt_int = atoi(s);
		break;
	case TCMU_OPT_STR:
		s++;
		while (isblank(*s))
			s++;
		/* skip first " or ' if exist */
		if (*s == '"' || *s == '\'')
			s++;
		r = q - 1;
		while (isblank(*r))
			r--;
		/* skip last " or ' if exist */
		if (*r == '"' || *r == '\'')
			*r = '\0';

		if (option->opt_str)
			/* free if this is reconfig */
			free(option->opt_str);
		option->opt_str = strdup(s);
		break;
	default:
		tcmu_err("option type %d not supported!\n");
		break;
	}
}

static void tcmu_parse_options(struct tcmu_config *cfg, char *buf, int len, bool reloading)
{
	char *cur = buf, *end = buf + len;

	while (cur < end) {
		/* skip blanks lines */
		TCMU_SKIP_BLANK_LINES(cur, end);

		/* skip comments with '#' */
		TCMU_SKIP_COMMENT_LINES(cur, end);

		if (cur >= end)
			break;

		if (!isalpha(*cur))
			continue;

		/* parse the options from config file to tcmu_options[] */
		tcmu_parse_option(&cur, end);
	}

	/* parse the options from tcmu_options[] to struct tcmu_config */
	tcmu_conf_set_options(cfg, reloading);
}

static int tcmu_load_config(struct tcmu_config *cfg, bool reloading)
{
	int ret = -1;
	int fd, len;
	char *buf;

	buf = malloc(TCMU_MAX_CFG_FILE_SIZE);
	if (!buf)
		return -ENOMEM;

	fd = open(cfg->path, O_RDONLY);
	if (fd < 0) {
		tcmu_err("Failed to open file '%s', %m\n", cfg->path);
		goto free_buf;
	}

	len = tcmu_read_config(fd, buf, TCMU_MAX_CFG_FILE_SIZE);
	close(fd);
	if (len < 0) {
		tcmu_err("Failed to read file '%s'\n", cfg->path);
		goto free_buf;
	}

	buf[len] = '\0';

	tcmu_parse_options(cfg, buf, len, reloading);

	ret = 0;
free_buf:
	free(buf);
	return ret;
}

#define BUF_LEN 1024
static void *dyn_config_start(void *arg)
{
	struct tcmu_config *cfg = arg;
	int monitor, wd, len;
	char buf[BUF_LEN];

	monitor = inotify_init();
	if (monitor == -1) {
		tcmu_err("Failed to init inotify %m\n");
		return NULL;
	}

	wd = inotify_add_watch(monitor, cfg->path, IN_ALL_EVENTS);
	if (wd == -1) {
		tcmu_err("Failed to add \"%s\" to inotify %m\n", cfg->path);
		return NULL;
	}

	tcmu_info("Inotify is watching \"%s\", wd: %d, mask: IN_ALL_EVENTS\n",
		  cfg->path, wd);

	while (1) {
		struct inotify_event *event;
		char *p;

		len = read(monitor, buf, BUF_LEN);
		if (len == -1) {
			tcmu_warn("Failed to read inotify: %m\n");
			continue;
		}

		for (p = buf; p < buf + len;) {
			event = (struct inotify_event *)p;

			tcmu_info("event->mask: 0x%x\n", event->mask);

			if (event->wd != wd)
				continue;

			/*
			 * If force to write to the unwritable or crashed
			 * config file, the vi/vim will try to move and
			 * delete the config file and then recreate it again
			 * via the *.swp
			 */
			if ((event->mask & IN_IGNORED) && !access(cfg->path, F_OK))
				wd = inotify_add_watch(monitor, cfg->path, IN_ALL_EVENTS);

			/* Try to reload the config file */
			if (event->mask & IN_MODIFY || event->mask & IN_IGNORED)
				tcmu_load_config(cfg, true);

			p += sizeof(struct inotify_event) + event->len;
		}
	}

	return NULL;
}

struct tcmu_config *tcmu_setup_config(const char *path)
{
	struct tcmu_config *cfg;

	cfg = calloc(1, sizeof(*cfg));
	if (cfg == NULL) {
		tcmu_err("Alloc TCMU config failed!\n");
		return NULL;
	}

	if (!path)
		path = "/etc/tcmu/tcmu.conf"; /* the default config file */

	cfg->path = strdup(path);
	if (!cfg->path) {
		tcmu_err("failed to copy path: %s\n", path);
		goto free_cfg;
	}

	if (tcmu_load_config(cfg, false)) {
		tcmu_err("Loading TCMU config failed!\n");
		goto free_path;
	}

	/*
	 * If the dynamic reloading thread fails to start, it will fall
	 * back to static config
	 */
	if (pthread_create(&cfg->thread_id, NULL, dyn_config_start, cfg)) {
		tcmu_warn("Dynamic config started failed, fallling back to static!\n");
	} else {
		cfg->is_dynamic = true;
	}

	return cfg;

free_path:
	free(cfg->path);
free_cfg:
	free(cfg);
	return NULL;
}

static void tcmu_cancel_config_thread(struct tcmu_config *cfg)
{
	pthread_t thread_id = cfg->thread_id;
	void *join_retval;
	int ret;

	ret = pthread_cancel(thread_id);
	if (ret) {
		tcmu_err("pthread_cancel failed with value %d\n", ret);
		return;
	}

	pthread_join(thread_id, &join_retval);
	if (ret) {
		tcmu_err("pthread_join failed with value %d\n", ret);
		return;
	}

	if (join_retval != PTHREAD_CANCELED)
		tcmu_err("unexpected join retval: %p\n", join_retval);
}

void tcmu_destroy_config(struct tcmu_config *cfg)
{
	struct tcmu_conf_option *option, *next;

	if (!cfg)
		return;

	if (cfg->is_dynamic)
		tcmu_cancel_config_thread(cfg);

	list_for_each_safe(&tcmu_options, option, next, list) {
		list_del(&option->list);

		if (option->type == TCMU_OPT_STR)
			free(option->opt_str);
		free(option->key);
		free(option);
	}

	tcmu_conf_free_str_keys(cfg);
	if (cfg->log_dir_path)
		free(cfg->log_dir_path);
	free(cfg->path);
	free(cfg);
}
