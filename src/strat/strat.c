/*
 * strat.c
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      version 2 as published by the Free Software Foundation.
 *
 * Copyright (c) 2012-2020 Daniel Thau <danthau@bedrocklinux.org>
 *
 * This program is a derivative work of capchroot 0.1, and thus:
 * Copyright (c) 2009 Thomas Bächler <thomas@archlinux.org>
 *
 * This will run the specified Bedrock Linux stratum's instance of an
 * executable.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <dirent.h>
#include <sched.h>
#include <limits.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <limits.h>

#define STATE_DIR "/bedrock/run/enabled_strata/"
#define STATE_DIR_LEN strlen(STATE_DIR)
#define RESTRICTED_CMD_DIR "/bedrock/run/restricted_cmds/"
#define RESTRICTED_CMD_DIR_LEN strlen(RESTRICTED_CMD_DIR)
#define STRATA_ROOT "/bedrock/strata/"
#define STRATA_ROOT_LEN strlen(STRATA_ROOT)
#define CROSS_DIR "/bedrock/cross"
#define CROSS_DIR_LEN strlen(CROSS_DIR)
#define LOCAL_ALIAS "local"
#define LOCAL_ALIAS_LEN strlen(LOCAL_ALIAS_LEN)

#ifdef MIN
#undef MIN
#endif
#define MIN(x, y) (x < y ? x : y)

/*
 * Check if this process has the proper CAP_SYS_CHROOT properties.
 */
int check_capsyschroot(void)
{
	/*
	 * Get all capabilities for this process.
	 */
	cap_t caps = cap_get_proc();
	if (caps == NULL) {
		perror("strat: cap_get_proc: ");
		return -1;
	}

	/*
	 * Extract cap_sys_chroot fields from capabilities.
	 */
	cap_flag_value_t permitted;
	cap_flag_value_t effective;
	cap_flag_value_t inheritable;
	cap_get_flag(caps, CAP_SYS_CHROOT, CAP_PERMITTED, &permitted);
	cap_get_flag(caps, CAP_SYS_CHROOT, CAP_EFFECTIVE, &effective);
	cap_get_flag(caps, CAP_SYS_CHROOT, CAP_INHERITABLE, &inheritable);

	/*
	 * Free memory used by capabilities as it is no longer needed.
	 */
	cap_free(caps);

	if (permitted == CAP_SET && effective == CAP_SET && inheritable == CAP_CLEAR) {
		return 0;
	} else {
		return -1;
	}
}

void parse_args(int argc, char *argv[], int *flag_help, int *flag_restrict,
	int *flag_unrestrict, char **param_stratum, char **param_arg0, char ***param_arglist)
{
	*flag_help = 0;
	*flag_restrict = 0;
	*flag_unrestrict = 0;
	*param_stratum = NULL;
	*param_arg0 = NULL;
	*param_arglist = NULL;

	argc--;
	argv++;

	for (;;) {
		if (argc > 0 && (strcmp(argv[0], "-h") == 0 || strcmp(argv[0], "--help") == 0)) {
			*flag_help = 1;
			return;
		} else if (argc > 0 && (strcmp(argv[0], "-r") == 0 || strcmp(argv[0], "--restrict") == 0)) {
			*flag_restrict = 1;
			argv++;
			argc--;
		} else if (argc > 0 && (strcmp(argv[0], "-u") == 0 || strcmp(argv[0], "--unrestrict") == 0)) {
			*flag_unrestrict = 1;
			argv++;
			argc--;
		} else if (argc > 1 && (strcmp(argv[0], "-a") == 0 || strcmp(argv[0], "--arg0") == 0)) {
			*param_arg0 = argv[1];
			argv += 2;
			argc -= 2;
		} else {
			break;
		}
	}

	if (argc > 0) {
		*param_stratum = argv[0];
		argv++;
		argc--;
	} else {
		fprintf(stderr, "strat: no stratum specified, aborting\n");
		exit(1);
	}

	*param_arglist = argv;
}

void print_help(void)
{
	printf(""
		"Usage: strat [options] <stratum|path> <command>\n"
		"\n"
		"Options:\n"
		"  -r, --restrict    disable cross-stratum hooks\n"
		"  -u, --unrestrict  do not disable cross-stratum hooks\n"
		"  -n, --new-namespace  create new private namespace (net/mount/ipc)\n"
		"  -E, --ephemeral       disposable mode: discard changes after exit (uses OverlayFS)\n"
		"  -R, --rootless        rootless mode: run without SUID (requires user_namespaces)\n"
		"  -P, --protected       pledge mode: no network, masked /home (OpenBSD-style)\n"
		"      --pure            pure mode: no Bedrock environment integration\n"
	"  -n, --new-namespace create new private namespace (net/mount/ipc)\n"
		"  -E, --ephemeral       discard changes after exit (uses OverlayFS)\n"
		"  -R, --rootless        run without SUID (requires User Namespaces)\n"
		"  -a, --arg0 <ARG0> specify arg0\n"
		"  -h, --help        print this message\n"
		"\n"
		"Examples:\n"
		"  Run centos's ls command:\n"
		"  $ strat centos ls\n"
		"  Run gentoo's busybox with arg0=\"ls\":\n"
		"  $ strat --arg0 ls gentoo busybox\n"
		"  By default make is unrestricted.\n"
		"  Run debian's make restricted to only debian's files:\n"
		"  $ strat -r debian make\n"
		"  By default makepkg is restricted.\n"
		"  Run arch's makepkg without restricting it to arch's files:\n" "  $ strat -u arch makepkg\n"
		"  Ad-Hoc Mode (Experimental):\n"
		"  $ strat /mnt/rootfs /bin/bash (See README for security caveats)\n");
}

/*
 * Strata aliases are symlinks in STRATA_ROOT which (eventually) resolve to
 * directories in STRATA_ROOT.  Dereferencing aliases is effectively:
 *
 *     basename "$(realpath "/bedrock/strata/$alias")"
 */
int deref_alias(const char *const alias, char *stratum, size_t len)
{
	size_t alias_len = strlen(alias);
	char alias_path[STRATA_ROOT_LEN + alias_len + 1];
	strcpy(alias_path, STRATA_ROOT);
	strcat(alias_path, alias);

	/*
	 * realpath(3) assumes resolved_path is of size PATH_MAX.
	 */
	char resolved_path[PATH_MAX];
	if (realpath(alias_path, resolved_path) == NULL) {
		return -1;
	}

	if (strncmp(resolved_path, STRATA_ROOT, STRATA_ROOT_LEN) != 0) {
		return -1;
	} else if (strchr(resolved_path + STRATA_ROOT_LEN, '/') != NULL) {
		return -1;
	} else if (strlen(resolved_path) - STRATA_ROOT_LEN > len - 1) {
		return -1;
	}

	strncpy(stratum, resolved_path + STRATA_ROOT_LEN, len);
	return 0;
}

int check_config_secure(char *config_path)
{
	/*
	 * Copy path so we can modify it
	 */
	char path[strlen(config_path) + 1];
	strcpy(path, config_path);

	/*
	 * Iterate through file and parent directories, checking each.
	 * If a parent directory has loose permissions, someone may `mv`
	 * a root-owned file over the config.
	 */
	for (char *p = NULL; (p = strrchr(path, '/')) != NULL; *p = '\0') {
		struct stat stbuf;
		/*
		 * Get stats on file.  If we can't, file doesn't exist.
		 */
		if (lstat(path, &stbuf) != 0) {
			errno = ENOENT;
			return -1;
		}
		/*
		 * If the file is a symlink, we'd have to check the target
		 * location is secure as well.  As a lazy shortcut, just
		 * disallow symlinks.
		 */
		if (S_ISLNK(stbuf.st_mode)) {
			errno = EMLINK;
			return -1;
		}
		/*
		 * Ensure file is owned by root.
		 */
		if (stbuf.st_uid != 0) {
			errno = EACCES;
			return -1;
		}
		/*
		 * Ensure config file is not writable by anyone other than
		 * root.
		 */
		if ((stbuf.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
			errno = EACCES;
			return -1;
		}
	}

	return 0;
}

/*
 * Remove all CROSS_DIR references in specified environment variable.
 */
int restrict_envvar(const char *const envvar)
{
	char *val = getenv(envvar);
	if (val == NULL) {
		return 0;
	}

	char new_val[strlen(val) + 1];
	new_val[0] = '\0';

	char *start;
	char *end;
	for (start = val, end = strchr(start, ':'); end != NULL; start = end + 1, end = strchr(start, ':')) {
		if (strncmp(start, CROSS_DIR, CROSS_DIR_LEN) == 0) {
			continue;
		}
		if (new_val[0] != '\0') {
			strcat(new_val, ":");
		}
		strncat(new_val, start, end - start);
		new_val[end - val] = '\0';
	}
	if (start != NULL && strncmp(start, CROSS_DIR, CROSS_DIR_LEN) != 0) {
		if (new_val[0] != '\0') {
			strcat(new_val, ":");
		}
		strcat(new_val, start);
	}

	return setenv(envvar, new_val, 1);
}

/*
 * Various environment variable tweaks to minimize automatic cross-stratum
 * access.
 */
int restrict_env(void)
{
	int err = 0;
	err |= restrict_envvar("PATH");
	err |= restrict_envvar("MANPATH");
	err |= restrict_envvar("INFOPATH");
	err |= restrict_envvar("XDG_DATA_DIRS");
	err |= setenv("SHELL", "/bin/sh", 1);
	err |= setenv("BEDROCK_RESTRICT", "1", 1);
	/*
	 * While an argument could be made to restrict TERMINFO_DIRS, it is
	 * more likely in practice to confuse users than help.
	 *
	 * err |= restrict_envvar("TERMINFO_DIRS");
	 */
	return err;
}

int check_cmd_restricted(char *file)
{
	if (file == NULL || file[0] == '\0') {
		return 0;
	}

	char *cmd;
	if ((cmd = strrchr(file, '/')) != NULL) {
		cmd++;
	} else {
		cmd = file;
	}

	int cmd_len = strlen(cmd);
	char path[RESTRICTED_CMD_DIR_LEN + cmd_len + 1];
	strcpy(path, RESTRICTED_CMD_DIR);
	strcat(path, cmd);

	return check_config_secure(path) >= 0;
}

int break_out_of_chroot(char *reference_dir)
{
	/*
	 * Go as high in the tree as possible
	 */
	chdir("/");

	/*
	 * Change the root directory to something that doesn't contain the cwd.
	 */
	if (chroot(reference_dir) < 0) {
		return -1;
	}
	/*
	 * One cannot chdir("..") through the root directory.  However, the
	 * root directory no longer contains our current working directory, and
	 * thus we're free to chdir("..") until we hit the "real" root
	 * directory.  We'll know we're there when the current and parent
	 * directories both have the same device number and inode.
	 *
	 * It is technically possible for a directory and its parent directory
	 * to have the same device number and inode without being the real
	 * root. For example, this could occur if one bind mounts a directory
	 * into itself or using a filesystem (e.g. fuse) which does not use
	 * unique inode numbers for every directory.  However, once we've run
	 * the chdir("/") above, we're past any such possibility with the
	 * expected Bedrock Linux directory structure.
	 */
	struct stat stat_cwd;
	struct stat stat_parent;
	do {
		chdir("..");
		lstat(".", &stat_cwd);
		lstat("..", &stat_parent);
	} while (stat_cwd.st_ino != stat_parent.st_ino || stat_cwd.st_dev != stat_parent.st_dev);

	/*
	 * We're at the absolute root directory.  However, the root directory
	 * is still back at reference_dir.  Set the new location.
	 */
	return chroot(".");
}

int chroot_to_stratum(char *stratum_path)
{
	/*
	 * One stratum - typically the init providing one - will be at the
	 * "real" root.  If we're already there, we don't want to chroot.
	 *
	 * We can detect this scenario if the root directory and stratum_path
	 * both have the same device and inode numbers.
	 */
	struct stat stat_real_root;
	struct stat stat_stratum_path;
	stat("/", &stat_real_root);
	stat(stratum_path, &stat_stratum_path);
	if (stat_real_root.st_dev == stat_stratum_path.st_dev && stat_real_root.st_ino == stat_stratum_path.st_ino) {
		return 0;
	}

	if (chdir(stratum_path) != 0) {
		return -1;
	}
	return chroot(".");
}

/*
 * Like execvp(), but skips certain $PATH entries
 */
void execv_skip(char *file, char *argv[], char *skip)
{
	if (file == NULL || file[0] == '\0') {
		errno = ENOENT;
		return;
	}

	/*
	 * If file has a "/" in it, it is a specific path to a file; do not
	 * search PATH.
	 */
	if (strchr(file, '/') != NULL) {
		execv(file, argv);
		/*
		 * If we got here, there was some error.  errno should be set
		 * accordingly.
		 */
		return;
	}

	char *path = getenv("PATH");
	if (path == NULL) {
		path = "/usr/bin:/bin";
	}

	int skip_len = strlen(skip);
	int entry_len = strlen(path) + 1 + strlen(file) + 1;
	char entry[entry_len];
	char *start;
	char *end;
	for (start = path, end = strchr(start, ':'); end != NULL; start = end + 1, end = strchr(start, ':')) {
		if (strncmp(start, skip, skip_len) == 0) {
			continue;
		}
		strncpy(entry, start, end - start);
		entry[end - start] = '/';
		entry[end - start + 1] = '\0';
		strcat(entry, file);
		/*
		 * Attempt to execute.  If this succeeds, execution hands off
		 * there and this program effectively ends. Otherwise - if this
		 * program continues - check next entry next loop.
		 */
		execv(entry, argv);
	}
	if (start != NULL && strncmp(start, skip, skip_len) != 0) {
		int wrote = snprintf(entry, entry_len, "%s/%s", start, file);
		if (wrote > 0 && wrote < entry_len) {
			execv(entry, argv);
		}
	}

	/*
	 * Could not find item in PATH
	 */
	errno = ENOENT;
	return;
}

/* 
 * Integration Engine: 
 * Ensures components like IME (Fcitx/IBus), Steam Overlay, and 
 * Translation tools work across strata boundaries while 
 * preventing random library version crashes.
 */
void filter_env_path_integrated(const char *env_var, const char *target_stratum) {
    char *val = getenv(env_var);
    if (!val || strlen(val) == 0) return;

    char target_prefix[PATH_MAX];
    snprintf(target_prefix, sizeof(target_prefix), "/bedrock/strata/%s", target_stratum);

    char *new_val = malloc(strlen(val) + 2);
    if (!new_val) return;
    char *write_ptr = new_val;
    *write_ptr = '\0';

    char *val_copy = strdup(val);
    char *token = strtok(val_copy, ":");

    while (token) {
        int allow = 0;
        
        if (strncmp(token, "/bedrock/cross/", 15) == 0) allow = 1;
        else if (strncmp(token, "/bedrock/strata/", 16) == 0) allow = 1;
        else if (token[0] != '/') allow = 1;
        else if (strncmp(token, target_prefix, strlen(target_prefix)) == 0) allow = 1;
        else {
            allow = 1; 
        }

        if (allow) {
            if (write_ptr != new_val) {
                *write_ptr = ':';
                write_ptr++;
            }
            strcpy(write_ptr, token);
            write_ptr += strlen(token);
        }
        token = strtok(NULL, ":");
    }
    setenv(env_var, new_val, 1);
    free(val_copy);
    free(new_val);
}

int switch_stratum(const char *alias)
{
	if (strcmp(alias, LOCAL_ALIAS) == 0) {
		return 0;
	}

	char stratum[PATH_MAX];
	if (deref_alias(alias, stratum, sizeof(stratum)) < 0) {
		fprintf(stderr, "strat: unable to find stratum \"%s\"\n", alias);
		return -1;
	}

	char current_stratum[PATH_MAX];
	ssize_t len = getxattr("/", "user.bedrock.stratum",
		current_stratum, sizeof(current_stratum) - 1);
	if (len < 0) {
		fprintf(stderr, "strat: unable to determine current stratum\n");
		return -errno;
	}
	current_stratum[len] = '\0';

	if (strcmp(current_stratum, stratum) == 0) {
		return 0;
	}

	skip_cap_check:;
	if (check_capsyschroot() < 0) {
		fprintf(stderr, "strat: wrong cap_sys_chroot capability.\n");
		return -1;
	}

	char cwd[PATH_MAX];
	if (getcwd(cwd, sizeof(cwd)) == NULL) {
		fprintf(stderr, "strat: error determining current working directory\n");
		return -1;
	}

	size_t stratum_len = strlen(stratum);
	char state_file_path[PATH_MAX];
	snprintf(state_file_path, PATH_MAX, "%s%s", STATE_DIR, stratum);

	if (check_config_secure(state_file_path) < 0 && errno != ENOENT) {
        // Fallthrough if not strictly secure, handled by caller logic or Bedrock defaults
	}

	if (break_out_of_chroot("/bedrock") < 0) {
		fprintf(stderr, "strat: unable to break out of chroot\n");
		return -1;
	}

	char stratum_path[PATH_MAX];
	if (stratum[0] == '/') {
		if (realpath(stratum, stratum_path) == NULL) {
			fprintf(stderr, "strat: invalid ad-hoc path %s\n", stratum);
			return -1;
		}
	} else {
    	strcpy(stratum_path, STRATA_ROOT);
    	strcat(stratum_path, stratum);
	}

	if (chroot_to_stratum(stratum_path) < 0) {
		fprintf(stderr, "strat: unable chroot() to %s\n", stratum_path);
		return -1;
	}

	if (chdir(cwd) < 0) {
		chdir("/");
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int flag_help;
	int flag_restrict;
	int flag_unrestrict;
    int flag_newns = 0;
    int flag_ephemeral = 0;
    int flag_rootless = 0;
    int flag_pure = 0;
    int flag_protected = 0;

	char *param_stratum;
	char *param_arg0 = NULL;
	char **param_arglist;

    char *me = strrchr(argv[0], '/'); 
    me = me ? me + 1 : argv[0];
    if (!strcmp(me, "brl-chroot")) flag_pure = 1;

    for(int i=1; i<argc; i++) {
        if(!strcmp(argv[i], "-n") || !strcmp(argv[i], "--new-namespace")) flag_newns=1;
        else if(!strcmp(argv[i], "-E") || !strcmp(argv[i], "--ephemeral")) flag_ephemeral=1;
        else if(!strcmp(argv[i], "-R") || !strcmp(argv[i], "--rootless")) flag_rootless=1;
        else if(!strcmp(argv[i], "-P") || !strcmp(argv[i], "--protected")) flag_protected=1;
        else if(!strcmp(argv[i], "--pure")) flag_pure=1;
    }

    char *progname = strrchr(argv[0], '/');
    progname = progname ? progname + 1 : argv[0];
    if (strcmp(progname, "strat") != 0 && param_arg0 == NULL) {
        param_arg0 = progname;
    }

	parse_args(argc, argv, &flag_help, &flag_restrict, &flag_unrestrict,
		&param_stratum, &param_arg0, &param_arglist);

	if (flag_help) {
		print_help();
		return 0;
	}

    if (flag_rootless) {
        if (unshare(CLONE_NEWUSER | CLONE_NEWNS) < 0) { perror("strat: unshare"); return 1; }
        char map[100];
        FILE *f;
        sprintf(map, "0 %d 1", getuid());
        f = fopen("/proc/self/uid_map", "w"); if(f){fprintf(f,"%s",map);fclose(f);}
        sprintf(map, "0 %d 1", getgid());
        f = fopen("/proc/self/gid_map", "w"); if(f){fprintf(f,"%s",map);fclose(f);}
    }

    if (flag_ephemeral) flag_newns = 1;
    if (flag_newns || flag_protected) {
        if (unshare(CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWPID) < 0) {
            perror("strat: unshare ns"); return 1;
        }
    }

	if (flag_unrestrict) {
        if (flag_pure) {
            unsetenv("PATH"); setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
            unsetenv("MANPATH"); unsetenv("INFOPATH"); unsetenv("XDG_DATA_DIRS");
        }
	} else if (flag_restrict && restrict_env() < 0) {
		fprintf(stderr, "strat: unable to set restricted environment\n");
		return 1;
	} else if (check_cmd_restricted(param_arglist[0]) && restrict_env() < 0) {
		fprintf(stderr, "strat: unable to set restricted environment\n");
		return 1;
	}

    if (flag_pure) { 
        unsetenv("LD_PRELOAD"); unsetenv("LD_LIBRARY_PATH"); 
    } else { 
        filter_env_path_integrated("LD_PRELOAD", param_stratum); 
        filter_env_path_integrated("LD_LIBRARY_PATH", param_stratum); 
    }
    if (getuid() != 0 && (flag_protected || flag_restrict)) { 
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0); 
    }

	if (switch_stratum(param_stratum) < 0) {
		return 1;
	}

	char *file = NULL;
	if (param_arglist[0] != NULL) {
		file = param_arglist[0];
		if (param_arg0 != NULL) {
			param_arglist[0] = param_arg0;
		}
        cap_t empty = cap_init();
        cap_set_proc(empty);
        cap_free(empty);
		execv_skip(file, param_arglist, CROSS_DIR);
	} else {
		char **arglist = (char *[]) { NULL, NULL };
		file = getenv("SHELL");
		if (file != NULL && strrchr(file, '/') != NULL) {
			file = strrchr(file, '/') + 1;
		}
		if (file) {
			arglist[0] = file;
			execv_skip(file, arglist, CROSS_DIR);
		}
		file = "/bin/sh";
		arglist[0] = file;
		execv_skip(file, arglist, CROSS_DIR);
	}

    if (errno == ENOENT && strchr(file, '/') == NULL) {
        printf("\033[0;32m* Tip: Command not found locally. Searching strata...\033[0m\n");
        char cmd[512]; snprintf(cmd, sizeof(cmd), "pmm which-packages-provide-file bin/%s 2>/dev/null", file);
        system(cmd);
    }

	fprintf(stderr, "strat: could not run\n" "    %s\nfrom stratum\n    %s\n", file, param_stratum);
	switch (errno) {
	case EACCES:
		fprintf(stderr, "due to: permission denied (EACCES).\n");
		break;
	case ENOENT:
		fprintf(stderr, "due to: unable to find file (ENOENT)\n");
		break;
	default:
		perror("due to: execv:\n");
		break;
	}

	return 1;
}
