/*
 * Mental - OpenGL Compute Backend (Linux, Windows)
 *
 * Requires OpenGL >= 4.3 for compute shader support.
 * Rejected at init() time if the driver cannot provide 4.3+.
 *
 * On OpenGL 4.6+ we could feed SPIR-V directly via GL_ARB_gl_spirv,
 * but for now we always accept GLSL source (transpiled from SPIR-V
 * by spirv-cross when needed).
 *
 * Not built on macOS — Apple capped OpenGL at 4.1.
 */

#ifdef MENTAL_HAS_OPENGL

/* Must be included before gl.h */
#ifdef _WIN32
#  include <windows.h>
#endif

#include <GL/gl.h>

/*
 * We need OpenGL 4.3+ entry points that are not in the base gl.h.
 * Pull them through the platform's extension mechanism.
 */
#ifdef _WIN32
   /* wglGetProcAddress is declared in wingdi.h (via windows.h) */
#  define MENTAL_GL_GETPROC(name) wglGetProcAddress(name)
#else
#  include <GL/glx.h>
#  define MENTAL_GL_GETPROC(name) glXGetProcAddressARB((const GLubyte*)(name))
#endif

/* glext.h may or may not be present — define the tokens and types we
 * need ourselves if they are missing.  Windows/MinGW gl.h only covers
 * OpenGL 1.1 and lacks all modern types and constants. */

/* Modern GL types missing from Windows/MinGW gl.h */
#ifndef GL_VERSION_1_5
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
#endif
#ifndef GL_VERSION_2_0
typedef char GLchar;
#endif

#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER                 0x91B9
#endif
#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER          0x90D2
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS                 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS                    0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH                0x8B84
#endif
#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION                  0x821B
#endif
#ifndef GL_MINOR_VERSION
#define GL_MINOR_VERSION                  0x821C
#endif
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT                   0x0001
#endif
#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT                  0x0002
#endif
#ifndef GL_DYNAMIC_STORAGE_BIT
#define GL_DYNAMIC_STORAGE_BIT            0x0100
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT               0x0080
#endif
#ifndef GL_RENDERER
#define GL_RENDERER                       0x1F01
#endif
#ifndef GL_COPY_READ_BUFFER
#define GL_COPY_READ_BUFFER               0x8F36
#endif
#ifndef GL_SHADER_STORAGE_BARRIER_BIT
#define GL_SHADER_STORAGE_BARRIER_BIT     0x00002000
#endif
#ifndef GL_DYNAMIC_COPY
#define GL_DYNAMIC_COPY                   0x88EA
#endif

#include "mental_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  GL function pointer types                                         */
/*                                                                    */
/*  On Linux, GL/glext.h (included from GL/gl.h) provides all         */
/*  PFN*PROC typedefs.  On Windows, gl.h is minimal and we may need   */
/*  to define them ourselves.  Guard each with #ifndef.               */
/* ------------------------------------------------------------------ */

#ifndef PFNGLCREATESHADERPROC
typedef GLuint (APIENTRY *PFNGLCREATESHADERPROC)(GLenum);
#endif
#ifndef PFNGLSHADERSOURCEPROC
typedef void   (APIENTRY *PFNGLSHADERSOURCEPROC)(GLuint, GLsizei, const GLchar *const*, const GLint*);
#endif
#ifndef PFNGLCOMPILESHADERPROC
typedef void   (APIENTRY *PFNGLCOMPILESHADERPROC)(GLuint);
#endif
#ifndef PFNGLGETSHADERIVPROC
typedef void   (APIENTRY *PFNGLGETSHADERIVPROC)(GLuint, GLenum, GLint*);
#endif
#ifndef PFNGLGETSHADERINFOLOGPROC
typedef void   (APIENTRY *PFNGLGETSHADERINFOLOGPROC)(GLuint, GLsizei, GLsizei*, GLchar*);
#endif
#ifndef PFNGLCREATEPROGRAMPROC
typedef GLuint (APIENTRY *PFNGLCREATEPROGRAMPROC)(void);
#endif
#ifndef PFNGLATTACHSHADERPROC
typedef void   (APIENTRY *PFNGLATTACHSHADERPROC)(GLuint, GLuint);
#endif
#ifndef PFNGLLINKPROGRAMPROC
typedef void   (APIENTRY *PFNGLLINKPROGRAMPROC)(GLuint);
#endif
#ifndef PFNGLGETPROGRAMIVPROC
typedef void   (APIENTRY *PFNGLGETPROGRAMIVPROC)(GLuint, GLenum, GLint*);
#endif
#ifndef PFNGLGETPROGRAMINFOLOGPROC
typedef void   (APIENTRY *PFNGLGETPROGRAMINFOLOGPROC)(GLuint, GLsizei, GLsizei*, GLchar*);
#endif
#ifndef PFNGLUSEPROGRAMPROC
typedef void   (APIENTRY *PFNGLUSEPROGRAMPROC)(GLuint);
#endif
#ifndef PFNGLDELETESHADERPROC
typedef void   (APIENTRY *PFNGLDELETESHADERPROC)(GLuint);
#endif
#ifndef PFNGLDELETEPROGRAMPROC
typedef void   (APIENTRY *PFNGLDELETEPROGRAMPROC)(GLuint);
#endif
#ifndef PFNGLDISPATCHCOMPUTEPROC
typedef void   (APIENTRY *PFNGLDISPATCHCOMPUTEPROC)(GLuint, GLuint, GLuint);
#endif
#ifndef PFNGLMEMORYBARRIERPROC
typedef void   (APIENTRY *PFNGLMEMORYBARRIERPROC)(GLbitfield);
#endif
#ifndef PFNGLGENBUFFERSPROC
typedef void   (APIENTRY *PFNGLGENBUFFERSPROC)(GLsizei, GLuint*);
#endif
#ifndef PFNGLDELETEBUFFERSPROC
typedef void   (APIENTRY *PFNGLDELETEBUFFERSPROC)(GLsizei, const GLuint*);
#endif
#ifndef PFNGLBINDBUFFERPROC
typedef void   (APIENTRY *PFNGLBINDBUFFERPROC)(GLenum, GLuint);
#endif
#ifndef PFNGLBINDBUFFERBASEPROC
typedef void   (APIENTRY *PFNGLBINDBUFFERBASEPROC)(GLenum, GLuint, GLuint);
#endif
#ifndef PFNGLBUFFERDATAPROC
typedef void   (APIENTRY *PFNGLBUFFERDATAPROC)(GLenum, GLsizeiptr, const void*, GLenum);
#endif
#ifndef PFNGLBUFFERSUBDATAPROC
typedef void   (APIENTRY *PFNGLBUFFERSUBDATAPROC)(GLenum, GLintptr, GLsizeiptr, const void*);
#endif
#ifndef PFNGLGETBUFFERSUBDATAPROC
typedef void   (APIENTRY *PFNGLGETBUFFERSUBDATAPROC)(GLenum, GLintptr, GLsizeiptr, void*);
#endif
#ifndef PFNGLMAPBUFFERRANGEPROC
typedef void*  (APIENTRY *PFNGLMAPBUFFERRANGEPROC)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
#endif
#ifndef PFNGLUNMAPBUFFERPROC
typedef GLboolean (APIENTRY *PFNGLUNMAPBUFFERPROC)(GLenum);
#endif
#ifndef PFNGLCOPYBUFFERSUBDATAPROC
typedef void   (APIENTRY *PFNGLCOPYBUFFERSUBDATAPROC)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr);
#endif

/* ------------------------------------------------------------------ */
/*  Resolved function pointers (populated during init)                */
/* ------------------------------------------------------------------ */

static PFNGLCREATESHADERPROC        pglCreateShader;
static PFNGLSHADERSOURCEPROC        pglShaderSource;
static PFNGLCOMPILESHADERPROC       pglCompileShader;
static PFNGLGETSHADERIVPROC         pglGetShaderiv;
static PFNGLGETSHADERINFOLOGPROC    pglGetShaderInfoLog;
static PFNGLCREATEPROGRAMPROC       pglCreateProgram;
static PFNGLATTACHSHADERPROC        pglAttachShader;
static PFNGLLINKPROGRAMPROC         pglLinkProgram;
static PFNGLGETPROGRAMIVPROC        pglGetProgramiv;
static PFNGLGETPROGRAMINFOLOGPROC   pglGetProgramInfoLog;
static PFNGLUSEPROGRAMPROC          pglUseProgram;
static PFNGLDELETESHADERPROC        pglDeleteShader;
static PFNGLDELETEPROGRAMPROC       pglDeleteProgram;
static PFNGLDISPATCHCOMPUTEPROC     pglDispatchCompute;
static PFNGLMEMORYBARRIERPROC       pglMemoryBarrier;

static PFNGLGENBUFFERSPROC          pglGenBuffers;
static PFNGLDELETEBUFFERSPROC       pglDeleteBuffers;
static PFNGLBINDBUFFERPROC          pglBindBuffer;
static PFNGLBINDBUFFERBASEPROC      pglBindBufferBase;
static PFNGLBUFFERDATAPROC          pglBufferData;
static PFNGLBUFFERSUBDATAPROC       pglBufferSubData;
static PFNGLGETBUFFERSUBDATAPROC    pglGetBufferSubData;
static PFNGLCOPYBUFFERSUBDATAPROC   pglCopyBufferSubData;

/* ------------------------------------------------------------------ */
/*  Hidden context (headless)                                         */
/* ------------------------------------------------------------------ */

/*
 * We need a current GL context before we can query the version or call
 * any GL function.  On Linux we create a minimal pbuffer via GLX; on
 * Windows via WGL with a hidden window.  This keeps the backend usable
 * without a visible window or display server.
 */

#ifdef _WIN32

static HWND   g_hwnd;
static HDC    g_hdc;
static HGLRC  g_hglrc;

static int create_hidden_context(void) {
    /* Register a throwaway window class */
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "MentalGLHidden";
    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(0, "MentalGLHidden", "", 0,
                             0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) return -1;

    g_hdc = GetDC(g_hwnd);
    if (!g_hdc) return -1;

    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;

    int fmt = ChoosePixelFormat(g_hdc, &pfd);
    if (!fmt) return -1;
    SetPixelFormat(g_hdc, fmt, &pfd);

    g_hglrc = wglCreateContext(g_hdc);
    if (!g_hglrc) return -1;
    wglMakeCurrent(g_hdc, g_hglrc);
    return 0;
}

static void destroy_hidden_context(void) {
    if (g_hglrc) { wglMakeCurrent(NULL, NULL); wglDeleteContext(g_hglrc); g_hglrc = NULL; }
    if (g_hdc)   { ReleaseDC(g_hwnd, g_hdc); g_hdc = NULL; }
    if (g_hwnd)  { DestroyWindow(g_hwnd); g_hwnd = NULL; }
}

#else /* Linux / GLX */

#include <X11/Xlib.h>

static Display*    g_dpy;
static GLXContext  g_glx_ctx;
static GLXPbuffer  g_pbuf;

static int create_hidden_context(void) {
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) return -1;

    /* Request an fbconfig that supports pbuffers and OpenGL */
    static int fb_attribs[] = {
        GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        GLX_RED_SIZE,      1,
        None
    };
    int nconfigs = 0;
    GLXFBConfig* configs = glXChooseFBConfig(g_dpy, DefaultScreen(g_dpy),
                                              fb_attribs, &nconfigs);
    if (!configs || nconfigs == 0) {
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return -1;
    }

    /* Create 1x1 pbuffer (never displayed) */
    static int pb_attribs[] = { GLX_PBUFFER_WIDTH, 1, GLX_PBUFFER_HEIGHT, 1, None };
    g_pbuf = glXCreatePbuffer(g_dpy, configs[0], pb_attribs);

    g_glx_ctx = glXCreateNewContext(g_dpy, configs[0], GLX_RGBA_TYPE, NULL, True);
    XFree(configs);

    if (!g_glx_ctx) {
        glXDestroyPbuffer(g_dpy, g_pbuf);
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return -1;
    }

    glXMakeContextCurrent(g_dpy, g_pbuf, g_pbuf, g_glx_ctx);
    return 0;
}

static void destroy_hidden_context(void) {
    if (g_dpy) {
        glXMakeContextCurrent(g_dpy, None, None, NULL);
        if (g_glx_ctx)  { glXDestroyContext(g_dpy, g_glx_ctx); g_glx_ctx = NULL; }
        if (g_pbuf)     { glXDestroyPbuffer(g_dpy, g_pbuf); g_pbuf = 0; }
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
    }
}

#endif /* _WIN32 / Linux */

/* ------------------------------------------------------------------ */
/*  GL function loader                                                */
/* ------------------------------------------------------------------ */

static int load_gl_functions(void) {
#define LOAD(ptr, name) do { \
    *(void**)(&ptr) = (void*)MENTAL_GL_GETPROC(#name); \
    if (!ptr) return -1; \
} while (0)

    LOAD(pglCreateShader,       glCreateShader);
    LOAD(pglShaderSource,       glShaderSource);
    LOAD(pglCompileShader,      glCompileShader);
    LOAD(pglGetShaderiv,        glGetShaderiv);
    LOAD(pglGetShaderInfoLog,   glGetShaderInfoLog);
    LOAD(pglCreateProgram,      glCreateProgram);
    LOAD(pglAttachShader,       glAttachShader);
    LOAD(pglLinkProgram,        glLinkProgram);
    LOAD(pglGetProgramiv,       glGetProgramiv);
    LOAD(pglGetProgramInfoLog,  glGetProgramInfoLog);
    LOAD(pglUseProgram,         glUseProgram);
    LOAD(pglDeleteShader,       glDeleteShader);
    LOAD(pglDeleteProgram,      glDeleteProgram);
    LOAD(pglDispatchCompute,    glDispatchCompute);
    LOAD(pglMemoryBarrier,      glMemoryBarrier);
    LOAD(pglGenBuffers,         glGenBuffers);
    LOAD(pglDeleteBuffers,      glDeleteBuffers);
    LOAD(pglBindBuffer,         glBindBuffer);
    LOAD(pglBindBufferBase,     glBindBufferBase);
    LOAD(pglBufferData,         glBufferData);
    LOAD(pglBufferSubData,      glBufferSubData);
    LOAD(pglGetBufferSubData,   glGetBufferSubData);
    LOAD(pglCopyBufferSubData,  glCopyBufferSubData);

#undef LOAD
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Device state                                                      */
/* ------------------------------------------------------------------ */

static int g_gl_major = 0;
static int g_gl_minor = 0;
static char g_gl_renderer[256] = {0};

/* OpenGL device wrapper — one context per device (we only expose one) */
typedef struct {
    int index;
} OpenGLDevice;

/* OpenGL buffer wrapper */
typedef struct {
    GLuint ssbo;
    size_t size;
} OpenGLBuffer;

/* OpenGL kernel wrapper */
typedef struct {
    GLuint program;
} OpenGLKernel;

/* ------------------------------------------------------------------ */
/*  Backend interface implementation                                  */
/* ------------------------------------------------------------------ */

static int opengl_init(void) {
    if (create_hidden_context() != 0) return -1;

    /* Query version — requires a current context */
    glGetIntegerv(GL_MAJOR_VERSION, &g_gl_major);
    glGetIntegerv(GL_MINOR_VERSION, &g_gl_minor);

    if (g_gl_major < 4 || (g_gl_major == 4 && g_gl_minor < 3)) {
        /* Below 4.3 — no compute shaders, reject. */
        destroy_hidden_context();
        return -1;
    }

    const char* renderer = (const char*)glGetString(GL_RENDERER);
    if (renderer) {
        strncpy(g_gl_renderer, renderer, sizeof(g_gl_renderer) - 1);
    } else {
        snprintf(g_gl_renderer, sizeof(g_gl_renderer), "OpenGL %d.%d Device", g_gl_major, g_gl_minor);
    }

    if (load_gl_functions() != 0) {
        destroy_hidden_context();
        return -1;
    }

    return 0;
}

static void opengl_shutdown(void) {
    destroy_hidden_context();
    g_gl_major = 0;
    g_gl_minor = 0;
    g_gl_renderer[0] = '\0';
}

static int opengl_device_count(void) {
    /* OpenGL exposes a single device per context */
    return 1;
}

static int opengl_device_info(int index, char* name, size_t name_len) {
    if (index != 0) return -1;
    strncpy(name, g_gl_renderer, name_len);
    name[name_len - 1] = '\0';
    return 0;
}

static void* opengl_device_create(int index) {
    if (index != 0) return NULL;
    OpenGLDevice* dev = malloc(sizeof(OpenGLDevice));
    if (!dev) return NULL;
    dev->index = index;
    return dev;
}

static void opengl_device_destroy(void* dev) {
    free(dev);
}

/* ------------------------------------------------------------------ */
/*  Buffer operations (SSBOs)                                         */
/* ------------------------------------------------------------------ */

static void* opengl_buffer_alloc(void* dev, size_t bytes) {
    (void)dev;

    OpenGLBuffer* buf = malloc(sizeof(OpenGLBuffer));
    if (!buf) return NULL;

    pglGenBuffers(1, &buf->ssbo);
    pglBindBuffer(GL_SHADER_STORAGE_BUFFER, buf->ssbo);
    pglBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)bytes, NULL, GL_DYNAMIC_COPY);
    pglBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    buf->size = bytes;
    return buf;
}

static void opengl_buffer_write(void* buf, const void* data, size_t bytes) {
    OpenGLBuffer* gl_buf = (OpenGLBuffer*)buf;
    pglBindBuffer(GL_SHADER_STORAGE_BUFFER, gl_buf->ssbo);
    pglBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)bytes, data);
    pglBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

static void opengl_buffer_read(void* buf, void* data, size_t bytes) {
    OpenGLBuffer* gl_buf = (OpenGLBuffer*)buf;
    pglBindBuffer(GL_SHADER_STORAGE_BUFFER, gl_buf->ssbo);
    pglGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)bytes, data);
    pglBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

static void* opengl_buffer_resize(void* dev, void* old_buf, size_t old_size, size_t new_size) {
    (void)dev;
    OpenGLBuffer* gl_buf = (OpenGLBuffer*)old_buf;

    /* Create new SSBO */
    GLuint new_ssbo;
    pglGenBuffers(1, &new_ssbo);
    pglBindBuffer(GL_SHADER_STORAGE_BUFFER, new_ssbo);
    pglBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)new_size, NULL, GL_DYNAMIC_COPY);

    /* Copy old data */
    size_t copy_size = old_size < new_size ? old_size : new_size;
    pglBindBuffer(GL_COPY_READ_BUFFER, gl_buf->ssbo);
    pglCopyBufferSubData(GL_COPY_READ_BUFFER, GL_SHADER_STORAGE_BUFFER, 0, 0, (GLsizeiptr)copy_size);
    pglBindBuffer(GL_COPY_READ_BUFFER, 0);
    pglBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    /* Swap */
    pglDeleteBuffers(1, &gl_buf->ssbo);
    gl_buf->ssbo = new_ssbo;
    gl_buf->size = new_size;

    glFinish();
    return old_buf;
}

static void* opengl_buffer_clone(void* dev, void* src_buf, size_t size) {
    (void)dev;
    OpenGLBuffer* src = (OpenGLBuffer*)src_buf;

    OpenGLBuffer* clone = malloc(sizeof(OpenGLBuffer));
    if (!clone) return NULL;

    pglGenBuffers(1, &clone->ssbo);
    pglBindBuffer(GL_SHADER_STORAGE_BUFFER, clone->ssbo);
    pglBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)size, NULL, GL_DYNAMIC_COPY);

    pglBindBuffer(GL_COPY_READ_BUFFER, src->ssbo);
    pglCopyBufferSubData(GL_COPY_READ_BUFFER, GL_SHADER_STORAGE_BUFFER, 0, 0, (GLsizeiptr)size);
    pglBindBuffer(GL_COPY_READ_BUFFER, 0);
    pglBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    clone->size = size;

    glFinish();
    return clone;
}

static void opengl_buffer_destroy(void* buf) {
    if (!buf) return;
    OpenGLBuffer* gl_buf = (OpenGLBuffer*)buf;
    pglDeleteBuffers(1, &gl_buf->ssbo);
    free(gl_buf);
}

/* ------------------------------------------------------------------ */
/*  Kernel operations (compute shaders)                               */
/* ------------------------------------------------------------------ */

static void* opengl_kernel_compile(void* dev, const char* source, size_t source_len,
                                    char* error, size_t error_len) {
    (void)dev;

    GLuint shader = pglCreateShader(GL_COMPUTE_SHADER);
    GLint len = (GLint)source_len;
    pglShaderSource(shader, 1, &source, &len);
    pglCompileShader(shader);

    GLint status;
    pglGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        if (error && error_len > 0) {
            pglGetShaderInfoLog(shader, (GLsizei)error_len, NULL, error);
        }
        pglDeleteShader(shader);
        return NULL;
    }

    GLuint program = pglCreateProgram();
    pglAttachShader(program, shader);
    pglLinkProgram(program);
    pglDeleteShader(shader);

    pglGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) {
        if (error && error_len > 0) {
            pglGetProgramInfoLog(program, (GLsizei)error_len, NULL, error);
        }
        pglDeleteProgram(program);
        return NULL;
    }

    OpenGLKernel* kernel = malloc(sizeof(OpenGLKernel));
    if (!kernel) {
        pglDeleteProgram(program);
        return NULL;
    }
    kernel->program = program;
    return kernel;
}

static int opengl_kernel_workgroup_size(void* kernel) {
    (void)kernel;
    /* Default workgroup size for OpenGL compute shaders.  Matches the
     * layout(local_size_x = 256) declaration in transpiled GLSL. */
    return 256;
}

static void opengl_kernel_dispatch(void* kernel, void** inputs, int input_count,
                                    void* output, int work_size) {
    OpenGLKernel* gl_kernel = (OpenGLKernel*)kernel;

    pglUseProgram(gl_kernel->program);

    /* Bind input SSBOs to binding points 0..N-1 */
    for (int i = 0; i < input_count; i++) {
        if (inputs[i]) {
            OpenGLBuffer* buf = (OpenGLBuffer*)inputs[i];
            pglBindBufferBase(GL_SHADER_STORAGE_BUFFER, (GLuint)i, buf->ssbo);
        }
    }

    /* Bind output SSBO to the next binding point */
    OpenGLBuffer* out_buf = (OpenGLBuffer*)output;
    pglBindBufferBase(GL_SHADER_STORAGE_BUFFER, (GLuint)input_count, out_buf->ssbo);

    /* Dispatch — single dimension, workgroup size handled in shader */
    GLuint wg_size = (GLuint)opengl_kernel_workgroup_size(kernel);
    GLuint groups = ((GLuint)work_size + wg_size - 1) / wg_size;
    pglDispatchCompute(groups, 1, 1);

    /* Ensure writes are visible before any subsequent buffer read */
    pglMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    glFinish();
}

static void opengl_kernel_destroy(void* kernel) {
    if (!kernel) return;
    OpenGLKernel* gl_kernel = (OpenGLKernel*)kernel;
    pglDeleteProgram(gl_kernel->program);
    free(gl_kernel);
}


/* ------------------------------------------------------------------ */
/*  Backend descriptor                                                */
/* ------------------------------------------------------------------ */

static mental_backend g_opengl_backend = {
    .name = "OpenGL",
    .api = MENTAL_API_OPENGL,
    .init = opengl_init,
    .shutdown = opengl_shutdown,
    .device_count = opengl_device_count,
    .device_info = opengl_device_info,
    .device_create = opengl_device_create,
    .device_destroy = opengl_device_destroy,
    .buffer_alloc = opengl_buffer_alloc,
    .buffer_write = opengl_buffer_write,
    .buffer_read = opengl_buffer_read,
    .buffer_resize = opengl_buffer_resize,
    .buffer_clone = opengl_buffer_clone,
    .buffer_destroy = opengl_buffer_destroy,
    .kernel_compile = opengl_kernel_compile,
    .kernel_workgroup_size = opengl_kernel_workgroup_size,
    .kernel_dispatch = opengl_kernel_dispatch,
    .kernel_destroy = opengl_kernel_destroy,
    .viewport_attach = NULL,
    .viewport_present = NULL,
    .viewport_detach = NULL
};

mental_backend* opengl_backend = &g_opengl_backend;

#else
/* OpenGL not available at build time */
#include "mental_internal.h"
#include <stddef.h>
mental_backend* opengl_backend = NULL;
#endif /* MENTAL_HAS_OPENGL */
