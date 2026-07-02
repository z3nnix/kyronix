# Kyronix

> Operating system that sucks less.

![Commits per month](https://img.shields.io/github/commit-activity/m/kyronix-project/kyronix)
[![test](https://github.com/kyronix-project/kyronix/actions/workflows/test.yml/badge.svg)](https://github.com/kyronix-project/kyronix/actions/workflows/test.yml)
[![ISC](https://img.shields.io/badge/license-ISC-blue)](#)
[![x86-64](https://img.shields.io/badge/arch-x86__64-lightgrey)](#)

OS that boots into a Unix-like userspace. Runs musl-linked programs
out of the box (e.g tcc, nasm, vi, even a dillo web browser).

<img src="meta/screenshots/preview.jpg" alt="Kyronix preview" width="640">

## Features

### Kernel
- x86-64, 4-level paging, SMEP, NX-bit
- Limine bootloader (BIOS + UEFI)
- Preemptive scheduler (PIT ~1000 Hz)
- ELF64 loader with PIE + musl dynamic linker
- 150+ Linux-compatible syscalls
- Demand paging (anonymous + file-backed mmap, mprotect, mremap, brk)
- Real-time clock, CPUID, RDRAND

### Drivers
- Framebuffer console with PSF fonts
- PS/2 keyboard & mouse
- PCI enumeration (up to 64 devices)
- AHCI (SATA) disk
- virtio-net
- Serial console (COM1)
- evdev input subsystem (`/dev/input/event*`)
- Virtual terminals
- UIO (userspace I/O)

### Filesystems
- VFS with POSIX operations
- CPIO initramfs
- ext2 read/write
- FAT32
- procfs, devfs
- eventfd, pipe, AF_UNIX sockets

### Networking (LwIP)
- ARP, IPv4, ICMP, UDP, TCP
- DHCP client
- AF_INET socket API (sendto/recvfrom, connect, listen, accept)
- ping, wget, nc utilities

### Userspace
- **ksh**: shell with pipes, redirects, history, tilde expansion
- **vi**: text editor
- **Kyrobox**: 65 standalone POSIX command binaries
- **login**: password authentication
- Runs musl-linked programs: tcc, nasm, dillo, etc.

### Advanced
- Full signal handling (rt_sigaction, SIGCHLD, SIGPIPE, sigaltstack)
- clone() with CLONE_VM / CLONE_THREAD / CLONE_SETTLS
- Jails - Kyronix-native containerization (FS/PID/IPC isolation)
- Shared memory (SHM: shmget/shmat/shmdt/shmctl)
- futex (WAIT, WAKE, REQUEUE, CMP_REQUEUE)
- epoll, poll, select
- Comprehensive test suite (9 suites, CI-enforced)

## Build

### Dependencies

` gcc ; musl-tools ; qemu-system ; xorriso ; nasm `

### Quick start

```sh
make clean && make all && make run
```

Or without graphics (serial console):

```sh
make clean && make all && make run-serial
```

### Make targets

| Target | Does |
|--------|------|
| `all` | Build kernel + initrd + disk image |
| `iso` | Bootable ISO |
| `run` / `run-serial` / `run-uefi` | QEMU launch |
| `test-run` / `test-run-log` | Run test suite in QEMU |
| `user-build` | Build userspace programs |
| `fmt` / `fmt-check` | clang-format src |
| `clean` | Remove build artifacts |

## Project structure

| Path | Contents |
|------|----------|
| `kernel/` | Core kernel |
| `user/` | Userspace |
| `rootfs/` | Initramfs |
| `limine/` | Limine Bl |
| `meta/` | Screenshots |

## License

ISC. Do whatever you want.

