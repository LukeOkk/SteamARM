// A SIGSEGV handler that returns without fixing anything: on Linux the
// faulting load runs again, faults again, forever. Under lxrun the thread
// dies of the fault after 10000 identical ones (runtime/signal.c), so this
// program ends by signal 11 within seconds instead of spinning a core.
#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile unsigned long hits;
static void handler(int sig) { (void)sig; hits++; }

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigaction(SIGSEGV, &sa, NULL);
    printf("spinning\n");
    fflush(stdout);
    volatile int *p = (volatile int *)16;
    return *p;
}
