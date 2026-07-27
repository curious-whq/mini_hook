#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    void *first = malloc(9);
    if (first == NULL) {
        return 1;
    }
    free(first);

    sleep(2);

    void *second = malloc(9);
    if (second == NULL) {
        return 2;
    }
    free(second);
    return 0;
}
