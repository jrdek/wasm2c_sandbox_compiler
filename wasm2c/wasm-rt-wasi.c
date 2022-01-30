// This file contains modified versions of the emscripten runtime (as well as
// the wasi support it includes ) adapted for
//    - library sandboxing --- these apis have to support multiple wasm2c
//    sandboxed components and cannot use global variables.
//                             Instead they operate on the sbx context specified
//                             as the first parameter to the API
//    - compatibility --- APIs like setjmp/longjmp are implemented in a very
//    limited way upstream. Expanding this.
//    - security --- APIs like args_get, sys_open, fd_write, sys_read seemed to
//    have security bugs upstream.
//                   Additionally, we don't want to allow any file system
//                   access. Restrict opens to the null file (/dev/null on unix,
//                   nul on windows)

/////////////////////////////////////////////////////////////
////////// File: Missing stuff from emscripten
/////////////////////////////////////////////////////////////
#include <stdint.h>
#include <fcntl.h>
#include <sys/random.h>
#include <netinet/in.h>

#include "wasm-rt-os.h"
#include "wasi-types.h"



#ifndef WASM_RT_ADD_PREFIX
#define WASM_RT_ADD_PREFIX
#endif

/////////////////////////////////////////////////////////////
////////// File: Modified emscripten/tools/wasm2c/base.c
/////////////////////////////////////////////////////////////

/*
 * Base of all support for wasm2c code.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

// ssize_t detection: usually stdint provides it, but not on windows apparently
#ifdef _WIN32
#ifdef _MSC_VER
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif  // _MSC_VER
#endif  // _WIN32

#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif

#include "wasm-rt-impl.h"
#include "wasm-rt.h"

#if defined(__GNUC__)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#else
#define UNLIKELY(x) (x)
#define LIKELY(x) (x)
#endif

#if defined(_WIN32)
#define POSIX_PREFIX(func) _##func
#else
#define POSIX_PREFIX(func) func
#endif

#define TRAP(x) wasm_rt_trap(WASM_RT_TRAP_##x)

#define MEMACCESS(mem, addr) ((void*)&(mem->data[addr]))

#define UNCOND_MEMCHECK_SIZE(mem, a, sz) \
  if (UNLIKELY((a) + sz > mem->size))    \
  TRAP(OOB)

#define UNCOND_MEMCHECK(mem, a, t) UNCOND_MEMCHECK_SIZE(mem, a, sizeof(t))

#define DEFINE_WASI_LOAD(name, t1, t2, t3)                 \
  static inline t3 name(wasm_rt_memory_t* mem, u64 addr) { \
    UNCOND_MEMCHECK(mem, addr, t1);                        \
    t1 result;                                             \
    memcpy(&result, MEMACCESS(mem, addr), sizeof(t1));     \
    return (t3)(t2)result;                                 \
  }

#define DEFINE_WASI_STORE(name, t1, t2)                                \
  static inline void name(wasm_rt_memory_t* mem, u64 addr, t2 value) { \
    UNCOND_MEMCHECK(mem, addr, t1);                                    \
    t1 wrapped = (t1)value;                                            \
    memcpy(MEMACCESS(mem, addr), &wrapped, sizeof(t1));                \
  }

DEFINE_WASI_LOAD(wasm_i32_load, u32, u32, u32);
DEFINE_WASI_LOAD(wasm_i64_load, u64, u64, u64);
DEFINE_WASI_LOAD(wasm_f32_load, f32, f32, f32);
DEFINE_WASI_LOAD(wasm_f64_load, f64, f64, f64);
DEFINE_WASI_LOAD(wasm_i32_load8_s, s8, s32, u32);
DEFINE_WASI_LOAD(wasm_i64_load8_s, s8, s64, u64);
DEFINE_WASI_LOAD(wasm_i32_load8_u, u8, u32, u32);
DEFINE_WASI_LOAD(wasm_i64_load8_u, u8, u64, u64);
DEFINE_WASI_LOAD(wasm_i32_load16_s, s16, s32, u32);
DEFINE_WASI_LOAD(wasm_i64_load16_s, s16, s64, u64);
DEFINE_WASI_LOAD(wasm_i32_load16_u, u16, u32, u32);
DEFINE_WASI_LOAD(wasm_i64_load16_u, u16, u64, u64);
DEFINE_WASI_LOAD(wasm_i64_load32_s, s32, s64, u64);
DEFINE_WASI_LOAD(wasm_i64_load32_u, u32, u64, u64);
DEFINE_WASI_STORE(wasm_i32_store, u32, u32);
DEFINE_WASI_STORE(wasm_i64_store, u64, u64);
DEFINE_WASI_STORE(wasm_f32_store, f32, f32);
DEFINE_WASI_STORE(wasm_f64_store, f64, f64);
DEFINE_WASI_STORE(wasm_i32_store8, u8, u32);
DEFINE_WASI_STORE(wasm_i32_store16, u16, u32);
DEFINE_WASI_STORE(wasm_i64_store8, u8, u64);
DEFINE_WASI_STORE(wasm_i64_store16, u16, u64);
DEFINE_WASI_STORE(wasm_i64_store32, u32, u64);


#define CHECK_ERRNO() \
    do { \
        if (errno != 0){ \
          return from_syscall_ret(errno); \
        } \
    } while (0)

// Imports

#ifdef VERBOSE_LOGGING
#define VERBOSE_LOG(...) \
  { printf(__VA_ARGS__); }
#else
#define VERBOSE_LOG(...)
#endif

#define STUB_IMPORT_IMPL(ret, name, params, returncode) \
  ret name params {                                     \
    VERBOSE_LOG("[stub import: " #name "]\n");          \
    return returncode;                                  \
  }

// Generic abort method for a runtime error in the runtime.

static void abort_with_message(const char* message) {
  fprintf(stderr, "%s\n", message);
  TRAP(UNREACHABLE);
}

/////////////////////////////////////////// Emscripten runtime
//////////////////////////////////////////////////

// Setjmp/longjmp are not currently supported safely. So lonjmp with abort,
// setjmp can be a noop.
void Z_envZ_emscripten_longjmpZ_vii(wasm_sandbox_wasi_data* wasi_data,
                                    u32 buf,
                                    u32 value) {
  abort_with_message("longjmp not supported");
}

STUB_IMPORT_IMPL(u32,
                 Z_envZ_emscripten_setjmpZ_ii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 buf),
                 0);

void Z_envZ_emscripten_notify_memory_growthZ_vi(
    wasm_sandbox_wasi_data* wasi_data,
    u32 size) {}

u32 Z_envZ_getTempRet0Z_iv(wasm_sandbox_wasi_data* wasi_data) {
  return wasi_data->tempRet0;
}

void Z_envZ_setTempRet0Z_vi(wasm_sandbox_wasi_data* wasi_data, u32 x) {
  wasi_data->tempRet0 = x;
}

static const char* get_null_file_path() {
#ifdef _WIN32
  const char* null_file = "nul";
#else
  const char* null_file = "/dev/null";
#endif
  return null_file;
}

static int get_null_file_mode() {
#ifdef _WIN32
  const int null_file_mode = _S_IREAD | _S_IWRITE;
#else
  const int null_file_mode = S_IRUSR | S_IWUSR;
#endif
  return null_file_mode;
}

static int get_null_file_flags() {
#ifdef _WIN32
  const int null_file_flags = _O_CREAT;
#else
  const int null_file_flags = O_CREAT;
#endif
  return null_file_flags;
}

static int is_null_file(wasm_sandbox_wasi_data* wasi_data, u32 path) {
  const char* tainted_string =
      (const char*)MEMACCESS(wasi_data->heap_memory, path);
  // deliberately truncate length to u32, else UNCOND_MEMCHECK_SIZE may have
  // overflows
  u32 path_length = strlen(tainted_string) + 1;
  UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, path, path_length);

  const char* null_file = get_null_file_path();
  size_t null_file_len = strlen(null_file) + 1;
  if (null_file_len != path_length) {
    return 0;
  }

  if (memcmp(null_file, tainted_string, null_file_len) != 0) {
    return 0;
  }

  return 1;
}

// Syscalls return a negative error code
#define EM_EACCES -2

static u32 do_stat(wasm_sandbox_wasi_data* wasi_data, int nfd, u32 buf) {
  struct stat nbuf;
  if (fstat(nfd, &nbuf)) {
    VERBOSE_LOG("    error, %d %s\n", errno, strerror(errno));
    return EM_EACCES;
  }
  VERBOSE_LOG("    success, size=%ld\n", nbuf.st_size);
  wasm_i32_store(wasi_data->heap_memory, buf + 0, nbuf.st_dev);
  wasm_i32_store(wasi_data->heap_memory, buf + 4, 0);
  wasm_i32_store(wasi_data->heap_memory, buf + 8, nbuf.st_ino);
  wasm_i32_store(wasi_data->heap_memory, buf + 12, nbuf.st_mode);
  wasm_i32_store(wasi_data->heap_memory, buf + 16, nbuf.st_nlink);
  wasm_i32_store(wasi_data->heap_memory, buf + 20, nbuf.st_uid);
  wasm_i32_store(wasi_data->heap_memory, buf + 24, nbuf.st_gid);
  wasm_i32_store(wasi_data->heap_memory, buf + 28, nbuf.st_rdev);
  wasm_i32_store(wasi_data->heap_memory, buf + 32, 0);
  wasm_i64_store(wasi_data->heap_memory, buf + 40, nbuf.st_size);
#ifdef _WIN32
  wasm_i32_store(wasi_data->heap_memory, buf + 48,
                 512);  // fixed blocksize on windows
  wasm_i32_store(wasi_data->heap_memory, buf + 52,
                 0);  // but no reported blocks...
#else
  wasm_i32_store(wasi_data->heap_memory, buf + 48, nbuf.st_blksize);
  wasm_i32_store(wasi_data->heap_memory, buf + 52, nbuf.st_blocks);
#endif
#if defined(__APPLE__) || defined(__NetBSD__)
  wasm_i32_store(wasi_data->heap_memory, buf + 56, nbuf.st_atimespec.tv_sec);
  wasm_i32_store(wasi_data->heap_memory, buf + 60, nbuf.st_atimespec.tv_nsec);
  wasm_i32_store(wasi_data->heap_memory, buf + 64, nbuf.st_mtimespec.tv_sec);
  wasm_i32_store(wasi_data->heap_memory, buf + 68, nbuf.st_mtimespec.tv_nsec);
  wasm_i32_store(wasi_data->heap_memory, buf + 72, nbuf.st_ctimespec.tv_sec);
  wasm_i32_store(wasi_data->heap_memory, buf + 76, nbuf.st_ctimespec.tv_nsec);
#elif defined(_WIN32)
  wasm_i32_store(wasi_data->heap_memory, buf + 56,
                 gmtime(&nbuf.st_atime)->tm_sec);
  wasm_i32_store(wasi_data->heap_memory, buf + 60, 0);
  wasm_i32_store(wasi_data->heap_memory, buf + 64,
                 gmtime(&nbuf.st_mtime)->tm_sec);
  wasm_i32_store(wasi_data->heap_memory, buf + 68, 0);
  wasm_i32_store(wasi_data->heap_memory, buf + 72,
                 gmtime(&nbuf.st_ctime)->tm_sec);
  wasm_i32_store(wasi_data->heap_memory, buf + 76, 0);
#else
  wasm_i32_store(wasi_data->heap_memory, buf + 56, nbuf.st_atim.tv_sec);
  wasm_i32_store(wasi_data->heap_memory, buf + 60, nbuf.st_atim.tv_nsec);
  wasm_i32_store(wasi_data->heap_memory, buf + 64, nbuf.st_mtim.tv_sec);
  wasm_i32_store(wasi_data->heap_memory, buf + 68, nbuf.st_mtim.tv_nsec);
  wasm_i32_store(wasi_data->heap_memory, buf + 72, nbuf.st_ctim.tv_sec);
  wasm_i32_store(wasi_data->heap_memory, buf + 76, nbuf.st_ctim.tv_nsec);
#endif
  wasm_i64_store(wasi_data->heap_memory, buf + 80, nbuf.st_ino);
  return 0;
}

#define WASM_STDIN 0
#define WASM_STDOUT 1
#define WASM_STDERR 2

static void init_fds(wasm_sandbox_wasi_data* wasi_data) {
#ifndef _WIN32
  wasi_data->wasm_fd_to_native[WASM_STDIN] = STDIN_FILENO;
  wasi_data->wasm_fd_to_native[WASM_STDOUT] = STDOUT_FILENO;
  wasi_data->wasm_fd_to_native[WASM_STDERR] = STDERR_FILENO;
#else
  wasi_data->wasm_fd_to_native[WASM_STDIN] = _fileno(stdin);
  wasi_data->wasm_fd_to_native[WASM_STDOUT] = _fileno(stdout);
  wasi_data->wasm_fd_to_native[WASM_STDERR] = _fileno(stderr);
#endif
  wasi_data->next_wasm_fd = 3;
}

static u32 get_or_allocate_wasm_fd(wasm_sandbox_wasi_data* wasi_data, int nfd) {
  // If the native fd is already mapped, return the same wasm fd for it.
  for (uint32_t i = 0; i < wasi_data->next_wasm_fd; i++) {
    if (wasi_data->wasm_fd_to_native[i] == nfd) {
      return i;
    }
  }
  u32 fd = wasi_data->next_wasm_fd;
  if (fd >= WASM2C_WASI_MAX_FDS) {
    abort_with_message("ran out of fds");
  }
  wasi_data->wasm_fd_to_native[fd] = nfd;
  wasi_data->next_wasm_fd++;
  return fd;
}


// static void set_wasm_fd(wasm_sandbox_wasi_data* wasi_data, int nfrom, int to) {
//   if (to >= WASM2C_WASI_MAX_FDS) {
//     abort_with_message("ran out of fds");
//   }
//   wasi_data->wasm_fd_to_native[to] = nfrom;
// }

// static int get_native_fd(wasm_sandbox_wasi_data* wasi_data, u32 fd) {
//   if (fd >= WASM2C_WASI_MAX_FDS || fd >= wasi_data->next_wasm_fd) {
//     return -1;
//   }
//   return wasi_data->wasm_fd_to_native[fd];
// }

u32 Z_envZ___sys_accessZ_iii(wasm_sandbox_wasi_data* wasi_data,
                             u32 pathname,
                             u32 mode) {
  VERBOSE_LOG("  access: %s 0x%x\n",
              MEMACCESS(wasi_data->heap_memory, pathname), mode);

  // only permit access checks on the the null file
  if (!is_null_file(wasi_data, pathname)) {
    return -1;
  }

  const char* null_file = get_null_file_path();
  const int null_file_mode = get_null_file_mode();

  int result = POSIX_PREFIX(access)(null_file, null_file_mode);
  if (result < 0) {
    VERBOSE_LOG("    access error: %d %s\n", errno, strerror(errno));
    return EM_EACCES;
  }
  return 0;
}

u32 Z_envZ___sys_openZ_iiii(wasm_sandbox_wasi_data* wasi_data,
                            u32 path,
                            u32 flags,
                            u32 varargs) {
  VERBOSE_LOG("  open: %s %d\n", MEMACCESS(wasi_data->heap_memory, path),
              flags);

  // only permit opening the null file
  if (!is_null_file(wasi_data, path)) {
    return -1;
  }

  const char* null_file = get_null_file_path();
  const int null_file_flags = get_null_file_flags();
  const int null_file_mode = get_null_file_mode();

  // Specify the mode manually to make sure the sandboxed code is not using
  // unusual values
  int nfd = POSIX_PREFIX(open)(null_file, null_file_flags, null_file_mode);
  VERBOSE_LOG("    => native %d\n", nfd);
  if (nfd >= 0) {
    u32 fd = get_or_allocate_wasm_fd(wasi_data, nfd);
    VERBOSE_LOG("      => wasm %d\n", fd);
    return fd;
  }
  return -1;
}

// u32 Z_envZ___sys_fstat64Z_iii(wasm_sandbox_wasi_data* wasi_data,
//                               u32 fd,
//                               u32 buf) {
//   int nfd = get_native_fd(wasi_data, fd);
//   VERBOSE_LOG("  fstat64 %d (=> %d) %d\n", fd, nfd, buf);
//   if (nfd < 0) {
//     return EM_EACCES;
//   }
//   return do_stat(wasi_data, nfd, buf);
// }

// u32 Z_envZ___sys_stat64Z_iii(wasm_sandbox_wasi_data* wasi_data,
//                              u32 path,
//                              u32 buf) {
//   VERBOSE_LOG("  stat64: %s\n", MEMACCESS(wasi_data->heap_memory, path));
//   int fd = Z_envZ___sys_openZ_iiii(wasi_data, path, 0 /* read_only */, 0);
//   int nfd = get_native_fd(wasi_data, fd);
//   if (nfd < 0) {
//     VERBOSE_LOG("    error, %d %s\n", errno, strerror(errno));
//     return EM_EACCES;
//   }
//   return do_stat(wasi_data, nfd, buf);
// }

// u32 Z_envZ___sys_readZ_iiii(wasm_sandbox_wasi_data* wasi_data,
//                             u32 fd,
//                             u32 buf,
//                             u32 count) {
//   int nfd = get_native_fd(wasi_data, fd);
//   VERBOSE_LOG("  read %d (=> %d) %d %d\n", fd, nfd, buf, count);
//   if (nfd < 0) {
//     VERBOSE_LOG("    bad fd\n");
//     return EM_EACCES;
//   }

//   void* target_buf = MEMACCESS(wasi_data->heap_memory, buf);
//   UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, buf, count);

//   ssize_t ret = POSIX_PREFIX(read)(nfd, target_buf, count);
//   VERBOSE_LOG("    native read: %ld\n", ret);
//   if (ret < 0) {
//     VERBOSE_LOG("    read error %d %s\n", errno, strerror(errno));
//     return EM_EACCES;
//   }
//   return ret;
// }

STUB_IMPORT_IMPL(u32,
                 Z_envZ_dlopenZ_iii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a, u32 b),
                 1);
STUB_IMPORT_IMPL(u32,
                 Z_envZ_dlcloseZ_ii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a),
                 1);
STUB_IMPORT_IMPL(u32,
                 Z_envZ_dlsymZ_iii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a, u32 b),
                 0);
STUB_IMPORT_IMPL(u32,
                 Z_envZ_dlerrorZ_iv,
                 (wasm_sandbox_wasi_data * wasi_data),
                 0);
STUB_IMPORT_IMPL(u32,
                 Z_envZ_signalZ_iii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a, u32 b),
                 -1);
STUB_IMPORT_IMPL(u32,
                 Z_envZ_systemZ_ii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a),
                 -1);
STUB_IMPORT_IMPL(u32,
                 Z_envZ_utimesZ_iii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a, u32 b),
                 -1);
STUB_IMPORT_IMPL(u32,
                 Z_envZ___sys_rmdirZ_ii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a),
                 EM_EACCES);
STUB_IMPORT_IMPL(u32,
                 Z_envZ___sys_renameZ_iii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a, u32 b),
                 EM_EACCES);
STUB_IMPORT_IMPL(u32,
                 Z_envZ___sys_lstat64Z_iii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a, u32 b),
                 EM_EACCES);
STUB_IMPORT_IMPL(u32,
                 Z_envZ___sys_dup3Z_iiii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a, u32 b, u32 c),
                 EM_EACCES);
STUB_IMPORT_IMPL(u32,
                 Z_envZ___sys_dup2Z_iii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a, u32 b),
                 EM_EACCES);
STUB_IMPORT_IMPL(u32,
                 Z_envZ___sys_getcwdZ_iii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a, u32 b),
                 EM_EACCES);
STUB_IMPORT_IMPL(
    u32,
    Z_envZ___sys_ftruncate64Z_iiiii,
    (wasm_sandbox_wasi_data * wasi_data, u32 a, u32 b, u32 c, u32 d),
    EM_EACCES);
STUB_IMPORT_IMPL(u32,
                 Z_envZ___sys_unlinkZ_ii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 path),
                 EACCES);
STUB_IMPORT_IMPL(u32,
                 Z_envZ_pthread_mutexattr_initZ_ii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a),
                 0);
STUB_IMPORT_IMPL(u32,
                 Z_envZ_pthread_mutexattr_settypeZ_iii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a, u32 b),
                 0);
STUB_IMPORT_IMPL(u32,
                 Z_envZ_pthread_mutexattr_destroyZ_ii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a),
                 0);
STUB_IMPORT_IMPL(
    u32,
    Z_envZ_pthread_createZ_iiiii,
    (wasm_sandbox_wasi_data * wasi_data, u32 a, u32 b, u32 c, u32 d),
    -1);
STUB_IMPORT_IMPL(u32,
                 Z_envZ_pthread_joinZ_iii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a, u32 b),
                 -1);
STUB_IMPORT_IMPL(u32,
                 Z_envZ___cxa_thread_atexitZ_iiii,
                 (wasm_sandbox_wasi_data * wasi_data, u32 a, u32 b, u32 c),
                 -1);

/////////////////////////////////////////// WASI runtime
//////////////////////////////////////////////////

////////////// Supported WASI APIs
// errno_t clock_res_get(void* ctx, clockid_t clock_id, timestamp_t*
// resolution); errno_t clock_time_get(void* ctx, clockid_t clock_id,
// timestamp_t precision, timestamp_t* time);

// errno_t fd_close(void* ctx, fd_t fd);
// errno_t fd_read(void* ctx, fd_t fd, const iovec_t* iovs, size_t iovs_len,
// size_t* nread); errno_t fd_seek(void* ctx, fd_t fd, filedelta_t offset,
// whence_t whence, filesize_t* newoffset); errno_t fd_write(void* ctx, fd_t fd,
// const ciovec_t* iovs, size_t iovs_len, size_t* nwritten);
// --- Currently only the default descriptors of STDIN, STDOUT, STDERR and
// allowed by the runtime.
//     seek and close additionally throw an error when operating on STDIN,
//     STDOUT, STDERR. However, these functions are general if other descriptors
//     are allowed by the runtime in the future.

////////////// Partially supported WASI APIs

// errno_t args_get(void* ctx, char** argv, char* argv_buf);
// errno_t args_sizes_get(void* ctx, size_t* argc, size_t* argv_buf_size);
// --- Reads from wasi_data->main_argv, but the runtime does not provide a way
// for the host app to set wasi_data->main_argv right now

// errno_t proc_exit(void* ctx, exitcode_t rval);
// --- this is a no-op here in this runtime as the focus is on library
// sandboxing

// errno_t environ_get(void* ctx, char** environment, char* environ_buf);
// errno_t environ_sizes_get(void* ctx, size_t* environ_count, size_t*
// environ_buf_size);
// --- the wasm2c module evironment variable is just an empty environment

// errno_t fd_prestat_get(void* ctx, fd_t fd, prestat_t* buf);

////////////// Unsupported WASI APIs
// errno_t fd_advise(void* ctx, fd_t fd, filesize_t offset, filesize_t len,
// advice_t advice); errno_t fd_allocate(void* ctx, fd_t fd, filesize_t offset,
// filesize_t len); 
// errno_t fd_datasync(void* ctx, fd_t fd); errno_t
// fd_fdstat_get(void* ctx, fd_t fd, fdstat_t* buf); errno_t
// fd_fdstat_set_flags(void* ctx, fd_t fd, fdflags_t flags); errno_t
// fd_fdstat_set_rights(void* ctx, fd_t fd, rights_t fs_rights_base, rights_t
// fs_rights_inheriting); 
// errno_t fd_filestat_get(void* ctx, fd_t fd, filestat_t* buf); 
// errno_t fd_filestat_set_size(void* ctx, fd_t fd, filesize_t
// st_size); 
// errno_t fd_filestat_set_times(void* ctx, fd_t fd, timestamp_t
// st_atim, timestamp_t st_mtim, fstflags_t fst_flags); 
// errno_t fd_pread(void*
// ctx, fd_t fd, const iovec_t* iovs, size_t iovs_len, filesize_t offset,
// size_t* nread); 
// errno_t fd_prestat_dir_name(void* ctx, fd_t fd, char* path,
// size_t path_len); 
// errno_t fd_pwrite(void* ctx, fd_t fd, const ciovec_t* iovs,
// size_t iovs_len, filesize_t offset, size_t* nwritten); 
// errno_t fd_readdir(void* ctx, fd_t fd, void* buf, size_t buf_len, dircookie_t cookie,
// size_t* bufused); 
// errno_t fd_renumber(void* ctx, fd_t from, fd_t to); 
// errno_t fd_sync(void* ctx, fd_t fd); 
// errno_t fd_tell(void* ctx, fd_t fd, filesize_t*
// offset); 
// errno_t path_create_directory(void* ctx, fd_t fd, const char* path, size_t path_len); 
// errno_t path_filestat_get(void* ctx, fd_t fd, lookupflags_t flags, const char* path, size_t path_len, filestat_t* buf); 
// errno_t path_filestat_set_times(void* ctx, fd_t fd, lookupflags_t flags, const char*
// path, size_t path_len, timestamp_t st_atim, timestamp_t st_mtim, fstflags_t
// fst_flags); 
// errno_t path_link(void* ctx, fd_t old_fd, lookupflags_t
// old_flags, const char* old_path, size_t old_path_len, fd_t new_fd, const
// char* new_path, size_t new_path_len); 
// errno_t path_open(void* ctx, fd_t
// dirfd, lookupflags_t dirflags, const char* path, size_t path_len, oflags_t
// o_flags, rights_t fs_rights_base, rights_t fs_rights_inheriting, fdflags_t
// fs_flags, fd_t* fd); 
// errno_t path_readlink(void* ctx, fd_t fd, const char*
// path, size_t path_len, char* buf, size_t buf_len, size_t* bufused); 
// errno_t path_remove_directory(void* ctx, fd_t fd, const char* path, size_t path_len);
// errno_t path_rename(void* ctx, fd_t old_fd, const char* old_path, size_t
// old_path_len, fd_t new_fd, const char* new_path, size_t new_path_len);
// errno_t path_symlink(void* ctx, const char* old_path, size_t old_path_len,
// fd_t fd, const char* new_path, size_t new_path_len); 
// errno_t path_unlink_file(void* ctx, fd_t fd, const char* path, size_t path_len);
// errno_t poll_oneoff(void* ctx, const subscription_t* in, event_t* out, size_t
// nsubscriptions, size_t* nevents); 
// errno_t proc_raise(void* ctx, signal_t sig); 
// errno_t random_get(void* ctx, void* buf, size_t buf_len); 
// errno_t sched_yield(t* uvwasi); 
// errno_t sock_recv(void* ctx, fd_t sock, const
// iovec_t* ri_data, size_t ri_data_len, riflags_t ri_flags, size_t* ro_datalen,
// roflags_t* ro_flags); 

// errno_t sock_send(void* ctx, fd_t sock, const ciovec_t* si_data, size_t si_data_len, siflags_t si_flags, size_t* so_datalen); errno_t
// sock_shutdown(void* ctx, fd_t sock, sdflags_t how);

// Bad file descriptor.
#define WASI_BADF_ERROR 8
// Invalid argument
#define WASI_INVAL_ERROR 28
// Operation not permitted.
#define WASI_PERM_ERROR 63
#define WASI_DEFAULT_ERROR WASI_PERM_ERROR

// takes a file descriptor and returns the native version of that fd
// the name of the native fd will be the fd variable name + n (i.e., fd => nfd)
// messy, but whatever
// #define GET_NATIVE_FD_OR_ERR(func, wasi_data, fd) {\
//             int n#fd = get_native_fd(wasi_data, fd);\
//             VERBOSE_LOG("  #func wasm %d => native %d\n", fd, n#fd);\
//             if (n#fd < 0) {\
//                 return WASI_BADF_ERROR;\
//             }\
//            }


// TODO: do we need to map fd?

u32 Z_wasi_snapshot_preview1Z_fd_adviseZ_iijji(wasm_sandbox_wasi_data* wasi_data, u32 fd, u64 offset, u64 len, u32 advice){
  // int nfd = get_native_fd(wasi_data, fd);
  // if (nfd < 0) {
  //   return WASI_BADF_ERROR;
  // }

  int n_advice = advice_to_native(advice);
  int result = posix_fadvise(fd, offset, len, n_advice);
  CHECK_ERRNO();
  return 0;
  //return WASI_DEFAULT_ERROR;
}

u32 Z_wasi_snapshot_preview1Z_fd_allocateZ_iijj(wasm_sandbox_wasi_data* wasi_data, u32 fd, u64 offset, u64 len){
  // int nfd = get_native_fd(wasi_data, fd);
  // if (nfd < 0) {
  //   return WASI_BADF_ERROR;
  // }

  int result = posix_fallocate(fd, offset, len);
  CHECK_ERRNO();
  return 0;
}

u32 Z_wasi_snapshot_preview1Z_fd_datasyncZ_ii(wasm_sandbox_wasi_data* wasi_data, u32 fd){
  // int nfd = get_native_fd(wasi_data, fd);
  // if (nfd < 0) {
  //   return WASI_BADF_ERROR;
  // }

  int result = fdatasync(fd);
  CHECK_ERRNO();
  return 0;
  //return from_syscall_ret(result);
}

u32 Z_wasi_snapshot_preview1Z_fd_fdstat_getZ_iii(wasm_sandbox_wasi_data* wasi_data, u32 fd, u32 out){
  // int nfd = get_native_fd(wasi_data, fd);
  // if (nfd < 0) {
  //   return WASI_BADF_ERROR;
  // }
  struct stat statbuf;
  int result = fstat(fd, &statbuf);
  int filetype = statbuf.st_mode;

  
  int mode_flags = fsync(fd);

  wasm_i32_store16(wasi_data->heap_memory, out, filetype);
  wasm_i32_store16(wasi_data->heap_memory, out + 2, mode_flags); // need to convert
  wasm_i64_store(wasi_data->heap_memory, out + 4, 0);
  wasm_i64_store(wasi_data->heap_memory, out + 12, ULLONG_MAX);

  CHECK_ERRNO();
  return 0;
}

u32 Z_wasi_snapshot_preview1Z_fd_fdstat_set_flagsZ_iii(wasm_sandbox_wasi_data* wasi_data, u32 fd, u32 flags){
  // int nfd = get_native_fd(wasi_data, fd);
  // if (nfd < 0) {
  //   return WASI_BADF_ERROR;
  // }
  int n_flags = fdflags_to_native(flags);
  int result = fcntl(fd, F_SETFL, n_flags);
  CHECK_ERRNO();
  return 0;
  //return from_syscall_ret(result);
}

// noop for now
u32 Z_wasi_snapshot_preview1Z_fd_fdstat_set_rightsZ_iijj(wasm_sandbox_wasi_data* wasi_data, u32 fd, u64 fs_rights_base, u64 fs_rights_inheriting){
    return 0;
}

// TODO: add helper for clock parts
u32 Z_wasi_snapshot_preview1Z_fd_filestat_getZ_iii(wasm_sandbox_wasi_data* wasi_data, u32 fd, u32 out){
  // int nfd = get_native_fd(wasi_data, fd);
  // if (nfd < 0) {
  //   return WASI_BADF_ERROR;
  // }

  struct stat statbuf;
  int result = fstat(fd, &statbuf);
  CHECK_ERRNO();
  wasm_i64_store(wasi_data->heap_memory, out, statbuf.st_dev);
  wasm_i64_store(wasi_data->heap_memory, out + 8, statbuf.st_ino);
  wasm_i64_store(wasi_data->heap_memory, out + 16, filetype_to_native(statbuf.st_mode)); 
  wasm_i64_store(wasi_data->heap_memory, out + 24, statbuf.st_nlink);
  wasm_i64_store(wasi_data->heap_memory, out + 32, statbuf.st_size);
  //wasm_i64_store(wasi_data->heap_memory, out + 40, statbuf.st_atim);
  wasm_i64_store(wasi_data->heap_memory, out + 40, statbuf.st_atim.tv_sec * 1000000 + statbuf.st_atim.tv_nsec);
  wasm_i64_store(wasi_data->heap_memory, out + 48, statbuf.st_mtim.tv_sec * 1000000 + statbuf.st_mtim.tv_nsec);
  wasm_i64_store(wasi_data->heap_memory, out + 56, statbuf.st_ctim.tv_sec * 1000000 + statbuf.st_ctim.tv_nsec);

  //wasm_i64_store(wasi_data->heap_memory, out + 48, statbuf.st_mtim);
  //wasm_i64_store(wasi_data->heap_memory, out + 56, statbuf.st_ctim);

/*
    ctx_ref.write_u64(addr, r.dev);
    ctx_ref.write_u64(addr + 8, r.ino);
    ctx_ref.write_u64(addr + 16, r.filetype.to_wasi() as u64);
    ctx_ref.write_u64(addr + 24, r.nlink);
    ctx_ref.write_u64(addr + 32, r.size);
    ctx_ref.write_u64(addr + 40, r.atim.nsec());
    ctx_ref.write_u64(addr + 48, r.mtim.nsec());
    ctx_ref.write_u64(addr + 56, r.ctim.nsec());
*/
  return 0;
}

u32 Z_wasi_snapshot_preview1Z_fd_filestat_set_sizeZ_iij(wasm_sandbox_wasi_data* wasi_data, u32 fd, u64 st_size){
  // int nfd = get_native_fd(wasi_data, fd);
  // if (nfd < 0) {
  //   return WASI_BADF_ERROR;
  // }

  int result = ftruncate(fd, st_size);
  CHECK_ERRNO();
  return 0;
  //return from_syscall_ret(result);
}

u32 Z_wasi_snapshot_preview1Z_fd_filestat_set_timesZ_iijji(wasm_sandbox_wasi_data* wasi_data, u32 fd, u64 st_atim, u64 st_mtim, u32 fst_flags){
  // int nfd = get_native_fd(wasi_data, fd);
  // if (nfd < 0) {
  //   return WASI_BADF_ERROR;
  // }
  // struct timespec times[2];
  // struct timespec atim;
  // struct timespec mtim;
  

  // // let atim_nsec = if fst_flags.atim() {
  // //       atim.nsec() as i64
  // //   } else {
  // //       if fst_flags.atim_now() {
  // //           libc::UTIME_NOW
  // //       } else {
  // //           libc::UTIME_OMIT
  // //       }
  // //   };
  // //   let atim_spec = libc::timespec {
  // //       tv_sec: 0,
  // //       tv_nsec: atim_nsec,
  // //   };

  // times[0] = atim;
  // times[1] = mtim;
  // futimens(nfd, &times);

  // return 0;
  return WASI_DEFAULT_ERROR;
}

u32 Z_wasi_snapshot_preview1Z_fd_preadZ_iiiiji(wasm_sandbox_wasi_data* wasi_data, u32 fd, u32 iov, u32 iovcnt, u64 offset, u32 pnum){
  // int nfd = get_native_fd(wasi_data, fd);
  // VERBOSE_LOG("  fd_pread wasm %d => native %d\n", fd, nfd);
  // if (nfd < 0) {
  //   return WASI_BADF_ERROR;
  // }

  u32 num = 0;
  for (u32 i = 0; i < iovcnt; i++) {
    u32 ptr = wasm_i32_load(wasi_data->heap_memory, iov + i * 8);
    u32 len = wasm_i32_load(wasi_data->heap_memory, iov + i * 8 + 4);
    VERBOSE_LOG("    chunk %d %d\n", ptr, len);

    UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, ptr, len);

    ssize_t result = pread(fd, MEMACCESS(wasi_data->heap_memory, ptr), len, offset);
    CHECK_ERRNO();
    //POSIX_PREFIX(pread)(nfd, MEMACCESS(wasi_data->heap_memory, ptr), len, offset);
    if (result < 0) {
      VERBOSE_LOG("    error, %d %s\n", errno, strerror(errno));
      return WASI_DEFAULT_ERROR;
    }
    num += result;
    if ((size_t)result != len) {
      break;  // nothing more to read
    }
  }
  VERBOSE_LOG("    success: %d\n", num);
  wasm_i32_store(wasi_data->heap_memory, pnum, num);
  return 0;
}

u32 Z_wasi_snapshot_preview1Z_fd_prestat_dir_nameZ_iiii(wasm_sandbox_wasi_data* wasi_data, u32 fd, u32 path, u32 path_len){
    UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, path, path_len);
    assert(path_len != 0);
    wasm_i32_store8(wasi_data->heap_memory, path + (path_len - 1), '/');
    return 0;
}

u32 Z_wasi_snapshot_preview1Z_fd_pwriteZ_iiiiji(wasm_sandbox_wasi_data* wasi_data, u32 fd, u32 iov, u32 iovcnt, u64 offset, u32 pnum){
  // int nfd = get_native_fd(wasi_data, fd);
  // VERBOSE_LOG("  fd_write wasm %d => native %d\n", fd, nfd);
  // if (nfd < 0) {
  //   return WASI_DEFAULT_ERROR;
  // }

  u32 num = 0;
  for (u32 i = 0; i < iovcnt; i++) {
    u32 ptr = wasm_i32_load(wasi_data->heap_memory, iov + i * 8);
    u32 len = wasm_i32_load(wasi_data->heap_memory, iov + i * 8 + 4);
    VERBOSE_LOG("    chunk %d %d\n", ptr, len);

    UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, ptr, len);

    ssize_t result;
    // Use stdio for stdout/stderr to avoid mixing a low-level write() with
    // other logging code, which can change the order from the expected.
    if (fd == WASM_STDOUT) {
      result = fwrite(MEMACCESS(wasi_data->heap_memory, ptr), 1, len, stdout);
    } else if (fd == WASM_STDERR) {
      result = fwrite(MEMACCESS(wasi_data->heap_memory, ptr), 1, len, stderr);
    } else {
      result =
          pwrite(fd, MEMACCESS(wasi_data->heap_memory, ptr), len, offset);
    }
    CHECK_ERRNO();
    if (result < 0) {
      VERBOSE_LOG("    error, %d %s\n", errno, strerror(errno));
      return WASI_DEFAULT_ERROR;
    }
    if ((size_t)result != len) {
      VERBOSE_LOG("    amount error, %ld %d\n", result, len);
      return WASI_DEFAULT_ERROR;
    }
    num += len;
  }
  VERBOSE_LOG("    success: %d\n", num);
  wasm_i32_store(wasi_data->heap_memory, pnum, num);
  return 0;
}

u32 Z_wasi_snapshot_preview1Z_fd_readdirZ_iiiiji(wasm_sandbox_wasi_data* wasi_data, u32 fd, u32 buf, u32 buf_len, u64 cookie, u32 bufused){
    return WASI_DEFAULT_ERROR;
}

u32 Z_wasi_snapshot_preview1Z_fd_renumberZ_iii(wasm_sandbox_wasi_data* wasi_data, u32 from, u32 to){
  // int nfrom = get_native_fd(wasi_data, from);
  // if (nfrom < 0) {
  //   return WASI_DEFAULT_ERROR;
  // }
  // set_wasm_fd(wasi_data, nfrom, to);
  dup2(from, to);
  CHECK_ERRNO();
  close(from);
  CHECK_ERRNO();
  return 0;
}

u32 Z_wasi_snapshot_preview1Z_fd_syncZ_ii(wasm_sandbox_wasi_data* wasi_data, u32 fd){
  // int nfd = get_native_fd(wasi_data, fd);
  // if (nfd < 0) {
  //   return WASI_BADF_ERROR;
  // }
  int result = fsync(fd);
  CHECK_ERRNO();
  return 0;
  //return result;
}



u32 Z_wasi_snapshot_preview1Z_path_create_directoryZ_iiii(wasm_sandbox_wasi_data* wasi_data, u32 fd, u32 path, u32 path_len){
  // int nfd = get_native_fd(wasi_data, fd);
  // if (nfd < 0) {
  //   return WASI_BADF_ERROR;
  // }
  
  UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, path, path_len);
  wasm_i32_store8(wasi_data->heap_memory, path + (path_len - 1), 0);

  mkdirat(fd, MEMACCESS(wasi_data->heap_memory, path), S_IRUSR | S_IWUSR);
  CHECK_ERRNO();
  return 0;
    //return WASI_DEFAULT_ERROR;
}

u32 Z_wasi_snapshot_preview1Z_path_filestat_getZ_iiiiii(wasm_sandbox_wasi_data* wasi_data, u32 fd, u32 flags, u32 path, u32 path_len, u32 out){
  // int nfd = get_native_fd(wasi_data, fd);
  // if (nfd < 0) {
  //   return WASI_BADF_ERROR;
  // }
  int n_flags = lookupflags_to_native(flags); 

  UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, path, path_len);
  wasm_i32_store8(wasi_data->heap_memory, path + (path_len - 1), 0);

  struct stat statbuf;
  
  fstatat(fd, MEMACCESS(wasi_data->heap_memory, path), &statbuf, n_flags);
  CHECK_ERRNO();

  wasm_i64_store(wasi_data->heap_memory, out, statbuf.st_dev);
  wasm_i64_store(wasi_data->heap_memory, out + 8, statbuf.st_ino);
  wasm_i64_store(wasi_data->heap_memory, out + 16, statbuf.st_mode); // convert
  wasm_i64_store(wasi_data->heap_memory, out + 24, statbuf.st_nlink);
  wasm_i64_store(wasi_data->heap_memory, out + 32, statbuf.st_size);
  wasm_i64_store(wasi_data->heap_memory, out + 40, statbuf.st_atim.tv_sec * 1000000 + statbuf.st_atim.tv_nsec);
  wasm_i64_store(wasi_data->heap_memory, out + 48, statbuf.st_mtim.tv_sec * 1000000 + statbuf.st_mtim.tv_nsec);
  wasm_i64_store(wasi_data->heap_memory, out + 56, statbuf.st_ctim.tv_sec * 1000000 + statbuf.st_ctim.tv_nsec);

  // wasm_i64_store(wasi_data->heap_memory, out + 40, statbuf.st_atim);
  // wasm_i64_store(wasi_data->heap_memory, out + 48, statbuf.st_mtim);
  // wasm_i64_store(wasi_data->heap_memory, out + 56, statbuf.st_ctim);
  return 0;
}

u32 Z_wasi_snapshot_preview1Z_path_filestat_set_timesZ_iiiiijji(wasm_sandbox_wasi_data* wasi_data, u32 fd, u32 flags, u32 path, u32 path_len, u64 st_atim, u64 st_mtim, u32 fst_flags){
    UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, path, path_len);
    wasm_i32_store8(wasi_data->heap_memory, path + (path_len - 1), 0);

    // int utimensat(fd, MEMACCESS(wasi_data->heap_memory, path), const struct timespec times[2], int flags);
    
    CHECK_ERRNO();
    return 0;
    // return WASI_DEFAULT_ERROR;
}

u32 Z_wasi_snapshot_preview1Z_path_linkZ_iiiiiiii(wasm_sandbox_wasi_data* wasi_data, u32 old_fd, u32 old_flags, u32 old_path, u32 old_path_len, u32 new_fd, u32 new_path, u32 new_path_len){
  // int old_nfd = get_native_fd(wasi_data, old_fd);
  // if (old_nfd < 0) {
  //   return WASI_BADF_ERROR;
  // }

  // int new_nfd = get_native_fd(wasi_data, new_fd);
  // if (new_nfd < 0) {
  //   return WASI_BADF_ERROR;
  // }

  int n_flags = lookupflags_to_native(old_flags);

  UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, old_path, old_path_len);
  wasm_i32_store8(wasi_data->heap_memory, old_path + (old_path_len - 1), 0);

  UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, new_path, new_path_len);
  wasm_i32_store8(wasi_data->heap_memory, new_path + (new_path_len - 1), 0);

  linkat(old_fd, MEMACCESS(wasi_data->heap_memory, old_path), new_fd, MEMACCESS(wasi_data->heap_memory, new_path), n_flags);
  CHECK_ERRNO();
  return 0;
}

u32 Z_wasi_snapshot_preview1Z_path_openZ_iiiiiijjii(wasm_sandbox_wasi_data* wasi_data, u32 dirfd, u32 dirflags, u32 path, u32 path_len, u32 o_flags, u64 fs_rights_base, u64 fs_rights_inheriting, u32 fs_flags, u32 out){
    // int ndirfd = get_native_fd(wasi_data, dirfd);
    // if (ndirfd < 0) {
    //   return WASI_BADF_ERROR;
    // }

    UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, path, path_len);
    wasm_i32_store8(wasi_data->heap_memory, path + (path_len - 1), 0);

    int ndirflags = lookupflags_to_native(dirflags);
    int n_oflags = oflags_to_native(o_flags);
    int n_fs_flags = fdflags_to_native(fs_flags);
    int flags = ndirflags | n_oflags | n_fs_flags;

    int r = openat(dirfd, MEMACCESS(wasi_data->heap_memory, path), flags);
    CHECK_ERRNO();
    wasm_i64_store(wasi_data->heap_memory, out, r);
    return 0;
}

u32 Z_wasi_snapshot_preview1Z_path_readlinkZ_iiiiiii(wasm_sandbox_wasi_data* wasi_data, u32 fd, u32 path, u32 path_len, u32 buf, u32 buflen, u32 out){
    // int nfd = get_native_fd(wasi_data, fd);
    // if (nfd < 0) {
    //   return WASI_BADF_ERROR;
    // }
    
    UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, path, path_len);
    wasm_i32_store8(wasi_data->heap_memory, path + (path_len - 1), 0);

    
    u32 r = readlinkat(fd, MEMACCESS(wasi_data->heap_memory, path), MEMACCESS(wasi_data->heap_memory, buf), buflen);
    CHECK_ERRNO();
    wasm_i64_store(wasi_data->heap_memory, out, r);
    return 0;
}

u32 Z_wasi_snapshot_preview1Z_path_remove_directoryZ_iiii(wasm_sandbox_wasi_data* wasi_data, u32 fd, u32 path, u32 path_len){
      // int nfd = get_native_fd(wasi_data, fd);
      // if (nfd < 0) {
      //   return WASI_BADF_ERROR;
      // }
      
      UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, path, path_len);
      wasm_i32_store8(wasi_data->heap_memory, path + (path_len - 1), 0);
      
      unlinkat(fd, MEMACCESS(wasi_data->heap_memory, path), AT_REMOVEDIR);
      CHECK_ERRNO();
      return 0;
}

u32 Z_wasi_snapshot_preview1Z_path_renameZ_iiiiiii(wasm_sandbox_wasi_data* wasi_data, u32 old_fd, u32 old_path, u32 old_path_len, u32 new_fd, u32 new_path, u32 new_path_len){
    // int old_nfd = get_native_fd(wasi_data, old_fd);
    // if (old_nfd < 0) {
    //   return WASI_BADF_ERROR;
    // }

    // int new_nfd = get_native_fd(wasi_data, new_nfd);
    // if (new_nfd < 0) {
    //   return WASI_BADF_ERROR;
    // }

    UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, old_path, old_path_len);
    wasm_i32_store8(wasi_data->heap_memory, old_path + (old_path_len - 1), 0);

    UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, new_path, new_path_len);
    wasm_i32_store8(wasi_data->heap_memory, new_path + (new_path_len - 1), 0);

    renameat(old_fd, MEMACCESS(wasi_data->heap_memory, old_path), new_fd, MEMACCESS(wasi_data->heap_memory, new_path));
    CHECK_ERRNO();
    return 0;
}

u32 Z_wasi_snapshot_preview1Z_path_symlinkZ_iiiiii(wasm_sandbox_wasi_data* wasi_data, u32 old_path, u32 old_path_len, u32 fd, u32 new_path, u32 new_path_len){
    // int nfd = get_native_fd(wasi_data, fd);
    // if (nfd < 0) {
    //   return WASI_BADF_ERROR;
    // }

    UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, old_path, old_path_len);
    wasm_i32_store8(wasi_data->heap_memory, old_path + (old_path_len - 1), 0);

    UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, new_path, new_path_len);
    wasm_i32_store8(wasi_data->heap_memory, new_path + (new_path_len - 1), 0);

    symlinkat(MEMACCESS(wasi_data->heap_memory, old_path), fd, MEMACCESS(wasi_data->heap_memory, new_path));
    CHECK_ERRNO();
    return 0;
}

u32 Z_wasi_snapshot_preview1Z_path_unlink_fileZ_iiii(wasm_sandbox_wasi_data* wasi_data, u32 fd, u32 path, u32 path_len){
      // int nfd = get_native_fd(wasi_data, fd);
      // if (nfd < 0) {
      //   return WASI_BADF_ERROR;
      // }
      
      UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, path, path_len);
      wasm_i32_store8(wasi_data->heap_memory, path + (path_len - 1), 0);
      
      unlinkat(fd, MEMACCESS(wasi_data->heap_memory, path), 0);
      CHECK_ERRNO();
      return 0;
}

u32 Z_wasi_snapshot_preview1Z_poll_oneoffZ_iiiii(wasm_sandbox_wasi_data* wasi_data, u32 in, u32 out, u32 nsubscriptions, u32 nevents){
    return WASI_DEFAULT_ERROR;
}

u32 Z_wasi_snapshot_preview1Z_proc_raiseZ_ii(wasm_sandbox_wasi_data* wasi_data, u32 sig){
    return 0;
}


u32 Z_wasi_snapshot_preview1Z_random_getZ_iii(wasm_sandbox_wasi_data* wasi_data, u32 buf, u32 buf_len){
    UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, buf, buf_len);
    getrandom(MEMACCESS(wasi_data->heap_memory, buf), buf_len , 0);
    CHECK_ERRNO();
    return 0;
}

u32 Z_wasi_snapshot_preview1Z_sched_yieldZ_i(wasm_sandbox_wasi_data* wasi_data){
    return 0;
}


u32 Z_wasi_snapshot_preview1Z_sock_recvZ_iiiiiii(wasm_sandbox_wasi_data* wasi_data, u32 sock, u32 ri_data, u32 ri_data_len, u32 ri_flags, u32 ro_datalen, u32 ro_flags){
    // int nsock = get_native_fd(wasi_data, sock);
    // if (nsock < 0) {
    //   return WASI_BADF_ERROR;
    // }

    int len = recv(sock,  MEMACCESS(wasi_data->heap_memory, ri_data), ri_data_len, 0); // flags = 0 for now
    CHECK_ERRNO();
    wasm_i32_store(wasi_data->heap_memory, ro_datalen, len);
    wasm_i32_store(wasi_data->heap_memory, ro_flags, 0);

    return 0;
}

u32 Z_wasi_snapshot_preview1Z_sock_sendZ_iiiiii(wasm_sandbox_wasi_data* wasi_data, u32 sock, u32 si_data, u32 si_data_len, u32 si_flags, u32 so_datalen){
      // int nsock = get_native_fd(wasi_data, sock);
      // if (nsock < 0) {
      //   return WASI_BADF_ERROR;
      // }

    int len = send(sock,  MEMACCESS(wasi_data->heap_memory, si_data), si_data_len, 0); // flags = 0 for now
    CHECK_ERRNO();
    wasm_i32_store(wasi_data->heap_memory, so_datalen, len);
    return 0;
}

u32 Z_wasi_snapshot_preview1Z_sock_shutdownZ_iii(wasm_sandbox_wasi_data* wasi_data, u32 sock, u32 how){
    // int nsock = get_native_fd(wasi_data, sock);
    // if (nsock < 0) {
    //   return WASI_BADF_ERROR;
    // }
    int nhow = sdflags_to_native(how);
    shutdown(sock, nhow);
    CHECK_ERRNO();
    return 0;
}

u32 Z_wasi_snapshot_preview1Z_socketZ_iiiii(wasm_sandbox_wasi_data* wasi_data, u32 domain, u32 ty, u32 protocol, u32 out){
    // int nsock = get_native_fd(wasi_data, sock);
    // if (nsock < 0) {
    //   return WASI_BADF_ERROR;
    // }

    if (protocol != 0) {
      return EINVAL;
    }
    int ndomain = sock_domain_to_native(domain);
    int nty = sock_type_to_native(ty);

    int sock = socket(ndomain, nty, protocol);
    CHECK_ERRNO();
    wasm_i32_store(wasi_data->heap_memory, out, sock);
    return 0;

    //return WASI_DEFAULT_ERROR;
}


u32 Z_wasi_snapshot_preview1Z_sock_connectZ_iiii(wasm_sandbox_wasi_data* wasi_data, u32 sock, u32 sockaddr, u32 addrlen){
    // int nsock = get_native_fd(wasi_data, sock);
    // if (nsock < 0) {
    //     return WASI_BADF_ERROR;
    // }

    //struct sockaddr addr;
    u16 sin_family = wasm_i32_load16_u(wasi_data->heap_memory, sockaddr);
    u16 sin_port = wasm_i32_load16_u(wasi_data->heap_memory, sockaddr + 2);
    u32 sin_addr_in = wasm_i32_load(wasi_data->heap_memory, sockaddr + 4);

    struct sockaddr_in saddr; 
    saddr.sin_family = sin_family;
    saddr.sin_addr.s_addr = sin_addr_in;
    saddr.sin_port = sin_port;

    connect(sock, (struct sockaddr *)&saddr, addrlen);
    CHECK_ERRNO();

    return 0;
}

/////////////////////////////////////////////////////////////
////////// App environment operations
/////////////////////////////////////////////////////////////

// Original file: Modified emscripten/tools/wasm2c/main.c

u32 Z_wasi_snapshot_preview1Z_args_getZ_iii(wasm_sandbox_wasi_data* wasi_data,
                                            u32 argv,
                                            u32 argv_buf) {
  u32 buf_size = 0;
  for (u32 i = 0; i < wasi_data->main_argc; i++) {
    u32 ptr = argv_buf + buf_size;
    wasm_i32_store(wasi_data->heap_memory, argv + i * 4, ptr);

    char* arg = wasi_data->main_argv[i];
    // need to check length of argv strings before copying
    // deliberately truncate length to u32, else UNCOND_MEMCHECK_SIZE may have
    // overflows
    u32 len = strlen(arg) + 1;
    UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, ptr, len);

    memcpy(MEMACCESS(wasi_data->heap_memory, ptr), arg, len);
    // make sure string is null terminated
    wasm_i32_store8(wasi_data->heap_memory, ptr + (len - 1), 0);
    buf_size += len;
  }
  return 0;
}

u32 Z_wasi_snapshot_preview1Z_args_sizes_getZ_iii(
    wasm_sandbox_wasi_data* wasi_data,
    u32 pargc,
    u32 pargv_buf_size) {
  wasm_i32_store(wasi_data->heap_memory, pargc, wasi_data->main_argc);
  u32 buf_size = 0;
  for (u32 i = 0; i < wasi_data->main_argc; i++) {
    buf_size += strlen(wasi_data->main_argv[i]) + 1;
  }
  wasm_i32_store(wasi_data->heap_memory, pargv_buf_size, buf_size);
  return 0;
}

// Original file: Modified emscripten/tools/wasm2c/os.c

#ifdef WASM2C_WASI_EXIT_HOST_ON_MODULE_EXIT
void Z_wasi_snapshot_preview1Z_proc_exitZ_vi(wasm_sandbox_wasi_data* wasi_data,
                                             u32 x) {
  exit(1);
}
#else
void Z_wasi_snapshot_preview1Z_proc_exitZ_vi(wasm_sandbox_wasi_data* wasi_data,
                                             u32 x) {
  // upstream emscripten implements this as exit(x)
  // This seems like a bad idea as a misbehaving sandbox will cause the app to
  // exit Since this is a library sandboxing runtime, it's fine to do nothing
  // here. Worst case the library continues execution and returns malformed
  // data, which is already a possibility in any sandbox library
  VERBOSE_LOG(
      "wasm2c module called proc_exit: this is a noop in this runtime\n");
}
#endif

u32 Z_wasi_snapshot_preview1Z_environ_sizes_getZ_iii(
    wasm_sandbox_wasi_data* wasi_data,
    u32 pcount,
    u32 pbuf_size) {
  // TODO: Allow the sandbox to have its own env
  wasm_i32_store(wasi_data->heap_memory, pcount, 0);
  wasm_i32_store(wasi_data->heap_memory, pbuf_size, 0);
  return 0;
}

u32 Z_wasi_snapshot_preview1Z_environ_getZ_iii(
    wasm_sandbox_wasi_data* wasi_data,
    u32 __environ,
    u32 environ_buf) {
  // TODO: Allow the sandbox to have its own env
  return 0;
}

/////////////////////////////////////////////////////////////
////////// File operations
/////////////////////////////////////////////////////////////

u32 Z_wasi_snapshot_preview1Z_fd_prestat_getZ_iii(
    wasm_sandbox_wasi_data* wasi_data,
    u32 fd,
    u32 prestat) {
  // int nfd = get_native_fd(wasi_data, fd);
  // VERBOSE_LOG("  fd_prestat_get wasm %d => native %d\n", fd, nfd);
  // if (nfd < 0) {
  //   return WASI_BADF_ERROR;
  // }

  // homedir is always /
  return 1;
}

u32 Z_wasi_snapshot_preview1Z_fd_writeZ_iiiii(wasm_sandbox_wasi_data* wasi_data,
                                              u32 fd,
                                              u32 iov,
                                              u32 iovcnt,
                                              u32 pnum) {
  // int nfd = get_native_fd(wasi_data, fd);
  // VERBOSE_LOG("  fd_write wasm %d => native %d\n", fd, nfd);
  // if (nfd < 0) {
  //   return WASI_BADF_ERROR;
  // }

  u32 num = 0;
  for (u32 i = 0; i < iovcnt; i++) {
    u32 ptr = wasm_i32_load(wasi_data->heap_memory, iov + i * 8);
    u32 len = wasm_i32_load(wasi_data->heap_memory, iov + i * 8 + 4);
    VERBOSE_LOG("    chunk %d %d\n", ptr, len);

    UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, ptr, len);

    ssize_t result;
    // Use stdio for stdout/stderr to avoid mixing a low-level write() with
    // other logging code, which can change the order from the expected.
    if (fd == WASM_STDOUT) {
      result = fwrite(MEMACCESS(wasi_data->heap_memory, ptr), 1, len, stdout);
    } else if (fd == WASM_STDERR) {
      result = fwrite(MEMACCESS(wasi_data->heap_memory, ptr), 1, len, stderr);
    } else {
      result =
          POSIX_PREFIX(write)(fd, MEMACCESS(wasi_data->heap_memory, ptr), len);
    }
    CHECK_ERRNO();
    if (result < 0) {
      VERBOSE_LOG("    error, %d %s\n", errno, strerror(errno));
      return WASI_DEFAULT_ERROR;
    }
    if ((size_t)result != len) {
      VERBOSE_LOG("    amount error, %ld %d\n", result, len);
      return WASI_DEFAULT_ERROR;
    }
    num += len;
  }
  VERBOSE_LOG("    success: %d\n", num);
  wasm_i32_store(wasi_data->heap_memory, pnum, num);
  return 0;
}

u32 Z_wasi_snapshot_preview1Z_fd_readZ_iiiii(wasm_sandbox_wasi_data* wasi_data,
                                             u32 fd,
                                             u32 iov,
                                             u32 iovcnt,
                                             u32 pnum) {
  // int nfd = get_native_fd(wasi_data, fd);
  // VERBOSE_LOG("  fd_read wasm %d => native %d\n", fd, nfd);
  // if (nfd < 0) {
  //   return WASI_DEFAULT_ERROR;
  // }

  u32 num = 0;
  for (u32 i = 0; i < iovcnt; i++) {
    u32 ptr = wasm_i32_load(wasi_data->heap_memory, iov + i * 8);
    u32 len = wasm_i32_load(wasi_data->heap_memory, iov + i * 8 + 4);
    VERBOSE_LOG("    chunk %d %d\n", ptr, len);

    UNCOND_MEMCHECK_SIZE(wasi_data->heap_memory, ptr, len);

    ssize_t result =
        POSIX_PREFIX(read)(fd, MEMACCESS(wasi_data->heap_memory, ptr), len);
    CHECK_ERRNO();
    if (result < 0) {
      VERBOSE_LOG("    error, %d %s\n", errno, strerror(errno));
      return WASI_DEFAULT_ERROR;
    }
    num += result;
    if ((size_t)result != len) {
      break;  // nothing more to read
    }
  }
  VERBOSE_LOG("    success: %d\n", num);
  wasm_i32_store(wasi_data->heap_memory, pnum, num);
  return 0;
}

u32 Z_wasi_snapshot_preview1Z_fd_closeZ_ii(wasm_sandbox_wasi_data* wasi_data,
                                           u32 fd) {
  // int nfd = get_native_fd(wasi_data, fd);
  // VERBOSE_LOG("  close wasm %d => native %d\n", fd, nfd);
  // if (nfd < 0) {
  //   return WASI_DEFAULT_ERROR;
  // }
  // For additional safety don't allow seeking on the input, output and error
  // streams
  if (fd == STDOUT_FILENO || fd == STDERR_FILENO || fd == STDIN_FILENO) {
    return WASI_DEFAULT_ERROR;
  }
  POSIX_PREFIX(close)(fd);
  CHECK_ERRNO();
  return 0;
}

static int whence_to_native(u32 whence) {
  if (whence == 0)
    return SEEK_SET;
  if (whence == 1)
    return SEEK_CUR;
  if (whence == 2)
    return SEEK_END;
  return -1;
}

u32 Z_wasi_snapshot_preview1Z_fd_seekZ_iijii(wasm_sandbox_wasi_data* wasi_data,
                                             u32 fd,
                                             u64 offset,
                                             u32 whence,
                                             u32 new_offset) {
  // int nfd = get_native_fd(wasi_data, fd);
  int nwhence = whence_to_native(whence);
  VERBOSE_LOG("  seek %d (=> native %d) %ld %d (=> %d) %d\n", fd, nfd, offset,
              whence, nwhence, new_offset);
  // if (nfd < 0) {
  //   return WASI_DEFAULT_ERROR;
  // }

  // For additional safety don't allow seeking on the input, output and error
  // streams
  if (fd == STDOUT_FILENO || fd == STDERR_FILENO || fd == STDIN_FILENO) {
    return WASI_DEFAULT_ERROR;
  }

  off_t off = POSIX_PREFIX(lseek)(fd, offset, nwhence);
  CHECK_ERRNO();
  VERBOSE_LOG("    off: %ld\n", off);
  if (off == (off_t)-1) {
    VERBOSE_LOG("    error, %d %s\n", errno, strerror(errno));
    return WASI_DEFAULT_ERROR;
  }
  wasm_i64_store(wasi_data->heap_memory, new_offset, off);
  return 0;
}

u32 Z_wasi_snapshot_preview1Z_fd_tellZ_iii(wasm_sandbox_wasi_data* wasi_data, u32 fd, u32 out){
  return Z_wasi_snapshot_preview1Z_fd_seekZ_iijii(wasi_data, fd, 0, 1, out);
  // return wasi_fd_seek(wasi_data, fd, offset, 1);
  // return WASI_DEFAULT_ERROR;
}

// wasm2c includes a version of seek, the u64 offset in two u32 parts. Unclear
// if this is needed, as WASI does not require this, but no harm in including
// it.
u32 Z_wasi_snapshot_preview1Z_fd_seekZ_iiiiii(wasm_sandbox_wasi_data* wasi_data,
                                              u32 a,
                                              u32 b,
                                              u32 c,
                                              u32 d,
                                              u32 e) {
  return Z_wasi_snapshot_preview1Z_fd_seekZ_iijii(wasi_data, a,
                                                  b + (((u64)c) << 32), d, e);
}

/////////////////////////////////////////////////////////////
////////// Clock operations
/////////////////////////////////////////////////////////////

#define WASM_CLOCK_REALTIME 0
#define WASM_CLOCK_MONOTONIC 1
#define WASM_CLOCK_PROCESS_CPUTIME 2
#define WASM_CLOCK_THREAD_CPUTIME_ID 3

static int check_clock(u32 clock_id) {
  return clock_id == WASM_CLOCK_REALTIME || clock_id == WASM_CLOCK_MONOTONIC ||
         clock_id == WASM_CLOCK_PROCESS_CPUTIME ||
         clock_id == WASM_CLOCK_THREAD_CPUTIME_ID;
}

// out is a pointer to a u64 timestamp in nanoseconds
// https://github.com/WebAssembly/WASI/blob/main/phases/snapshot/docs.md#-timestamp-u64
u32 Z_wasi_snapshot_preview1Z_clock_time_getZ_iiji(
    wasm_sandbox_wasi_data* wasi_data,
    u32 clock_id,
    u64 precision,
    u32 out) {
  if (!check_clock(clock_id)) {
    return WASI_INVAL_ERROR;
  }

  struct timespec out_struct;
  int ret = os_clock_gettime(wasi_data->clock_data, clock_id, &out_struct);
  CHECK_ERRNO();
  u64 result =
      ((u64)out_struct.tv_sec) * 1000 * 1000 * 1000 + ((u64)out_struct.tv_nsec);
  wasm_i64_store(wasi_data->heap_memory, out, result);
  return ret;
}

u32 Z_wasi_snapshot_preview1Z_clock_res_getZ_iii(
    wasm_sandbox_wasi_data* wasi_data,
    u32 clock_id,
    u32 out) {
  if (!check_clock(clock_id)) {
    return WASI_INVAL_ERROR;
  }

  struct timespec out_struct;
  int ret = os_clock_getres(wasi_data->clock_data, clock_id, &out_struct);
  CHECK_ERRNO();
  u64 result =
      ((u64)out_struct.tv_sec) * 1000 * 1000 * 1000 + ((u64)out_struct.tv_nsec);
  wasm_i64_store(wasi_data->heap_memory, out, result);
  return ret;
}

/////////////////////////////////////////////////////////////
////////// Misc
/////////////////////////////////////////////////////////////
void wasm_rt_sys_init() {
  os_init();
}

void wasm_rt_init_wasi(wasm_sandbox_wasi_data* wasi_data) {
  init_fds(wasi_data);
  os_clock_init(&(wasi_data->clock_data));
  // Remove unused function warnings
  (void)wasm_i32_load;
  (void)wasm_i64_load;
  (void)wasm_f32_load;
  (void)wasm_f64_load;
  (void)wasm_i32_load8_s;
  (void)wasm_i64_load8_s;
  (void)wasm_i32_load8_u;
  (void)wasm_i64_load8_u;
  (void)wasm_i32_load16_s;
  (void)wasm_i64_load16_s;
  (void)wasm_i32_load16_u;
  (void)wasm_i64_load16_u;
  (void)wasm_i64_load32_s;
  (void)wasm_i64_load32_u;
  (void)wasm_i32_store;
  (void)wasm_i64_store;
  (void)wasm_f32_store;
  (void)wasm_f64_store;
  (void)wasm_i32_store8;
  (void)wasm_i32_store16;
  (void)wasm_i64_store8;
  (void)wasm_i64_store16;
  (void)wasm_i64_store32;
}

void wasm_rt_cleanup_wasi(wasm_sandbox_wasi_data* wasi_data) {
  os_clock_cleanup(&(wasi_data->clock_data));
}
