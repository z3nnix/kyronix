#define _POSIX_C_SOURCE 200809L

#include "commands.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "http.h"
#include "md5.h"
#include "util.h" 

int verbose_mode = 0;

static int extract_json_string(const char *json, const char *key, char *out, size_t n) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return -1;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return -1;
    snprintf(out, n, "%.*s", (int)(e - p), p);
    return 0;
}

static int parse_manifest(const char *json, PackageInfo *pkg) {
    memset(pkg, 0, sizeof(*pkg));
    if (extract_json_string(json, "name", pkg->name, sizeof(pkg->name)) != 0) return -1;
    if (extract_json_string(json, "version", pkg->version, sizeof(pkg->version)) != 0) return -1;
    extract_json_string(json, "description", pkg->description, sizeof(pkg->description));
    if (extract_json_string(json, "arch", pkg->arch, sizeof(pkg->arch)) != 0) return -1;

    const char *os = strstr(json, "\"os\"");
    if (!os) return -1;
    const char *lb = strchr(os, '[');
    const char *rb = strchr(os, ']');
    if (!lb || !rb || rb <= lb) return -1;
    const char *q1 = strchr(lb, '"');
    const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
    if (!q1 || !q2 || q2 > rb) return -1;
    snprintf(pkg->os, sizeof(pkg->os), "%.*s", (int)(q2 - q1 - 1), q1 + 1);
    return 0;
}

static int verify_checksum(const char *archive_path, const char *checksum_path) {
    size_t len = 0;
    char *txt = read_file(checksum_path, &len);
    if (!txt) return -1;
    trim_crlf(txt);

    char expected[33];
    if (txt[0] == '{') {
        if (extract_json_string(txt, "checksum", expected, sizeof(expected)) != 0) {
            free(txt);
            return -1;
        }
    } else {
        snprintf(expected, sizeof(expected), "%s", txt);
    }

    char actual[33];
    if (md5_file_hex(archive_path, actual) != 0) {
        free(txt);
        return -1;
    }

    int match = strcasecmp(expected, actual) == 0;
    log_info("verify", "expected=%s actual=%s result=%s", expected, actual, match ? "match" : "mismatch");

    free(txt);
    return match ? 0 : -1;
}

void cmd_health(void) {
    char *endpoint = read_endpoint();
    if (!endpoint) dief("failed to load endpoint");

    char url[1024];
    snprintf(url, sizeof(url), "%s/health", endpoint);

    int code = 0;
    char *body = http_get_body(url, &code);
    free(body);

    if (code != 200) dief("server not healthy");
    log_ok("server alive");
    free(endpoint);
}

void cmd_set(const char *endpoint) {
    set_endpoint(endpoint);
}

void cmd_get(const char *name) {
    char *endpoint = read_endpoint();
    if (!endpoint) dief("failed to load endpoint");

    char url[1024];
    snprintf(url, sizeof(url), "%s/packages/%s", endpoint, name);

    int code = 0;
    char *manifest = http_get_body(url, &code);
    if (code == 403) dief("server rejected client user-agent");
    if (code != 200) dief("manifest request failed");

    PackageInfo pkg;
    if (parse_manifest(manifest, &pkg) != 0) {
        free(manifest);
        dief("manifest parse failed");
    }
    free(manifest);

    if (strcmp(pkg.arch, "x86-64") != 0) dief("unsupported architecture: %s", pkg.arch);
    if (strcmp(pkg.os, "linux-compatible") != 0) dief("unsupported OS target: %s", pkg.os);

    char tmp_template[] = "/tmp/pkg-XXXXXX";
    char *tmp_dir = mkdtemp(tmp_template);
    if (!tmp_dir) dief("temp dir failed");

    char archive_path[512], checksum_path[512], script_path[512], extract_dir[512], install_dir[512];
    snprintf(archive_path, sizeof(archive_path), "%s/%s", tmp_dir, "package.gz");
    snprintf(checksum_path, sizeof(checksum_path), "%s/%s", tmp_dir, "package.gz.md5");
    snprintf(script_path, sizeof(script_path), "%s/%s", tmp_dir, "install.sh");
    snprintf(extract_dir, sizeof(extract_dir), "%s/%s", tmp_dir, "extract");
    snprintf(install_dir, sizeof(install_dir), "%s/.local/share/pkg/packages/%s", home_dir(), name);

    ensure_dir(extract_dir);
    ensure_dir(install_dir);

    char base[1024];
    snprintf(base, sizeof(base), "%s/packages/%s", endpoint, name);

    char url_archive[1200], url_checksum[1200], url_script[1200];
    snprintf(url_archive, sizeof(url_archive), "%s/archive", base);
    snprintf(url_checksum, sizeof(url_checksum), "%s/checksum", base);
    snprintf(url_script, sizeof(url_script), "%s/install.sh", base);

    if (http_download(url_archive, archive_path) != 0) dief("archive download failed");
    if (http_download(url_checksum, checksum_path) != 0) dief("checksum download failed");
    if (http_download(url_script, script_path) != 0) dief("install.sh download failed");

    if (verify_checksum(archive_path, checksum_path) != 0) dief("checksum mismatch");
    if (verbose_mode) log_info("verify", "md5 ok");

    if (verbose_mode) log_info("extract", "tar -xzf %s -C %s", archive_path, extract_dir);
    char sh_extract[2048];
    snprintf(sh_extract, sizeof(sh_extract), "cd %s && tar -xzf %s", extract_dir, archive_path);
    char *tar_argv[] = { "sh", "-c", sh_extract, NULL };
    if (run_cmd(tar_argv) != 0) dief("archive extraction failed");

    if (verbose_mode) log_info("exec", "sh %s %s %s %s %s", script_path, extract_dir, archive_path, checksum_path, install_dir);
    char *sh_argv[] = { "sh", script_path, extract_dir, archive_path, checksum_path, install_dir, NULL };
    if (run_cmd(sh_argv) != 0) dief("install script failed");

    log_ok("installed %s %s", pkg.name, pkg.version);
    free(endpoint);
}
