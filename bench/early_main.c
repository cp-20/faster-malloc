/* early_main.c -- see early_lib.c.  Prints the constructor-built
 * string and exits nonzero unless it survived intact. */
#include <stdio.h>
#include <string.h>

const char *early_str(void);

int main(void)
{
    const char *s = early_str();
    if (!s || strcmp(s, "early-grown") != 0) {
        printf("earlyinit FAIL: %s\n", s ? s : "(null)");
        return 1;
    }
    printf("earlyinit ok\n");
    return 0;
}
