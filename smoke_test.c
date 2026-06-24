#include <stdlib.h>

int main(void)
{
    volatile unsigned char *ptr = malloc(64);
    if (ptr == NULL) {
        return 1;
    }

    ptr[0] = 0x5a;
    free((void *)ptr);
    return 0;
}
