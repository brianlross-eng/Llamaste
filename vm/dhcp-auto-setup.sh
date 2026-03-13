#!/bin/sh
# Automated Alpine setup + dnsmasq DHCP server
# Run this from the Alpine live ISO as root

set -e

# --- Install Alpine to disk (non-interactive) ---

# Create answer file
cat > /tmp/answers <<'ANSWERS'
KEYMAPOPTS="us us"
HOSTNAMEOPTS="-n dhcp-server"
INTERFACESOPTS="auto lo
iface lo inet loopback

auto eth0
iface eth0 inet static
    address 10.0.50.1
    netmask 255.255.255.0
"
DNSOPTS="none"
TIMEZONEOPTS="-z UTC"
PROXYOPTS="none"
APKREPOSOPTS="-1"
USEROPTS="-a -g wheel root"
SSHDOPTS="none"
NTPOPTS="-c busybox"
DISKOPTS="-m sys /dev/sda"
LABOROPTS="none"
ANSWERS

# Run setup with answer file, auto-confirm disk erase
yes | setup-alpine -f /tmp/answers -e 2>&1 || true

# Mount installed system to configure it before reboot
mount /dev/sda3 /mnt
mount /dev/sda1 /mnt/boot

# Set root password to empty (auto-login)
sed -i 's|^root:.*|root::0:0:root:/root:/bin/ash|' /mnt/etc/shadow

# Configure network on installed system
cat > /mnt/etc/network/interfaces <<'EOF'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet static
    address 10.0.50.1
    netmask 255.255.255.0
EOF

# Pre-install dnsmasq config
mkdir -p /mnt/etc
cat > /mnt/etc/dnsmasq.conf <<'EOF'
# Isolated DHCP server - no routing, no DNS forwarding
interface=eth0
bind-interfaces
dhcp-range=10.0.50.100,10.0.50.200,255.255.255.0,12h
log-dhcp
no-resolv
no-poll
port=0
EOF

# Create first-boot script to install and enable dnsmasq
cat > /mnt/etc/local.d/setup-dnsmasq.start <<'SCRIPT'
#!/bin/sh
# One-time first-boot setup
if [ ! -f /usr/sbin/dnsmasq ]; then
    # Try to set up apk repos
    setup-apkrepos -1 2>/dev/null || true
    apk update 2>/dev/null
    apk add dnsmasq 2>/dev/null
    if [ -f /usr/sbin/dnsmasq ]; then
        rc-update add dnsmasq default
        rc-service dnsmasq start
        echo "=== DHCP Server configured and running ==="
    else
        echo "WARNING: Could not install dnsmasq (no network to repo?)"
        echo "Run manually: apk add dnsmasq && rc-update add dnsmasq default && rc-service dnsmasq start"
    fi
fi
SCRIPT
chmod +x /mnt/etc/local.d/setup-dnsmasq.start

# Enable local service for first-boot script
chroot /mnt rc-update add local default 2>/dev/null || true

# Unmount
umount /mnt/boot
umount /mnt

echo ""
echo "=== Alpine installed to /dev/sda ==="
echo "=== Will configure dnsmasq on first boot ==="
echo "Rebooting in 3 seconds..."
sleep 3
reboot
