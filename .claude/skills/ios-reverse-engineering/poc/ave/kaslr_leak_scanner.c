/*
 * kaslr_leak_scanner.c — Exhaustive KASLR info leak scanner for iOS 27.1 (A20 Pro)
 *
 * Tries every conceivable userspace technique to discover kernel pointers or
 * the KernelCache KASLR slide. Educational security research on user's own device.
 *
 * Build: clang -o kaslr_leak_scanner kaslr_leak_scanner.c \
 *   -framework IOKit -framework IOSurface -framework CoreFoundation -lbsm
 *
 * Bundle ID: com.research.kaslrscanner
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_port.h>
#include <mach/host_info.h>
#include <mach/task_info.h>
#include <mach/thread_act.h>
#include <mach/thread_info.h>
#include <mach/vm_region.h>
#include <mach/vm_map.h>
#include <mach/processor_set.h>

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

#include <sys/proc_info.h>

/* IOSurface extern declarations (linked via -framework IOSurface) */
typedef struct __IOSurface *IOSurfaceRef;
typedef uint32_t IOSurfaceID;
extern IOSurfaceRef IOSurfaceCreate(CFDictionaryRef properties);
extern IOSurfaceID IOSurfaceGetID(IOSurfaceRef surface);
extern void *IOSurfaceGetBaseAddress(IOSurfaceRef surface);
extern kern_return_t IOSurfaceLock(IOSurfaceRef surface, uint32_t options, uint32_t *seed);
extern kern_return_t IOSurfaceUnlock(IOSurfaceRef surface, uint32_t options, uint32_t *seed);
extern size_t IOSurfaceGetAllocSize(IOSurfaceRef surface);
extern uint32_t IOSurfaceGetSeed(IOSurfaceRef surface);
extern CFDictionaryRef IOSurfaceCopyAllValues(IOSurfaceRef surface);

/* mach_vm types — iOS SDK doesn't expose mach_vm.h */
typedef uint64_t mach_vm_address_t;
typedef uint64_t mach_vm_size_t;

/* proc_pidinfo / proc_pidfdinfo — declared here for iOS compatibility */
extern int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
extern int proc_pidfdinfo(int pid, int fd, int flavor, void *buffer, int buffersize);

/* ========================================================================== */
/* Helpers                                                                     */
/* ========================================================================== */

static int g_leak_count = 0;

static int looks_like_kptr(uint64_t v) {
    /* Kernel pointers on arm64 iOS: 0xFFFFFE00_00000000 .. 0xFFFFFFFF_FFFFFFFF
       With MTE tags bits[59:56] can vary, so pattern is 0xF?FFFE... */
    if (v == 0 || v == (uint64_t)-1) return 0;
    if ((v >> 40) == 0xFFFFFE) return 1;           /* plain kernel ptr */
    if ((v & 0xF0FFFF0000000000ULL) == 0xF0FFFE0000000000ULL) return 1; /* MTE-tagged */
    if (v > 0xFFFFFF0000000000ULL) return 1;        /* very high — suspicious */
    return 0;
}

static void check_val(const char *label, uint64_t v) {
    if (looks_like_kptr(v)) {
        printf("  [LEAK?] %s = 0x%016llx\n", label, (unsigned long long)v);
        g_leak_count++;
    }
}

static void hexdump(const char *label, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    printf("  %s (%zu bytes):\n", label, len);
    for (size_t i = 0; i < len; i += 16) {
        printf("    %04zx: ", i);
        for (size_t j = 0; j < 16 && i + j < len; j++)
            printf("%02x ", p[i + j]);
        printf("\n");
    }
    /* scan for 8-byte aligned kernel pointers in the buffer */
    for (size_t i = 0; i + 8 <= len; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        if (looks_like_kptr(v)) {
            printf("  [LEAK?] %s+0x%zx = 0x%016llx\n", label, i, (unsigned long long)v);
            g_leak_count++;
        }
    }
}

/* ========================================================================== */
/* 1. IOKit Registry Property Enumeration                                      */
/* ========================================================================== */

static void scan_cfvalue(const char *svc_name, const char *key, CFTypeRef val);

static void scan_cfdata(const char *svc_name, const char *key, CFDataRef data) {
    CFIndex len = CFDataGetLength(data);
    const uint8_t *p = CFDataGetBytePtr(data);
    for (CFIndex i = 0; i + 8 <= len; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        if (looks_like_kptr(v)) {
            printf("  [LEAK?] IOKit %s / %s +%lld = 0x%016llx\n",
                   svc_name, key, (long long)i, (unsigned long long)v);
            g_leak_count++;
        }
    }
}

static void scan_cfnumber(const char *svc_name, const char *key, CFNumberRef num) {
    int64_t v = 0;
    if (CFNumberGetValue(num, kCFNumberSInt64Type, &v)) {
        if (looks_like_kptr((uint64_t)v)) {
            printf("  [LEAK?] IOKit %s / %s = 0x%016llx\n",
                   svc_name, key, (unsigned long long)(uint64_t)v);
            g_leak_count++;
        }
    }
}

static void scan_cfarray(const char *svc_name, const char *key, CFArrayRef arr) {
    CFIndex cnt = CFArrayGetCount(arr);
    for (CFIndex i = 0; i < cnt && i < 64; i++) {
        CFTypeRef elem = CFArrayGetValueAtIndex(arr, i);
        char subkey[256];
        snprintf(subkey, sizeof(subkey), "%s[%lld]", key, (long long)i);
        scan_cfvalue(svc_name, subkey, elem);
    }
}

static void scan_cfdict_cb(const void *k, const void *v, void *ctx) {
    const char *svc_name = (const char *)ctx;
    char key[256] = "?";
    if (CFGetTypeID(k) == CFStringGetTypeID())
        CFStringGetCString((CFStringRef)k, key, sizeof(key), kCFStringEncodingUTF8);
    scan_cfvalue(svc_name, key, (CFTypeRef)v);
}

static void scan_cfvalue(const char *svc_name, const char *key, CFTypeRef val) {
    if (!val) return;
    CFTypeID tid = CFGetTypeID(val);
    if (tid == CFDataGetTypeID()) {
        scan_cfdata(svc_name, key, (CFDataRef)val);
    } else if (tid == CFNumberGetTypeID()) {
        scan_cfnumber(svc_name, key, (CFNumberRef)val);
    } else if (tid == CFArrayGetTypeID()) {
        scan_cfarray(svc_name, key, (CFArrayRef)val);
    } else if (tid == CFDictionaryGetTypeID()) {
        CFDictionaryApplyFunction((CFDictionaryRef)val, scan_cfdict_cb, (void *)svc_name);
    }
}

static void scan_iokit_registry(void) {
    printf("\n===== 1. IOKit Registry Property Enumeration =====\n");
    io_iterator_t iter;
    kern_return_t kr = IOServiceGetMatchingServices(
        kIOMainPortDefault,
        IOServiceMatching("IOService"),
        &iter);
    if (kr != KERN_SUCCESS) {
        printf("  IOServiceGetMatchingServices failed: 0x%x\n", kr);
        return;
    }
    int svc_count = 0;
    io_service_t svc;
    while ((svc = IOIteratorNext(iter)) != 0) {
        svc_count++;
        io_name_t name;
        IORegistryEntryGetName(svc, name);

        CFMutableDictionaryRef props = NULL;
        kr = IORegistryEntryCreateCFProperties(svc, &props, kCFAllocatorDefault, 0);
        if (kr == KERN_SUCCESS && props) {
            CFDictionaryApplyFunction(props, scan_cfdict_cb, (void *)name);
            CFRelease(props);
        }
        IOObjectRelease(svc);
        if (svc_count > 2000) break; /* safety limit */
    }
    IOObjectRelease(iter);
    printf("  Scanned %d IOService entries\n", svc_count);
}

/* ========================================================================== */
/* 2. mach_port_kobject_description                                            */
/* ========================================================================== */

static void scan_kobject(void) {
    printf("\n===== 2. mach_port_kobject_description =====\n");

    mach_port_t ports[] = {
        mach_host_self(),
        mach_task_self(),
        MACH_PORT_NULL /* placeholder for IOSurface UC, filled below */
    };
    const char *names[] = { "host_self", "task_self", "iosurface_uc" };

    /* open IOSurface user client */
    io_service_t svc = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("IOSurfaceRoot"));
    io_connect_t uc = 0;
    if (svc) {
        IOServiceOpen(svc, mach_task_self(), 0, &uc);
        IOObjectRelease(svc);
    }
    ports[2] = uc;

    for (int i = 0; i < 3; i++) {
        if (!MACH_PORT_VALID(ports[i])) continue;

        /* mach_port_kobject_description (MIG 3248) */
        unsigned int kotype = 0;
        mach_vm_address_t kaddr = 0;
        char desc[512] = {0};
        mach_msg_type_number_t desc_len = sizeof(desc);
        kern_return_t kr = mach_port_kobject_description(
            mach_task_self(), ports[i], &kotype, desc, &desc_len);
        if (kr == KERN_SUCCESS) {
            printf("  %s: kotype=%u desc=\"%s\"\n", names[i], kotype, desc);
            /* try parsing hex addr from description */
            char *hex = strstr(desc, "0x");
            while (hex) {
                uint64_t v = strtoull(hex, NULL, 16);
                check_val(names[i], v);
                hex = strstr(hex + 1, "0x");
            }
        } else {
            printf("  %s: mach_port_kobject_description failed: %s\n",
                   names[i], mach_error_string(kr));
        }

        /* mach_port_kernel_object (older API) */
        unsigned int kotype2 = 0;
        unsigned int kaddr2 = 0;
        kr = mach_port_kernel_object(mach_task_self(), ports[i], &kotype2, &kaddr2);
        if (kr == KERN_SUCCESS) {
            printf("  %s: kernel_object kotype=%u addr=0x%x\n", names[i], kotype2, kaddr2);
            check_val(names[i], (uint64_t)kaddr2);
        }
    }

    if (uc) IOServiceClose(uc);
}

/* ========================================================================== */
/* 3. mach_port_get_context / mach_port_get_attributes                         */
/* ========================================================================== */

static void scan_port_attrs(void) {
    printf("\n===== 3. mach_port_get_context / get_attributes =====\n");

    mach_port_t ports[] = { mach_host_self(), mach_task_self() };
    const char *names[] = { "host_self", "task_self" };

    /* also open IOSurface UC */
    io_service_t svc = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("IOSurfaceRoot"));
    io_connect_t uc = 0;
    if (svc) { IOServiceOpen(svc, mach_task_self(), 0, &uc); IOObjectRelease(svc); }

    for (int i = 0; i < 2; i++) {
        mach_port_context_t ctx = 0;
        kern_return_t kr = mach_port_get_context(mach_task_self(), ports[i], &ctx);
        if (kr == KERN_SUCCESS) {
            printf("  %s context = 0x%llx\n", names[i], (unsigned long long)ctx);
            check_val(names[i], (uint64_t)ctx);
        }

        /* try various attribute flavors */
        for (int flavor = 1; flavor <= 10; flavor++) {
            mach_port_info_ext_t info;
            mach_msg_type_number_t cnt = sizeof(info) / sizeof(natural_t);
            kr = mach_port_get_attributes(mach_task_self(), ports[i], flavor,
                                          (mach_port_info_t)&info, &cnt);
            if (kr == KERN_SUCCESS && cnt > 0) {
                printf("  %s attr flavor %d: %u words\n", names[i], flavor, cnt);
                uint32_t *words = (uint32_t *)&info;
                for (unsigned j = 0; j + 1 < cnt; j += 2) {
                    uint64_t v = ((uint64_t)words[j + 1] << 32) | words[j];
                    check_val(names[i], v);
                }
            }
        }
    }

    if (MACH_PORT_VALID(uc)) {
        mach_port_context_t ctx = 0;
        kern_return_t kr = mach_port_get_context(mach_task_self(), uc, &ctx);
        if (kr == KERN_SUCCESS) {
            printf("  iosurface_uc context = 0x%llx\n", (unsigned long long)ctx);
            check_val("iosurface_uc_ctx", (uint64_t)ctx);
        }
        IOServiceClose(uc);
    }
}

/* ========================================================================== */
/* 4. proc_pidinfo — all flavors                                               */
/* ========================================================================== */

static void scan_proc_pidinfo(void) {
    printf("\n===== 4. proc_pidinfo (all flavors) =====\n");
    pid_t pid = getpid();
    uint8_t buf[4096];

    for (int flavor = 1; flavor <= 20; flavor++) {
        memset(buf, 0, sizeof(buf));
        int ret = proc_pidinfo(pid, flavor, 0, buf, sizeof(buf));
        if (ret > 0) {
            printf("  flavor %d: %d bytes returned\n", flavor, ret);
            for (int off = 0; off + 8 <= ret; off += 8) {
                uint64_t v;
                memcpy(&v, buf + off, 8);
                if (looks_like_kptr(v)) {
                    char label[64];
                    snprintf(label, sizeof(label), "proc_pidinfo[%d]+%d", flavor, off);
                    printf("  [LEAK?] %s = 0x%016llx\n", label, (unsigned long long)v);
                    g_leak_count++;
                }
            }
        }
    }

    /* Also try PROC_PIDFDVNODEINFO on our file descriptors */
    struct proc_fdinfo fdinfo[256];
    int nfds = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fdinfo, sizeof(fdinfo));
    if (nfds > 0) {
        int count = nfds / sizeof(struct proc_fdinfo);
        printf("  PROC_PIDLISTFDS: %d fds\n", count);
        for (int i = 0; i < count && i < 32; i++) {
            uint8_t vinfo[1024];
            memset(vinfo, 0, sizeof(vinfo));
            int r2 = proc_pidfdinfo(pid, fdinfo[i].proc_fd,
                                     PROC_PIDFDVNODEPATHINFO, vinfo, sizeof(vinfo));
            if (r2 > 0) {
                for (int off = 0; off + 8 <= r2; off += 8) {
                    uint64_t v;
                    memcpy(&v, vinfo + off, 8);
                    if (looks_like_kptr(v)) {
                        char label[64];
                        snprintf(label, sizeof(label), "fd%d_vnodeinfo+%d",
                                 fdinfo[i].proc_fd, off);
                        printf("  [LEAK?] %s = 0x%016llx\n", label, (unsigned long long)v);
                        g_leak_count++;
                    }
                }
            }
        }
    }
}

/* ========================================================================== */
/* 5. task_info — all flavors                                                  */
/* ========================================================================== */

static void scan_task_info(void) {
    printf("\n===== 5. task_info (all flavors) =====\n");
    for (int flavor = 1; flavor <= 30; flavor++) {
        uint64_t buf[128];
        memset(buf, 0, sizeof(buf));
        mach_msg_type_number_t cnt = sizeof(buf) / sizeof(natural_t);
        kern_return_t kr = task_info(mach_task_self(), flavor,
                                     (task_info_t)buf, &cnt);
        if (kr == KERN_SUCCESS && cnt > 0) {
            printf("  flavor %d: %u words\n", flavor, cnt);
            int nbytes = cnt * sizeof(natural_t);
            for (int off = 0; off + 8 <= nbytes; off += 8) {
                uint64_t v;
                memcpy(&v, (uint8_t *)buf + off, 8);
                if (looks_like_kptr(v)) {
                    char label[64];
                    snprintf(label, sizeof(label), "task_info[%d]+%d", flavor, off);
                    printf("  [LEAK?] %s = 0x%016llx\n", label, (unsigned long long)v);
                    g_leak_count++;
                }
            }
        }
    }

    /* TASK_DYLD_INFO specifically — returns all_image_info_addr */
    struct task_dyld_info dinfo;
    mach_msg_type_number_t dcnt = TASK_DYLD_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), TASK_DYLD_INFO,
                                 (task_info_t)&dinfo, &dcnt);
    if (kr == KERN_SUCCESS) {
        printf("  TASK_DYLD_INFO: all_image_info_addr = 0x%llx size = 0x%llx\n",
               (unsigned long long)dinfo.all_image_info_addr,
               (unsigned long long)dinfo.all_image_info_size);
        check_val("dyld_all_image_info_addr", dinfo.all_image_info_addr);
    }
}

/* ========================================================================== */
/* 6. host_info — all flavors                                                  */
/* ========================================================================== */

static void scan_host_info(void) {
    printf("\n===== 6. host_info (all flavors) =====\n");
    mach_port_t host = mach_host_self();
    for (int flavor = 1; flavor <= 20; flavor++) {
        uint64_t buf[128];
        memset(buf, 0, sizeof(buf));
        mach_msg_type_number_t cnt = sizeof(buf) / sizeof(natural_t);
        kern_return_t kr = host_info(host, flavor, (host_info_t)buf, &cnt);
        if (kr == KERN_SUCCESS && cnt > 0) {
            printf("  flavor %d: %u words\n", flavor, cnt);
            int nbytes = cnt * sizeof(natural_t);
            for (int off = 0; off + 8 <= nbytes; off += 8) {
                uint64_t v;
                memcpy(&v, (uint8_t *)buf + off, 8);
                if (looks_like_kptr(v)) {
                    char label[64];
                    snprintf(label, sizeof(label), "host_info[%d]+%d", flavor, off);
                    printf("  [LEAK?] %s = 0x%016llx\n", label, (unsigned long long)v);
                    g_leak_count++;
                }
            }
        }
    }

    /* host_processor_info */
    natural_t nproc = 0;
    processor_info_array_t pinfo = NULL;
    mach_msg_type_number_t pinfo_cnt = 0;
    kern_return_t kr = host_processor_info(host, PROCESSOR_CPU_LOAD_INFO,
                                            &nproc, &pinfo, &pinfo_cnt);
    if (kr == KERN_SUCCESS && pinfo) {
        printf("  host_processor_info: %u CPUs, %u words\n", nproc, pinfo_cnt);
        for (unsigned i = 0; i + 1 < pinfo_cnt; i += 2) {
            uint64_t v = ((uint64_t)pinfo[i + 1] << 32) | pinfo[i];
            check_val("processor_info", v);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)pinfo,
                      pinfo_cnt * sizeof(natural_t));
    }
}

/* ========================================================================== */
/* 7. thread_info — all flavors                                                */
/* ========================================================================== */

static void scan_thread_info(void) {
    printf("\n===== 7. thread_info (all flavors) =====\n");
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t tcnt = 0;
    kern_return_t kr = task_threads(mach_task_self(), &threads, &tcnt);
    if (kr != KERN_SUCCESS) {
        printf("  task_threads failed: %s\n", mach_error_string(kr));
        return;
    }
    printf("  %u threads\n", tcnt);

    for (unsigned t = 0; t < tcnt && t < 8; t++) {
        for (int flavor = 1; flavor <= 15; flavor++) {
            uint64_t buf[128];
            memset(buf, 0, sizeof(buf));
            mach_msg_type_number_t cnt = sizeof(buf) / sizeof(natural_t);
            kr = thread_info(threads[t], flavor, (thread_info_t)buf, &cnt);
            if (kr == KERN_SUCCESS && cnt > 0) {
                int nbytes = cnt * sizeof(natural_t);
                for (int off = 0; off + 8 <= nbytes; off += 8) {
                    uint64_t v;
                    memcpy(&v, (uint8_t *)buf + off, 8);
                    if (looks_like_kptr(v)) {
                        char label[64];
                        snprintf(label, sizeof(label), "thread[%u]_info[%d]+%d",
                                 t, flavor, off);
                        printf("  [LEAK?] %s = 0x%016llx\n", label, (unsigned long long)v);
                        g_leak_count++;
                    }
                }
            }
        }
    }

    /* thread_get_state — ARM64 register dump */
    for (unsigned t = 0; t < tcnt && t < 4; t++) {
#ifdef __arm64__
        arm_thread_state64_t state;
        mach_msg_type_number_t scnt = ARM_THREAD_STATE64_COUNT;
        kr = thread_get_state(threads[t], ARM_THREAD_STATE64,
                              (thread_state_t)&state, &scnt);
        if (kr == KERN_SUCCESS) {
            printf("  thread[%u] ARM64 state: pc=0x%llx lr=0x%llx sp=0x%llx fp=0x%llx\n",
                   t,
                   (unsigned long long)__darwin_arm_thread_state64_get_pc(state),
                   (unsigned long long)__darwin_arm_thread_state64_get_lr(state),
                   (unsigned long long)__darwin_arm_thread_state64_get_sp(state),
                   (unsigned long long)__darwin_arm_thread_state64_get_fp(state));
            /* scan all x registers */
            for (int r = 0; r < 29; r++) {
                check_val("thread_xN", state.__x[r]);
            }
        }
#endif
    }

    for (unsigned t = 0; t < tcnt; t++)
        mach_port_deallocate(mach_task_self(), threads[t]);
    vm_deallocate(mach_task_self(), (vm_address_t)threads,
                  tcnt * sizeof(thread_act_t));
}

/* ========================================================================== */
/* 8. mach_zone_info                                                           */
/* ========================================================================== */

static void scan_zone_info(void) {
    printf("\n===== 8. mach_zone_info =====\n");

    /* mach_zone_info requires host_priv, but try anyway */
    mach_zone_name_array_t znames = NULL;
    mach_msg_type_number_t zname_cnt = 0;
    mach_zone_info_array_t zinfo = NULL;
    mach_msg_type_number_t zinfo_cnt = 0;
    kern_return_t kr = mach_zone_info(mach_host_self(),
                                       &znames, &zname_cnt,
                                       &zinfo, &zinfo_cnt);
    if (kr == KERN_SUCCESS) {
        printf("  mach_zone_info: %u zones!\n", zname_cnt);
        for (unsigned i = 0; i < zname_cnt && i < 20; i++) {
            printf("    zone '%s': cur_size=%llu max_size=%llu elem_size=%llu\n",
                   znames[i].mzn_name,
                   (unsigned long long)zinfo[i].mzi_cur_size,
                   (unsigned long long)zinfo[i].mzi_max_size,
                   (unsigned long long)zinfo[i].mzi_elem_size);
        }
    } else {
        printf("  mach_zone_info: %s (expected — needs host_priv)\n",
               mach_error_string(kr));
    }

    /* try host_zone_info if available (iOS sometimes has it) */
    /* also try mach_memory_info */
    mach_memory_info_t *minfo = NULL;
    mach_msg_type_number_t mcnt = 0;
    kr = mach_memory_info(mach_host_self(),
                          (mach_zone_name_array_t *)&minfo, &mcnt,
                          NULL, NULL);
    if (kr == KERN_SUCCESS) {
        printf("  mach_memory_info: %u entries — scanning for kptrs\n", mcnt);
        uint64_t *raw = (uint64_t *)minfo;
        unsigned nwords = (mcnt * sizeof(mach_memory_info_t)) / 8;
        for (unsigned i = 0; i < nwords; i++) {
            check_val("mach_memory_info", raw[i]);
        }
    } else {
        printf("  mach_memory_info: %s\n", mach_error_string(kr));
    }
}

/* ========================================================================== */
/* 9. IOSurface user client scalar methods                                     */
/* ========================================================================== */

static void scan_iosurface_scalars(void) {
    printf("\n===== 9. IOSurface UC scalar methods =====\n");
    io_service_t svc = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("IOSurfaceRoot"));
    if (!svc) { printf("  No IOSurfaceRoot\n"); return; }
    io_connect_t uc = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &uc);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) { printf("  IOServiceOpen failed\n"); return; }

    for (uint32_t sel = 0; sel <= 30; sel++) {
        uint64_t out[16] = {0};
        uint32_t out_cnt = 16;
        kr = IOConnectCallScalarMethod(uc, sel, NULL, 0, out, &out_cnt);
        if (kr == KERN_SUCCESS && out_cnt > 0) {
            printf("  sel%u: %u outputs:", sel, out_cnt);
            for (uint32_t j = 0; j < out_cnt; j++) {
                printf(" 0x%llx", (unsigned long long)out[j]);
                char label[32];
                snprintf(label, sizeof(label), "sel%u[%u]", sel, j);
                check_val(label, out[j]);
            }
            printf("\n");
        }
    }
    IOServiceClose(uc);
}

/* ========================================================================== */
/* 10. IOSurface UC struct methods                                             */
/* ========================================================================== */

static void scan_iosurface_structs(void) {
    printf("\n===== 10. IOSurface UC struct methods =====\n");
    io_service_t svc = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("IOSurfaceRoot"));
    if (!svc) { printf("  No IOSurfaceRoot\n"); return; }
    io_connect_t uc = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &uc);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) { printf("  IOServiceOpen failed\n"); return; }

    /* Create a surface first so lookups have something */
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int w = 32, h = 32, bpr = 128, alloc = 128 * 32, fmt = 0x42475241; /* BGRA */
    CFNumberRef wn = CFNumberCreate(NULL, kCFNumberIntType, &w);
    CFNumberRef hn = CFNumberCreate(NULL, kCFNumberIntType, &h);
    CFNumberRef bn = CFNumberCreate(NULL, kCFNumberIntType, &bpr);
    CFNumberRef an = CFNumberCreate(NULL, kCFNumberIntType, &alloc);
    CFNumberRef fn = CFNumberCreate(NULL, kCFNumberIntType, &fmt);
    CFDictionarySetValue(props, CFSTR("IOSurfaceWidth"), wn);
    CFDictionarySetValue(props, CFSTR("IOSurfaceHeight"), hn);
    CFDictionarySetValue(props, CFSTR("IOSurfaceBytesPerRow"), bn);
    CFDictionarySetValue(props, CFSTR("IOSurfaceAllocSize"), an);
    CFDictionarySetValue(props, CFSTR("IOSurfacePixelFormat"), fn);
    CFRelease(wn); CFRelease(hn); CFRelease(bn); CFRelease(an); CFRelease(fn);

    IOSurfaceRef surf = IOSurfaceCreate(props);
    CFRelease(props);
    if (surf) {
        printf("  Created IOSurface ID %u\n", IOSurfaceGetID(surf));
    }

    /* Try struct methods — provide surface ID as input for lookup-type selectors */
    uint32_t surf_id = surf ? IOSurfaceGetID(surf) : 0;

    for (uint32_t sel = 0; sel <= 30; sel++) {
        uint8_t out_buf[4096];
        size_t out_len = sizeof(out_buf);
        memset(out_buf, 0, out_len);

        /* Try with surface ID as struct input */
        uint8_t in_buf[256];
        memset(in_buf, 0, sizeof(in_buf));
        memcpy(in_buf, &surf_id, 4);

        kr = IOConnectCallStructMethod(uc, sel, in_buf, 8, out_buf, &out_len);
        if (kr == KERN_SUCCESS && out_len > 0) {
            printf("  sel%u struct: %zu bytes\n", sel, out_len);
            if (out_len <= 256) {
                hexdump("  sel_out", out_buf, out_len > 128 ? 128 : out_len);
            }
            /* scan for kptrs */
            for (size_t off = 0; off + 8 <= out_len; off += 8) {
                uint64_t v;
                memcpy(&v, out_buf + off, 8);
                if (looks_like_kptr(v)) {
                    char label[64];
                    snprintf(label, sizeof(label), "sel%u_struct+%zu", sel, off);
                    printf("  [LEAK?] %s = 0x%016llx\n", label, (unsigned long long)v);
                    g_leak_count++;
                }
            }
        }

        /* Also try with no input */
        out_len = sizeof(out_buf);
        memset(out_buf, 0, out_len);
        kr = IOConnectCallStructMethod(uc, sel, NULL, 0, out_buf, &out_len);
        if (kr == KERN_SUCCESS && out_len > 0) {
            printf("  sel%u struct (no input): %zu bytes\n", sel, out_len);
            for (size_t off = 0; off + 8 <= out_len; off += 8) {
                uint64_t v;
                memcpy(&v, out_buf + off, 8);
                if (looks_like_kptr(v)) {
                    char label[64];
                    snprintf(label, sizeof(label), "sel%u_struct_noinput+%zu", sel, off);
                    printf("  [LEAK?] %s = 0x%016llx\n", label, (unsigned long long)v);
                    g_leak_count++;
                }
            }
        }
    }

    if (surf) CFRelease(surf);
    IOServiceClose(uc);
}

/* ========================================================================== */
/* 11. sysctl enumeration                                                      */
/* ========================================================================== */

static void try_sysctl(const char *name) {
    uint8_t buf[1024];
    size_t len = sizeof(buf);
    memset(buf, 0, len);
    if (sysctlbyname(name, buf, &len, NULL, 0) == 0 && len >= 8) {
        for (size_t off = 0; off + 8 <= len; off += 8) {
            uint64_t v;
            memcpy(&v, buf + off, 8);
            if (looks_like_kptr(v)) {
                printf("  [LEAK?] sysctl %s +%zu = 0x%016llx\n",
                       name, off, (unsigned long long)v);
                g_leak_count++;
            }
        }
    }
}

static void scan_sysctls(void) {
    printf("\n===== 11. sysctl enumeration =====\n");

    /* Known interesting sysctls */
    static const char *names[] = {
        "kern.version", "kern.ostype", "kern.osrelease",
        "kern.bootargs", "kern.bootsessionuuid",
        "kern.uuid", "kern.boottime",
        "kern.proc.all", "kern.maxproc",
        "hw.memsize", "hw.physmem", "hw.usermem",
        "hw.machine", "hw.model", "hw.ncpu",
        "hw.cachelinesize", "hw.pagesize",
        "hw.busfrequency", "hw.cpufrequency",
        "hw.l1icachesize", "hw.l1dcachesize", "hw.l2cachesize",
        "machdep.cpu.brand_string",
        "debug.iokit", "debug.iotrace",
        "kern.memorystatus_vm_pressure_level",
        "kern.slide",  /* worth a shot! */
        "kern.kaslr_slide", /* probably doesn't exist */
        "kern.kernelcacheuuid",
        "kern.osbuildconfig",
        "kern.osproductversion",
        "kern.iossupportversion",
        "vm.swapusage",
        "vm.loadavg",
        "kern.aotmode",
        "kern.singleuser",
        "kern.waketime",
        "kern.willshutdown",
        "kern.progressmeterenable",
        "kern.monotonicclock_usecs",
        "kern.timer.longterm.qlen",
        "kern.timer.longterm.threshold",
        "security.mac.amfi.developer_mode_status",
        "security.mac.amfi.verbose_logging",
        "kern.hv_vmm_present",
        "hw.optional.arm.FEAT_MTE",
        "hw.optional.arm.FEAT_MTE2",
        NULL
    };

    for (int i = 0; names[i]; i++) {
        try_sysctl(names[i]);
    }

    /* Try numeric sysctl tree walk for kern (1), hw (6), debug (5), machdep (7) */
    int trees[] = { 1, 5, 6, 7 };
    for (int t = 0; t < 4; t++) {
        for (int sub = 0; sub < 200; sub++) {
            int mib[2] = { trees[t], sub };
            uint8_t buf[1024];
            size_t len = sizeof(buf);
            memset(buf, 0, len);
            if (sysctl(mib, 2, buf, &len, NULL, 0) == 0 && len >= 8) {
                for (size_t off = 0; off + 8 <= len; off += 8) {
                    uint64_t v;
                    memcpy(&v, buf + off, 8);
                    if (looks_like_kptr(v)) {
                        printf("  [LEAK?] sysctl(%d,%d)+%zu = 0x%016llx\n",
                               trees[t], sub, off, (unsigned long long)v);
                        g_leak_count++;
                    }
                }
            }
        }
    }
}

/* ========================================================================== */
/* 12. vm_region_recurse_64                                                    */
/* ========================================================================== */

static void scan_vm_regions(void) {
    printf("\n===== 12. vm_region_recurse_64 =====\n");
    mach_vm_address_t addr = 0;
    int region_count = 0;

    while (1) {
        mach_vm_size_t size = 0;
        uint32_t depth = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;

        vm_address_t addr32 = (vm_address_t)addr;
        vm_size_t size32 = 0;
        kern_return_t kr = vm_region_recurse_64(
            mach_task_self(), &addr32, &size32, &depth,
            (vm_region_recurse_info_t)&info, &cnt);
        addr = addr32;
        size = size32;
        if (kr != KERN_SUCCESS) break;

        /* scan info struct for kptrs */
        uint64_t *raw = (uint64_t *)&info;
        unsigned nwords = sizeof(info) / 8;
        for (unsigned i = 0; i < nwords; i++) {
            if (looks_like_kptr(raw[i])) {
                printf("  [LEAK?] vm_region 0x%llx info+%u = 0x%016llx\n",
                       (unsigned long long)addr, i * 8, (unsigned long long)raw[i]);
                g_leak_count++;
            }
        }

        /* Also check: object ID sometimes encodes kernel addresses */
        if (info.object_id != 0) {
            check_val("vm_region_object_id", info.object_id);
        }

        region_count++;
        addr += size;
        if (addr == 0) break; /* wrapped */
        if (region_count > 5000) break;
    }
    printf("  Scanned %d VM regions\n", region_count);
}

/* ========================================================================== */
/* 13. thread_get_state (ARM64)                                                */
/* ========================================================================== */

static void scan_thread_state(void) {
    printf("\n===== 13. thread_get_state =====\n");
    /* Already done in scan_thread_info, but let's also try exception state */
#ifdef __arm64__
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t tcnt = 0;
    kern_return_t kr = task_threads(mach_task_self(), &threads, &tcnt);
    if (kr != KERN_SUCCESS) return;

    for (unsigned t = 0; t < tcnt && t < 4; t++) {
        /* ARM_EXCEPTION_STATE64 = 7 */
        uint64_t estate[32];
        memset(estate, 0, sizeof(estate));
        mach_msg_type_number_t ecnt = sizeof(estate) / sizeof(natural_t);
        kr = thread_get_state(threads[t], 7 /* ARM_EXCEPTION_STATE64 */,
                              (thread_state_t)estate, &ecnt);
        if (kr == KERN_SUCCESS && ecnt > 0) {
            printf("  thread[%u] exception state: %u words\n", t, ecnt);
            for (unsigned i = 0; i < ecnt / 2; i++) {
                check_val("exc_state", estate[i]);
            }
        }

        /* ARM_DEBUG_STATE64 = 15 */
        uint64_t dstate[128];
        memset(dstate, 0, sizeof(dstate));
        mach_msg_type_number_t dcnt = sizeof(dstate) / sizeof(natural_t);
        kr = thread_get_state(threads[t], 15 /* ARM_DEBUG_STATE64 */,
                              (thread_state_t)dstate, &dcnt);
        if (kr == KERN_SUCCESS && dcnt > 0) {
            printf("  thread[%u] debug state: %u words\n", t, dcnt);
            for (unsigned i = 0; i < dcnt / 2 && i < 32; i++) {
                check_val("dbg_state", dstate[i]);
            }
        }
    }

    for (unsigned t = 0; t < tcnt; t++)
        mach_port_deallocate(mach_task_self(), threads[t]);
    vm_deallocate(mach_task_self(), (vm_address_t)threads,
                  tcnt * sizeof(thread_act_t));
#endif
}

/* ========================================================================== */
/* 14. Mach message audit token                                                */
/* ========================================================================== */

static void scan_audit_token(void) {
    printf("\n===== 14. Mach message audit token =====\n");

    /* Send a message to ourselves and examine the audit token */
    mach_port_t port;
    kern_return_t kr = mach_port_allocate(mach_task_self(),
                                           MACH_PORT_RIGHT_RECEIVE, &port);
    if (kr != KERN_SUCCESS) return;

    mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);

    /* Send */
    struct {
        mach_msg_header_t hdr;
        char pad[64];
    } send_msg = {0};
    send_msg.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    send_msg.hdr.msgh_size = sizeof(mach_msg_header_t);
    send_msg.hdr.msgh_remote_port = port;
    send_msg.hdr.msgh_local_port = MACH_PORT_NULL;
    send_msg.hdr.msgh_id = 42;

    kr = mach_msg(&send_msg.hdr, MACH_SEND_MSG, sizeof(mach_msg_header_t),
                  0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) {
        printf("  mach_msg send failed: %s\n", mach_error_string(kr));
        goto done;
    }

    /* Receive with trailer to get audit token */
    struct {
        mach_msg_header_t hdr;
        char pad[64];
        mach_msg_audit_trailer_t trailer;
    } recv_msg = {0};

    kr = mach_msg(&recv_msg.hdr,
                  MACH_RCV_MSG | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
                  MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT),
                  0, sizeof(recv_msg), port, 1000, MACH_PORT_NULL);
    if (kr == KERN_SUCCESS) {
        printf("  Received msg, trailer type=%u size=%u\n",
               recv_msg.trailer.msgh_trailer_type,
               recv_msg.trailer.msgh_trailer_size);

        /* dump audit token */
        uint32_t *at = (uint32_t *)&recv_msg.trailer.msgh_audit;
        printf("  audit_token:");
        for (int i = 0; i < 8; i++)
            printf(" 0x%08x", at[i]);
        printf("\n");

        /* Check 64-bit pairs */
        for (int i = 0; i < 4; i++) {
            uint64_t v = ((uint64_t)at[i * 2 + 1] << 32) | at[i * 2];
            check_val("audit_token", v);
        }
    }

done:
    mach_port_destroy(mach_task_self(), port);
}

/* ========================================================================== */
/* 15. IOSurface shared memory — check for kernel VA leaks                     */
/* ========================================================================== */

static void scan_iosurface_shared(void) {
    printf("\n===== 15. IOSurface shared memory =====\n");

    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int w = 64, h = 64, bpr = 256, alloc = 256 * 64, fmt = 0x42475241;
    CFNumberRef wn = CFNumberCreate(NULL, kCFNumberIntType, &w);
    CFNumberRef hn = CFNumberCreate(NULL, kCFNumberIntType, &h);
    CFNumberRef bn = CFNumberCreate(NULL, kCFNumberIntType, &bpr);
    CFNumberRef an = CFNumberCreate(NULL, kCFNumberIntType, &alloc);
    CFNumberRef fn = CFNumberCreate(NULL, kCFNumberIntType, &fmt);
    CFDictionarySetValue(props, CFSTR("IOSurfaceWidth"), wn);
    CFDictionarySetValue(props, CFSTR("IOSurfaceHeight"), hn);
    CFDictionarySetValue(props, CFSTR("IOSurfaceBytesPerRow"), bn);
    CFDictionarySetValue(props, CFSTR("IOSurfaceAllocSize"), an);
    CFDictionarySetValue(props, CFSTR("IOSurfacePixelFormat"), fn);
    CFRelease(wn); CFRelease(hn); CFRelease(bn); CFRelease(an); CFRelease(fn);

    IOSurfaceRef surf = IOSurfaceCreate(props);
    CFRelease(props);
    if (!surf) { printf("  Failed to create IOSurface\n"); return; }

    IOSurfaceLock(surf, 0, NULL);
    void *base = IOSurfaceGetBaseAddress(surf);
    printf("  IOSurface ID: %u\n", IOSurfaceGetID(surf));
    printf("  Base address (userspace): %p\n", base);
    printf("  Alloc size: %zu\n", IOSurfaceGetAllocSize(surf));
    printf("  Seed: %u\n", IOSurfaceGetSeed(surf));
    IOSurfaceUnlock(surf, 0, NULL);

    /* check all accessible IOSurface properties for kernel-like values */
    uint32_t sid = IOSurfaceGetID(surf);
    printf("  Surface ID: %u (0x%x)\n", sid, sid);
    check_val("IOSurfaceGetID", (uint64_t)sid);

    /* Try IOSurfaceCopyAllValues — may return internal kernel addresses as values */
    CFDictionaryRef all_vals = IOSurfaceCopyAllValues(surf);
    if (all_vals) {
        printf("  IOSurfaceCopyAllValues returned %ld keys\n",
               (long)CFDictionaryGetCount(all_vals));
        CFDictionaryApplyFunction(all_vals, scan_cfdict_cb, (void *)"IOSurf");
        CFRelease(all_vals);
    }

    /* Also open user client and try to get surface info via external method */
    io_service_t svc = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("IOSurfaceRoot"));
    if (svc) {
        io_connect_t uc = 0;
        IOServiceOpen(svc, mach_task_self(), 0, &uc);
        IOObjectRelease(svc);

        if (MACH_PORT_VALID(uc)) {
            /* sel6 = s_get_value, sel2 = s_lookup by ID */
            uint64_t scal_in[1] = { sid };
            uint64_t scal_out[8] = {0};
            uint32_t scal_out_cnt = 8;

            kern_return_t kr = IOConnectCallScalarMethod(uc, 2 /* lookup */,
                                                          scal_in, 1,
                                                          scal_out, &scal_out_cnt);
            if (kr == KERN_SUCCESS) {
                printf("  sel2(lookup) returned %u scalars:", scal_out_cnt);
                for (uint32_t j = 0; j < scal_out_cnt; j++) {
                    printf(" 0x%llx", (unsigned long long)scal_out[j]);
                    check_val("sel2_lookup", scal_out[j]);
                }
                printf("\n");
            }

            /* sel9 = s_get_ycbcr_matrix, various getters */
            for (uint32_t getter = 3; getter <= 15; getter++) {
                memset(scal_out, 0, sizeof(scal_out));
                scal_out_cnt = 8;
                kr = IOConnectCallScalarMethod(uc, getter, scal_in, 1,
                                               scal_out, &scal_out_cnt);
                if (kr == KERN_SUCCESS && scal_out_cnt > 0) {
                    for (uint32_t j = 0; j < scal_out_cnt; j++)
                        check_val("iosurf_getter", scal_out[j]);
                }
            }

            IOServiceClose(uc);
        }
    }

    CFRelease(surf);
}

/* ========================================================================== */
/* 16. BONUS: IOKit user client trap methods                                   */
/* ========================================================================== */

static void scan_iokit_traps(void) {
    printf("\n===== 16. IOKit trap methods (iokit_user_client_trap) =====\n");

    io_service_t svc = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("IOSurfaceRoot"));
    if (!svc) return;
    io_connect_t uc = 0;
    IOServiceOpen(svc, mach_task_self(), 0, &uc);
    IOObjectRelease(svc);
    if (!MACH_PORT_VALID(uc)) return;

    /* IOConnectTrap calls getExternalTrapForIndex in kernel.
       The return is typically 0 for unused traps, but some drivers
       return function pointers (which would be kernel addresses). */
    for (uint32_t idx = 0; idx < 8; idx++) {
        /* There's no direct API to call traps by index from userspace
           in a way that returns the function pointer. But we can try
           IOConnectTrap0..6 and observe results. */
        /* On arm64, IOConnectTrap uses a Mach trap that goes:
           iokit_user_client_trap(connect, index, p1..p6)
           It calls the trap function and returns its result. */
        uint64_t result = IOConnectTrap0(uc, idx);
        if (result != 0 && result != (uint64_t)-1) {
            printf("  Trap %u returned: 0x%llx\n", idx, (unsigned long long)result);
            check_val("iokit_trap", result);
        }
    }

    IOServiceClose(uc);
}

/* ========================================================================== */
/* 17. BONUS: Try other IOKit drivers for leaks                                */
/* ========================================================================== */

static void scan_other_iokit_drivers(void) {
    printf("\n===== 17. Other IOKit driver methods =====\n");

    static const char *drivers[] = {
        "AppleMobileFileIntegrity",
        "IOHIDSystem",
        "AppleKeyStore",
        "IOUserEthernet",
        "AppleSPU",
        NULL
    };

    for (int d = 0; drivers[d]; d++) {
        io_service_t svc = IOServiceGetMatchingService(
            kIOMainPortDefault, IOServiceMatching(drivers[d]));
        if (!svc) continue;

        io_connect_t uc = 0;
        kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &uc);
        IOObjectRelease(svc);
        if (kr != KERN_SUCCESS || !MACH_PORT_VALID(uc)) continue;

        printf("  Opened %s user client\n", drivers[d]);

        /* try scalar methods 0-10 */
        for (uint32_t sel = 0; sel <= 10; sel++) {
            uint64_t out[8] = {0};
            uint32_t out_cnt = 8;
            kr = IOConnectCallScalarMethod(uc, sel, NULL, 0, out, &out_cnt);
            if (kr == KERN_SUCCESS && out_cnt > 0) {
                for (uint32_t j = 0; j < out_cnt; j++) {
                    if (out[j] != 0) {
                        char label[64];
                        snprintf(label, sizeof(label), "%s_sel%u[%u]",
                                 drivers[d], sel, j);
                        printf("  %s = 0x%llx\n", label, (unsigned long long)out[j]);
                        check_val(label, out[j]);
                    }
                }
            }
        }

        IOServiceClose(uc);
    }
}

/* ========================================================================== */
/* 18. BONUS: host_get_special_port variants                                   */
/* ========================================================================== */

static void scan_host_special_ports(void) {
    printf("\n===== 18. host_get_special_port =====\n");
    mach_port_t host = mach_host_self();

    for (int port_id = 0; port_id <= 16; port_id++) {
        mach_port_t sp = MACH_PORT_NULL;
        kern_return_t kr = host_get_special_port(host, 0, port_id, &sp);
        if (kr == KERN_SUCCESS && MACH_PORT_VALID(sp)) {
            printf("  special_port[%d] = 0x%x", port_id, sp);

            /* try kobject on this port */
            unsigned kotype = 0;
            char desc[256] = {0};
            mach_msg_type_number_t desc_len = sizeof(desc);
            kr = mach_port_kobject_description(
                mach_task_self(), sp, &kotype, desc, &desc_len);
            if (kr == KERN_SUCCESS) {
                printf(" kotype=%u desc=\"%s\"", kotype, desc);
                char *hex = strstr(desc, "0x");
                while (hex) {
                    uint64_t v = strtoull(hex, NULL, 16);
                    check_val("special_port_desc", v);
                    hex = strstr(hex + 1, "0x");
                }
            }
            printf("\n");

            if (sp != host && sp != mach_task_self())
                mach_port_deallocate(mach_task_self(), sp);
        }
    }
}

/* ========================================================================== */
/* 19. BONUS: task_get_special_port                                            */
/* ========================================================================== */

static void scan_task_special_ports(void) {
    printf("\n===== 19. task_get_special_port =====\n");

    for (int port_id = 1; port_id <= 10; port_id++) {
        mach_port_t sp = MACH_PORT_NULL;
        kern_return_t kr = task_get_special_port(mach_task_self(), port_id, &sp);
        if (kr == KERN_SUCCESS && MACH_PORT_VALID(sp)) {
            printf("  task_special_port[%d] = 0x%x\n", port_id, sp);

            unsigned kotype = 0;
            char desc[256] = {0};
            mach_msg_type_number_t desc_len = sizeof(desc);
            kr = mach_port_kobject_description(
                mach_task_self(), sp, &kotype, desc, &desc_len);
            if (kr == KERN_SUCCESS) {
                printf("    kotype=%u desc=\"%s\"\n", kotype, desc);
                char *hex = strstr(desc, "0x");
                while (hex) {
                    uint64_t v = strtoull(hex, NULL, 16);
                    check_val("task_special_port_desc", v);
                    hex = strstr(hex + 1, "0x");
                }
            }
        }
    }
}

/* ========================================================================== */
/* 20. BONUS: processor_set info                                               */
/* ========================================================================== */

static void scan_processor_set(void) {
    printf("\n===== 20. processor_set info =====\n");
    mach_port_t host = mach_host_self();
    processor_set_name_array_t psets = NULL;
    mach_msg_type_number_t pcnt = 0;
    kern_return_t kr = host_processor_sets(host, &psets, &pcnt);
    if (kr == KERN_SUCCESS) {
        printf("  %u processor sets\n", pcnt);
        for (unsigned i = 0; i < pcnt; i++) {
            /* try processor_set_info */
            struct processor_set_basic_info binfo;
            mach_msg_type_number_t bicnt = PROCESSOR_SET_BASIC_INFO_COUNT;
            kr = processor_set_info(psets[i], PROCESSOR_SET_BASIC_INFO,
                                     &host, (processor_set_info_t)&binfo, &bicnt);
            if (kr == KERN_SUCCESS) {
                printf("  pset[%u]: count=%d default=%d\n",
                       i, binfo.processor_count, binfo.default_policy);
            }
        }
    } else {
        printf("  host_processor_sets: %s\n", mach_error_string(kr));
    }
}

/* ========================================================================== */
/* main                                                                        */
/* ========================================================================== */

int main(int argc, char *argv[]) {
    printf("KASLR Leak Scanner for iOS 27.1 (A20 Pro)\n");
    printf("PID: %d, UID: %d\n", getpid(), getuid());
    printf("=========================================\n");

    scan_iokit_registry();
    scan_kobject();
    scan_port_attrs();
    scan_proc_pidinfo();
    scan_task_info();
    scan_host_info();
    scan_thread_info();
    scan_zone_info();
    scan_iosurface_scalars();
    scan_iosurface_structs();
    scan_sysctls();
    scan_vm_regions();
    scan_thread_state();
    scan_audit_token();
    scan_iosurface_shared();
    scan_iokit_traps();
    scan_other_iokit_drivers();
    scan_host_special_ports();
    scan_task_special_ports();
    scan_processor_set();

    printf("\n=========================================\n");
    printf("SCAN COMPLETE: %d potential leak candidates found\n", g_leak_count);
    if (g_leak_count == 0)
        printf("No kernel pointer leaks detected via standard APIs.\n");
    else
        printf("Review [LEAK?] entries above — some may be false positives.\n");

    return 0;
}
