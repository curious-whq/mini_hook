#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

typedef int (*snapshot_fn)(void);

int main(void)
{
    snapshot_fn snapshot =
        (snapshot_fn)dlsym(RTLD_DEFAULT, "mini_hole_snapshot_now");
    volatile unsigned char *inherited = malloc(7);
    if (snapshot == NULL || inherited == NULL) {
        _exit(1);
    }
    inherited[0] = 7;

    pid_t child = fork();
    if (child < 0) {
        _exit(2);
    }
    if (child == 0) {
        if (snapshot() != 0) {
            _exit(3);
        }
        free((void *)inherited);
        if (snapshot() != 0) {
            _exit(4);
        }

        volatile unsigned char *nine = malloc(9);
        if (nine == NULL) {
            _exit(5);
        }
        nine[0] = 9;
        if (snapshot() != 0) {
            _exit(6);
        }
        free((void *)nine);
        if (snapshot() != 0) {
            _exit(7);
        }
        _exit(0);
    }

    int status = 0;
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        _exit(8);
    }
    free((void *)inherited);
    _exit(0);
}
