#ifndef KYROBOX_H
#define KYROBOX_H

typedef int (*kx_applet_fn)(int argc, char **argv);

typedef struct {
	const char *name;
	kx_applet_fn fn;
} kx_applet_t;

const kx_applet_t *kx_applet_find(const char *name);
int kx_applet_run(const char *name, int argc, char **argv);
const kx_applet_t *kx_applet_list(int *count);

typedef int (*kx_resolver_fn)(const char *name, int argc, char **argv, int *handled);

void kx_install_default_resolver(void);

#endif
