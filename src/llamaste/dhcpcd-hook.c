// dhcpcd-hook.c — Minimal dhcpcd hook replacement (no /bin/sh needed)
//
// dhcpcd calls this binary instead of the shell-based dhcpcd-run-hooks.
// It reads DHCP parameters from environment variables and applies them
// using ioctl/netlink. This allows dhcpcd to work on systems without
// a shell (BR2_SYSTEM_BIN_SH_NONE=y).
//
// Environment variables set by dhcpcd:
//   interface         - network interface name (e.g. "wlan0")
//   reason            - DHCP event (BOUND, RENEW, REBIND, REBOOT, etc.)
//   new_ip_address    - assigned IP
//   new_subnet_mask   - subnet mask (or new_subnet_cidr)
//   new_routers       - default gateway(s), space-separated
//   new_domain_name_servers - DNS servers, space-separated
//   new_domain_name   - domain name
//   new_host_name     - hostname

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/wait.h>

static int set_ip_address(const char* ifname, const char* ip, const char* mask) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    // Set IP address
    struct sockaddr_in* addr = (struct sockaddr_in*)&ifr.ifr_addr;
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, ip, &addr->sin_addr);
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
        fprintf(stderr, "[dhcpcd-hook] SIOCSIFADDR %s %s: %s\n", ifname, ip, strerror(errno));
        close(fd);
        return -1;
    }

    // Set netmask
    if (mask && mask[0]) {
        memset(&ifr.ifr_netmask, 0, sizeof(ifr.ifr_netmask));
        addr = (struct sockaddr_in*)&ifr.ifr_netmask;
        addr->sin_family = AF_INET;
        inet_pton(AF_INET, mask, &addr->sin_addr);
        if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0) {
            fprintf(stderr, "[dhcpcd-hook] SIOCSIFNETMASK %s %s: %s\n", ifname, mask, strerror(errno));
        }
    }

    // Bring interface up
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ioctl(fd, SIOCSIFFLAGS, &ifr);
    }

    close(fd);
    return 0;
}

static int add_default_route(const char* ifname, const char* gateway) {
    // Delete old default route first
    pid_t pid = fork();
    if (pid == 0) {
        execl("/sbin/ip", "ip", "route", "del", "default", (char*)NULL);
        _exit(0);
    }
    if (pid > 0) waitpid(pid, NULL, 0);

    // Add new default route via ip command (SIOCADDRT is unreliable)
    pid = fork();
    if (pid == 0) {
        execl("/sbin/ip", "ip", "route", "add", "default",
              "via", gateway, "dev", ifname, (char*)NULL);
        _exit(1);
    }
    if (pid > 0) {
        int st = 0;
        waitpid(pid, &st, 0);
        int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        fprintf(stderr, "[dhcpcd-hook] ip route add default via %s dev %s (exit=%d)\n",
                gateway, ifname, rc);
        return rc == 0 ? 0 : -1;
    }
    return -1;
}

static void write_resolv_conf(const char* dns_servers) {
    if (!dns_servers || !dns_servers[0]) return;

    FILE* f = fopen("/etc/resolv.conf", "w");
    if (!f) return;

    // Parse space-separated DNS servers
    char buf[512];
    strncpy(buf, dns_servers, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* saveptr = NULL;
    char* token = strtok_r(buf, " ", &saveptr);
    while (token) {
        fprintf(f, "nameserver %s\n", token);
        token = strtok_r(NULL, " ", &saveptr);
    }
    fclose(f);
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    const char* ifname    = getenv("interface");
    const char* reason    = getenv("reason");
    const char* new_ip    = getenv("new_ip_address");
    const char* new_mask  = getenv("new_subnet_mask");
    const char* new_gw    = getenv("new_routers");
    const char* new_dns   = getenv("new_domain_name_servers");

    if (!ifname || !reason) {
        return 0;  // Missing required env vars — skip silently
    }

    fprintf(stderr, "[dhcpcd-hook] %s: reason=%s ip=%s gw=%s dns=%s\n",
            ifname, reason,
            new_ip ? new_ip : "(none)",
            new_gw ? new_gw : "(none)",
            new_dns ? new_dns : "(none)");

    // Handle BOUND, RENEW, REBIND, REBOOT — apply new config
    if (strcmp(reason, "BOUND") == 0 ||
        strcmp(reason, "RENEW") == 0 ||
        strcmp(reason, "REBIND") == 0 ||
        strcmp(reason, "REBOOT") == 0 ||
        strcmp(reason, "INFORM") == 0) {

        if (new_ip && new_ip[0]) {
            set_ip_address(ifname, new_ip, new_mask);
            fprintf(stderr, "[dhcpcd-hook] %s: configured %s/%s\n",
                    ifname, new_ip, new_mask ? new_mask : "?");
        }

        if (new_gw && new_gw[0]) {
            // Use first router only
            char gw_buf[64];
            strncpy(gw_buf, new_gw, sizeof(gw_buf) - 1);
            gw_buf[sizeof(gw_buf) - 1] = '\0';
            char* space = strchr(gw_buf, ' ');
            if (space) *space = '\0';
            add_default_route(ifname, gw_buf);
        }

        if (new_dns && new_dns[0]) {
            write_resolv_conf(new_dns);
        }
    }

    // Handle EXPIRE, RELEASE, STOP — could remove config, but we
    // let the interface go down naturally
    // (dhcpcd already handles removing the lease)

    return 0;
}
