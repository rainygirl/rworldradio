#include "paths.h"

#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_CANDIDATES 12

static int is_file(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

/* Directory holding the running executable. _NSGetExecutablePath is the only
 * way to get this on Mac OS X and has been there since 10.2; argv[0] is not
 * reliable when the app is launched by the Finder. */
static int executable_dir(char *out, size_t size)
{
    char raw[2048];
    uint32_t raw_size = sizeof(raw);
    char resolved[2048];
    char *slash;

    if (_NSGetExecutablePath(raw, &raw_size) != 0)
        return 0;
    /* The path may contain "." and ".." components and symlinks; the bundle
     * check below compares directory names, so it has to be canonical. */
    if (!realpath(raw, resolved))
        snprintf(resolved, sizeof(resolved), "%s", raw);

    slash = strrchr(resolved, '/');
    if (!slash)
        return 0;
    *slash = '\0';
    snprintf(out, size, "%s", resolved);
    return 1;
}

/* Contents/Resources of the enclosing .app, if the binary is running from one.
 * A bundled binary lives at RWorldRadio.app/Contents/MacOS/rworldradio and the
 * convention is for its data to sit in RWorldRadio.app/Contents/Resources. */
static int bundle_resources(char *out, size_t size)
{
    char dir[2048];
    char *slash;

    if (!executable_dir(dir, sizeof(dir)))
        return 0;
    slash = strrchr(dir, '/');
    if (!slash || strcmp(slash + 1, "MacOS") != 0)
        return 0; /* not running from a bundle */
    *slash = '\0';
    snprintf(out, size, "%s/Resources", dir);
    return 1;
}

/* Builds the candidate list, highest priority first. Returns how many were
 * written. */
static int candidates(const char *filename, char list[][2048], int max)
{
    char dir[2048];
    const char *home;
    int count = 0;
    int level;

    /* A bundle's own Resources wins: if the app was installed, that copy is the
     * one meant to be used even if a checkout happens to be lying around. */
    if (count < max && bundle_resources(dir, sizeof(dir)))
        snprintf(list[count++], 2048, "%s/%s", dir, filename);

    /* Then the checkout layouts: beside the binary and up to three levels above
     * it, which covers build/ and build/RWorldRadio.app/... */
    if (executable_dir(dir, sizeof(dir))) {
        char up[2048];
        snprintf(up, sizeof(up), "%s", dir);
        for (level = 0; level < 4 && count < max; level++) {
            char *slash;
            snprintf(list[count++], 2048, "%s/data/%s", up, filename);
            slash = strrchr(up, '/');
            if (!slash)
                break;
            *slash = '\0';
        }
    }

    home = getenv("HOME");
    if (home && count < max)
        snprintf(list[count++], 2048,
                 "%s/Library/Application Support/RWorldRadio/data/%s", home, filename);
    if (count < max)
        snprintf(list[count++], 2048,
                 "/Library/Application Support/RWorldRadio/data/%s", filename);

    /* Last: relative to the current directory, for running out of a checkout. */
    if (count < max)
        snprintf(list[count++], 2048, "../data/%s", filename);
    if (count < max)
        snprintf(list[count++], 2048, "data/%s", filename);

    return count;
}

int paths_find_data_file(const char *filename, char *out, size_t size)
{
    char list[MAX_CANDIDATES][2048];
    int count = candidates(filename, list, MAX_CANDIDATES);
    int index;

    if (count == 0) {
        snprintf(out, size, "data/%s", filename);
        return 0;
    }

    for (index = 0; index < count; index++) {
        if (is_file(list[index])) {
            snprintf(out, size, "%s", list[index]);
            return 1;
        }
    }
    snprintf(out, size, "%s", list[0]);
    return 0;
}

void paths_describe_search_order(const char *filename, char *out, size_t size)
{
    char list[MAX_CANDIDATES][2048];
    int count = candidates(filename, list, MAX_CANDIDATES);
    int index;
    size_t used = 0;

    for (index = 0; index < count && used + 1 < size; index++) {
        int written = snprintf(out + used, size - used, "%s%s",
                               index ? "\n" : "", list[index]);
        if (written < 0)
            break;
        used += (size_t)written;
    }
    if (count == 0 && size)
        out[0] = '\0';
}
