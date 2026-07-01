TARGET     := kernel.elf
ISO        := kyronix.iso
DISK_IMG   := disk.img
LIMINE_DIR := limine
BUILD_DIR  := build

ifneq (, $(shell which x86_64-elf-gcc 2>/dev/null))
    CC := x86_64-elf-gcc
    LD := x86_64-elf-ld
else
    CC := gcc
    LD := ld
endif

LWIP_SRC := kernel/net/lwip/src

# === Kernel configuration (Kconfig toolchain) ===
KCONF := scripts/kconfig/conf
NCONF := scripts/kconfig/nconf
CONFIG_H := kernel/config.h

# Bootstrap: ensure kernel/config.h exists for -include (needed during Makefile parsing for parallel builds)
$(shell if ! [ -f $(CONFIG_H) ]; then \
	echo '/* Auto-generated */' > $(CONFIG_H); \
	echo '#define CONFIG_KMEMLEAK 1' >> $(CONFIG_H); \
	echo '#define CONFIG_LOG_LEVEL 1' >> $(CONFIG_H); \
fi)

CFLAGS := \
    -std=c11           \
    -O2                \
    -Wall -Wextra      \
    -Wno-unused-parameter \
    -Wno-error         \
    -ffreestanding     \
    -fno-stack-protector \
    -fno-pic -fno-pie  \
    -m64 -march=x86-64 \
    -mno-80387         \
    -mno-mmx           \
    -mno-sse           \
    -mno-sse2          \
    -mno-red-zone      \
    -mcmodel=kernel    \
    -U_FORTIFY_SOURCE  \
    -fno-omit-frame-pointer \
    -include $(CONFIG_H) \
    -Ikernel           \
    -Ikernel/boot      \
    -Ikernel/net       \
    -I$(LWIP_SRC)/include

LDFLAGS := \
    -T linker.ld       \
    -nostdlib          \
    -static            \
    -z max-page-size=0x1000

SRCS := \
    kernel/kernel.c                    \
    kernel/arch/x86_64/gdt.c          \
    kernel/arch/x86_64/idt.c          \
    kernel/arch/x86_64/pit.c          \
    kernel/mm/pmm.c                   \
    kernel/mm/vmm.c                   \
    kernel/mm/vma.c                   \
    kernel/mm/heap.c                  \
    kernel/mm/shm.c                   \
    kernel/arch/x86_64/syscall_setup.c \
    kernel/syscall/syscall.c          \
    kernel/syscall/epoll.c            \
    kernel/syscall/file.c             \
    kernel/syscall/poll.c             \
    kernel/syscall/socket.c           \
    kernel/syscall/time.c             \
    kernel/exec/elf.c                  \
    kernel/exec/process.c              \
    kernel/proc/proc.c                 \
    kernel/proc/jail.c                 \
    kernel/proc/signal.c               \
    kernel/fs/ext2.c                   \
    kernel/fs/vfs.c                    \
    kernel/fs/devfs.c                  \
    kernel/fs/eventfd.c                \
    kernel/fs/fdctl.c                  \
    kernel/fs/fdpipe.c                 \
    kernel/fs/procfs.c                 \
    kernel/fs/unix_socket.c            \
    kernel/fs/inet_socket.c            \
    kernel/fs/pipe.c                   \
    kernel/fs/cpio.c                   \
    kernel/fs/fat32.c                  \
    kernel/drivers/ahci.c             \
    kernel/drivers/serial.c           \
    kernel/drivers/kbd.c              \
    kernel/drivers/tty.c              \
    kernel/drivers/fb.c               \
    kernel/drivers/pci.c              \
    kernel/drivers/virtio_net.c       \
    kernel/net/net.c                  \
    kernel/net/lwip_glue.c            \
    kernel/net/netif/kyronix_netif.c  \
    $(LWIP_SRC)/core/init.c           \
    $(LWIP_SRC)/core/def.c            \
    $(LWIP_SRC)/core/dns.c            \
    $(LWIP_SRC)/core/inet_chksum.c    \
    $(LWIP_SRC)/core/ip.c             \
    $(LWIP_SRC)/core/mem.c            \
    $(LWIP_SRC)/core/memp.c           \
    $(LWIP_SRC)/core/netif.c          \
    $(LWIP_SRC)/core/pbuf.c           \
    $(LWIP_SRC)/core/raw.c            \
    $(LWIP_SRC)/core/stats.c          \
    $(LWIP_SRC)/core/sys.c            \
    $(LWIP_SRC)/core/tcp.c            \
    $(LWIP_SRC)/core/tcp_in.c         \
    $(LWIP_SRC)/core/tcp_out.c        \
    $(LWIP_SRC)/core/timeouts.c       \
    $(LWIP_SRC)/core/udp.c            \
    $(LWIP_SRC)/core/ipv4/etharp.c    \
    $(LWIP_SRC)/core/ipv4/icmp.c      \
    $(LWIP_SRC)/core/ipv4/ip4.c       \
    $(LWIP_SRC)/core/ipv4/ip4_addr.c  \
    $(LWIP_SRC)/core/ipv4/ip4_frag.c  \
    $(LWIP_SRC)/netif/ethernet.c      \
    kernel/drivers/uio.c              \
    kernel/drivers/fbdev.c            \
    kernel/drivers/input.c            \
    kernel/drivers/ps2mouse.c         \
    kernel/drivers/vt.c               \
    kernel/lib/string.c               \
    kernel/lib/printf.c                \
    kernel/lib/log.c                   \
    kernel/lib/kallsyms.c             \
    kernel/mm/kmemleak.c              \
    kernel/crypto/chacha20.c

ASM_SRCS := \
    kernel/arch/x86_64/idt_stubs.S    \
    kernel/arch/x86_64/syscall_entry.S \
    kernel/proc/sched.S                \
    kernel/drivers/psf_font.S

OBJS := $(SRCS:%.c=$(BUILD_DIR)/%.o) $(ASM_SRCS:%.S=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

# === Kallsyms (post-process) ===
# kallsyms_data.c is checked in as a stub; 'make kallsyms' regenerates it
# from the final kernel.elf via nm(1).
KALLSYMS_SRC := kernel/kallsyms_data.c
KALLSYMS_OBJ := $(BUILD_DIR)/kernel/kallsyms_data.o

SRC_DIR  := .
INITRD   := initrd.cpio

.PHONY: all iso run run-serial run-uefi clean user-build xorg testrunner test-initrd test-iso test-run test-run-log fmt fmt-check disk config.h kallsyms nconfig

all: config.h $(TARGET) $(INITRD) $(DISK_IMG)

# Build kconfig tools from source (one-time, cached by make)
$(KCONF) $(NCONF): $(wildcard scripts/kconfig/*.c scripts/kconfig/*.h scripts/kconfig/*.l scripts/kconfig/*.y scripts/kconfig/Makefile)
	$(MAKE) -s -C $(@D)

# Generate kernel/config.h from kernel/Kconfig via kconfig tools
$(CONFIG_H): kernel/Kconfig $(KCONF)
	@KCONFIG_AUTOHEADER=$@ $(KCONF) --syncconfig kernel/Kconfig < /dev/null 2>/dev/null
	@touch $@

# Root config.h acts as a sentinel
config.h: $(CONFIG_H)
	@touch $@

# Interactive kernel config editor (ncurses)
nconfig: kernel/Kconfig $(NCONF)
	$(NCONF) kernel/Kconfig
	KCONFIG_AUTOHEADER=$(CONFIG_H) $(KCONF) --syncconfig kernel/Kconfig < /dev/null

$(DISK_IMG):
	dd if=/dev/zero of=$@ bs=1M count=128 status=none
	mkfs.ext2 -b 4096 -L kyronix $@
	@echo "  Built: $@"

disk: $(DISK_IMG)

build/libatomic_asneeded.a:
	@mkdir -p $(@D)
	ar rcs $@

user-build: build/libatomic_asneeded.a
	$(MAKE) -C user

xorg:
	@bash scripts/get-xorg.sh

INITRD_DEPS := $(shell find rootfs -not -name '.gitignore' -type f | sort) \
               $(wildcard build/bin/*)

$(INITRD): $(INITRD_DEPS) | user-build
	@cd rootfs && find . -not -name '.gitignore' | sort | cpio -o --format=newc --owner=0:0 --reproducible > ../$@ 2>/dev/null
	@echo "  Built: $@"

# Regenerate kallsyms table from the freshly-linked kernel.elf
kallsyms: $(TARGET)
	@echo "  GEN     $(KALLSYMS_SRC)"
	@nm -n $(TARGET) | grep -E ' [TtWw] ' | \
	  awk 'BEGIN { n = 0; \
	               print "#include \"lib/kallsyms.h\""; \
	               print "#include <stdint.h>"; \
	               print "const sym_entry_t kallsyms_table[] = {"; } \
	       { printf "    { 0x%s, \"%s\" },\n", $$1, $$3; n++ } \
	       END { print "};"; print "const int kallsyms_num = " n ";" }' > $(KALLSYMS_SRC).tmp && \
	mv $(KALLSYMS_SRC).tmp $(KALLSYMS_SRC)
	@echo "  KALLSYMS regenerated ($$(wc -l < $(KALLSYMS_SRC)) lines)"

$(KALLSYMS_OBJ): $(KALLSYMS_SRC)

$(TARGET): config.h $(OBJS) $(KALLSYMS_OBJ)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(KALLSYMS_OBJ)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) -m64 -march=x86-64 -c $< -o $@

-include $(DEPS)

$(LIMINE_DIR)/limine: $(LIMINE_DIR)/limine.c
	cc $(LIMINE_DIR)/limine.c -o $@

iso: $(TARGET) $(INITRD) $(LIMINE_DIR)/limine
	rm -rf iso_root
	mkdir -p iso_root/boot/limine
	mkdir -p iso_root/EFI/BOOT

	cp $(TARGET)              iso_root/boot/kernel.elf
	cp $(INITRD)              iso_root/boot/initrd.cpio
	cp limine.conf            iso_root/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys    iso_root/boot/limine/
	cp $(LIMINE_DIR)/limine-bios-cd.bin iso_root/boot/limine/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin iso_root/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI        iso_root/EFI/BOOT/
	cp $(LIMINE_DIR)/BOOTIA32.EFI       iso_root/EFI/BOOT/

	xorriso -as mkisofs              \
	    -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot                \
	    -boot-load-size 4            \
	    -boot-info-table             \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part               \
	    --efi-boot-image             \
	    --protective-msdos-label     \
	    iso_root -o $(ISO)

	./$(LIMINE_DIR)/limine bios-install $(ISO)
	@echo ""
	@echo "  Built: $(ISO)"

run: iso $(DISK_IMG)
	qemu-system-x86_64              \
	    -M q35                      \
	    -enable-kvm                 \
	    -cpu host                   \
	    -m 2G                       \
	    -cdrom $(ISO)               \
	    -boot d                     \
	    -serial stdio               \
	    -vga qxl                    \
	    -global qxl-vga.vgamem_mb=1024 \
	    -netdev user,id=n0          \
	    -device virtio-net-pci,netdev=n0 

run-serial: iso $(DISK_IMG)
	qemu-system-x86_64              \
	    -M q35                      \
	    -m 2G                       \
	    -cdrom $(ISO)               \
	    -boot d                     \
	    -display none               \
	    -serial stdio               \
	    -drive file=$(DISK_IMG),format=raw,if=none,id=hd0 \
	    -device ahci,id=ahci \
	    -device ide-hd,drive=hd0,bus=ahci.0

OVMF ?= /usr/share/edk2/x64/OVMF.fd

run-uefi: iso $(DISK_IMG)
	qemu-system-x86_64              \
	    -M q35                      \
	    -m 2G                       \
	    -cdrom $(ISO)               \
	    -bios $(OVMF)               \
	    -boot d                     \
	    -serial stdio               \
	    -vga qxl                    \
	    -global qxl-vga.vgamem_mb=1024 \
	    -drive file=$(DISK_IMG),format=raw,if=none,id=hd0 \
	    -device ahci,id=ahci \
	    -device ide-hd,drive=hd0,bus=ahci.0

TEST_ROOTFS := test_rootfs
TEST_INITRD := test-initrd.cpio
TEST_ISO    := kyronix-test.iso

testrunner: build/libatomic_asneeded.a
	$(MAKE) -C user/testrunner

test-initrd: $(TARGET) testrunner build/libatomic_asneeded.a
	$(MAKE) -C user/kyrobox
	$(MAKE) -C user/fetch
	$(MAKE) -C user/shell
	rm -rf $(TEST_ROOTFS) $(TEST_INITRD)
	mkdir -p $(TEST_ROOTFS)/bin $(TEST_ROOTFS)/mnt
	cp build/bin/testrunner $(TEST_ROOTFS)/init
	cp build/bin/ksh        $(TEST_ROOTFS)/bin/
	ln -sf ksh $(TEST_ROOTFS)/bin/sh
	for app in basename cat chgrp chmod chown cksum clear cmp cp cut date dd dirname du echo env false \
	    find grep head hostname kill killall less link ln ls mkdir mktemp mv nc nslookup ping printenv printf ps pwd readlink reboot rm rmdir \
	    sed seq sleep sort sync tail tee test touch tr true tty uname uniq unlink wc wget which whoami yes; do \
	    cp build/bin/$$app $(TEST_ROOTFS)/bin/; \
	done
	cp build/bin/fetch     $(TEST_ROOTFS)/bin/
	cp build/bin/make      $(TEST_ROOTFS)/bin/
	cd $(TEST_ROOTFS) && find . | sort | cpio -o --format=newc --owner=0:0 --reproducible > ../$(TEST_INITRD) 2>/dev/null
	@echo "  Built: $(TEST_INITRD)"

test-iso: $(TARGET) test-initrd $(LIMINE_DIR)/limine
	rm -rf iso_root
	mkdir -p iso_root/boot/limine
	mkdir -p iso_root/EFI/BOOT
	cp $(TARGET)                    iso_root/boot/kernel.elf
	cp $(TEST_INITRD)               iso_root/boot/initrd.cpio
	cp limine.conf                  iso_root/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys    iso_root/boot/limine/
	cp $(LIMINE_DIR)/limine-bios-cd.bin iso_root/boot/limine/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin iso_root/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI        iso_root/EFI/BOOT/
	cp $(LIMINE_DIR)/BOOTIA32.EFI       iso_root/EFI/BOOT/
	xorriso -as mkisofs              \
	    -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot                \
	    -boot-load-size 4            \
	    -boot-info-table             \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part               \
	    --efi-boot-image             \
	    --protective-msdos-label     \
	    iso_root -o $(TEST_ISO)
	./$(LIMINE_DIR)/limine bios-install $(TEST_ISO)
	@echo ""
	@echo "  Built: $(TEST_ISO)"

test-run: test-iso $(DISK_IMG)
	qemu-system-x86_64              \
	    -M q35                      \
	    -m 512M                     \
	    -cdrom $(TEST_ISO)          \
	    -display none               \
	    -serial stdio               \
	    -netdev user,id=n0          \
	    -device virtio-net-pci,netdev=n0 \
	    -drive file=$(DISK_IMG),format=raw,if=none,id=hd0 \
	    -device ahci,id=ahci \
	    -device ide-hd,drive=hd0,bus=ahci.0 \
	    -no-reboot

test-run-log: test-iso $(DISK_IMG)
	@qemu-system-x86_64              \
	    -M q35                      \
	    -m 512M                     \
	    -cdrom $(TEST_ISO)          \
	    -device isa-debug-exit,iobase=0x501 \
	    -display none               \
	    -netdev user,id=n0          \
	    -device virtio-net-pci,netdev=n0 \
	    -drive file=$(DISK_IMG),format=raw,if=none,id=hd0 \
	    -device ahci,id=ahci \
	    -device ide-hd,drive=hd0,bus=ahci.0 \
	    -serial file:test.log       \
	    -no-reboot 2>/dev/null;     \
	grep -E "(TEST|RESULT|ALL|SOME|KMEMLEAK)" test.log 2>/dev/null; \
	if grep -q "ALL TESTS PASSED" test.log 2>/dev/null; then \
	    pass=1; \
	else \
	    pass=0; \
	fi; \
	leaks=$$(grep -oP 'KMEMLEAK: \K[0-9]+' test.log 2>/dev/null || echo 0); \
	if [ "$$leaks" -gt 0 ] && [ "$$leaks" != "0" ]; then \
	    echo ""; \
	    echo "KMEMLEAK: $$leaks leak(s) detected in kernel heap"; \
	    pass=0; \
	fi; \
	if [ "$$pass" = "1" ]; then \
	    echo ""; \
	    echo "PASS"; \
	else \
	    echo ""; \
	    echo "FAIL"; \
	    exit 1; \
	fi

FMT_FILES := $(shell find kernel/ -name '*.[ch]')

fmt: $(FMT_FILES)
	clang-format -i -style=file $?

fmt-check: $(FMT_FILES)
	clang-format --dry-run -Werror -style=file $?

clean:
	rm -f $(TARGET) $(ISO) $(INITRD) $(TEST_ISO) $(TEST_INITRD) $(DISK_IMG)
	rm -f $(CONFIG_H) .config
	rm -rf $(BUILD_DIR) iso_root rootfs/bin $(TEST_ROOTFS)
	$(MAKE) -C user clean
	$(MAKE) -C scripts/kconfig clean 2>/dev/null; true
	$(MAKE) -C $(LIMINE_DIR) clean 2>/dev/null; true
