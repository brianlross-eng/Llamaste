@echo off
REM Create an Alpine Linux VM that serves DHCP on the Ethernet adapter
REM Network is isolated - no routing to WiFi

set VBOX="C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
set VMNAME=DHCP-Server
set VMDIR=D:\Llamaste\vm\DHCP-Server
set ISO=D:\Llamaste\vm\alpine-virt.iso
set DISK=%VMDIR%\dhcp-disk.vdi

echo === Creating DHCP Server VM ===

REM Clean up if exists
%VBOX% showvminfo %VMNAME% >nul 2>&1 && (
    echo Removing existing VM...
    %VBOX% controlvm %VMNAME% poweroff >nul 2>&1
    timeout /t 2 >nul
    %VBOX% unregistervm %VMNAME% --delete >nul 2>&1
)

REM Create VM
mkdir "%VMDIR%" 2>nul
%VBOX% createvm --name %VMNAME% --ostype Linux_64 --register --basefolder "D:\Llamaste\vm"
%VBOX% modifyvm %VMNAME% --memory 256 --cpus 1 --vram 16
%VBOX% modifyvm %VMNAME% --boot1 dvd --boot2 disk --boot3 none

REM Storage
%VBOX% createmedium disk --filename "%DISK%" --size 2048 --format VDI
%VBOX% storagectl %VMNAME% --name "SATA" --add sata --portcount 2
%VBOX% storageattach %VMNAME% --storagectl "SATA" --port 0 --device 0 --type hdd --medium "%DISK%"
%VBOX% storageattach %VMNAME% --storagectl "SATA" --port 1 --device 0 --type dvddrive --medium "%ISO%"

REM Network: Bridged to Realtek Ethernet (the physical jack)
%VBOX% modifyvm %VMNAME% --nic1 bridged --bridgeadapter1 "Realtek PCIe 2.5GbE Family Controller"

REM Serial console for headless access
%VBOX% modifyvm %VMNAME% --uart1 0x3F8 4 --uart-mode1 disconnected

echo.
echo === VM Created ===
echo Start with: %VBOX% startvm %VMNAME%
echo Or headless: %VBOX% startvm %VMNAME% --type headless
echo.
echo After Alpine boots, run these commands to set up DHCP:
echo   setup-alpine (accept defaults, use sda for sys disk)
echo   Then see setup-dhcp.sh for dnsmasq config
