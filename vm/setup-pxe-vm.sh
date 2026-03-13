#!/bin/sh
# Complete PXE server setup for Alpine live ISO
# Run after: mount /dev/sda /data

# 1. Configure repos and install packages
echo "=== Installing packages ==="
echo 'http://dl-cdn.alpinelinux.org/alpine/v3.21/main' > /etc/apk/repositories
echo 'http://dl-cdn.alpinelinux.org/alpine/v3.21/community' >> /etc/apk/repositories
apk update
apk add dnsmasq python3

# 2. Configure network
echo "=== Configuring network ==="
ifconfig eth0 10.0.50.1 netmask 255.255.255.0 up
ifconfig eth1 up
udhcpc -i eth1 -q 2>/dev/null
echo "nameserver 10.0.3.2" > /etc/resolv.conf
echo "nameserver 8.8.8.8" >> /etc/resolv.conf

# 3. Configure dnsmasq (DHCP + TFTP + PXE)
echo "=== Configuring dnsmasq ==="
cat > /etc/dnsmasq.conf << 'EOF'
interface=eth0
bind-interfaces
dhcp-range=10.0.50.100,10.0.50.200,255.255.255.0,12h
dhcp-match=set:ipxe,175
dhcp-boot=tag:ipxe,boot.ipxe
dhcp-boot=undionly.kpxe
enable-tftp
tftp-root=/data/tftp
log-dhcp
EOF

# 4. Write boot.ipxe (iPXE script for kernel download)
echo "=== Writing boot.ipxe ==="
cat > /data/tftp/boot.ipxe << 'EOF'
#!ipxe
echo Booting Llamaste via kernel+initramfs...
kernel http://10.0.50.1/bzImage
boot
EOF

# 5. Start services
echo "=== Starting services ==="
dnsmasq -C /etc/dnsmasq.conf
cd /data/http && python3 -m http.server 80 &
sleep 1

# 6. Verify
echo "=== Verifying ==="
echo -n "dnsmasq: "; pgrep dnsmasq > /dev/null && echo OK || echo FAIL
echo -n "HTTP: "; wget -T 2 -q -O /dev/null http://127.0.0.1/boot.ipxe && echo OK || echo FAIL
echo -n "eth0: "; ip addr show eth0 | grep 'inet '
echo ""
echo "=== PXE Server Ready ==="
echo "  DHCP: 10.0.50.100-200"
echo "  TFTP: /data/tftp/"
echo "  HTTP: http://10.0.50.1/ (port 80)"
echo "  Files: $(ls /data/http/)"
