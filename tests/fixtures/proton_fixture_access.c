#define _GNU_SOURCE

#include <pthread.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <time.h>

static void EnableProcessInspection(void) {
    (void)prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
    (void)prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
}

static void *MaintainProcessInspection(void *context) {
    const struct timespec interval = { .tv_sec = 0, .tv_nsec = 50000000 };
    int attempt;

    (void)context;
    for (attempt = 0; attempt < 200; attempt++) {
        EnableProcessInspection();
        (void)nanosleep(&interval, NULL);
    }

    return NULL;
}

__attribute__((constructor)) static void StartProcessInspection(void) {
    pthread_t thread;

    EnableProcessInspection();
    if (pthread_create(&thread, NULL, MaintainProcessInspection, NULL) == 0) {
        (void)pthread_detach(thread);
    }
}
