#define _GNU_SOURCE

#include "commands.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "http.h"
#include "md5.h"
#include "util.h"

int verbose_mode = 0;

static int line_exists(const char *buf, const char *name) {
    if (!buf || !name) return 0;
    size_t nlen = strlen(name);
    const char *p = buf;
    while (*p) {
        if (strncmp(p, name, nlen) == 0 &&
            (p[nlen] == '\n' || p[nlen] == '\0'))
            return 1;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 0;
}

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
    extract_json_string(json, "maintainer", pkg->maintainer, sizeof(pkg->maintainer));
    extract_json_string(json, "license", pkg->license, sizeof(pkg->license));
    extract_json_string(json, "homepage", pkg->homepage, sizeof(pkg->homepage));

    const char *rev = strstr(json, "\"revision\"");
    if (rev) {
        rev += 10;
        while (*rev && (*rev == ' ' || *rev == ':')) rev++;
        pkg->revision = atoi(rev);
    }
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
    free(txt);
    return match ? 0 : -1;
}

void cmd_health(void) {
    char *endpoint = read_endpoint();
    if (!endpoint) dief("failed to load endpoint");

    log_step("checking", "%s", endpoint);

    char url[1024];
    snprintf(url, sizeof(url), "%s/health", endpoint);

    int code = 0;
    char *body = http_get_body(url, &code);
    free(body);

    if (code != 200) dief("registry not responding (HTTP %d)", code);
    log_done("registry online");
    free(endpoint);
}

void cmd_set(const char *endpoint) {
    set_endpoint(endpoint);
}

void cmd_list(void) {
    const char *home = home_dir();
    char reg_dir[512];
    snprintf(reg_dir, sizeof(reg_dir), "%s/.pkg/installed", home);

    DIR *d = opendir(reg_dir);
    if (!d) {
        log_info("no packages installed");
        return;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char manifest_path[1024];
        snprintf(manifest_path, sizeof(manifest_path), "%s/%s/manifest", reg_dir, ent->d_name);

        FILE *f = fopen(manifest_path, "r");
        if (f) {
            char version[64] = "?";
            char desc[256] = "";
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "version=%63s", version) == 1) continue;
                if (sscanf(line, "description=%255[^\n]", desc) == 1) continue;
            }
            fclose(f);
            fprintf(stdout, "  %s%-20s%s %s%-8s%s %s\n",
                    ANSI_CYAN, ent->d_name, ANSI_RESET,
                    ANSI_GREEN, version, ANSI_RESET,
                    desc);
        } else {
            fprintf(stdout, "  %s%-20s%s\n", ANSI_CYAN, ent->d_name, ANSI_RESET);
        }
        count++;
    }
    closedir(d);

    if (count == 0) {
        log_info("no packages installed");
    } else {
        fprintf(stdout, "\n  %d package(s) installed\n", count);
    }
}

void cmd_remove(const char *name) {
    const char *home = home_dir();
    char reg_dir[512];
    snprintf(reg_dir, sizeof(reg_dir), "%s/.pkg/installed/%s", home, name);

    struct stat st;
    if (stat(reg_dir, &st) != 0) {
        dief("package '%s' is not installed", name);
    }

    log_step("removing", "%s", name);

    int removed = 0;

    char files_path[1024];
    snprintf(files_path, sizeof(files_path), "%s/files", reg_dir);

    FILE *f = fopen(files_path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
            if (len == 0) continue;

            struct stat fst;
            if (stat(line, &fst) == 0) {
                if (remove(line) == 0) {
                    removed++;
                } else {
                    log_warn("could not remove %s", line);
                }
            }
        }
        fclose(f);
    }

    char *rm_argv[] = { "rm", "-rf", reg_dir, NULL };
    run_cmd(rm_argv);

    log_done("removed %s (%d file%s deleted)", name, removed, removed == 1 ? "" : "s");
}

void cmd_get(const char *name) {
    char *endpoint = read_endpoint();
    if (!endpoint) dief("failed to load endpoint");

    char url[1024];
    snprintf(url, sizeof(url), "%s/packages/%s", endpoint, name);

    log_step("resolving", "%s", name);

    int code = 0;
    char *manifest = http_get_body(url, &code);
    if (code == 403) dief("access denied by registry");
    if (code != 200) dief("package '%s' not found", name);

    PackageInfo pkg;
    if (parse_manifest(manifest, &pkg) != 0) {
        free(manifest);
        dief("malformed package manifest");
    }
    free(manifest);

    if (strcmp(pkg.arch, "x86-64") != 0) dief("architecture %s not supported", pkg.arch);

    if (pkg.description[0])
        fprintf(stdout, "  %s%s%s - %s\n", ANSI_BOLD, pkg.name, ANSI_RESET, pkg.description);
    fprintf(stdout, "  version:    %s rev %d\n", pkg.version, pkg.revision);
    fprintf(stdout, "  arch:       %s\n", pkg.arch);
    if (pkg.maintainer[0]) fprintf(stdout, "  maintainer: %s\n", pkg.maintainer);
    if (pkg.license[0])    fprintf(stdout, "  license:    %s\n", pkg.license);
    if (pkg.homepage[0])   fprintf(stdout, "  homepage:   %s\n", pkg.homepage);
    fprintf(stdout, "\n");

    char tmp_template[] = "/tmp/pkg-XXXXXX";
    char *tmp_dir = mkdtemp(tmp_template);
    if (!tmp_dir) dief("failed to create temporary directory");

    char archive_path[512], checksum_path[512], script_path[512], extract_dir[512];
    snprintf(archive_path, sizeof(archive_path), "%s/%s", tmp_dir, "package.gz");
    snprintf(checksum_path, sizeof(checksum_path), "%s/%s", tmp_dir, "package.gz.md5");
    snprintf(script_path, sizeof(script_path), "%s/%s", tmp_dir, "install.sh");
    snprintf(extract_dir, sizeof(extract_dir), "%s/%s", tmp_dir, "extract");

    ensure_dir(extract_dir);

    const char *install_dir = "/usr/bin";

    char base[1024];
    snprintf(base, sizeof(base), "%s/packages/%s", endpoint, name);

    char url_archive[1200], url_checksum[1200], url_script[1200];
    snprintf(url_archive, sizeof(url_archive), "%s/archive", base);
    snprintf(url_checksum, sizeof(url_checksum), "%s/checksum", base);
    snprintf(url_script, sizeof(url_script), "%s/install.sh", base);

    log_step("downloading", "archive");
    if (http_download(url_archive, archive_path, "archive") != 0) dief("archive download failed");

    log_step("downloading", "checksum");
    if (http_download(url_checksum, checksum_path, "checksum") != 0) dief("checksum download failed");

    log_step("downloading", "install script");
    if (http_download(url_script, script_path, "script") != 0) dief("install script download failed");

    log_step("verifying", "checksum");
    if (verify_checksum(archive_path, checksum_path) != 0) dief("checksum mismatch - archive corrupted");
    log_info("integrity verified");

    log_step("extracting", "archive");
    char *tar_argv[] = { "tar", "-xzf", archive_path, NULL };
    if (run_cmd_in(extract_dir, tar_argv) != 0) dief("archive extraction failed");
    log_info("extracted to %s", extract_dir);

    log_step("running", "install script");

    char pre_scan[1024];
    snprintf(pre_scan, sizeof(pre_scan), "%s/.pre", tmp_dir);
    FILE *pf = fopen(pre_scan, "w");
    if (pf) {
        DIR *pd = opendir(install_dir);
        if (pd) {
            struct dirent *ent;
            while ((ent = readdir(pd)) != NULL)
                if (ent->d_name[0] != '.')
                    fprintf(pf, "%s\n", ent->d_name);
            closedir(pd);
        }
        fclose(pf);
    }

    char *sh_argv[] = { "sh", script_path, extract_dir, archive_path, checksum_path, (char *)install_dir, NULL };
    if (run_cmd(sh_argv) != 0) dief("install script failed");

    log_step("activating", "binaries");

    const char *home = home_dir();
    char reg_parent[512];
    snprintf(reg_parent, sizeof(reg_parent), "%s/.pkg", home);
    ensure_dir(reg_parent);
    snprintf(reg_parent, sizeof(reg_parent), "%s/.pkg/installed", home);
    ensure_dir(reg_parent);

    char reg_dir[512];
    snprintf(reg_dir, sizeof(reg_dir), "%s/.pkg/installed/%s", home, name);
    ensure_dir(reg_dir);

    char files_path[1024];
    snprintf(files_path, sizeof(files_path), "%s/files", reg_dir);
    FILE *flist = fopen(files_path, "w");

    char *pre_names = NULL;
    size_t pre_sz = 0;
    pre_names = read_file(pre_scan, &pre_sz);

    DIR *ad = opendir(install_dir);
    if (ad) {
        struct dirent *ent;
        while ((ent = readdir(ad)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (ent->d_type != DT_REG) continue;

            if (line_exists(pre_names, ent->d_name)) continue;

            char bin_path[512];
            snprintf(bin_path, sizeof(bin_path), "%s/%s", install_dir, ent->d_name);

            struct stat bst;
            if (stat(bin_path, &bst) == 0) {
                chmod(bin_path, 0755);
                if (flist) fprintf(flist, "%s\n", bin_path);
            }
        }
        closedir(ad);
    }
    free(pre_names);
    if (flist) fclose(flist);

    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest", reg_dir);
    FILE *mf = fopen(manifest_path, "w");
    if (mf) {
        fprintf(mf, "name=%s\n", pkg.name);
        fprintf(mf, "version=%s\n", pkg.version);
        fprintf(mf, "description=%s\n", pkg.description);
        fprintf(mf, "arch=%s\n", pkg.arch);
        fprintf(mf, "install_dir=%s\n", install_dir);
        fclose(mf);
    }

    log_done("installed %s %s", pkg.name, pkg.version);
    free(endpoint);
}
