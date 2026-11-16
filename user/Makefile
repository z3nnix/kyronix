ROOTFS := ../rootfs
WAYLAND ?= 0
KYROBOX_SPLIT ?= 0
KYROBOX_UTILS := awk basename cat chgrp chmod chown chroot cksum clear cmp cp cut date dd diff dirname du echo env false \
	fdisk file find grep head hostname id insmod kair kill killall less link ln ls lsblk lspci lsmod mkdir mktemp mount mv nc nslookup ntpd patch ping printenv printf ps pwd readlink realpath \
	passwd reboot rm rmdir rmmod sed seq sleep sort stat stress sync tail tee test top touch tr true tty umount uname uniq unlink useradd userdel wc wget which whoami xargs yes tree

include ../mk/output.mk

.PHONY: all clean

all:
	@$(MAKE) --no-print-directory -C init
	@$(MAKE) --no-print-directory -C logctl
	@$(MAKE) --no-print-directory -C servctl
	@$(MAKE) --no-print-directory -C jailctl
	@$(MAKE) --no-print-directory KYROBOX_SPLIT=$(KYROBOX_SPLIT) -C kyrobox
	@$(MAKE) --no-print-directory -C shell
	@$(MAKE) --no-print-directory -C login
	@$(MAKE) --no-print-directory -C vi
	@$(MAKE) --no-print-directory -C pkg/client
	@$(MAKE) --no-print-directory -C installer
	@$(MAKE) --no-print-directory -C libudev
	@mkdir -p $(ROOTFS)/bin $(ROOTFS)/usr/libexec
	@rm -f $(ROOTFS)/bin/logger $(ROOTFS)/bin/sv $(ROOTFS)/bin/appimage-run
	@cp ../build/bin/pkg $(ROOTFS)/bin/
	@cp ../build/bin/installer $(ROOTFS)/bin/
	@cp ../build/bin/mkfs.ext2 $(ROOTFS)/bin/
	@cp ../build/bin/limine-install $(ROOTFS)/usr/libexec/
ifeq ($(KYROBOX_SPLIT),1)
	@cp ../build/bin/kyronix-init $(ROOTFS)/bin/init
	@cp ../build/bin/logctl $(ROOTFS)/bin/
	@cp ../build/bin/servctl $(ROOTFS)/bin/
	@cp ../build/bin/jailctl $(ROOTFS)/bin/
	@cp ../build/bin/login $(ROOTFS)/bin/
	@cp ../build/bin/vi $(ROOTFS)/bin/
	@cp ../build/bin/kxsh $(ROOTFS)/bin/
	@for app in $(KYROBOX_UTILS); do cp ../build/bin/$$app $(ROOTFS)/bin/; done
	@ln -sf kxsh $(ROOTFS)/bin/sh
	@ln -sf test $(ROOTFS)/bin/[
	@ln -sf reboot $(ROOTFS)/bin/poweroff
	@ln -sf reboot $(ROOTFS)/bin/halt
else
	@cp ../build/bin/kyrobox $(ROOTFS)/bin/
	@for app in $(KYROBOX_UTILS) init login logctl servctl jailctl vi kxsh sh '[' poweroff halt; do ln -sf kyrobox $(ROOTFS)/bin/$$app; done
endif
	@if [ -s ../build/bash-stage/bin/bash ]; then \
	    cp ../build/bash-stage/bin/bash $(ROOTFS)/bin/bash; \
	else \
	    echo "  [WARN] no static bash; script AppRuns will not run"; \
	fi
	@chmod 4755 $(ROOTFS)/bin/passwd
	@chmod 0755 $(ROOTFS)/etc
	@chmod 0600 $(ROOTFS)/etc/shadow
	@$(MAKE) --no-print-directory -C libudev install
	@$(MAKE) --no-print-directory -C extras install
ifeq ($(WAYLAND),1)
	@$(MAKE) --no-print-directory -C extras/blobs/wayland install
	@$(MAKE) --no-print-directory -C extras/blobs/gtk
endif

clean:
	@$(MAKE) --no-print-directory -C init clean
	@$(MAKE) --no-print-directory -C logctl clean
	@$(MAKE) --no-print-directory -C servctl clean
	@$(MAKE) --no-print-directory -C jailctl clean
	@$(MAKE) --no-print-directory -C shell clean
	@$(MAKE) --no-print-directory -C login clean
	@$(MAKE) --no-print-directory -C vi clean
	@$(MAKE) --no-print-directory -C kyrobox clean
	@$(MAKE) --no-print-directory -C pkg/client clean
	@$(MAKE) --no-print-directory -C installer clean
	@$(MAKE) --no-print-directory -C extras clean
	@$(MAKE) --no-print-directory -C libudev clean
	@$(MAKE) --no-print-directory -C extras/blobs/wayland clean
	@rm -f $(ROOTFS)/bin/*
	@rm -f $(ROOTFS)/usr/libexec/limine-install
