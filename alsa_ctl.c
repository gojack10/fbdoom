// alsa_ctl.c — probe and set ALSA mixer controls via /dev/snd/controlC0
// Build: armv7a-linux-androideabi21-clang -Os -std=gnu99 -o alsa_ctl alsa_ctl.c
//
// Lists all controls, then optionally sets a named control value.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sound/asound.h>

static int ctl_fd;

static void list_controls(void) {
    struct snd_ctl_elem_list elist;
    memset(&elist, 0, sizeof(elist));

    // First get count
    elist.space = 0;
    elist.pids = NULL;
    if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &elist) < 0) {
        perror("ELEM_LIST(count)");
        return;
    }
    int count = elist.count;
    printf("Total controls: %d\n", count);
    if (count == 0) return;

    // Allocate id array
    struct snd_ctl_elem_id *ids = calloc(count, sizeof(struct snd_ctl_elem_id));
    if (!ids) { fprintf(stderr, "malloc failed\n"); return; }

    elist.offset = 0;
    elist.space = count;
    elist.pids = ids;
    if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &elist) < 0) {
        perror("ELEM_LIST");
        free(ids);
        return;
    }

    for (int i = 0; i < count; i++) {
        struct snd_ctl_elem_info info;
        memset(&info, 0, sizeof(info));
        info.id = ids[i];

        if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) {
            printf("  [%d] numid=%u name=??? (info failed)\n", i, ids[i].numid);
            continue;
        }

        // Read current value
        struct snd_ctl_elem_value val;
        memset(&val, 0, sizeof(val));
        val.id = info.id;
        if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) < 0) {
            printf("  [%d] numid=%u name=\"%s\" iface=%d type=%d (read failed)\n",
                   i, info.id.numid, info.id.name, info.id.iface, info.type);
            continue;
        }

        const char *type_str = "?";
        switch (info.type) {
            case SNDRV_CTL_ELEM_TYPE_BOOLEAN: type_str = "BOOL"; break;
            case SNDRV_CTL_ELEM_TYPE_INTEGER: type_str = "INT"; break;
            case SNDRV_CTL_ELEM_TYPE_ENUMERATED: type_str = "ENUM"; break;
            case SNDRV_CTL_ELEM_TYPE_BYTES: type_str = "BYTES"; break;
            case SNDRV_CTL_ELEM_TYPE_IEC958: type_str = "IEC958"; break;
            case SNDRV_CTL_ELEM_TYPE_INTEGER64: type_str = "INT64"; break;
        }

        printf("  [%d] numid=%u \"%s\" iface=%d type=%s count=%u",
               i, info.id.numid, info.id.name, info.id.iface, type_str, info.count);

        if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
            printf(" min=%ld max=%ld step=%ld",
                   info.value.integer.min, info.value.integer.max, info.value.integer.step);
            printf(" val=");
            for (unsigned int j = 0; j < info.count && j < 4; j++)
                printf("%ld ", val.value.integer.value[j]);
        } else if (info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN) {
            printf(" val=");
            for (unsigned int j = 0; j < info.count && j < 8; j++)
                printf("%ld ", val.value.integer.value[j]);
        } else if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
            printf(" val=%ld", val.value.integer.value[0]);
            // Read enum item names
            struct snd_ctl_elem_info enum_info;
            memset(&enum_info, 0, sizeof(enum_info));
            enum_info.id = info.id;
            enum_info.id.numid = 0; // need to iterate items
            enum_info.value.enumerated.item = val.value.integer.value[0];
            if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &enum_info) == 0) {
                printf(" (\"%s\")", (const char *)enum_info.value.enumerated.name);
            }
            // Show all available items
            printf(" items=");
            for (int ei = 0; ei < 8; ei++) {
                struct snd_ctl_elem_info ei_info;
                memset(&ei_info, 0, sizeof(ei_info));
                ei_info.id = info.id;
                ei_info.id.numid = 0;
                ei_info.value.enumerated.item = ei;
                if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &ei_info) < 0) break;
                if (ei > 0) printf(",");
                printf("%d:\"%s\"", ei, (const char *)ei_info.value.enumerated.name);
            }
        }
        printf("\n");
    }

    free(ids);
}

static int set_control(const char *name_substr, long new_val, int all_matches) {
    struct snd_ctl_elem_list elist;
    memset(&elist, 0, sizeof(elist));

    elist.space = 0;
    elist.pids = NULL;
    if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &elist) < 0) {
        perror("ELEM_LIST(count)");
        return 1;
    }
    int count = elist.count;
    if (count == 0) { printf("No controls\n"); return 1; }

    struct snd_ctl_elem_id *ids = calloc(count, sizeof(struct snd_ctl_elem_id));
    if (!ids) return 1;

    elist.offset = 0;
    elist.space = count;
    elist.pids = ids;
    if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &elist) < 0) {
        perror("ELEM_LIST");
        free(ids);
        return 1;
    }

    int found = 0;
    for (int i = 0; i < count; i++) {
        // Get info first (for access/type checking)
        struct snd_ctl_elem_info info;
        memset(&info, 0, sizeof(info));
        info.id = ids[i];
        if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) continue;

        if (strstr((const char *)info.id.name, name_substr)) {
            // Check writable
            if (!(info.access & SNDRV_CTL_ELEM_ACCESS_WRITE)) {
                printf("SKIP \"%s\" (read-only)\n", info.id.name);
                continue;
            }

            // Read current value first
            struct snd_ctl_elem_value val;
            memset(&val, 0, sizeof(val));
            val.id = info.id;
            if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) < 0) {
                printf("SKIP \"%s\" (read failed)\n", info.id.name);
                continue;
            }

            printf("SET \"%s\"", info.id.name);
            for (unsigned int j = 0; j < info.count && j < 4; j++) {
                long old = val.value.integer.value[j];
                long clamped = new_val;
                if (info.type == SNDRV_CTL_ELEM_TYPE_INTEGER) {
                    if (clamped < info.value.integer.min) clamped = info.value.integer.min;
                    if (clamped > info.value.integer.max) clamped = info.value.integer.max;
                }
                val.value.integer.value[j] = clamped;
                printf(" [%u]=%ld→%ld", j, old, clamped);
            }

            if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) < 0) {
                printf(" WRITE FAILED: %s\n", strerror(errno));
            } else {
                printf(" OK\n");
                found = 1;
            }

            if (!all_matches) break;
        }
    }

    free(ids);
    if (!found) printf("No control matching \"%s\"\n", name_substr);
    return found ? 0 : 1;
}

int main(int argc, char **argv) {
    ctl_fd = open("/dev/snd/controlC0", O_RDWR);
    if (ctl_fd < 0) {
        perror("open /dev/snd/controlC0");
        return 1;
    }

    if (argc == 1) {
        // List mode
        list_controls();
    } else if (argc >= 3) {
        // Set mode: alsa_ctl <name_substr> <value> [all]
        int all = (argc >= 4 && strcmp(argv[3], "all") == 0);
        long val = strtol(argv[2], NULL, 0);
        return set_control(argv[1], val, all);
    } else {
        fprintf(stderr, "Usage: %s                     (list controls)\n", argv[0]);
        fprintf(stderr, "       %s <name> <value> [all] (set control)\n", argv[0]);
        return 1;
    }

    close(ctl_fd);
    return 0;
}
