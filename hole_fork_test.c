#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    void *parent_ptr = malloc(7);
    if (parent_ptr == NULL) {
        return 1;
    }
    free(parent_ptr);

    pid_t child = fork();
    if (child < 0) {
        return 2;
    }
    if (child == 0) {
        void *child_ptr = malloc(9);
        if (child_ptr == NULL) {
            exit(3);
        }
        free(child_ptr);
        exit(0);
    }

    int status = 0;
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return 4;
    }
    return 0;
}
