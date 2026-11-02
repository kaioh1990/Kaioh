// Linux system call emulation for TinyGo on the PS Vita.
//
// Go's syscall package (linux/arm flavour) is used as-is, but its low-level
// RawSyscall6 is redirected here (see the Vita goroot copy) instead of
// executing "svc #0", which the Vita kernel rejects. Each Linux ARM EABI
// system call Go needs is mapped onto newlib / Sce* calls.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <psp2/kernel/rng.h>
#include <psp2/io/stat.h>

// ---- Linux ARM EABI system call numbers
#define L_read 3
#define L_write 4
#define L_close 6
#define L_chdir 12
#define L_getpid 20
#define L_getuid 24
#define L_ioctl 54
#define L_getppid 64
#define L_fsync 118
#define L_fchdir 133
#define L__llseek 140
#define L_fdatasync 148
#define L_sched_yield 158
#define L_pread64 180
#define L_pwrite64 181
#define L_getcwd 183
#define L_ftruncate64 194
#define L_fstat64 197
#define L_getuid32 199
#define L_getgid32 200
#define L_geteuid32 201
#define L_getegid32 202
#define L_getgroups32 205
#define L_getdents64 217
#define L_fcntl64 221
#define L_gettid 224
#define L_openat 322
#define L_mkdirat 323
#define L_fstatat64 327
#define L_unlinkat 328
#define L_renameat 329
#define L_readlinkat 332
#define L_fchmodat 333
#define L_faccessat 334
#define L_utimensat 348
#define L_prlimit64 369
#define L_renameat2 382
#define L_getrandom 384
#define L_statx 397
#define L_faccessat2 439

// Linux open flags (ARM)
#define LO_ACCMODE 03
#define LO_CREAT 0100
#define LO_EXCL 0200
#define LO_TRUNC 01000
#define LO_APPEND 02000
#define LO_DIRECTORY 040000
#define L_AT_FDCWD (-100)
#define L_AT_REMOVEDIR 0x200

__attribute__((weak)) void vlog(const char *s) {}

// ---- working directory (kept here so relative paths work regardless of newlib)
static char cwd[512] = "ux0:data";

static void normalize(char *p) {
	// collapse "/./", "//" and "/x/../"
	char out[512]; int o = 0; size_t n = strlen(p);
	size_t i = 0;
	// keep device prefix "ux0:" as-is
	char *colon = strchr(p, ':');
	if (colon) { size_t k = colon - p + 1; memcpy(out, p, k); o = k; i = k; }
	while (i < n && o < 510) {
		if (p[i] == '/') {
			while (i < n && p[i] == '/') i++;
			if (o == 0 || out[o - 1] != ':') { if (o == 0 || out[o - 1] != '/') out[o++] = '/'; }
			continue;
		}
		size_t j = i; while (j < n && p[j] != '/') j++;
		size_t len = j - i;
		if (len == 1 && p[i] == '.') { i = j; continue; }
		if (len == 2 && p[i] == '.' && p[i + 1] == '.') {
			while (o > 0 && out[o - 1] == '/') o--;
			while (o > 0 && out[o - 1] != '/' && out[o - 1] != ':') o--;
			i = j; continue;
		}
		if (o > 0 && out[o - 1] != '/' && out[o - 1] != ':') out[o++] = '/';
		memcpy(out + o, p + i, len); o += len; i = j;
	}
	while (o > 1 && out[o - 1] == '/' && out[o - 2] != ':') o--;
	out[o] = 0;
	strcpy(p, out);
}

static void resolve(const char *in, char *out) {
	if (strchr(in, ':')) snprintf(out, 512, "%s", in);           // ux0:...
	else if (in[0] == '/') snprintf(out, 512, "ux0:%s", in);     // /foo -> ux0:/foo
	else snprintf(out, 512, "%s/%s", cwd, in);
	normalize(out);
}

// ---- directory handles (Linux reads directories with open + getdents64)
#define DIRFD_BASE 0x4000
#define MAXDIRS 32
static struct { DIR *d; char path[512]; struct dirent pending; int has_pending; } dirs[MAXDIRS];

static int is_dirfd(int fd) { return fd >= DIRFD_BASE && fd < DIRFD_BASE + MAXDIRS && dirs[fd - DIRFD_BASE].d; }

static int ret(int r) { return r < 0 ? -(errno ? errno : EIO) : r; }

// Linux struct stat64 (ARM EABI), 104 bytes
struct lstat64 {
	uint64_t dev; uint16_t pad1; uint8_t p0[2]; uint32_t ino32;
	uint32_t mode; uint32_t nlink; uint32_t uid; uint32_t gid;
	uint64_t rdev; uint16_t pad2; uint8_t p1[6];
	int64_t size; int32_t blksize; uint8_t p2[4]; int64_t blocks;
	int32_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
	uint64_t ino;
};
_Static_assert(sizeof(struct lstat64) == 104, "stat64 layout");

static void to_lstat(const struct stat *s, struct lstat64 *l) {
	memset(l, 0, sizeof *l);
	l->mode = s->st_mode; l->nlink = 1; l->size = s->st_size;
	l->blksize = 4096; l->blocks = (s->st_size + 511) / 512;
	l->atime = s->st_atime; l->mtime = s->st_mtime; l->ctime = s->st_ctime;
	l->ino = l->ino32 = (uint32_t)s->st_ino;
}

static int path_stat(const char *p, struct lstat64 *l) {
	char full[512]; struct stat s;
	resolve(p, full);
	if (stat(full, &s) < 0) return -(errno ? errno : ENOENT);
	to_lstat(&s, l);
	return 0;
}

static int do_openat(int dirfd, const char *path, int lflags, int mode) {
	char full[512]; struct stat s;
	resolve(path, full);
	if (stat(full, &s) == 0 && S_ISDIR(s.st_mode)) {
		for (int i = 0; i < MAXDIRS; i++) if (!dirs[i].d) {
			DIR *d = opendir(full);
			if (!d) return -(errno ? errno : ENOENT);
			dirs[i].d = d; dirs[i].has_pending = 0; snprintf(dirs[i].path, 512, "%s", full);
			return DIRFD_BASE + i;
		}
		return -EMFILE;
	}
	if (lflags & LO_DIRECTORY) return -ENOTDIR;
	int f = lflags & LO_ACCMODE;
	if (lflags & LO_CREAT) f |= O_CREAT;
	if (lflags & LO_EXCL) f |= O_EXCL;
	if (lflags & LO_TRUNC) f |= O_TRUNC;
	if (lflags & LO_APPEND) f |= O_APPEND;
	return ret(open(full, f, mode));
}

struct ldirent64 { uint64_t ino; int64_t off; uint16_t reclen; uint8_t type; char name[]; };

static int do_getdents(int fd, char *buf, int len) {
	if (!is_dirfd(fd)) return -EBADF;
	int i = fd - DIRFD_BASE;
	int used = 0;
	for (;;) {
		struct dirent *e;
		if (dirs[i].has_pending) { e = &dirs[i].pending; }
		else { e = readdir(dirs[i].d); if (!e) break; }
		size_t nl = strlen(e->d_name);
		int rec = (int)((sizeof(struct ldirent64) + nl + 1 + 7) & ~7u);
		if (used + rec > len) {
			if (!dirs[i].has_pending) { dirs[i].pending = *e; dirs[i].has_pending = 1; }
			break;
		}
		struct ldirent64 *o = (struct ldirent64 *)(buf + used);
		o->ino = 1; o->off = used + rec; o->reclen = rec;
		o->type = SCE_S_ISDIR(e->d_stat.st_mode) ? 4 : 8; // DT_DIR / DT_REG
		memcpy(o->name, e->d_name, nl + 1);
		used += rec;
		dirs[i].has_pending = 0;
	}
	return used;
}

int tinygo_vita_syscall(int trap, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6) {
	errno = 0;
	switch (trap) {
	case L_read:
		if (is_dirfd(a1)) return -EISDIR;
		return ret(read(a1, (void *)a2, a3));
	case L_write:
		return ret(write(a1, (const void *)a2, a3));
	case L_close:
		if (is_dirfd(a1)) { closedir(dirs[a1 - DIRFD_BASE].d); dirs[a1 - DIRFD_BASE].d = 0; return 0; }
		return ret(close(a1));
	case L_openat:
		return do_openat((int)a1, (const char *)a2, (int)a3, (int)a4);
	case L_chdir: {
		char full[512]; struct stat s;
		resolve((const char *)a1, full);
		if (stat(full, &s) < 0) return -ENOENT;
		if (!S_ISDIR(s.st_mode)) return -ENOTDIR;
		snprintf(cwd, sizeof cwd, "%s", full);
		return 0;
	}
	case L_fchdir:
		if (!is_dirfd(a1)) return -EBADF;
		snprintf(cwd, sizeof cwd, "%s", dirs[a1 - DIRFD_BASE].path);
		return 0;
	case L_getcwd: {
		size_t n = strlen(cwd) + 1;
		if (n > a2) return -ERANGE;
		memcpy((void *)a1, cwd, n);
		return (int)n;
	}
	case L__llseek: { // fd, off_hi, off_lo, loff_t *result, whence
		if (is_dirfd(a1)) { *(int64_t *)a4 = 0; return 0; }
		int64_t off = ((int64_t)a2 << 32) | (uint32_t)a3;
		off_t r = lseek(a1, (off_t)off, (int)a5);
		if (r < 0) return -(errno ? errno : EINVAL);
		*(int64_t *)a4 = r;
		return 0;
	}
	case L_pread64: case L_pwrite64: { // fd, buf, count, pad, off_lo, off_hi
		int64_t off = ((int64_t)a6 << 32) | (uint32_t)a5;
		off_t cur = lseek(a1, 0, SEEK_CUR);
		if (lseek(a1, (off_t)off, SEEK_SET) < 0) return -(errno ? errno : EINVAL);
		int r = trap == L_pread64 ? read(a1, (void *)a2, a3) : write(a1, (const void *)a2, a3);
		int e = errno;
		lseek(a1, cur, SEEK_SET);
		errno = e;
		return ret(r);
	}
	case L_fstat64: {
		if (is_dirfd(a1)) return path_stat(dirs[a1 - DIRFD_BASE].path, (struct lstat64 *)a2);
		struct stat s;
		if (fstat(a1, &s) < 0) return -(errno ? errno : EBADF);
		to_lstat(&s, (struct lstat64 *)a2);
		return 0;
	}
	case L_fstatat64: // dirfd, path, stat, flags
		return path_stat((const char *)a2, (struct lstat64 *)a3);
	case L_getdents64:
		return do_getdents(a1, (char *)a2, a3);
	case L_mkdirat: {
		char full[512]; resolve((const char *)a2, full);
		return ret(mkdir(full, a3));
	}
	case L_unlinkat: {
		char full[512]; resolve((const char *)a2, full);
		return ret((a3 & L_AT_REMOVEDIR) ? rmdir(full) : unlink(full));
	}
	case L_renameat: case L_renameat2: {
		char f1[512], f2[512];
		resolve((const char *)a2, f1); resolve((const char *)a4, f2);
		return ret(rename(f1, f2));
	}
	case L_faccessat: case L_faccessat2: {
		struct lstat64 l;
		return path_stat((const char *)a2, &l);
	}
	case L_ftruncate64: // fd, pad, lo, hi
		return ret(ftruncate(a1, (off_t)(((int64_t)a4 << 32) | (uint32_t)a3)));
	case L_fcntl64:
		return 0;
	case L_fsync: case L_fdatasync: case L_utimensat: case L_fchmodat: case L_sched_yield:
		return 0;
	case L_ioctl:
		return -ENOTTY;
	case L_readlinkat:
		return -EINVAL;
	case L_getpid: case L_gettid:
		return 1;
	case L_getppid:
		return 0;
	case L_getuid: case L_getuid32: case L_getgid32: case L_geteuid32: case L_getegid32:
		return 0;
	case L_getgroups32:
		return 0;
	case L_getrandom: {
		unsigned char *p = (unsigned char *)a1; size_t left = a2;
		while (left) {
			size_t n = left > 64 ? 64 : left;
			if (sceKernelGetRandomNumber(p, n) < 0) return -EIO;
			p += n; left -= n;
		}
		return (int)a2;
	}
	case L_prlimit64: case L_statx:
		return -ENOSYS;
	}
	char msg[64];
	snprintf(msg, sizeof msg, "vita: unsupported Linux syscall %d", trap);
	vlog(msg);
	return -ENOSYS;
}
