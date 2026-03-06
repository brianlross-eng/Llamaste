# force-replace-vdi.ps1 — Force replace VDI by stopping all VBox processes
# Must be run as Administrator

$vdi = "D:\Llamaste\vm\Llamaste\llamaste-disk.vdi"
$newVdi = "D:\Llamaste\vm\Llamaste\llamaste-disk-new.vdi"
$uuid = "144eeb0b-4df1-4213-ab6e-ac0c3ed35bf0"
$vboxmanage = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"

if (-not (Test-Path $newVdi)) {
    Write-Host "ERROR: $newVdi not found"
    exit 1
}

# Stop VBoxSDS Windows service
Write-Host "Stopping VBoxSDS service..."
Stop-Service VBoxSDS -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

# Kill ALL VBox processes
Write-Host "Killing all VBox processes..."
Get-Process | Where-Object { $_.Name -like "VBox*" } | ForEach-Object {
    Write-Host "  Killing $($_.Name) (PID $($_.Id))..."
    Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 5

# Check what has the file locked (if handle.exe available)
Write-Host "Checking file lock..."
$attempts = 0
$maxAttempts = 5
while ($attempts -lt $maxAttempts) {
    try {
        $stream = [System.IO.File]::Open($vdi, 'Open', 'ReadWrite', 'None')
        $stream.Close()
        Write-Host "VDI is unlocked!"
        break
    } catch {
        $attempts++
        Write-Host "  Still locked (attempt $attempts/$maxAttempts), waiting..."
        Start-Sleep -Seconds 3
    }
}

# Replace VDI
Write-Host "Replacing VDI..."
Copy-Item -Path $newVdi -Destination $vdi -Force -ErrorAction Stop
Remove-Item -Path $newVdi -Force -ErrorAction SilentlyContinue
Write-Host "VDI replaced."

# Restart VBoxSDS
Write-Host "Restarting VBoxSDS..."
Start-Service VBoxSDS -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3

# Fix UUID
Write-Host "Fixing UUID..."
& $vboxmanage internalcommands sethduuid $vdi $uuid

# Start VM
Write-Host "Starting VM..."
& $vboxmanage startvm Llamaste --type headless

Write-Host "Done! Check http://localhost:8080"
