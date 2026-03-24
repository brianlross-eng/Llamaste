#pragma once
#include <string>
#ifndef _WIN32
#include <unistd.h>   // pid_t
#endif

void init_mount_filesystems();
bool init_mount_data();
void init_mount_esp();    // Mount ESP (partition 2) at /boot/efi for grubenv access
void init_create_data_dirs();
void init_tune_performance();
void init_set_hostname(const std::string& default_name);
std::string init_parse_boot_mode();
void init_bring_up_loopback();
void init_apply_network_config();
#ifndef _WIN32
// Daemon PIDs (for supervisor restart monitoring)
extern pid_t g_udevd_pid;
extern pid_t g_dbus_pid;
extern pid_t g_bluetoothd_pid;
#endif

void init_load_modules();          // Load critical GPU modules + start eudev auto-detection
void init_start_udevd();           // Start udevd daemon for hardware auto-detection
void init_start_dbus();            // Start dbus-daemon (required by BlueZ)
void init_start_bluetoothd();      // Start BlueZ bluetoothd daemon
void init_setup_audio();           // Unmute ALSA mixer controls and set volume
bool do_live_pivot(char** argv);   // squashfs overlay pivot for live ISO boot
