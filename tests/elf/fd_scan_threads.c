// What Android's zygote does before every fork: list /proc/self/fd and stat
// each descriptor, refusing any directory but the listing's own -- while
// threads exit (the runtime removes their /proc/self/task entries). A
// descriptor the runtime opens for that must never show up in the list.
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void *quick(void *a)
{
    (void)a;
    struct timespec ts = { 0, 3000000 };     // long enough to get a /proc/self/task entry
    nanosleep(&ts, NULL);
    return NULL;
}

static void *spawner(void *a)
{
    volatile int *stop = a;
    while (!*stop) {
        pthread_t t[8];
        for (int i = 0; i < 8; i++) pthread_create(&t[i], NULL, quick, NULL);
        for (int i = 0; i < 8; i++) pthread_join(t[i], NULL);
    }
    return NULL;
}

int main(void)
{
    volatile int stop = 0;
    pthread_t sp;
    pthread_create(&sp, NULL, spawner, (void *)&stop);
    int scans = 0, bad = 0;
    char what[128] = "";
    time_t end = time(NULL) + 3;
    while (time(NULL) < end) {
        DIR *task = opendir("/proc/self/task");      // the runtime makes an entry per thread
        if (task) { while (readdir(task)) {} closedir(task); }
        DIR *d = opendir("/proc/self/fd");
        if (!d) continue;
        int self = dirfd(d);
        struct dirent *e;
        while ((e = readdir(d))) {
            int fd = atoi(e->d_name);
            if (e->d_name[0] == '.' || fd == self) continue;
            struct stat st;
            if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
                bad++;
                snprintf(what, sizeof what, "fd %d is a directory", fd);
            }
        }
        closedir(d);
        scans++;
    }
    stop = 1;
    pthread_join(sp, NULL);
    int ok = bad == 0 && scans > 20;
    printf("  %s  %d scans of /proc/self/fd while threads came and went: %d directories%s%s\n",
           ok ? "OK " : "MAL", scans, bad, bad ? " -- " : "", what);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return !ok;
}
