#include <tigt.h>

int main(void)
{
    if (tigt_cp437_codepoint(0x82) != 0x00e9)
        return 1;
    if (tigt_snapshot_write_fd(-1, TIGT_SNAPSHOT_PNG) != TIGT_ERROR_ARGUMENT)
        return 2;
    return 0;
}
