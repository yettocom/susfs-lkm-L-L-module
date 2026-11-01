// SPDX-License-Identifier: GPL-2.0-only
// Independent probes. Existing/unknown implementations are never stacked.
//
// Two questions, two modes:
//   (no arguments)   does something already answer the SUSFS supercalls on this kernel
//   --status-json    what does the ko we just loaded say about itself
//
// The second exists so a controller does not have to parse a human-readable node: /proc/susfs_status
// already reports the layers, the route and the refusals, and JSON is what a script can trust.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define STATUS_PATH "/proc/susfs_status"

// Same meanings the wire probe has always returned.
#define WIRE_PRESENT 0
#define WIRE_ABSENT 10
#define WIRE_UNKNOWN 12

struct text_reply { char text[16]; int err; };

// The node's own names are literals, but a value copied from userspace (owner_nonce) is not, so
// escape rather than emit broken JSON.
static void json_value(const char *s)
{
    putchar('"');
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            putchar('\\');
            putchar(c);
        } else if (c < 0x20) {
            printf("\\u%04x", c);
        } else {
            putchar(c);
        }
    }
    putchar('"');
}

static int status_json(void)
{
    char line[512];
    char version[64] = "", variant[64] = "", implementation[64] = "", route[64] = "";
    char last_denied[128] = "", owner[256] = "";
    long total = -1, armed = -1, denied = -1;
    int first_layer = 1;
    FILE *f;

    // The node is created 0777, but the sus_path layer answers ENOENT to a caller that may not
    // see it, so from a non-root shell "cannot open" says nothing about whether the module is
    // loaded.  Say which one it is instead of reporting a false absence.
    if (geteuid() != 0) {
        puts("{\"resource\":\"susfs_status\",\"present\":false,\"reason\":"
             "\"not root - the node answers ENOENT to non-root callers\"}");
        return WIRE_UNKNOWN;
    }
    f = fopen(STATUS_PATH, "re");
    if (!f) {
        printf("{\"resource\":\"susfs_status\",\"present\":false,\"errno\":");
        json_value(strerror(errno));
        if (errno == ENOENT)
            printf(",\"reason\":\"node absent, or hidden from this caller\"");
        puts("}");
        return errno == ENOENT ? WIRE_ABSENT : WIRE_UNKNOWN;
    }
    printf("{\"resource\":\"susfs_status\",\"present\":true,\"layers\":[");

    // The node prints "layer <name> up|down" rows after the scalars; emitting them as they are
    // read keeps this a single pass and the node's order is stable because it is generated.
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        char *eq;

        if (nl)
            *nl = '\0';
        if (!strncmp(line, "layer ", 6)) {
            char *name = line + 6;
            char *state = strrchr(name, ' ');

            if (!state)
                continue;
            *state++ = '\0';
            if (!first_layer)
                putchar(',');
            first_layer = 0;
            printf("{\"name\":");
            json_value(name);
            printf(",\"up\":%s}", strcmp(state, "up") ? "false" : "true");
            continue;
        }
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq++ = '\0';
        if (!strcmp(line, "susfs_version")) snprintf(version, sizeof(version), "%s", eq);
        else if (!strcmp(line, "susfs_variant")) snprintf(variant, sizeof(variant), "%s", eq);
        else if (!strcmp(line, "susfs_implementation")) snprintf(implementation, sizeof(implementation), "%s", eq);
        else if (!strcmp(line, "route")) snprintf(route, sizeof(route), "%s", eq);
        else if (!strcmp(line, "total")) total = strtol(eq, NULL, 10);
        else if (!strcmp(line, "armed")) armed = strtol(eq, NULL, 10);
        else if (!strcmp(line, "denied")) denied = strtol(eq, NULL, 10);
        else if (!strcmp(line, "last_denied")) snprintf(last_denied, sizeof(last_denied), "%s", eq);
        else if (!strcmp(line, "owner")) snprintf(owner, sizeof(owner), "%s", eq);
    }
    fclose(f);

    printf("],\"susfs_version\":");
    json_value(version);
    printf(",\"susfs_variant\":");
    json_value(variant);
    printf(",\"implementation\":");
    json_value(implementation);
    printf(",\"route\":");
    json_value(route);
    printf(",\"total\":%ld,\"armed\":%ld,\"denied\":%ld,\"last_denied\":", total, armed, denied);
    json_value(last_denied);
    printf(",\"owner\":");
    json_value(owner);
    puts("}");
    return WIRE_PRESENT;
}

static int wire_probe(void)
{
    const unsigned commands[] = {0x555e1, 0x555e3};

    if (geteuid() != 0) return WIRE_UNKNOWN;
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        struct text_reply reply = { .text = {0}, .err = 126 };
        errno = 0;
        long result = syscall(SYS_reboot, 0xdeadbeefU, 0xfafafafaU, commands[i], &reply);

        if (reply.err != 126 || reply.text[0] != '\0') {
            puts("Existing SUSFS control interface detected (not a compatibility guarantee).");
            return WIRE_PRESENT;
        }
        // Reboot(2) must reject this magic with -EINVAL (or -ENOSYS). Returning success means
        // something else consumed the call and left the reply untouched, and any other errno
        // (a seccomp/denial path) hides the truth too: both are "cannot tell", not "absent".
        if (result != -1 || (errno != EINVAL && errno != ENOSYS)) return WIRE_UNKNOWN;
    }
    puts("No responding SUSFS version/variant interface.");
    return WIRE_ABSENT;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--status-json"))
        return status_json();
    return wire_probe();
}
