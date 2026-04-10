/*
 * Mental - HLSL and WGSL to SPIRV compilation
 *
 * Uses external compilers:
 * - HLSL: Microsoft DXC compiler
 * - WGSL: Naga from the wgpu project
 */

#include "transpile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <direct.h>
#include <windows.h>
#define rmdir _rmdir
#define MENTAL_QUOTE ""

/* Windows mkdtemp: _mktemp generates name, _mkdir creates it. */
static char* win_mkdtemp(char* tmpl) {
    if (!_mktemp(tmpl)) return NULL;
    if (_mkdir(tmpl) != 0) return NULL;
    return tmpl;
}
#define mkdtemp(t) win_mkdtemp(t)
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <spawn.h>
#include <errno.h>
#define MENTAL_QUOTE "'"
extern char **environ;
#endif

/* Maximum time (in seconds) to wait for an external tool to complete. */
#define MENTAL_TOOL_TIMEOUT_SECS 33

/*
 * Run a command with a timeout.  Captures stdout+stderr into output_buf.
 * Returns the process exit status, or -1 on timeout/error.
 * On timeout, the child process is killed.
 */
static int run_command_with_timeout(const char* cmd, char* output_buf,
                                    size_t output_buf_size, int timeout_secs) {
    if (output_buf && output_buf_size > 0) output_buf[0] = '\0';

#ifdef _WIN32
    /* Windows: CreateProcess + WaitForSingleObject */
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE read_pipe, write_pipe;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return -1;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {0};
    /* Route through cmd.exe so shell features (2>&1, quoting) work */
    char cmd_copy[4096];
    snprintf(cmd_copy, sizeof(cmd_copy), "cmd.exe /c %s", cmd);

    if (!CreateProcessA(NULL, cmd_copy, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return -1;
    }
    CloseHandle(write_pipe);

    /* Wait with timeout */
    DWORD wait_result = WaitForSingleObject(pi.hProcess, timeout_secs * 1000);
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(read_pipe);
        if (output_buf) snprintf(output_buf, output_buf_size, "Process timed out after %d seconds", timeout_secs);
        return -1;
    }

    /* Read output */
    if (output_buf && output_buf_size > 1) {
        DWORD bytes_read = 0;
        ReadFile(read_pipe, output_buf, (DWORD)(output_buf_size - 1), &bytes_read, NULL);
        output_buf[bytes_read] = '\0';
    }
    CloseHandle(read_pipe);

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;

#else
    /* POSIX: posix_spawn with pipe and waitpid timeout.
     * posix_spawn is lighter than fork() — it doesn't clone the parent's
     * entire virtual memory page table. */
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    /* Set up file actions: redirect stdout+stderr to the write end */
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    /* Spawn: sh -c "command" */
    const char* argv[] = { "/bin/sh", "-c", cmd, NULL };
    pid_t pid;
    int spawn_err = posix_spawn(&pid, "/bin/sh", &actions, NULL,
                                 (char* const*)argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);

    if (spawn_err != 0) {
        close(pipefd[0]);
        return -1;
    }

    /* Read output using select() with the full timeout.
     * select() sleeps efficiently (no CPU usage) until either:
     *   - data arrives on the pipe (child produced output)
     *   - the timeout expires (child is hung)
     * When the child exits, the pipe's write end closes, causing
     * read() to return 0 (EOF), which exits the loop. */
    size_t total_read = 0;
    struct timeval deadline;
    deadline.tv_sec = timeout_secs;
    deadline.tv_usec = 0;

    while (deadline.tv_sec > 0 || deadline.tv_usec > 0) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(pipefd[0], &fds);

        /* select() modifies deadline to reflect time remaining */
        int ready = select(pipefd[0] + 1, &fds, NULL, NULL, &deadline);
        if (ready <= 0) break;  /* timeout or error */

        if (output_buf && total_read < output_buf_size - 1) {
            ssize_t n = read(pipefd[0], output_buf + total_read,
                            output_buf_size - 1 - total_read);
            if (n <= 0) break;  /* EOF — child closed the pipe */
            total_read += n;
        } else {
            /* Buffer full — drain and discard */
            char discard[1024];
            if (read(pipefd[0], discard, sizeof(discard)) <= 0) break;
        }
    }

    if (output_buf) output_buf[total_read] = '\0';
    close(pipefd[0]);

    /* Reap the child — check if it already exited */
    int wstatus;
    pid_t result = waitpid(pid, &wstatus, WNOHANG);
    if (result == pid) {
        /* Child exited normally */
        return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
    }

    /* Child is still running — timeout. Kill it. */
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    if (output_buf) snprintf(output_buf + total_read,
                             output_buf_size - total_read,
                             "\nProcess timed out after %d seconds", timeout_secs);
    return -1;
#endif
}

/*
 * Normalize backslashes to forward slashes in-place.
 * Windows paths with backslashes break in MSYS2/MinGW popen
 * because the shell interprets \x as escape sequences.
 * Windows APIs accept forward slashes just fine.
 */
#ifdef _WIN32
static void normalize_path(char* path) {
    for (; *path; path++) {
        if (*path == '\\') *path = '/';
    }
}
#else
#define normalize_path(p) ((void)0)
#endif

/*
 * Portable temp directory prefix.
 * Windows: uses TEMP/TMP env var (e.g. C:\Users\...\AppData\Local\Temp)
 * Unix: /tmp
 */
static const char* get_tmp_prefix(void) {
#ifdef _WIN32
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
    if (!tmp) tmp = ".";
    return tmp;
#else
    return "/tmp";
#endif
}

/* Configured tool paths — set via mental_set_tool_path(). */
static char g_dxc_path[4096];
static char g_naga_path[4096];
static char g_pocl_path[4096];

void mental_set_tool_path(mental_tool tool, const char* path) {
    char* dest = NULL;
    size_t cap = 0;

    switch (tool) {
    case MENTAL_TOOL_DXC:  dest = g_dxc_path;  cap = sizeof(g_dxc_path);  break;
    case MENTAL_TOOL_NAGA: dest = g_naga_path; cap = sizeof(g_naga_path); break;
    case MENTAL_TOOL_POCL: dest = g_pocl_path; cap = sizeof(g_pocl_path); break;
    default: return;
    }

    if (path) {
        strncpy(dest, path, cap - 1);
        dest[cap - 1] = '\0';
        normalize_path(dest);
    } else {
        dest[0] = '\0';
    }
}

const char* mental_get_tool_path(mental_tool tool) {
    switch (tool) {
    case MENTAL_TOOL_DXC:  return g_dxc_path[0]  ? g_dxc_path  : NULL;
    case MENTAL_TOOL_NAGA: return g_naga_path[0] ? g_naga_path : NULL;
    case MENTAL_TOOL_POCL: return g_pocl_path[0] ? g_pocl_path : NULL;
    default: return NULL;
    }
}

/* Helper to read file into buffer */
static unsigned char* read_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        return NULL;
    }

    unsigned char* data = malloc(size);
    if (!data) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(data, 1, size, f);
    fclose(f);

    if (read != (size_t)size) {
        free(data);
        return NULL;
    }

    *out_len = size;
    return data;
}

unsigned char* mental_hlsl_to_spirv(const char* source, size_t source_len,
                                    size_t* out_len, char* error, size_t error_len) {
    const char* dxc = mental_get_tool_path(MENTAL_TOOL_DXC);
    if (!dxc) {
        if (error) strncpy(error, "DXC compiler not configured (call mental_set_tool_path)", error_len - 1);
        return NULL;
    }

    /* Create temporary directory */
    char tmpdir[1024];
    snprintf(tmpdir, sizeof(tmpdir), "%s/mental_hlsl_XXXXXX", get_tmp_prefix());
    if (!mkdtemp(tmpdir)) {
        if (error) strncpy(error, "Failed to create temporary directory", error_len - 1);
        return NULL;
    }
    normalize_path(tmpdir);

    /* Write HLSL source to temp file */
    char src_path[1024];
    snprintf(src_path, sizeof(src_path), "%s/shader.hlsl", tmpdir);
    FILE* f = fopen(src_path, "wb");
    if (!f) {
        if (error) strncpy(error, "Failed to write HLSL source", error_len - 1);
        rmdir(tmpdir);
        return NULL;
    }
    fwrite(source, 1, source_len, f);
    fclose(f);

    /* Compile HLSL to SPIRV using DXC */
    char cmd[4096];
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/shader.spv", tmpdir);
    snprintf(cmd, sizeof(cmd),
             MENTAL_QUOTE "%s" MENTAL_QUOTE " -spirv -T cs_6_0 -E main -Fo "
             MENTAL_QUOTE "%s" MENTAL_QUOTE " "
             MENTAL_QUOTE "%s" MENTAL_QUOTE " 2>&1",
             dxc, out_path, src_path);

    char output_buf[4096] = {0};
    int status = run_command_with_timeout(cmd, output_buf, sizeof(output_buf),
                                          MENTAL_TOOL_TIMEOUT_SECS);

    if (status != 0) {
        if (error) snprintf(error, error_len, "DXC compilation failed: %s", output_buf);
        remove(src_path);
        rmdir(tmpdir);
        return NULL;
    }

    /* Read compiled SPIRV */
    unsigned char* spirv = read_file(out_path, out_len);

    /* Cleanup */
    remove(src_path);
    remove(out_path);
    rmdir(tmpdir);

    if (!spirv && error) {
        strncpy(error, "Failed to read SPIRV output", error_len - 1);
    }

    return spirv;
}

unsigned char* mental_wgsl_to_spirv(const char* source, size_t source_len,
                                    size_t* out_len, char* error, size_t error_len) {
    const char* naga = mental_get_tool_path(MENTAL_TOOL_NAGA);
    if (!naga) {
        if (error) strncpy(error, "Naga compiler not configured (call mental_set_tool_path)", error_len - 1);
        return NULL;
    }

    /* Create temporary directory */
    char tmpdir[1024];
    snprintf(tmpdir, sizeof(tmpdir), "%s/mental_wgsl_XXXXXX", get_tmp_prefix());
    if (!mkdtemp(tmpdir)) {
        if (error) strncpy(error, "Failed to create temporary directory", error_len - 1);
        return NULL;
    }
    normalize_path(tmpdir);

    /* Write WGSL source to temp file */
    char src_path[1024];
    snprintf(src_path, sizeof(src_path), "%s/shader.wgsl", tmpdir);
    FILE* f = fopen(src_path, "wb");
    if (!f) {
        if (error) strncpy(error, "Failed to write WGSL source", error_len - 1);
        rmdir(tmpdir);
        return NULL;
    }
    fwrite(source, 1, source_len, f);
    fclose(f);

    /* Compile WGSL to SPIRV using Naga */
    char cmd[4096];
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/shader.spv", tmpdir);
    snprintf(cmd, sizeof(cmd),
             MENTAL_QUOTE "%s" MENTAL_QUOTE " "
             MENTAL_QUOTE "%s" MENTAL_QUOTE " "
             MENTAL_QUOTE "%s" MENTAL_QUOTE " 2>&1",
             naga, src_path, out_path);

    char output_buf[4096] = {0};
    int status = run_command_with_timeout(cmd, output_buf, sizeof(output_buf),
                                          MENTAL_TOOL_TIMEOUT_SECS);

    if (status != 0) {
        if (error) snprintf(error, error_len, "Naga compilation failed: %s", output_buf);
        remove(src_path);
        rmdir(tmpdir);
        return NULL;
    }

    /* Read compiled SPIRV */
    unsigned char* spirv = read_file(out_path, out_len);

    /* Cleanup */
    remove(src_path);
    remove(out_path);
    rmdir(tmpdir);

    if (!spirv && error) {
        strncpy(error, "Failed to read SPIRV output", error_len - 1);
    }

    return spirv;
}

/*
 * SPIR-V -> WGSL via Naga.
 *
 * spirv-cross has no WGSL backend, so we use the same Naga tool
 * that handles WGSL -> SPIR-V, just with reversed file extensions.
 * Naga infers direction from .spv (input) and .wgsl (output).
 */
char* mental_spirv_to_wgsl(const unsigned char* spirv, size_t spirv_len,
                            size_t* out_len, char* error, size_t error_len) {
    const char* naga = mental_get_tool_path(MENTAL_TOOL_NAGA);
    if (!naga) {
        if (error) strncpy(error, "Naga compiler not configured (call mental_set_tool_path)", error_len - 1);
        return NULL;
    }

    char tmpdir[1024];
    snprintf(tmpdir, sizeof(tmpdir), "%s/mental_spv2wgsl_XXXXXX", get_tmp_prefix());
    if (!mkdtemp(tmpdir)) {
        if (error) strncpy(error, "Failed to create temporary directory", error_len - 1);
        return NULL;
    }
    normalize_path(tmpdir);

    char src_path[1024];
    snprintf(src_path, sizeof(src_path), "%s/shader.spv", tmpdir);
    FILE* f = fopen(src_path, "wb");
    if (!f) {
        if (error) strncpy(error, "Failed to write SPIR-V input", error_len - 1);
        rmdir(tmpdir);
        return NULL;
    }
    fwrite(spirv, 1, spirv_len, f);
    fclose(f);

    char cmd[4096];
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/shader.wgsl", tmpdir);
    snprintf(cmd, sizeof(cmd),
             MENTAL_QUOTE "%s" MENTAL_QUOTE " "
             MENTAL_QUOTE "%s" MENTAL_QUOTE " "
             MENTAL_QUOTE "%s" MENTAL_QUOTE " 2>&1",
             naga, src_path, out_path);

    char output_buf[4096] = {0};
    int status = run_command_with_timeout(cmd, output_buf, sizeof(output_buf),
                                          MENTAL_TOOL_TIMEOUT_SECS);

    if (status != 0) {
        if (error) snprintf(error, error_len, "Naga SPIR-V to WGSL failed: %s", output_buf);
        remove(src_path);
        rmdir(tmpdir);
        return NULL;
    }

    size_t wgsl_len = 0;
    unsigned char* wgsl_raw = read_file(out_path, &wgsl_len);

    remove(src_path);
    remove(out_path);
    rmdir(tmpdir);

    if (!wgsl_raw) {
        if (error) strncpy(error, "Failed to read WGSL output", error_len - 1);
        return NULL;
    }

    char* result = (char*)realloc(wgsl_raw, wgsl_len + 1);
    if (!result) { free(wgsl_raw); return NULL; }
    result[wgsl_len] = '\0';
    *out_len = wgsl_len;
    return result;
}
