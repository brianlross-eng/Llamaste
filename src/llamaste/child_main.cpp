#include "supervisor.h"
#include <cstdio>
#include <unistd.h>
#include <csignal>

static volatile bool g_running = true;

static void child_signal(int) {
    g_running = false;
}

int child_main(const SupervisorConfig& config) {
    signal(SIGTERM, child_signal);
    signal(SIGINT, child_signal);

    fprintf(stderr, "[child] Inference child started\n");
    fprintf(stderr, "[child] Model: %s\n",
            config.model_path.empty() ? "(none)" : config.model_path.c_str());
    fprintf(stderr, "[child] Port: %d\n", config.http_port);

    fprintf(stderr, "[child] Stub server running (waiting for real implementation)\n");

    while (g_running) {
        sleep(1);
    }

    fprintf(stderr, "[child] Shutting down gracefully\n");
    return 0;
}
