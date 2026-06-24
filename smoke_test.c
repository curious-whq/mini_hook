#include <stdlib.h>

int main(void)
{
    volatile unsigned char *ptr = malloc(64);
    if (ptr == NULL) {
        return 1;
    }

    ptr[0] = 0x5a;
    free((void *)ptr);

    ptr = calloc(8, 16);
    if (ptr == NULL || ptr[0] != 0) {
        return 2;
    }

    ptr = realloc((void *)ptr, 256);
    if (ptr == NULL) {
        return 3;
    }
    free((void *)ptr);
    return 0;
}
