/* SPDX-License-Identifier: Apache-2.0 */

#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(off_t) >= 8, "qqmem needs 64-bit file offsets");

struct options {
    const char *device;
    uint64_t address;
    uint32_t value;
    unsigned width;
    bool write;
};

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage:\n"
            "  qqmem read ADDRESS --width 8|16|32 [--device PATH]\n"
            "  qqmem write ADDRESS VALUE --width 8|16|32 [--device PATH]\n"
            "\n"
            "Addresses and values: decimal, 0x-prefixed hex, or 0-prefixed octal.\n"
            "Default device: /dev/mem. Addresses must be naturally aligned.\n"
            "Reads print one hex value. Writes do not read before or afterwards.\n"
            "WARNING: even reads can have hardware side effects.\n");
}

static bool parse_number(const char *text, uint64_t *value)
{
    char *end;
    uintmax_t parsed;

    if (text[0] < '0' || text[0] > '9') {
        return false;
    }
    errno = 0;
    parsed = strtoumax(text, &end, 0);
    if (errno == ERANGE || *end != '\0' || parsed > UINT64_MAX) {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static int parse_options(int argc, char **argv, struct options *opts)
{
    static const struct option long_options[] = {
        {"width", required_argument, NULL, 'w'},
        {"device", required_argument, NULL, 'd'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    bool width_seen = false;
    bool device_seen = false;
    uint64_t value;
    unsigned bytes;
    int option;

    if (argc == 2 &&
        (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        usage(stdout);
        return 0;
    }
    if (argc < 2 ||
        (strcmp(argv[1], "read") != 0 && strcmp(argv[1], "write") != 0)) {
        fprintf(stderr, "qqmem: expected 'read' or 'write'\n");
        usage(stderr);
        return 2;
    }
    opts->write = strcmp(argv[1], "write") == 0;
    opts->device = "/dev/mem";

    argc--;
    argv++;
    if (argc == 2 &&
        (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        usage(stdout);
        return 0;
    }

    if (argc < (opts->write ? 3 : 2) ||
        !parse_number(argv[1], &opts->address)) {
        fprintf(stderr, "qqmem: expected a valid unsigned ADDRESS\n");
        return 2;
    }
    if (opts->write) {
        if (!parse_number(argv[2], &value)) {
            fprintf(stderr, "qqmem: expected a valid unsigned VALUE\n");
            return 2;
        }
    } else {
        value = 0;
    }

    optind = opts->write ? 3 : 2;
    opterr = 0;
    while ((option = getopt_long(argc, argv, "+w:d:h", long_options, NULL)) != -1) {
        switch (option) {
        case 'w':
            if (width_seen ||
                (strcmp(optarg, "8") != 0 && strcmp(optarg, "16") != 0 &&
                 strcmp(optarg, "32") != 0)) {
                fprintf(stderr, "qqmem: specify --width once, as 8, 16, or 32\n");
                return 2;
            }
            width_seen = true;
            opts->width = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 'd':
            if (device_seen || optarg[0] == '\0') {
                fprintf(stderr, "qqmem: specify --device once with a nonempty path\n");
                return 2;
            }
            device_seen = true;
            opts->device = optarg;
            break;
        default:
            fprintf(stderr, "qqmem: unknown option or missing option argument\n");
            return 2;
        }
    }
    if (optind != argc || !width_seen) {
        fprintf(stderr, "qqmem: expected --width 8|16|32 and no extra operands\n");
        return 2;
    }

    bytes = opts->width / 8;
    if (opts->address % bytes != 0) {
        fprintf(stderr, "qqmem: ADDRESS must be aligned to %u bytes\n", bytes);
        return 2;
    }
    if (opts->address > (uint64_t)INT64_MAX - (bytes - 1)) {
        fprintf(stderr, "qqmem: ADDRESS exceeds the supported signed 64-bit offset range\n");
        return 2;
    }
    if (value > (UINT32_MAX >> (32 - opts->width))) {
        fprintf(stderr, "qqmem: VALUE does not fit in %u bits\n", opts->width);
        return 2;
    }
    opts->value = (uint32_t)value;
    return -1;
}

static int access_memory(const struct options *opts)
{
    const unsigned bytes = opts->width / 8;
    struct stat statbuf;
    long page_size = sysconf(_SC_PAGESIZE);
    uint64_t base;
    size_t offset, length;
    void *mapping;
    volatile unsigned char *address;
    uint32_t value = 0;
    int fd, status = 0;

    if (page_size <= 0) {
        fprintf(stderr, "qqmem: cannot determine the system page size\n");
        return 1;
    }
    base = opts->address - opts->address % (uint64_t)page_size;
    offset = (size_t)(opts->address - base);
    if (offset > SIZE_MAX - bytes) {
        fprintf(stderr, "qqmem: mapping length is not representable\n");
        return 1;
    }
    length = offset + bytes;

    fd = open(opts->device, (opts->write ? O_RDWR : O_RDONLY) | O_SYNC | O_CLOEXEC);
    if (fd == -1) {
        fprintf(stderr, "qqmem: cannot open %s: %s\n", opts->device, strerror(errno));
        return 1;
    }
    if (fstat(fd, &statbuf) == -1) {
        fprintf(stderr, "qqmem: cannot stat %s: %s\n", opts->device, strerror(errno));
        close(fd);
        return 1;
    }
    if (S_ISREG(statbuf.st_mode) &&
        (uint64_t)statbuf.st_size < opts->address + bytes) {
        fprintf(stderr, "qqmem: access extends beyond the end of %s\n", opts->device);
        close(fd);
        return 1;
    }

    mapping = mmap(NULL, length, opts->write ? PROT_WRITE : PROT_READ,
                   MAP_SHARED, fd, (off_t)base);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "qqmem: cannot map %s at 0x%" PRIX64 ": %s\n",
                opts->device, base, strerror(errno));
        close(fd);
        return 1;
    }
    address = (volatile unsigned char *)mapping + offset;
    if (opts->write) {
        switch (opts->width) {
        case 8:  *(volatile uint8_t *)address = (uint8_t)opts->value; break;
        case 16: *(volatile uint16_t *)address = (uint16_t)opts->value; break;
        case 32: *(volatile uint32_t *)address = opts->value; break;
        }
    } else {
        switch (opts->width) {
        case 8:  value = *(volatile uint8_t *)address; break;
        case 16: value = *(volatile uint16_t *)address; break;
        case 32: value = *(volatile uint32_t *)address; break;
        }
        if (printf("0x%0*" PRIX32 "\n", (int)(opts->width / 4), value) < 0 ||
            fflush(stdout) == EOF) {
            fprintf(stderr, "qqmem: cannot write output: %s\n", strerror(errno));
            status = 1;
        }
    }

    if (munmap(mapping, length) == -1) {
        fprintf(stderr, "qqmem: cannot unmap %s: %s\n", opts->device, strerror(errno));
        status = 1;
    }
    if (close(fd) == -1) {
        fprintf(stderr, "qqmem: cannot close %s: %s\n", opts->device, strerror(errno));
        status = 1;
    }
    return status;
}

int main(int argc, char **argv)
{
    struct options opts = {0};
    int status = parse_options(argc, argv, &opts);

    return status == -1 ? access_memory(&opts) : status;
}
