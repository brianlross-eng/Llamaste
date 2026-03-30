# Bug Report: Llamaste Has No Internet Access

**Date:** 2026-03-30
**Severity:** Critical — blocks model download, WiFi setup, all cloud features
**Hardware:** GMKtec EVO-X2, installed to NVMe, server mode

---

## Symptom

Llamaste gets an IP address (192.168.137.208) but cannot reach the internet.
- network.ping("8.8.8.8") → reachable: false
- network.ping("huggingface.co") → DNS resolution failed
- model.search() → "Failed to connect to Hugging Face API"
- model.download() → fails silently

---

## Root Cause: Three Stacked Bugs

### Bug 1 — grub.cfg missing ip=dhcp (PRIMARY)

init_apply_network_config() in init.cpp assumes the kernel already
configured networking via ip=dhcp on the kernel command line:

  // DHCP mode — kernel ip=dhcp already configured the interface.

But grub.cfg does NOT include ip=dhcp on the linux kernel line.
So the kernel boots with no network configuration at all.

**Fix:** Add ip=dhcp to all linux kernel lines in grub.cfg:

  linux /bzImage root=... ip=dhcp llamaste.mode=server ...

---

### Bug 2 — dhcpcd hooks need /bin/sh but none exists

child_main.cpp calls spawn_dhcpcd() which successfully gets an IP lease.
But dhcpcd writes the default gateway and /etc/resolv.conf via shell
hook scripts. Llamaste has no shell (BR2_SYSTEM_BIN_SH_NONE=y), so
dhcpcd silently fails to:
  - Add the default route (ip route add default via <gateway>)
  - Write /etc/resolv.conf

The code already manually writes /etc/resolv.conf as a workaround
(around line 1766 in child_main.cpp). But it never manually adds
the default route.

**Fix:** In child_main.cpp, after spawn_dhcpcd() gets a lease and
the IP is confirmed, read the gateway from /proc/net/route and add
the default route manually using SIOCADDRT ioctl. The exact code
to do this already exists in init_apply_network_config() in init.cpp
— reuse it.

Here is the logic to read gateway from /proc/net/route:

  FILE* rt = fopen("/proc/net/route", "r");
  char line[256], iface[32];
  unsigned long dest, gateway;
  while (fgets(line, sizeof(line), rt)) {
      if (sscanf(line, "%31s %lx %lx", iface, &dest, &gateway) == 3) {
          if (dest == 0 && gateway != 0) {
              // This is the default route entry — gateway is in host byte order
              struct in_addr gw_addr;
              gw_addr.s_addr = (uint32_t)gateway;
              // Now call SIOCADDRT with this gateway
          }
      }
  }

Then use SIOCADDRT (same as init_apply_network_config) to add the route.

---

### Bug 3 — SSL verification fails when ca-certificates.crt missing

In tools_model_download.cpp, both http_get() and the download function
check for /etc/ssl/certs/ca-certificates.crt before setting CURLOPT_CAINFO:

  if (access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0) {
      curl_easy_setopt(curl, CURLOPT_CAINFO, "...");
  }
  // else: nothing — libcurl uses compiled-in default which may fail

If the CA cert file doesn't exist or can't be read, libcurl falls back
to its compiled-in SSL verification which may fail on Hugging Face's CDN.

**Fix:** Add CURLOPT_SSL_VERIFYPEER=0 fallback in both http_get() and
the download function when ca-certificates.crt is not accessible:

  if (access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0) {
      curl_easy_setopt(curl, CURLOPT_CAINFO,
                       "/etc/ssl/certs/ca-certificates.crt");
  } else {
      // No CA bundle — disable peer verification as fallback
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }

---

## Fix Priority

1. Bug 1 (grub.cfg ip=dhcp) — one line, fixes 90% of the problem
2. Bug 2 (manual SIOCADDRT after dhcpcd) — covers edge cases
3. Bug 3 (SSL fallback) — required for model download to work even
   after network is fixed

All three must be fixed for model download to work end-to-end.

---

## How to Verify Fix

After deploying:
1. Boot from installed NVMe (server mode)
2. Call network.ping("8.8.8.8") → should return reachable: true
3. Call network.ping("huggingface.co") → should return reachable: true
4. Call model.search("qwen") → should return model list
5. Call model.download() on a small model → should complete successfully
