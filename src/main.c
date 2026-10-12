#include "lab.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef TEST
#define main main_exclude
#endif

int main(void)
{
    char *greeting = get_greeting("World");
    if (greeting) {
        printf("%s\n", greeting);
        free(greeting); // Free the allocated memory for the greeting
    } else {
        printf("Failed to create greeting.\n");
    }

    printf("3 * 4 = %d\n", multiply(3, 4));
    printf("bad_add(2, 3) = %d (intentionally wrong; the real sum is 5)\n", bad_add(2, 3));

    return 0;
}