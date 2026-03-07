#pragma once
#include <string>

void init_mount_filesystems();
bool init_mount_data();
void init_mount_esp();    // Mount ESP (partition 2) at /boot/efi for grubenv access
void init_create_data_dirs();
void init_tune_performance();
void init_set_hostname(const std::string& default_name);
std::string init_parse_boot_mode();
void init_apply_network_config();
bool do_live_pivot(char** argv);   // squashfs overlay pivot for live ISO boot
