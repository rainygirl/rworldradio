/* PID 1 for the QEMU PowerPC run.
 *
 * The test suite is linked in with its main() renamed (-Dmain=rwr_test_main),
 * so this is one static binary rather than an init plus a child process - the
 * initramfs then needs no shell and no /proc.
 *
 * Exiting as PID 1 panics the kernel, so the result is printed with a marker the
 * host greps for and then the machine is powered off explicitly. */

#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>

int rwr_test_main(int argc, char **argv);

int main(void)
{
    char *argv[] = { "test_core", "/stations.dat", "/cacert.der", NULL };
    int status;

    printf("### RWR-BEGIN big-endian PowerPC run ###\n");
    fflush(stdout);

    status = rwr_test_main(3, argv);

    printf("### RWR-EXIT %d ###\n", status);
    fflush(stdout);
    fflush(stderr);
    sync();

    /* LINUX_REBOOT_CMD_POWER_OFF; spelled out so this needs no kernel header. */
    reboot(0x4321FEDC);
    for (;;)
        pause();
    return status;
}
