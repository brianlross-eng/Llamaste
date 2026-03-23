# Bug Report: DHCP Not Working on First Boot (Kernel-Level vs Userspace)

**Date:** 2026-03-23
**Hardware:** GMKtec EVO-X2, but affects any hardware
**Severity:** High — machine gets no IP address on first boot without workaround

---

## Symptom

After installing Llamaste to disk and booting, the machine gets no IP address.
The ethernet link shows UP (cable detected) but no DHCP lease is obtained.
The web UI is unreachable from the network.

In live mode from USB this does NOT happen — DHCP works fine.

---

## Root Cause

**Live mode** uses `dhcpcd` — a userspace DHCP client that Llamaste spawns
itself in `child_main.cpp`. This works reliably.

**Installed mode** — the kernel command line in `grub.cfg` does NOT include
`ip=dhcp`, meaning the kernel makes no attempt to configure the network.
When the binary starts and spawns `dhcpcd`, Windows ICS (or any DHCP server)
may not respond fast enough if it hasn't seen a request from this MAC address
before, causing `dhcpcd` to time out or not get a lease on the first attempt.

The specific failure mode observed:
- Machine boots from installed NVMe
- Ethernet link comes UP
- `dhcpcd` sends DHCP DISCOVER
- Windows ICS DHCP is cold / not yet aware of this MAC
- No OFFER received within timeout
- Machine has no IP, web UI unreachable

---

## Workaround (discovered during testing)

Add a static IP to the GRUB boot parameters by pressing `e` at the GRUB menu
and appending to the `linux` line:

```
ip=192.168.137.50::192.168.137.1:255.255.255.0::eth0:off
```

Format: `ip=<client-ip>::<gateway>:<netmask>::<interface>:off`

This bypasses DHCP entirely by giving the kernel a static IP before the
binary even starts. Works reliably but requires manual intervention each boot.

**Note:** On the EVO-X2 specifically, the kernel ignored this static IP
and obtained a DHCP address anyway (192.168.137.208) — suggesting dhcpcd
ran and succeeded on the second attempt once Windows ICS was warmed up.

---

## Fix Options

### Option 1: Add DHCP retry logic in child_main.cpp (RECOMMENDED)

In `child_main.cpp`, the wired ethernet auto-DHCP section already waits
up to 10s for a lease. Increase the retry count and add a re-spawn:

```cpp
// Current: single dhcpcd spawn, 10s wait
// Fix: if no IP after 10s, kill dhcpcd and respawn (retry once)
if (no_ip_after_10s) {
    kill_dhcpcd(iface);
    sleep(2);
    spawn_dhcpcd(iface);
    // wait another 15s
}
```

### Option 2: Add `ip=dhcp` to grub.cfg kernel line

In `br2-external/board/llamaste/grub.cfg`, add `ip=dhcp` to the kernel line:

```
menuentry "Llamaste Server" {
    linux /bzImage root=${rootdev} rootfstype=squashfs ro quiet \
        console=tty0 console=ttyS0,115200 \
        init=/opt/llamaste/llamaste \
        ip=dhcp \
        llamaste.mode=server llamaste.slot=${active_slot}
}
```

This makes the kernel itself attempt DHCP during boot (before PID 1 starts),
giving two chances to get an address. If kernel DHCP succeeds, dhcpcd finds
the interface already configured and exits cleanly.

**Caveat:** Kernel-level DHCP (`ip=dhcp`) has a short timeout (~15s) and
may still fail if the DHCP server is slow. Use both options together for
maximum reliability.

### Option 3: Configure dhcpcd with longer timeout and more retries

Add a `dhcpcd.conf` to the rootfs overlay with extended timeouts:

```
# /etc/dhcpcd.conf
timeout 30
reboot 10
```

This gives dhcpcd 30 seconds to obtain a lease (default is ~10s) and
retries after 10 seconds if the initial attempt fails.

---

## Recommended Fix (combine all three)

1. Add `ip=dhcp` to `grub.cfg` kernel lines (kernel-level first attempt)
2. Add `dhcpcd.conf` with `timeout 30` to rootfs overlay
3. Add retry logic in `child_main.cpp` wired ethernet section

This creates three layers of DHCP retry, making cold-start network
acquisition reliable regardless of DHCP server responsiveness.

---

## Testing Notes

- **EVO-X2 specific:** The kernel ignored `ip=192.168.137.50::...` static
  assignment and still used DHCP — this may be because dhcpcd (userspace)
  overrides the kernel's static assignment when it successfully gets a lease.
  This is actually desirable behavior.
- **Windows ICS:** Windows Internet Connection Sharing DHCP is slower to
  respond to new MAC addresses than a dedicated router. This is the most
  common scenario where this bug manifests.
- **Standard routers:** May not exhibit this bug at all if the router
  responds to DHCP DISCOVER quickly.
