ALPINE_REPO := https://dl-cdn.alpinelinux.org/alpine/v3.20
OUT         := ../../build/bin
ROOTFS      := ../../rootfs
TMPDIR      := /tmp/kyronix-extras

APK_VERSIONS := \
	busybox-1.36.1-r31 \
	gzip-1.13-r0 \
	bzip2-1.0.8-r6 \
	xz-5.8.3-r0 \
	coreutils-9.5-r2 \
	patch-2.7.6-r10

APK_FILES := $(addsuffix .apk,$(APK_VERSIONS))

APPLETS   := tar gzip gunzip bunzip2 md5sum sha256sum install
STANDALONE := bzip2 xz patch

.PHONY: all clean install

all: $(OUT)/busybox $(OUT)/unxz $(STANDALONE:%=$(OUT)/%) $(APPLETS:%=$(OUT)/%)

$(OUT)/busybox: $(OUT)/.extras-fetch
	@cp $(TMPDIR)/extracted/bin/busybox $@
	@chmod +x $@
	@for app in $(APPLETS); do ln -sf busybox $(OUT)/$$app; done
	@echo "  extras: busybox + $(words $(APPLETS)) applets"

$(OUT)/bzip2 $(OUT)/xz $(OUT)/patch: $(OUT)/.extras-fetch
	@cp $(TMPDIR)/extracted/usr/bin/$(notdir $@) $@
	@chmod +x $@
	@echo "  extras: $(notdir $@)"

$(OUT)/unxz: $(OUT)/xz
	@ln -sf xz $@
	@echo "  extras: unxz -> xz"

$(APPLETS:%=$(OUT)/%): $(OUT)/busybox

$(OUT)/.extras-fetch: $(APK_FILES:%=$(TMPDIR)/%)
	@mkdir -p $(TMPDIR)/extracted
	@for apk in $(APK_FILES:%=$(TMPDIR)/%); do \
		echo "  extras: extracting $$apk"; \
		tar -xzf $$apk -C $(TMPDIR)/extracted; \
	done
	@touch $@

$(TMPDIR)/%.apk:
	@mkdir -p $(TMPDIR)
	@curl -sL -o $@ $(ALPINE_REPO)/main/x86_64/$*.apk
	@if file $@ 2>/dev/null | grep -q HTML; then rm -f $@; touch $@; fi

install: all
	@mkdir -p $(ROOTFS)/bin
	@cp $(OUT)/busybox $(ROOTFS)/bin/
	@for app in $(APPLETS); do \
		if [ ! -e $(ROOTFS)/bin/$$app ]; then \
			ln -sf busybox $(ROOTFS)/bin/$$app; \
		fi; \
	done
	@for bin in $(STANDALONE); do \
		if [ ! -e $(ROOTFS)/bin/$$bin ]; then \
			cp $(OUT)/$$bin $(ROOTFS)/bin/; \
		fi; \
	done
	@if [ ! -e $(ROOTFS)/bin/unxz ] && [ -e $(ROOTFS)/bin/xz ]; then \
		ln -sf xz $(ROOTFS)/bin/unxz; \
	fi
	@echo "  extras: installed to rootfs/bin/"

clean:
	rm -rf $(TMPDIR)
	rm -f $(OUT)/busybox $(STANDALONE:%=$(OUT)/%) $(OUT)/unxz
	rm -f $(APPLETS:%=$(OUT)/%) $(OUT)/.extras-fetch
