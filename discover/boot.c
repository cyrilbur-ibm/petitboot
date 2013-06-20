
#define _GNU_SOURCE

#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <dirent.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>

#include <log/log.h>
#include <pb-protocol/pb-protocol.h>
#include <system/system.h>
#include <talloc/talloc.h>
#include <url/url.h>

#include "device-handler.h"
#include "boot.h"
#include "paths.h"
#include "resource.h"

static const char *boot_hook_dir = PKG_SYSCONF_DIR "/boot.d";
enum {
	BOOT_HOOK_EXIT_OK	= 0,
	BOOT_HOOK_EXIT_UPDATE	= 2,
};


struct boot_task {
	char *local_image;
	char *local_initrd;
	char *local_dtb;
	char *args;

	bool dry_run;
};

/**
 * kexec_load - kexec load helper.
 */
static int kexec_load(struct boot_task *boot_task)
{
	int result;
	const char *argv[7];
	const char **p;
	char *s_initrd = NULL;
	char *s_dtb = NULL;
	char *s_args = NULL;

	p = argv;
	*p++ = pb_system_apps.kexec;	/* 1 */
	*p++ = "-l";			/* 2 */

	if (boot_task->local_initrd) {
		s_initrd = talloc_asprintf(NULL, "--initrd=%s",
				boot_task->local_initrd);
		assert(s_initrd);
		*p++ = s_initrd;	 /* 3 */
	}

	if (boot_task->local_dtb) {
		s_dtb = talloc_asprintf(NULL, "--dtb=%s", boot_task->local_dtb);
		assert(s_dtb);
		*p++ = s_dtb;		 /* 4 */
	}

	if (boot_task->args) {
		s_args = talloc_asprintf(NULL, "--append=%s", boot_task->args);
		assert(s_args);
		*p++ = s_args;		/* 5 */
	}

	*p++ = boot_task->local_image;	/* 6 */
	*p++ = NULL;			/* 7 */

	result = pb_run_cmd(argv, 1, boot_task->dry_run);

	if (result)
		pb_log("%s: failed: (%d)\n", __func__, result);

	talloc_free(s_initrd);
	talloc_free(s_dtb);
	talloc_free(s_args);

	return result;
}

/**
 * kexec_reboot - Helper to boot the new kernel.
 *
 * Must only be called after a successful call to kexec_load().
 */

static int kexec_reboot(bool dry_run)
{
	int result = 0;
	const char *argv[4];
	const char **p;

	/* First try running shutdown.  Init scripts should run 'exec -e' */

	p = argv;
	*p++ = pb_system_apps.shutdown;	/* 1 */
	*p++ =  "-r";			/* 2 */
	*p++ =  "now";			/* 3 */
	*p++ =  NULL;			/* 4 */

	result = pb_run_cmd(argv, 1, dry_run);

	/* On error, force a kexec with the -e option */

	if (result) {
		p = argv;
		*p++ = pb_system_apps.kexec;	/* 1 */
		*p++ = "-e";			/* 2 */
		*p++ = NULL;			/* 3 */

		result = pb_run_cmd(argv, 1, 0);
	}

	if (result)
		pb_log("%s: failed: (%d)\n", __func__, result);

	/* okay, kexec -e -f */
	if (result) {
		p = argv;
		*p++ = pb_system_apps.kexec;	/* 1 */
		*p++ = "-e";			/* 2 */
		*p++ = "-f";			/* 3 */
		*p++ = NULL;			/* 4 */

		result = pb_run_cmd(argv, 1, 0);
	}

	if (result)
		pb_log("%s: failed: (%d)\n", __func__, result);


	return result;
}

static void update_status(boot_status_fn fn, void *arg, int type,
		char *message)
{
	struct boot_status status;

	status.type = type;
	status.message = message;
	status.progress = -1;
	status.detail = NULL;

	fn(arg, &status);
}

static void boot_hook_update_param(void *ctx, struct boot_task *task,
		const char *name, const char *value)
{
	struct p {
		const char *name;
		char **p;
	} *param, params[] = {
		{ "boot_image",		&task->local_image },
		{ "boot_initrd",	&task->local_initrd },
		{ "boot_dtb",		&task->local_dtb },
		{ "boot_args",		&task->args },
		{ NULL, NULL },
	};

	for (param = params; param->name; param++) {
		if (strcmp(param->name, name))
			continue;

		*param->p = talloc_strdup(ctx, value);
		return;
	}
}

static void boot_hook_update(void *ctx, const char *hookname,
		struct boot_task *task, char *buf)
{
	char *line, *name, *val, *sep;
	char *saveptr;

	for (;; buf = NULL) {

		line = strtok_r(buf, "\n", &saveptr);
		if (!line)
			break;

		sep = strchr(line, '=');
		if (!sep)
			continue;

		*sep = '\0';
		name = line;
		val = sep + 1;

		boot_hook_update_param(ctx, task, name, val);

		pb_log("boot hook %s specified %s=%s\n",
				hookname, name, val);
	}
}

static void boot_hook_setenv(struct boot_task *task)
{
	unsetenv("boot_image");
	unsetenv("boot_initrd");
	unsetenv("boot_dtb");
	unsetenv("boot_args");

	setenv("boot_image", task->local_image, 1);
	if (task->local_initrd)
		setenv("boot_initrd", task->local_initrd, 1);
	if (task->local_dtb)
		setenv("boot_dtb", task->local_dtb, 1);
	if (task->args)
		setenv("boot_args", task->args, 1);
}

static int hook_filter(const struct dirent *dirent)
{
	return dirent->d_type == DT_REG || dirent->d_type == DT_LNK;
}

static int hook_cmp(const struct dirent **a, const struct dirent **b)
{
	return strcmp((*a)->d_name, (*b)->d_name);
}

static void run_boot_hooks(void *ctx, struct boot_task *task,
		boot_status_fn status_fn, void *status_arg)
{
	struct dirent **hooks;
	int i, n;

	n = scandir(boot_hook_dir, &hooks, hook_filter, hook_cmp);
	if (n < 1)
		return;

	update_status(status_fn, status_arg, BOOT_STATUS_INFO,
			"running boot hooks");

	boot_hook_setenv(task);

	for (i = 0; i < n; i++) {
		const char *argv[2] = { NULL, NULL };
		char *path, *buf;
		int buf_len, rc;

		path = join_paths(ctx, boot_hook_dir, hooks[i]->d_name);

		if (access(path, X_OK))
			continue;

		pb_log("running boot hook %s\n", hooks[i]->d_name);

		argv[0] = path;
		rc = pb_run_cmd_pipe(argv, 1, task->dry_run, ctx,
				&buf, &buf_len);

		/* if the hook returned with BOOT_HOOK_EXIT_UPDATE,
		 * then we process stdout to look for updated params
		 */
		if (rc == BOOT_HOOK_EXIT_UPDATE) {
			boot_hook_update(ctx, hooks[i]->d_name, task, buf);
			boot_hook_setenv(task);
		}

		talloc_free(buf);
		talloc_free(path);
	}

	free(hooks);
}

int boot(void *ctx, struct discover_boot_option *opt, struct boot_command *cmd,
		int dry_run, boot_status_fn status_fn, void *status_arg)
{
	struct boot_task boot_task;
	struct pb_url *image, *initrd, *dtb;
	unsigned int clean_image = 0;
	unsigned int clean_initrd = 0;
	unsigned int clean_dtb = 0;
	int result;

	image = NULL;
	initrd = NULL;
	dtb = NULL;
	boot_task.dry_run = dry_run;

	if (cmd && cmd->boot_image_file) {
		image = pb_url_parse(opt, cmd->boot_image_file);
	} else if (opt && opt->boot_image) {
		image = opt->boot_image->url;
	} else {
		pb_log("%s: no image specified", __func__);
		return -1;
	}

	if (cmd && cmd->initrd_file) {
		initrd = pb_url_parse(opt, cmd->initrd_file);
	} else if (opt && opt->initrd) {
		initrd = opt->initrd->url;
	}

	if (cmd && cmd->dtb_file) {
		dtb = pb_url_parse(opt, cmd->dtb_file);
	} else if (opt && opt->dtb) {
		dtb = opt->dtb->url;
	}

	if (cmd && cmd->boot_args) {
		boot_task.args = talloc_strdup(ctx, cmd->boot_args);
	} else if (opt && opt->option->boot_args) {
		boot_task.args = talloc_strdup(ctx, opt->option->boot_args);
	} else {
		boot_task.args = NULL;
	}

	result = -1;

	update_status(status_fn, status_arg, BOOT_STATUS_INFO,
			"loading kernel");
	boot_task.local_image = load_url(NULL, image, &clean_image);
	if (!boot_task.local_image) {
		update_status(status_fn, status_arg, BOOT_STATUS_ERROR,
				"Couldn't load kernel image");
		goto no_load;
	}

	boot_task.local_initrd = NULL;
	if (initrd) {
		update_status(status_fn, status_arg, BOOT_STATUS_INFO,
				"loading initrd");
		boot_task.local_initrd = load_url(NULL, initrd, &clean_initrd);
		if (!boot_task.local_initrd) {
			update_status(status_fn, status_arg, BOOT_STATUS_ERROR,
					"Couldn't load initrd image");
			goto no_load;
		}
	}

	boot_task.local_dtb = NULL;
	if (dtb) {
		update_status(status_fn, status_arg, BOOT_STATUS_INFO,
				"loading device tree");
		boot_task.local_dtb = load_url(NULL, dtb, &clean_dtb);
		if (!boot_task.local_dtb) {
			update_status(status_fn, status_arg, BOOT_STATUS_ERROR,
					"Couldn't load device tree");
			goto no_load;
		}
	}

	run_boot_hooks(ctx, &boot_task, status_fn, status_arg);

	update_status(status_fn, status_arg, BOOT_STATUS_INFO,
			"performing kexec_load");

	result = kexec_load(&boot_task);

	if (result) {
		update_status(status_fn, status_arg, BOOT_STATUS_ERROR,
				"kexec load failed");
	}

no_load:
	if (clean_image)
		unlink(boot_task.local_image);
	if (clean_initrd)
		unlink(boot_task.local_initrd);
	if (clean_dtb)
		unlink(boot_task.local_dtb);

	talloc_free(boot_task.local_image);
	talloc_free(boot_task.local_initrd);
	talloc_free(boot_task.local_dtb);

	if (!result) {
		update_status(status_fn, status_arg, BOOT_STATUS_INFO,
				"performing kexec reboot");

		result = kexec_reboot(boot_task.dry_run);

		if (result) {
			update_status(status_fn, status_arg, BOOT_STATUS_ERROR,
					"kexec reboot failed");
		}
	}

	return result;
}
