/*
 * kmon.c - Bedrock Linux Kernel Monitor
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <time.h>
#include <fcntl.h>

#define EVENT_SIZE  (sizeof(struct inotify_event))
#define BUF_LEN     (1024 * (EVENT_SIZE + 16))
#define DEBOUNCE_MS 5000 

#define CONFIG_FILE "/bedrock/run/kmon_config"
#define LOG_TAG "brl-kmon: "

char g_stratum[256] = {0};
char g_command[1024] = {0};

void load_config() {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (!fp) return;
    char line[1280];
    if (fgets(line, sizeof(line), fp)) {
        char *sep = strchr(line, ':');
        if (sep) {
            *sep = '\0';
            strncpy(g_stratum, line, sizeof(g_stratum) - 1);
            strncpy(g_command, sep + 1, sizeof(g_command) - 1);
            g_command[strcspn(g_command, "\n")] = 0;
        }
    }
    fclose(fp);
}

void trigger_update() {
    load_config();
    if (strlen(g_stratum) == 0 || strlen(g_command) == 0) return;
    printf(LOG_TAG "Triggering update on stratum '%s' with '%s'\n", g_stratum, g_command);
    pid_t pid = fork();
    if (pid == 0) {
        char *args[] = {"/bedrock/bin/strat", "-r", g_stratum, "/bedrock/libexec/busybox", "sh", "-c", g_command, NULL};
        execv(args[0], args);
        _exit(1);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) return 1;
    int wd = inotify_add_watch(fd, "/boot", IN_CLOSE_WRITE | IN_MOVED_TO);
    if (wd < 0) return 1;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pending_update = 0;
    int timeout = -1;
    while (1) {
        int ret = poll(&pfd, 1, timeout);
        if (ret == 0 && pending_update) {
            trigger_update();
            pending_update = 0;
            timeout = -1;
            continue;
        }
        if (pfd.revents & POLLIN) {
            char buf[BUF_LEN];
            int len = read(fd, buf, BUF_LEN);
            if (len > 0) {
                int i = 0;
                while (i < len) {
                    struct inotify_event *event = (struct inotify_event *) &buf[i];
                    if (event->len) pending_update = 1;
                    i += EVENT_SIZE + event->len;
                }
                if (pending_update) timeout = DEBOUNCE_MS;
            }
        }
    }
    return 0;
}
