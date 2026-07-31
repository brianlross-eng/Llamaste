#!/bin/bash
# Deploy a new Llamaste ISO to the PXE server VM
# Usage: ./deploy-pxe.sh [path-to-iso]
#
# The PXE server VM (DHCP-Server) must be running.
# SSH key: /tmp/pxe_key (generated during setup)

set -e

ISO="${1:-D:\\Llamaste\\vm\\llamaste.iso}"
SSH_KEY="/tmp/pxe_key"
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i $SSH_KEY -p 2222"
HOST="root@127.0.0.1"

echo "=== Llamaste PXE Deploy ==="
echo "ISO: $ISO"

if [ ! -f "$ISO" ]; then
    echo "ERROR: ISO not found: $ISO"
    exit 1
fi

if [ ! -f "$SSH_KEY" ]; then
    echo "ERROR: SSH key not found. Was the PXE server set up in this session?"
    echo "Regenerate with: ssh-keygen -t ed25519 -f /tmp/pxe_key -N ''"
    exit 1
fi

# Check VM is reachable
echo "Checking PXE server..."
ssh $SSH_OPTS $HOST "echo OK" 2>/dev/null || {
    echo "ERROR: Cannot reach PXE server VM. Is it running?"
    echo "Start with: VBoxManage startvm DHCP-Server --type gui"
    exit 1
}

# Copy the ISO
SIZE=$(ls -lh "$ISO" | awk '{print $5}')
echo "Copying $SIZE ISO to PXE server..."
scp $SSH_OPTS "$ISO" $HOST:/data/http/llamaste.iso

# Verify
echo "Verifying..."
ssh $SSH_OPTS $HOST 'ls -lh /data/http/llamaste.iso; wget -q -O /dev/null --spider http://10.0.50.1/llamaste.iso && echo "HTTP serving OK" || echo "HTTP ERROR"' 2>/dev/null

echo ""
echo "=== Deploy Complete ==="
echo "Test laptop can now PXE boot from the isolated ethernet."
echo "PXE server: 10.0.50.1, DHCP range: 10.0.50.100-200"
