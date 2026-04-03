// netlink_route.h — Netlink RTM_NEWROUTE/RTM_DELROUTE for default gateway
// Replaces legacy SIOCADDRT ioctl and fork/exec "ip route" approaches,
// both of which silently failed on real hardware.
#pragma once
#ifndef _WIN32

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <arpa/inet.h>

// Delete the current default route (0.0.0.0/0) from the main routing table.
// Returns 0 on success or if no default route exists, -1 on error.
inline int netlink_del_default_route() {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        fprintf(stderr, "[netlink] del: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_nl sa = {};
    sa.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "[netlink] del: bind() failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    struct {
        struct nlmsghdr nh;
        struct rtmsg   rt;
    } req = {};

    req.nh.nlmsg_len   = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.nh.nlmsg_type  = RTM_DELROUTE;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.nh.nlmsg_seq   = 100;

    req.rt.rtm_family  = AF_INET;
    req.rt.rtm_dst_len = 0;           // 0.0.0.0/0
    req.rt.rtm_table   = RT_TABLE_MAIN;
    req.rt.rtm_scope   = RT_SCOPE_UNIVERSE;

    struct sockaddr_nl dest = {};
    dest.nl_family = AF_NETLINK;

    if (sendto(fd, &req, req.nh.nlmsg_len, 0,
               (struct sockaddr*)&dest, sizeof(dest)) < 0) {
        fprintf(stderr, "[netlink] del: sendto() failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    char buf[4096];
    int len = recv(fd, buf, sizeof(buf), 0);
    close(fd);

    if (len > 0) {
        struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
        if (nlh->nlmsg_type == NLMSG_ERROR) {
            struct nlmsgerr* err = (struct nlmsgerr*)NLMSG_DATA(nlh);
            if (err->error != 0 && err->error != -ESRCH) {
                // ESRCH = no such route — that's fine
                fprintf(stderr, "[netlink] del: RTM_DELROUTE error: %s\n",
                        strerror(-err->error));
                return -1;
            }
        }
    }
    fprintf(stderr, "[netlink] Deleted old default route (if any)\n");
    return 0;
}

// Add a default route (0.0.0.0/0) via gateway on the given interface.
// Uses NLM_F_REPLACE so it works even if a default route already exists.
// Returns 0 on success, -1 on failure.
inline int netlink_add_default_route(const char* gateway, const char* iface) {
    struct in_addr gw_addr;
    if (inet_pton(AF_INET, gateway, &gw_addr) != 1) {
        fprintf(stderr, "[netlink] add: invalid gateway '%s'\n", gateway);
        return -1;
    }

    unsigned int ifindex = if_nametoindex(iface);
    if (ifindex == 0) {
        fprintf(stderr, "[netlink] add: unknown interface '%s': %s\n",
                iface, strerror(errno));
        return -1;
    }

    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        fprintf(stderr, "[netlink] add: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_nl sa = {};
    sa.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "[netlink] add: bind() failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    // Build RTM_NEWROUTE message with RTA_GATEWAY + RTA_OIF
    struct {
        struct nlmsghdr nh;
        struct rtmsg   rt;
        char           attrbuf[256];
    } req = {};

    req.nh.nlmsg_len   = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.nh.nlmsg_type  = RTM_NEWROUTE;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE | NLM_F_ACK;
    req.nh.nlmsg_seq   = 101;

    req.rt.rtm_family   = AF_INET;
    req.rt.rtm_dst_len  = 0;           // 0.0.0.0/0 = default route
    req.rt.rtm_src_len  = 0;
    req.rt.rtm_tos      = 0;
    req.rt.rtm_table    = RT_TABLE_MAIN;
    req.rt.rtm_protocol = RTPROT_STATIC;
    req.rt.rtm_scope    = RT_SCOPE_UNIVERSE;
    req.rt.rtm_type     = RTN_UNICAST;

    // Append RTA_GATEWAY (4 bytes = IPv4 address)
    struct rtattr* rta;
    rta = (struct rtattr*)(((char*)&req) + NLMSG_ALIGN(req.nh.nlmsg_len));
    rta->rta_type = RTA_GATEWAY;
    rta->rta_len  = RTA_LENGTH(4);
    memcpy(RTA_DATA(rta), &gw_addr.s_addr, 4);
    req.nh.nlmsg_len = NLMSG_ALIGN(req.nh.nlmsg_len) + RTA_ALIGN(rta->rta_len);

    // Append RTA_OIF (4 bytes = interface index)
    rta = (struct rtattr*)(((char*)&req) + NLMSG_ALIGN(req.nh.nlmsg_len));
    rta->rta_type = RTA_OIF;
    rta->rta_len  = RTA_LENGTH(4);
    memcpy(RTA_DATA(rta), &ifindex, 4);
    req.nh.nlmsg_len = NLMSG_ALIGN(req.nh.nlmsg_len) + RTA_ALIGN(rta->rta_len);

    // Send to kernel
    struct sockaddr_nl dest = {};
    dest.nl_family = AF_NETLINK;

    if (sendto(fd, &req, req.nh.nlmsg_len, 0,
               (struct sockaddr*)&dest, sizeof(dest)) < 0) {
        fprintf(stderr, "[netlink] add: sendto() failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    // Read ACK response
    char buf[4096];
    int len = recv(fd, buf, sizeof(buf), 0);
    close(fd);

    if (len < 0) {
        fprintf(stderr, "[netlink] add: recv() failed: %s\n", strerror(errno));
        return -1;
    }

    struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
    if (nlh->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr* err = (struct nlmsgerr*)NLMSG_DATA(nlh);
        if (err->error != 0) {
            fprintf(stderr, "[netlink] RTM_NEWROUTE FAILED: %s (errno=%d)\n",
                    strerror(-err->error), -err->error);
            return -1;
        }
    }

    fprintf(stderr, "[netlink] OK: default route via %s dev %s (ifindex=%u)\n",
            gateway, iface, ifindex);
    return 0;
}

// Dump /proc/net/route to stderr for diagnostics.
inline void netlink_dump_routes(const char* label) {
    FILE* rf = fopen("/proc/net/route", "r");
    if (!rf) {
        fprintf(stderr, "[route-dump:%s] Cannot open /proc/net/route\n", label);
        return;
    }

    fprintf(stderr, "[route-dump:%s] === Routing table ===\n", label);
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), rf)) {
        if (n++ == 0) continue;  // skip header

        char iface[32];
        unsigned long dest, gw, flags, mask;
        if (sscanf(line, "%31s %lx %lx %lx %*d %*d %*d %lx",
                   iface, &dest, &gw, &flags, &mask) >= 4) {
            struct in_addr d, g, m;
            d.s_addr = (uint32_t)dest;
            g.s_addr = (uint32_t)gw;
            m.s_addr = (uint32_t)mask;
            char dbuf[16], gbuf[16], mbuf[16];
            inet_ntop(AF_INET, &d, dbuf, sizeof(dbuf));
            inet_ntop(AF_INET, &g, gbuf, sizeof(gbuf));
            inet_ntop(AF_INET, &m, mbuf, sizeof(mbuf));
            fprintf(stderr, "  %s: dst=%-15s gw=%-15s mask=%-15s flags=0x%lx%s\n",
                    iface, dbuf, gbuf, mbuf, flags,
                    (dest == 0 && gw != 0) ? " <-- DEFAULT" : "");
        }
    }
    if (n <= 1) fprintf(stderr, "  (no routes)\n");
    fprintf(stderr, "[route-dump:%s] === end ===\n", label);
    fclose(rf);
}

#endif // _WIN32
