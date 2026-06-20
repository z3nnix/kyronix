CC := musl-gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -fno-pie -L../../build
LDFLAGS ?= -static -no-pie
OUT := ../../build/bin
UTILS := basename cat chgrp chmod chown cksum clear cmp cp cut date dd dirname du echo env false \
	find grep head hostname kill link ln ls mkdir mktemp mv nc nslookup ping printenv printf pwd readlink reboot rm rmdir \
	sed seq sleep sort sync tail tee test touch tr true tty uname uniq unlink wc wget which whoami yes

FNO_LINK_LIBATOMIC := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -fno-link-libatomic -o /dev/null 2>/dev/null && echo '-fno-link-libatomic')
CFLAGS += $(FNO_LINK_LIBATOMIC)

.PHONY: all clean

all: $(UTILS:%=$(OUT)/%)

$(OUT)/%: %.c common.h
	mkdir -p ../../build/bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(UTILS:%=$(OUT)/%)
