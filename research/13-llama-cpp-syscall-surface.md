# llama.cpp Syscall Surface & OS Dependency Analysis

## Complete Syscall Inventory

### Memory Management (Essential)
| Syscall | Purpose | Can Remove? |
|---------|---------|-------------|
| `mmap(MAP_SHARED)` | Memory-map model files from disk | No — this IS model loading |
| `mmap(MAP_ANONYMOUS)` | Heap backing, scratch buffers | Could replace with static allocation |
| `munmap()` | Free mapped regions | No (paired with mmap) |
| `mlock()` | Pin model in physical RAM | Optional but important |
| `madvise(MADV_HUGEPAGE)` | Request transparent huge pages | Optional optimization |
| `brk()` | Small heap allocations | Could replace with bump allocator |

### File I/O (Essential)
| Syscall | Purpose | Can Remove? |
|---------|---------|-------------|
| `openat()` | Open model file | No |
| `read()` / `pread64()` | Read file data (fallback) | Yes if mmap works |
| `fstat()` | Get model file size | No (needed for mmap) |
| `close()` | Release file descriptor | No |

### Threading (Essential for CPU Inference)
| Syscall | Purpose | Can Remove? |
|---------|---------|-------------|
| `clone(CLONE_THREAD)` | Create worker threads | No — ggml needs real parallelism |
| `sched_getaffinity()` | Detect available cores | Could hardcode |
| `sched_setaffinity()` | Pin threads to cores | Optional optimization |
| `clock_gettime()` | Performance timing | Could use rdtsc directly |

### Networking — Server Mode (Essential)
| Syscall | Purpose | Can Remove? |
|---------|---------|-------------|
| `socket()` | Create TCP/UDP sockets | No — server needs network |
| `bind()` | Bind to port | No |
| `listen()` | Accept connections | No |
| `accept4()` | New client connection | No |
| `read()` / `write()` | Socket I/O | No |
| `epoll_create/ctl/wait` | I/O multiplexing | Could use poll() instead |
| `sendto()` / `recvfrom()` | UDP multicast discovery | Only for clustering |

### System Info (Partially Essential)
| Syscall | Purpose | Can Remove? |
|---------|---------|-------------|
| `sysconf()` | CPU count, page size | Could hardcode or use CPUID |
| Read `/proc/cpuinfo` | SIMD feature detection | Could use CPUID instruction |
| `uname()` | OS identification | Not needed |
| `getpid()` | Process ID | Minimal, but libc uses it |

### NOT Used by llama.cpp
- `futex()` — Uses spinlock barriers instead (deliberate choice for performance)
- `fork()` / `exec()` — Single process
- `pipe()` / `dup2()` — No IPC needed
- `signal()` / `sigaction()` — Minimal signal handling
- `ioctl()` — Not directly (libc may use for terminal)
- `fcntl()` — Minimal
- Anything related to users, groups, permissions, namespaces

---

## libc Function Dependencies

### Required from libc
```
Memory:     malloc, free, calloc, aligned_alloc, memcpy, memset, memmove
Math:       sqrtf, expf, logf, sinf, cosf, tanhf, powf, fabsf, fmaf
String:     strlen, strcmp, strncpy, snprintf, sscanf
Threading:  pthread_create, pthread_join, pthread_mutex_lock/unlock
File:       open, read, close, fstat, mmap
Network:    socket, bind, listen, accept, send, recv
System:     sysconf, clock_gettime
I/O:        printf, fprintf, fopen, fclose (logging only)
```

### NOT Required
```
Exceptions:     -fno-exceptions (llama.cpp compatible)
RTTI:           -fno-rtti (llama.cpp compatible)
Locale:         Not used (all ASCII/UTF-8)
Wide chars:     Not used
Regex:          Not used
Complex math:   Not used
```

### musl libc Coverage
musl provides all of the above in ~600 KB. Everything llama.cpp needs is covered.

---

## Kernel Feature Map

### Absolute Minimum Kernel Configuration
```
# Hardware (x86-64)
CONFIG_64BIT=y
CONFIG_SMP=y                    # Multi-core support
CONFIG_X86_LOCAL_APIC=y         # Timer interrupts
CONFIG_X86_IO_APIC=y            # Device interrupts

# Memory
CONFIG_MMU=y                    # Page tables (hardware requirement)
CONFIG_TRANSPARENT_HUGEPAGE=y   # Optional but valuable
CONFIG_HUGETLBFS=y              # Optional explicit huge pages

# Process/Threading
CONFIG_MULTIUSER=n              # Don't need users
CONFIG_SYSVIPC=n                # Don't need SysV IPC
CONFIG_POSIX_MQUEUE=n           # Don't need POSIX MQ
CONFIG_FUTEX=n                  # llama.cpp doesn't use futex!

# Filesystem (minimal)
CONFIG_PROC_FS=y                # /proc/cpuinfo for CPU detection
CONFIG_FAT_FS=y                 # Model file storage
CONFIG_VFAT_FS=y                # Long filenames

# Networking
CONFIG_NET=y
CONFIG_INET=y                   # TCP/IP
CONFIG_IP_MULTICAST=y           # Cluster discovery
# Skip: netfilter, iptables, bridge, vlan, ipv6 (unless needed)

# Storage (pick what target hardware needs)
CONFIG_ATA=y                    # SATA
CONFIG_SATA_AHCI=y              # AHCI controller
CONFIG_BLK_DEV_NVME=y           # NVMe SSD
CONFIG_BLK_DEV_SD=y             # SCSI disk (USB drives)
CONFIG_USB_STORAGE=y            # USB mass storage

# NIC (pick what target hardware needs)
CONFIG_E1000E=y                 # Intel NICs
CONFIG_R8169=y                  # Realtek NICs
# Or: CONFIG_VIRTIO_NET=y for VMs

# Skip everything else
CONFIG_MODULES=n                # No loadable modules (everything built-in)
CONFIG_PRINTK=y                 # Basic console output
CONFIG_SERIAL_8250=y            # Serial console
```

**Estimated kernel size with this config: ~3-5 MB compressed**

---

## What Could Be Embedded INTO llama-server

Instead of separate OS services, these could be compiled into the llama-server binary itself:

| Currently OS Service | Embeddable? | How |
|---------------------|-------------|-----|
| CPU feature detection | Yes | CPUID instruction directly (already partially done) |
| HTTP server | Yes | Already built-in (httplib) |
| Web UI | Yes | Already embedded as static assets |
| Model file loading | Yes | Already handles mmap |
| Configuration | Yes | JSON config file reader (already exists) |
| Metrics/monitoring | Yes | /metrics endpoint (already exists) |
| mDNS/discovery | Yes | ~200 lines of UDP multicast code |
| Cluster coordination | Yes | Could embed into llama-server |
| Health monitoring | Yes | /health endpoint (already exists) |
| Watchdog petting | Yes | ~10 lines writing to /dev/watchdog |

### What CANNOT Be Embedded
| Service | Why Not |
|---------|---------|
| Page table management | Hardware/kernel privilege requirement |
| Interrupt handling | Requires ring 0 |
| Thread scheduler | Requires ring 0 |
| NIC driver | Requires ring 0 (or DPDK-style userspace, but complex) |
| Storage driver | Requires ring 0 |
| TCP/IP stack | Could embed (lwIP) but complex |

---

## Minimal Boot Sequence

### Current (Full Linux)
```
BIOS/UEFI → GRUB2 → vmlinuz → initramfs → /sbin/init →
mount filesystems → start services → llama-server
~3-10 seconds
```

### Stripped Linux
```
UEFI → vmlinuz (built-in initramfs) → init=/opt/llama-server →
mount /proc → read /proc/cpuinfo → mmap model → serve
~1-3 seconds
```

### Unikernel (OSv)
```
UEFI/BIOS → OSv kernel → llama-server (ring 0) →
read model → serve
~5 milliseconds
```

### Theoretical Bare Metal
```
UEFI → load model via UEFI Block I/O → ExitBootServices →
custom kernel (MMU + threads + lwIP) → serve
~100 milliseconds (mostly model loading from disk)
```
