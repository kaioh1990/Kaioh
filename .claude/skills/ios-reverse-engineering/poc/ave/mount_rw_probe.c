/*
 * mount_rw_probe.c — Mount flag manipulation probe for data-only privesc
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * This probe demonstrates the mount flag manipulation primitive:
 * Given kernel R/W, find and modify struct mount to clear MNT_RDONLY/MNT_NOSUID.
 *
 * struct mount layout (XNU):
 *   +0x00: TAILQ_ENTRY(mount) mnt_list
 *   +0x10: struct vfsops *mnt_op
 *   +0x18: struct vfstable *mnt_vtable
 *   +0x20: struct mount *mnt_vnodecovered_mount  (varies by version)
 *   +0x70: int32_t mnt_flag          (MNT_RDONLY=0x1, MNT_NOSUID=0x8)
 *   +0x74: int32_t mnt_kern_flag
 *   +0xC0: char mnt_vfsstat.f_fstypename[16]
 *   +0xD0: char mnt_vfsstat.f_mntonname[1024]
 *   +0x4D0: char mnt_vfsstat.f_mntfromname[1024]
 *
 * NOTE: Offsets are APPROXIMATE and MUST be validated on iOS 27.1.
 * The mnt_flag offset varies between XNU versions.
 *
 * Strategy:
 * 1. Read /proc/mounts or use statfs() to enumerate mounts
 * 2. With kernel R/W: scan kernel heap for struct mount signatures
 *    - Match mnt_vfsstat.f_mntonname against known mount points
 * 3. Once found: clear MNT_RDONLY (0x1) and MNT_NOSUID (0x8) bits
 * 4. Remount succeeds → can write to previously read-only filesystem
 *
 * This probe ONLY does the INFORMATION GATHERING phase:
 * - Enumerate mount points and flags
 * - Determine which mounts need flag clearing
 * - Measure struct mount size for heap scanning
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0
 *        -framework CoreFoundation -o MountRWProbe mount_rw_probe.c
 *
 * Bundle ID: com.research.mountrw
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <mach/mach.h>

#define MNT_RDONLY   0x00000001
#define MNT_NOSUID   0x00000008
#define MNT_NODEV    0x00000010
#define MNT_NOEXEC   0x00000004
#define MNT_ROOTFS   0x00004000

static void print_mount_flags(uint32_t flags) {
    if (flags & MNT_RDONLY)  printf("RDONLY ");
    if (flags & MNT_NOSUID)  printf("NOSUID ");
    if (flags & MNT_NODEV)   printf("NODEV ");
    if (flags & MNT_NOEXEC)  printf("NOEXEC ");
    if (flags & MNT_ROOTFS)  printf("ROOTFS ");
    printf("(0x%x)", flags);
}

static void enumerate_mounts(void) {
    struct statfs *mounts;
    int count = getmntinfo(&mounts, MNT_NOWAIT);
    if (count <= 0) {
        printf("ERROR: getmntinfo failed\n");
        return;
    }

    printf("=== Mount Points (%d total) ===\n\n", count);

    for (int i = 0; i < count; i++) {
        printf("Mount %d:\n", i);
        printf("  on:    %s\n", mounts[i].f_mntonname);
        printf("  from:  %s\n", mounts[i].f_mntfromname);
        printf("  type:  %s\n", mounts[i].f_fstypename);
        printf("  flags: ");
        print_mount_flags(mounts[i].f_flags);
        printf("\n");
        printf("  bsize: %u, blocks: %llu, bfree: %llu\n",
               mounts[i].f_bsize, mounts[i].f_blocks, mounts[i].f_bfree);

        int writable = !(mounts[i].f_flags & MNT_RDONLY);
        int has_suid = !(mounts[i].f_flags & MNT_NOSUID);
        printf("  writable: %s, suid: %s\n",
               writable ? "YES" : "NO",
               has_suid ? "YES" : "NO");

        if (strcmp(mounts[i].f_mntonname, "/") == 0) {
            printf("  ** ROOT FILESYSTEM **\n");
            printf("  ** Target: clear RDONLY to write to /\n");
        }
        if (strcmp(mounts[i].f_mntonname, "/private/var") == 0 ||
            strcmp(mounts[i].f_mntonname, "/var") == 0 ||
            strstr(mounts[i].f_mntonname, "/private/var") != NULL) {
            printf("  ** /var PARTITION **\n");
            if (mounts[i].f_flags & MNT_NOSUID) {
                printf("  ** Target: clear NOSUID for setuid binaries\n");
            }
            if (mounts[i].f_flags & MNT_RDONLY) {
                printf("  ** Target: clear RDONLY for write access\n");
            }
        }
        printf("\n");
    }
}

static void analyze_jailbreak_targets(void) {
    struct statfs *mounts;
    int count = getmntinfo(&mounts, MNT_NOWAIT);
    if (count <= 0) return;

    printf("=== Jailbreak Mount Targets ===\n\n");
    printf("For rootless jailbreak (/var/jb layout):\n\n");

    for (int i = 0; i < count; i++) {
        if (strstr(mounts[i].f_mntonname, "/var") != NULL ||
            strcmp(mounts[i].f_mntonname, "/private/var") == 0) {
            printf("Target: %s\n", mounts[i].f_mntonname);
            printf("  Current flags: 0x%x\n", mounts[i].f_flags);

            uint32_t bits_to_clear = 0;
            if (mounts[i].f_flags & MNT_RDONLY) {
                bits_to_clear |= MNT_RDONLY;
                printf("  Need to clear: MNT_RDONLY (0x1)\n");
            }
            if (mounts[i].f_flags & MNT_NOSUID) {
                bits_to_clear |= MNT_NOSUID;
                printf("  Need to clear: MNT_NOSUID (0x8)\n");
            }
            if (bits_to_clear == 0) {
                printf("  Already writable + suid allowed!\n");
            } else {
                printf("  New flags after clear: 0x%x\n",
                       mounts[i].f_flags & ~bits_to_clear);
            }
            printf("\n");
        }
    }

    printf("=== Kernel Heap Scan Strategy ===\n\n");
    printf("struct mount is in regular kalloc (NOT zalloc_ro).\n");
    printf("To find it with kernel R/W:\n");
    printf("  1. Search heap for f_mntonname string (e.g., \"/private/var\")\n");
    printf("  2. String is at offset ~0xD0 in struct mount\n");
    printf("  3. mnt_flag is at offset ~0x70 (VERIFY on iOS 27.1)\n");
    printf("  4. Clear bits: flag &= ~(MNT_RDONLY | MNT_NOSUID)\n");
    printf("  5. Write back modified flag value\n");
    printf("\n");
    printf("IMPORTANT: mnt_flag offset varies by XNU version.\n");
    printf("Must verify on-device or from kernelcache symbols.\n");
}

static void test_var_writability(void) {
    printf("=== Testing /var writability ===\n\n");

    const char *test_paths[] = {
        "/var/tmp/mount_rw_test",
        "/var/mobile/mount_rw_test",
        "/private/var/tmp/mount_rw_test",
        NULL
    };

    for (int i = 0; test_paths[i]; i++) {
        FILE *f = fopen(test_paths[i], "w");
        if (f) {
            fprintf(f, "mount_rw_probe test\n");
            fclose(f);
            printf("  %s: WRITABLE (wrote successfully)\n", test_paths[i]);
            unlink(test_paths[i]);
        } else {
            printf("  %s: NOT WRITABLE (errno %d: %s)\n",
                   test_paths[i], errno, strerror(errno));
        }
    }

    printf("\n");
    printf("Testing /var/jb (rootless jailbreak directory):\n");
    int rc = mkdir("/var/jb", 0755);
    if (rc == 0) {
        printf("  /var/jb: CREATED (unexpected on non-jailbroken device)\n");
        rmdir("/var/jb");
    } else {
        printf("  /var/jb: CANNOT CREATE (errno %d: %s)\n",
               errno, strerror(errno));
        printf("  This is EXPECTED on non-jailbroken device.\n");
        printf("  After mount flag manipulation, this should succeed.\n");
    }
}

int main(int argc, char *argv[]) {
    printf("=== Mount R/W Probe v1.0 ===\n");
    printf("Educational iOS security research\n\n");

    if (argc < 2) {
        printf("Usage: %s <mode>\n", argv[0]);
        printf("  enumerate  — List all mount points and flags\n");
        printf("  targets    — Analyze jailbreak mount targets\n");
        printf("  test_write — Test /var writability (pre/post exploit)\n");
        printf("  all        — Run all analyses\n");
        return 1;
    }

    const char *mode = argv[1];

    if (strcmp(mode, "enumerate") == 0 || strcmp(mode, "all") == 0) {
        enumerate_mounts();
    }
    if (strcmp(mode, "targets") == 0 || strcmp(mode, "all") == 0) {
        analyze_jailbreak_targets();
    }
    if (strcmp(mode, "test_write") == 0 || strcmp(mode, "all") == 0) {
        test_var_writability();
    }

    return 0;
}
