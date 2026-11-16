CC := gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -fno-pie -L../../build
LDFLAGS ?= -static -lm -lcrypt
OUT := ../../build/bin
BUILD := ../../build
KYROBOX_SPLIT ?= 0
UTILS := awk basename cat chgrp chmod chown chroot cksum clear cmp cp cut date dd diff dirname du echo env false \
	fdisk file find grep head hostname id insmod kair kill killall less link ln ls lsblk lspci lsmod mkdir mktemp mount mv nc nslookup ntpd patch ping printenv printf ps pwd readlink realpath \
	passwd reboot rm rmdir rmmod sed seq sleep sort stat stress sync tail tee test top touch tr true tty umount unshare uname uniq unlink useradd userdel wc wget which whoami xargs yes tree
CORE := init login logctl servctl jailctl vi
MONO_ALL := $(UTILS) $(CORE) kxsh sh [ poweroff halt
MONO_GEN := $(BUILD)/kyrobox_applets_gen.c
VI_CFLAGS := -std=c11 -O2 -march=x86-64 -mtune=generic -fno-pie -D_GNU_SOURCE -Wall -Wno-parentheses -Wno-dangling-pointer -Wno-address -Wno-unused-but-set-variable -Wno-stringop-truncation -I.

FNO_LINK_LIBATOMIC := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -fno-link-libatomic -o /dev/null 2>/dev/null && echo '-fno-link-libatomic')
CFLAGS += $(FNO_LINK_LIBATOMIC)

include ../../mk/output.mk

.PHONY: all split monolith clean

ifeq ($(KYROBOX_SPLIT),1)
all: split
else
all: monolith
endif

split: $(UTILS:%=$(OUT)/%)

$(OUT)/%: %.c common.h kyrobox.h
	@$(call step,CC $<)
	@mkdir -p $(OUT)
	@$(CC) $(CFLAGS) -DKX_APPLET=\"$*\" -o $@ $< $(LDFLAGS)

monolith: $(OUT)/kyrobox

$(MONO_GEN): Makefile
	@mkdir -p $(BUILD)
	@printf '%s\n' '#include "kyrobox.h"' > $@
	@for n in $(UTILS); do printf 'int %s_main(int argc, char **argv);\n' "$$n"; done >> $@
	@for n in $(CORE); do printf 'int %s_main(int argc, char **argv);\n' "$$n"; done >> $@
	@printf '%s\n' 'int kxsh_main(int argc, char **argv);' >> $@
	@printf '%s\n' 'const kx_applet_t kx_applets[] = {' >> $@
	@for n in $(UTILS); do printf '\t{"%s", %s_main},\n' "$$n" "$$n"; done >> $@
	@for n in $(CORE); do printf '\t{"%s", %s_main},\n' "$$n" "$$n"; done >> $@
	@printf '%s\n' '	{"kxsh", kxsh_main},' '	{"sh", kxsh_main},' '	{"[", test_main},' '	{"poweroff", reboot_main},' '	{"halt", reboot_main},' '};' >> $@
	@count=`expr $(words $(UTILS)) + $(words $(CORE)) + 5`; printf 'const int kx_applets_count = %s;\n' "$$count" >> $@

$(BUILD)/vi_monolith.o: ../vi/vi.c ../vi/stubs.c ../vi/libbb.h
	@$(call step,CC vi.c)
	@mkdir -p $(BUILD)
	@$(CC) $(VI_CFLAGS) -DKX_MONOLITHIC -c ../vi/vi.c -o $(BUILD)/vi_vi.o
	@$(CC) $(VI_CFLAGS) -DKX_MONOLITHIC -c ../vi/stubs.c -o $(BUILD)/vi_stubs.o
	@$(LD) -r -o $@ $(BUILD)/vi_vi.o $(BUILD)/vi_stubs.o 2>/dev/null || $(CC) -nostdlib -r -o $@ $(BUILD)/vi_vi.o $(BUILD)/vi_stubs.o

$(OUT)/kyrobox: $(MONO_GEN) $(UTILS:%=%.c) kyrobox.c kx_resolver.c ../shell/kxsh.c ../init/init.c ../login/login.c ../logctl/logctl.c ../servctl/servctl.c ../jailctl/jailctl.c $(BUILD)/vi_monolith.o common.h kyrobox.h kx_resolver.h
	@$(call step,CC kyrobox)
	@mkdir -p $(OUT)
	@$(CC) $(CFLAGS) -DKX_MONOLITHIC -I. -I../shell -o $@ kyrobox.c kx_resolver.c $(UTILS:%=%.c) \
		../shell/kxsh.c ../init/init.c ../login/login.c ../logctl/logctl.c ../servctl/servctl.c ../jailctl/jailctl.c \
		$(BUILD)/vi_monolith.o $(MONO_GEN) $(LDFLAGS)

clean:
	rm -f $(UTILS:%=$(OUT)/%) $(OUT)/kyrobox $(MONO_GEN) $(BUILD)/vi_vi.o $(BUILD)/vi_stubs.o $(BUILD)/vi_monolith.o
