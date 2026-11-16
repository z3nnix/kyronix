#ifndef KX_RESOLVER_H
#define KX_RESOLVER_H

typedef int (*kx_resolver_fn)(const char *name, int argc, char **argv, int *handled);

void kx_set_applet_resolver(kx_resolver_fn fn);
kx_resolver_fn kx_get_applet_resolver(void);
int kx_try_applet(const char *name, int argc, char **argv, int *handled);

#endif
