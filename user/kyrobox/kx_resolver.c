#include "kx_resolver.h"

static kx_resolver_fn g_kx_resolver;

void kx_set_applet_resolver(kx_resolver_fn fn) {

	g_kx_resolver = fn;
}

kx_resolver_fn kx_get_applet_resolver(void) {

	return g_kx_resolver;
}

int kx_try_applet(const char *name, int argc, char **argv, int *handled) {

	if (handled) {
		*handled = 0;
	}
	if (!g_kx_resolver) {
		return 0;
	}
	return g_kx_resolver(name, argc, argv, handled);
}
