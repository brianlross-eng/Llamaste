#include <stdio.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/reboot.h>

int main(void) {
    /* Mount /proc so we can read /proc/cmdline */
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

    printf("\n\n");
    printf("  Llamaste stub init — pipeline verification\n");
    printf("  If you see this, Buildroot + GRUB + kernel + PID 1 works!\n");
    printf("\n");

    /* Keep running (PID 1 must not exit) */
    while (1) {
        sleep(3600);
    }

    return 0;
}
