#include "supervisor.h"
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <fcntl.h>
#include <time.h>

static volatile sig_atomic_t g_child_exited = 0;
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile pid_t g_child_pid = 0;

static void supervisor_sigchld(int) {
    g_child_exited = 1;
}

static void supervisor_sigterm(int) {
    g_shutdown_requested = 1;
}

extern int child_main(const SupervisorConfig& config);

static pid_t spawn_child(const SupervisorConfig& config) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("[supervisor] fork failed");
        return -1;
    }
    if (pid == 0) {
        int rc = child_main(config);
        _exit(rc);
    }
    return pid;
}

static int open_watchdog() {
    int fd = open("/dev/watchdog", O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[supervisor] No hardware watchdog available\n");
    } else {
        fprintf(stderr, "[supervisor] Hardware watchdog opened\n");
    }
    return fd;
}

static void kick_watchdog(int fd) {
    if (fd >= 0) {
        write(fd, "V", 1);
    }
}

[[noreturn]] void supervisor_run(const SupervisorConfig& config) {
    struct sigaction sa = {};
    sa.sa_handler = supervisor_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, nullptr);

    sa.sa_handler = supervisor_sigterm;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    int watchdog_fd = open_watchdog();
    int crash_count = 0;
    time_t last_crash = 0;

    while (!g_shutdown_requested) {
        fprintf(stderr, "[supervisor] Spawning inference child...\n");
        g_child_exited = 0;
        g_child_pid = spawn_child(config);

        if (g_child_pid < 0) {
            fprintf(stderr, "[supervisor] Failed to spawn child, retrying in 5s\n");
            sleep(5);
            continue;
        }

        fprintf(stderr, "[supervisor] Child PID %d running\n", g_child_pid);

        while (!g_child_exited && !g_shutdown_requested) {
            kick_watchdog(watchdog_fd);
            sleep(10);
        }

        if (g_shutdown_requested) break;

        int status = 0;
        waitpid(g_child_pid, &status, 0);

        if (WIFEXITED(status)) {
            fprintf(stderr, "[supervisor] Child exited with code %d\n",
                    WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "[supervisor] Child killed by signal %d\n",
                    WTERMSIG(status));
        }

        time_t now = time(nullptr);
        if (now - last_crash < 60) {
            crash_count++;
        } else {
            crash_count = 1;
        }
        last_crash = now;

        if (crash_count > 3) {
            fprintf(stderr,
                "[supervisor] Too many crashes, waiting 30s before restart\n");
            sleep(30);
            crash_count = 0;
        } else {
            sleep(2);
        }
    }

    fprintf(stderr, "[supervisor] Shutting down...\n");

    if (g_child_pid > 0) {
        kill(g_child_pid, SIGTERM);
        int status;
        alarm(10);
        waitpid(g_child_pid, &status, 0);
        alarm(0);
    }

    if (watchdog_fd >= 0) {
        write(watchdog_fd, "V", 1);
        close(watchdog_fd);
    }

    sync();
    umount2("/data", MNT_DETACH);
    reboot(RB_POWER_OFF);
    _exit(0);
}
