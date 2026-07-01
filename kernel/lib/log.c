#include "lib/log.h"
#include "drivers/serial.h"
#include "lib/printf.h"
#include <stdarg.h>

static void log_putchar(char c, void *ctx) {
    (void) ctx;
    serial_putchar(COM1, c);
}

static int g_klog_level = KLOG_WARN;

void klog_set_level(int level) { g_klog_level = level; }
int klog_get_level(void) { return g_klog_level; }

void klog_printf(int level, const char *fmt, ...) {
    if (level > g_klog_level) return;

    va_list ap;
    va_start(ap, fmt);
    vprintf_cb(log_putchar, NULL, fmt, ap);
    va_end(ap);
}
