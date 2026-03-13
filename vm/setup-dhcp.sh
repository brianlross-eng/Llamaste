#!/bin/sh
# Run this INSIDE the Alpine VM after setup-alpine completes and reboots
# Sets up dnsmasq as a DHCP server on the bridged ethernet interface

# Static IP on eth0
cat > /etc/network/interfaces <<'EOF'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet static
    address 10.0.50.1
    netmask 255.255.255.0
EOF

# Bring up interface
ifdown eth0 2>/dev/null
ifup eth0

# Install dnsmasq
apk update
apk add dnsmasq

# Configure dnsmasq as DHCP-only (no DNS forwarding, no upstream)
cat > /etc/dnsmasq.conf <<'EOF'
# DHCP server on isolated ethernet segment
interface=eth0
bind-interfaces

# DHCP range: 10.0.50.100 - 10.0.50.200, 12h leases
dhcp-range=10.0.50.100,10.0.50.200,255.255.255.0,12h

# No default gateway (isolated network, no routing)
# dhcp-option=3  (omitting option 3 = no gateway)

# No DNS servers pushed (isolated)
# dhcp-option=6  (omitting option 6 = no DNS)

# Log DHCP transactions
log-dhcp

# Don't read /etc/resolv.conf or forward DNS
no-resolv
no-poll

# Only DHCP, no DNS listening
port=0
EOF

# Enable and start dnsmasq
rc-update add dnsmasq default
rc-service dnsmasq start

echo ""
echo "=== DHCP Server Ready ==="
echo "Interface: eth0 = 10.0.50.1/24"
echo "DHCP range: 10.0.50.100 - 10.0.50.200"
echo "No gateway, no DNS forwarding (isolated network)"
echo ""
echo "Monitor leases: cat /var/lib/misc/dnsmasq.leases"
echo "Monitor logs:   logread -f | grep dnsmasq"
