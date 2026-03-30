I know exactly what's happening now. Simple explanation for Claude Code:

The sequence:

Kernel boots with ip=dhcp → gets IP + gateway + route from Windows ICS ✅
init.cpp runs → doesn't touch routing ✅
child_main.cpp runs → sees wired interface with carrier → spawns dhcpcd again ❌
dhcpcd renews the lease → resets the interface → tries to run hook scripts to re-add the route → no shell → route never added ❌

The kernel already did the job. dhcpcd is undoing it.
The fix — one check in child_main.cpp:
In the wired ethernet auto-DHCP section, before calling spawn_dhcpcd(ifname), check if the interface already has an IP. If it does, skip dhcpcd entirely — the kernel already configured it:
cpp// Skip dhcpcd if kernel ip=dhcp already configured this interface
bool already_has_ip = false;
{
    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk >= 0) {
        struct ifreq ifr2 = {};
        strncpy(ifr2.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
        if (ioctl(sk, SIOCGIFADDR, &ifr2) == 0) {
            auto* sa = reinterpret_cast<struct sockaddr_in*>(&ifr2.ifr_addr);
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
            if (strcmp(ip, "0.0.0.0") != 0) {
                already_has_ip = true;
                fprintf(stderr, "[net] %s: already configured by kernel (%s), skipping dhcpcd\n",
                        ifname.c_str(), ip);
            }
        }
        close(sk);
    }
}
if (!already_has_ip) {
    spawn_dhcpcd(ifname);
    // ... rest of dhcpcd wait loop
}
No ioctl route manipulation. No fork/exec. No SIOCADDRT. Just don't spawn dhcpcd when the kernel already did the work.
