#!/bin/sh
# Complete PXE server setup for Alpine live ISO (VirtualBox internal network)
# Run after boot: login as root, then: mkdir -p /data && mount /dev/sda /data && sh /data/start-pxe.sh

set -e

echo "=== Llamaste PXE Server Setup ==="

# 1. Network — CRITICAL: lo needs 127.0.0.1 (Alpine live doesn't set it)
echo "[1/6] Configuring network..."
ifconfig lo 127.0.0.1 netmask 255.0.0.0 up
ifconfig eth0 10.0.50.1 netmask 255.255.255.0 up
# eth1 = NAT for internet access (if present)
ifconfig eth1 up 2>/dev/null && udhcpc -i eth1 -q 2>/dev/null || true
echo "nameserver 8.8.8.8" > /etc/resolv.conf

# 2. Install packages
echo "[2/6] Installing packages..."
echo 'http://dl-cdn.alpinelinux.org/alpine/v3.21/main' > /etc/apk/repositories
echo 'http://dl-cdn.alpinelinux.org/alpine/v3.21/community' >> /etc/apk/repositories
apk update && apk add dnsmasq syslinux python3

# 3. Setup PXELINUX (for BIOS PXE boot — VBox iPXE can't do HTTP or scripts)
echo "[3/6] Setting up PXELINUX..."
cp /usr/share/syslinux/pxelinux.0 /data/tftp/ 2>/dev/null || true
cp /usr/share/syslinux/ldlinux.c32 /data/tftp/ 2>/dev/null || true
mkdir -p /data/tftp/pxelinux.cfg
cat > /data/tftp/pxelinux.cfg/default << 'EOF'
DEFAULT llamaste
LABEL llamaste
  KERNEL bzImage
  APPEND quiet
EOF

# 4. Configure dnsmasq (DHCP + TFTP)
echo "[4/6] Configuring dnsmasq..."
cat > /etc/dnsmasq.conf << 'EOF'
interface=eth0
bind-interfaces
dhcp-range=10.0.50.100,10.0.50.200,255.255.255.0,12h
dhcp-boot=pxelinux.0
enable-tftp
tftp-root=/data/tftp
log-dhcp
EOF

# 5. Write iPXE boot script (for UEFI PXE clients that DO support HTTP+scripts)
echo "[5/6] Writing boot.ipxe..."
cat > /data/tftp/boot.ipxe << 'IPXEOF'
#!ipxe
echo Booting Llamaste via PXE...
kernel http://10.0.50.1/bzImage
boot
IPXEOF

# 6. Start services
echo "[6/6] Starting services..."
killall dnsmasq 2>/dev/null || true
killall python3 2>/dev/null || true

dnsmasq -C /etc/dnsmasq.conf
echo "  dnsmasq started (DHCP + TFTP)"

cd /data/http && python3 -m http.server 80 --bind 0.0.0.0 &
echo "  HTTP server started on port 80"

# Verify
sleep 2
echo ""
echo "=== Verification ==="
echo -n "dnsmasq: "; pgrep dnsmasq > /dev/null && echo "OK" || echo "FAIL"
echo -n "python3: "; pgrep python3 > /dev/null && echo "OK" || echo "FAIL"
echo -n "HTTP:    "; wget -T 2 -q -O /dev/null http://127.0.0.1/bzImage && echo "OK" || echo "FAIL"
echo -n "eth0:    "; ifconfig eth0 | grep 'inet addr'
echo ""
echo "=== PXE Server Ready ==="
echo "  DHCP range: 10.0.50.100-200"
echo "  TFTP root:  /data/tftp/ (pxelinux.0 + bzImage)"
echo "  HTTP root:  /data/http/ (bzImage + rootfs.squashfs)"
echo "  Boot: PXE-Test VM -> PXELINUX -> bzImage -> initramfs -> squashfs -> Llamaste"
echo ""
ls -lh /data/tftp/bzImage /data/http/rootfs.squashfs
