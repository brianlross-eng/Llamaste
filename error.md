Root cause: execl("/sbin/ip", ...) silently fails in both init.cpp and child_main.cpp
When execl("/sbin/ip", ...) can't find the binary, it falls through to _exit(0). The parent sees exit code 0 and thinks the route was applied. It wasn't. No error is logged.
Fix — replace every ip route add/del fork with direct SIOCADDRT ioctl. No external binary needed, no path guessing:
cppstatic void apply_default_route(const std::string& gw_str, const std::string& iface) {
    struct in_addr gw = {};
    if (inet_pton(AF_INET, gw_str.c_str(), &gw) != 1) {
        fprintf(stderr, "[net] Bad gateway address: %s\n", gw_str.c_str());
        return;
    }

    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk < 0) return;

    // Delete any existing default route first
    struct rtentry rt = {};
    ((struct sockaddr_in*)&rt.rt_dst)->sin_family    = AF_INET;
    ((struct sockaddr_in*)&rt.rt_dst)->sin_addr.s_addr = 0;
    ((struct sockaddr_in*)&rt.rt_genmask)->sin_family = AF_INET;
    ((struct sockaddr_in*)&rt.rt_genmask)->sin_addr.s_addr = 0;
    rt.rt_flags = RTF_UP | RTF_GATEWAY;
    ioctl(sk, SIOCDELRT, &rt);  // ignore error if no existing route

    // Add new default route
    memset(&rt, 0, sizeof(rt));
    ((struct sockaddr_in*)&rt.rt_gateway)->sin_family    = AF_INET;
    ((struct sockaddr_in*)&rt.rt_gateway)->sin_addr      = gw;
    ((struct sockaddr_in*)&rt.rt_dst)->sin_family        = AF_INET;
    ((struct sockaddr_in*)&rt.rt_dst)->sin_addr.s_addr   = 0;
    ((struct sockaddr_in*)&rt.rt_genmask)->sin_family    = AF_INET;
    ((struct sockaddr_in*)&rt.rt_genmask)->sin_addr.s_addr = 0;
    rt.rt_flags = RTF_UP | RTF_GATEWAY;
    if (!iface.empty())
        strncpy(rt.rt_dev, iface.c_str(), IFNAMSIZ - 1);

    int ret = ioctl(sk, SIOCADDRT, &rt);
    fprintf(stderr, "[net] SIOCADDRT default via %s dev %s: %s\n",
            gw_str.c_str(), iface.c_str(), ret == 0 ? "OK" : strerror(errno));
    close(sk);
}
Call this instead of the fork()/execl("/sbin/ip", ...) pattern everywhere. No shell, no external binary, works in PID 1 context. Also needs #include <net/route.h> which is already included.
For the DHCP path gateway detection, /proc/net/ipconfig likely doesn't exist — use SIOCGIFADDR on the interface after dhcpcd runs to get the IP, then derive the gateway from dhcpcd's lease at /var/lib/dhcpcd/<iface>.info (routers= field).