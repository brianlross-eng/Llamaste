@echo off
REM Check if VM is running, start if not
REM Try Llamaste first, then Llamaste2 (renamed after VDI rebuild)
set VMNAME=Llamaste
"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" list vms 2>nul | findstr /C:"\"Llamaste\"" >nul
if errorlevel 1 (
    set VMNAME=Llamaste2
)
"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" showvminfo %VMNAME% --machinereadable 2>nul | findstr /C:"VMState=\"running\"" >nul
if errorlevel 1 (
    echo Starting VM %VMNAME%...
    "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" startvm %VMNAME% --type headless
) else (
    echo VM %VMNAME% already running
)
echo VM running on port 8080
:loop
timeout /t 60 /nobreak >nul
goto loop
