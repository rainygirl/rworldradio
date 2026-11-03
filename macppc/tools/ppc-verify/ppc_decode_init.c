/* PID 1 for the decode run: decodes both fixtures, then powers off. */
#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>

int decode_probe_main(int argc, char **argv);

int main(void)
{
    char *mp3[] = { "decode_probe", "/test.mp3", "mp3", NULL };
    char *aac[] = { "decode_probe", "/test.aac", "aac", NULL };

    printf("### RWR-DECODE-BEGIN ###\n");
    decode_probe_main(3, mp3);
    printf("---\n");
    decode_probe_main(3, aac);
    printf("### RWR-DECODE-END ###\n");
    fflush(stdout);
    sync();
    reboot(0x4321FEDC);
    for (;;) pause();
    return 0;
}
