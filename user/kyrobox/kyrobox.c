#include "common.h"
#include "kyrobox.h"
#include "kx_resolver.h"

extern const kx_applet_t kx_applets[];
extern const int kx_applets_count;

const kx_applet_t *kx_applet_find(const char *name) {

	if (!name) {
		return NULL;
	}
	for (int i = 0; i < kx_applets_count; i++) {
		if (strcmp(kx_applets[i].name, name) == 0) {
			return &kx_applets[i];
		}
	}
	return NULL;
}

int kx_applet_run(const char *name, int argc, char **argv) {

	const kx_applet_t *a = kx_applet_find(name);
	if (!a) {
		return -1;
	}
	return a->fn(argc, argv);
}

const kx_applet_t *kx_applet_list(int *count) {

	if (count) {
		*count = kx_applets_count;
	}
	return kx_applets;
}

int kyrobox_main(int argc, char **argv) {

	kx_prog = "kyrobox";
	kx_install_default_resolver();
	const char *base = kx_base(argv[0]);
	if (strcmp(base, "kyrobox") != 0) {
		const kx_applet_t *a = kx_applet_find(base);
		if (a) {
			return a->fn(argc, argv);
		}
	}
	if (argc < 2) {
		int n = 0;
		const kx_applet_t *list = kx_applet_list(&n);
		fprintf(stderr, "usage: kyrobox <applet> [args...]\n");
		for (int i = 0; i < n; i++) {
			fprintf(stderr, " %s", list[i].name);
		}
		fputc('\n', stderr);
		return 1;
	}
	const char *name = argv[1];
	const kx_applet_t *a = kx_applet_find(name);
	if (!a) {
		fprintf(stderr, "kyrobox: %s: applet not found\n", name);
		return 127;
	}
	return a->fn(argc - 1, argv + 1);
}

int main(int argc, char **argv) {

	return kyrobox_main(argc, argv);
}

static int kx_default_resolver(const char *name, int argc, char **argv, int *handled) {

	const kx_applet_t *a = kx_applet_find(name);
	if (!a) {
		if (handled) {
			*handled = 0;
		}
		return 0;
	}
	if (handled) {
		*handled = 1;
	}
	return a->fn(argc, argv);
}

void kx_install_default_resolver(void) {

	kx_set_applet_resolver(kx_default_resolver);
}
