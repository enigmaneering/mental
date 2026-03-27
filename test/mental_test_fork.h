/*
 * Mental - Portable Cross-Process Test Helper
 *
 * Provides fork-like semantics on all platforms:
 *   Unix:    fork() + mmap(MAP_SHARED|MAP_ANONYMOUS) + waitpid()
 *   Windows: CreateProcess + CreateFileMapping
 *
 * Usage:
 *   // 1. Create shared memory
 *   void *shared;
 *   mental_test_shm shm = mental_test_shm_create("test_name", size, &shared);
 *
 *   // 2. Write setup data to shared (parent UUID, etc)
 *   ...
 *
 *   // 3. Run child
 *   int exit_code = mental_test_run_child(&shm, child_fn, shared, size);
 *
 *   // 4. Check results in shared
 *   ...
 *
 *   // 5. Cleanup
 *   mental_test_shm_destroy(&shm);
 *
 * On Windows, register child functions and call mental_test_child_dispatch()
 * at the start of main().
 */

#ifndef MENTAL_TEST_FORK_H
#define MENTAL_TEST_FORK_H

#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Child function: receives shared memory, returns exit code */
typedef int (*mental_test_child_fn)(void *shared, size_t shared_size);

/* ================================================================== */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct {
    HANDLE hMap;
    void *ptr;
    size_t size;
    char name[128];
} mental_test_shm;

/* Child registry for Windows dispatch */
#define MENTAL_TEST_MAX_CHILDREN 16
typedef struct { const char *name; mental_test_child_fn fn; } mental_test_child_reg;
static mental_test_child_reg g_child_reg[MENTAL_TEST_MAX_CHILDREN];
static int g_child_reg_count = 0;

static void mental_test_register_child(const char *name, mental_test_child_fn fn) {
    if (g_child_reg_count < MENTAL_TEST_MAX_CHILDREN) {
        g_child_reg[g_child_reg_count].name = name;
        g_child_reg[g_child_reg_count].fn = fn;
        g_child_reg_count++;
    }
}

/* Call from main() — if this is a child process, run and exit */
static int mental_test_child_dispatch(int argc, char **argv) {
    /* argv: <exe> --mental-child <fn-name> <shm-name> <shm-size> */
    if (argc < 5 || strcmp(argv[1], "--mental-child") != 0) return 0;

    const char *fn_name = argv[2];
    const char *shm_name = argv[3];
    size_t shm_size = (size_t)atoll(argv[4]);

    mental_test_child_fn fn = NULL;
    for (int i = 0; i < g_child_reg_count; i++) {
        if (strcmp(g_child_reg[i].name, fn_name) == 0) {
            fn = g_child_reg[i].fn;
            break;
        }
    }
    if (!fn) exit(99);

    HANDLE hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm_name);
    if (!hMap) exit(98);
    void *shared = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, shm_size);
    if (!shared) { CloseHandle(hMap); exit(97); }

    int rc = fn(shared, shm_size);
    UnmapViewOfFile(shared);
    CloseHandle(hMap);
    exit(rc);
    return 1;
}

static mental_test_shm mental_test_shm_create(const char *name, size_t size, void **out) {
    mental_test_shm shm = {0};
    shm.size = size;
    snprintf(shm.name, sizeof(shm.name), "mental_test_%s_%lu", name, (unsigned long)GetCurrentProcessId());
    shm.hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)size, shm.name);
    if (shm.hMap) {
        shm.ptr = MapViewOfFile(shm.hMap, FILE_MAP_ALL_ACCESS, 0, 0, size);
        if (shm.ptr) memset(shm.ptr, 0, size);
    }
    *out = shm.ptr;
    return shm;
}

static int mental_test_run_child(mental_test_shm *shm, const char *child_name,
                                  void *shared, size_t shared_size) {
    (void)shared; (void)shared_size;
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "\"%s\" --mental-child %s %s %zu", exe, child_name, shm->name, shm->size);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return -1;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)exit_code;
}

static void mental_test_shm_destroy(mental_test_shm *shm) {
    if (shm->ptr) UnmapViewOfFile(shm->ptr);
    if (shm->hMap) CloseHandle(shm->hMap);
}

#else /* Unix */

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    void *ptr;
    size_t size;
} mental_test_shm;

static int mental_test_child_dispatch(int argc, char **argv) {
    (void)argc; (void)argv;
    return 0;
}

static void mental_test_register_child(const char *name, mental_test_child_fn fn) {
    (void)name; (void)fn;
}

static mental_test_shm mental_test_shm_create(const char *name, size_t size, void **out) {
    (void)name;
    mental_test_shm shm = {0};
    shm.size = size;
    shm.ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm.ptr == MAP_FAILED) shm.ptr = NULL;
    else memset(shm.ptr, 0, size);
    *out = shm.ptr;
    return shm;
}

static int mental_test_run_child(mental_test_shm *shm, const char *child_name,
                                  void *shared, size_t shared_size) {
    (void)shm; (void)child_name;
    pid_t pid = fork();
    if (pid < 0) return -1;
    /* This function doesn't know the child_fn on Unix — caller uses fork inline.
     * This stub exists for API compatibility but shouldn't be called on Unix.
     * Use the MENTAL_TEST_FORK macro instead. */
    (void)shared; (void)shared_size;
    if (pid == 0) _exit(99); /* shouldn't reach here */
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void mental_test_shm_destroy(mental_test_shm *shm) {
    if (shm->ptr) munmap(shm->ptr, shm->size);
}

/*
 * Unix convenience macro: handles fork inline so child code
 * can access local variables.
 * Usage:
 *   MENTAL_TEST_FORK(child_fn, shared, shared_size, exit_code);
 */
#define MENTAL_TEST_FORK(fn, shared, size, exit_code_var) do { \
    pid_t _pid = fork(); \
    if (_pid == 0) { _exit(fn(shared, size)); } \
    int _st; waitpid(_pid, &_st, 0); \
    exit_code_var = WIFEXITED(_st) ? WEXITSTATUS(_st) : -1; \
} while(0)

#endif /* _WIN32 */

#endif /* MENTAL_TEST_FORK_H */
