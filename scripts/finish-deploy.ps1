# finish-deploy.ps1 — Replace VDI, fix UUID, start VM
# Run: powershell.exe -ExecutionPolicy Bypass -File D:\Llamaste\scripts\finish-deploy.ps1

$vdi = "D:\Llamaste\vm\Llamaste2\llamaste-disk.vdi"
$newVdi = "D:\Llamaste\vm\Llamaste2\llamaste-disk-new.vdi"
$uuid = "26fe17f1-60c3-4002-9dfc-bf27620e1d3a"
$vboxmanage = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"

# Check if new VDI exists
if (-not (Test-Path $newVdi)) {
    Write-Host "[deploy] ERROR: $newVdi not found. Run deploy-to-vdi.sh first."
    exit 1
}

# Stop VBoxSDS service (this also kills VBoxSVC)
Write-Host "[deploy] Stopping VirtualBox system service..."
net stop VBoxSDS 2>$null
Start-Sleep -Seconds 2

# Kill any remaining VBox processes
Write-Host "[deploy] Killing remaining VBox processes..."
Get-Process -Name "VBox*" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3

# Verify no locks
$locked = $false
try {
    $stream = [System.IO.File]::Open($vdi, 'Open', 'ReadWrite', 'None')
    $stream.Close()
    Write-Host "[deploy] VDI is unlocked."
} catch {
    Write-Host "[deploy] WARNING: VDI still locked, trying anyway..."
    $locked = $true
}

# Replace VDI
Write-Host "[deploy] Replacing VDI..."
try {
    Copy-Item -Path $newVdi -Destination $vdi -Force -ErrorAction Stop
    Remove-Item -Path $newVdi -Force -ErrorAction SilentlyContinue
    Write-Host "[deploy] VDI replaced."
} catch {
    Write-Host "[deploy] ERROR: Failed to replace VDI: $_"
    # Restart VBoxSDS
    net start VBoxSDS 2>$null
    exit 1
}

# Restart VBoxSDS service (needed for VBoxManage)
Write-Host "[deploy] Restarting VirtualBox service..."
net start VBoxSDS 2>$null
Start-Sleep -Seconds 3

# Fix UUID
Write-Host "[deploy] Fixing UUID..."
& $vboxmanage internalcommands sethduuid $vdi $uuid

# Start VM
Write-Host "[deploy] Starting VM..."
& $vboxmanage startvm Llamaste2 --type headless

Write-Host "[deploy] Done! Check http://localhost:8080 in ~5 seconds"
