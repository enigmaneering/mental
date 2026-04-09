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
 *
 * All platform libraries (GL, GLX, X11, WGL) are loaded dynamically
 * via dlopen/LoadLibrary so no link-time dependency is required.
 * If the libraries are absent at runtime, init() returns -1 gracefully.
 */

#if defined(_WIN32) || defined(__linux__)

/* ------------------------------------------------------------------ */
/*  Platform-specific dlopen macros                                   */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
#  include <windows.h>
#  define GL_DLOPEN(path)     LoadLibraryA(path)
#  define GL_DLSYM(lib, sym)  GetProcAddress((HMODULE)(lib), sym)
#  define GL_DLCLOSE(lib)     FreeLibrary((HMODULE)(lib))
#  define GL_LIB_NAME         "opengl32.dll"
#else
#  include <dlfcn.h>
#  define GL_DLOPEN(path)     dlopen(path, RTLD_LAZY)
#  define GL_DLSYM(lib, sym)  dlsym(lib, sym)
#  define GL_DLCLOSE(lib)     dlclose(lib)
#  define GL_LIB_NAME_1       "libGL.so.1"
#  define GL_LIB_NAME_2       "libGL.so"
#  define X11_LIB_NAME        "libX11.so"
#  define X11_LIB_NAME_2      "libX11.so.6"
#endif

#include "mental_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  GL types and constants                                            */
/*                                                                    */
/*  We define everything we need ourselves since we no longer include  */
/*  any GL headers (no link-time dependency).                         */
/* ------------------------------------------------------------------ */

/* Base GL types */
typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef void           GLvoid;
typedef int            GLint;
typedef unsigned int   GLuint;
typedef int            GLsizei;
typedef float          GLfloat;
typedef double         GLdouble;
typedef char           GLchar;
typedef ptrdiff_t      GLsizeiptr;
typedef ptrdiff_t      GLintptr;
typedef unsigned char  GLubyte;

/* Constants */
#define GL_COMPUTE_SHADER                 0x91B9
#define GL_SHADER_STORAGE_BUFFER          0x90D2
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_MAJOR_VERSION                  0x821B
#define GL_MINOR_VERSION                  0x821C
#define GL_MAP_READ_BIT                   0x0001
#define GL_MAP_WRITE_BIT                  0x0002
#define GL_DYNAMIC_STORAGE_BIT            0x0100
#define GL_MAP_COHERENT_BIT               0x0080
#define GL_RENDERER                       0x1F01
#define GL_COPY_READ_BUFFER               0x8F36
#define GL_SHADER_STORAGE_BARRIER_BIT     0x00002000
#define GL_DYNAMIC_COPY                   0x88EA
#define GL_TRUE                           1
#define GL_FALSE                          0

#ifndef APIENTRY
#  ifdef _WIN32
#    define APIENTRY __stdcall
#  else
#    define APIENTRY
#  endif
#endif

/* ------------------------------------------------------------------ */
/*  GL function pointer types                                         */
/* ------------------------------------------------------------------ */

/* Base GL functions (from the library itself) */
typedef void        (APIENTRY *pfn_glGetIntegerv)(GLenum, GLint*);
typedef const GLubyte* (APIENTRY *pfn_glGetString)(GLenum);
typedef void        (APIENTRY *pfn_glFinish)(void);

/* Extension / 4.3+ functions (loaded via platform extension loader) */
typedef GLuint      (APIENTRY *pfn_glCreateShader)(GLenum);
typedef void        (APIENTRY *pfn_glShaderSource)(GLuint, GLsizei, const GLchar *const*, const GLint*);
typedef void        (APIENTRY *pfn_glCompileShader)(GLuint);
typedef void        (APIENTRY *pfn_glGetShaderiv)(GLuint, GLenum, GLint*);
typedef void        (APIENTRY *pfn_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef GLuint      (APIENTRY *pfn_glCreateProgram)(void);
typedef void        (APIENTRY *pfn_glAttachShader)(GLuint, GLuint);
typedef void        (APIENTRY *pfn_glLinkProgram)(GLuint);
typedef void        (APIENTRY *pfn_glGetProgramiv)(GLuint, GLenum, GLint*);
typedef void        (APIENTRY *pfn_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void        (APIENTRY *pfn_glUseProgram)(GLuint);
typedef void        (APIENTRY *pfn_glDeleteShader)(GLuint);
typedef void        (APIENTRY *pfn_glDeleteProgram)(GLuint);
typedef void        (APIENTRY *pfn_glDispatchCompute)(GLuint, GLuint, GLuint);
typedef void        (APIENTRY *pfn_glMemoryBarrier)(GLbitfield);
typedef void        (APIENTRY *pfn_glGenBuffers)(GLsizei, GLuint*);
typedef void        (APIENTRY *pfn_glDeleteBuffers)(GLsizei, const GLuint*);
typedef void        (APIENTRY *pfn_glBindBuffer)(GLenum, GLuint);
typedef void        (APIENTRY *pfn_glBindBufferBase)(GLenum, GLuint, GLuint);
typedef void        (APIENTRY *pfn_glBufferData)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void        (APIENTRY *pfn_glBufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*);
typedef void        (APIENTRY *pfn_glGetBufferSubData)(GLenum, GLintptr, GLsizeiptr, void*);
typedef void        (APIENTRY *pfn_glCopyBufferSubData)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr);

/* ------------------------------------------------------------------ */
/*  Platform-specific context function pointer types                  */
/* ------------------------------------------------------------------ */

#ifdef _WIN32

/* wglGetProcAddress — loaded from opengl32.dll */
typedef void* (APIENTRY *pfn_wglGetProcAddress)(const char*);
typedef void* (APIENTRY *pfn_wglCreateContext)(void*);
typedef int   (APIENTRY *pfn_wglMakeCurrent)(void*, void*);
typedef int   (APIENTRY *pfn_wglDeleteContext)(void*);

static pfn_wglGetProcAddress  p_wglGetProcAddress;
static pfn_wglCreateContext   p_wglCreateContext;
static pfn_wglMakeCurrent     p_wglMakeCurrent;
static pfn_wglDeleteContext   p_wglDeleteContext;

#else /* Linux */

/* X11 function pointer types */
typedef struct _XDisplay Display;
typedef unsigned long XID;
typedef XID Window;

typedef Display* (*pfn_XOpenDisplay)(const char*);
typedef int      (*pfn_XCloseDisplay)(Display*);
typedef int      (*pfn_XDefaultScreen)(Display*);

static pfn_XOpenDisplay   p_XOpenDisplay;
static pfn_XCloseDisplay  p_XCloseDisplay;
static pfn_XDefaultScreen p_XDefaultScreen;

/* GLX types — opaque pointers */
typedef struct __GLXcontextRec* GLXContext;
typedef XID GLXPbuffer;
typedef XID GLXDrawable;
typedef struct __GLXFBConfigRec* GLXFBConfig;

/* GLX constants */
#define GLX_DRAWABLE_TYPE     0x8010
#define GLX_PBUFFER_BIT       0x00000004
#define GLX_RENDER_TYPE       0x8011
#define GLX_RGBA_BIT          0x00000001
#define GLX_RED_SIZE          8
#define GLX_PBUFFER_WIDTH     0x8041
#define GLX_PBUFFER_HEIGHT    0x8042
#define GLX_RGBA_TYPE         0x8014
#define None                  0L

/* GLX function pointer types */
typedef GLXFBConfig* (*pfn_glXChooseFBConfig)(Display*, int, const int*, int*);
typedef GLXPbuffer   (*pfn_glXCreatePbuffer)(Display*, GLXFBConfig, const int*);
typedef void         (*pfn_glXDestroyPbuffer)(Display*, GLXPbuffer);
typedef GLXContext   (*pfn_glXCreateNewContext)(Display*, GLXFBConfig, int, GLXContext, int);
typedef void         (*pfn_glXDestroyContext)(Display*, GLXContext);
typedef int          (*pfn_glXMakeContextCurrent)(Display*, GLXDrawable, GLXDrawable, GLXContext);
typedef void         (*pfn_XFree)(void*);
typedef void*        (*pfn_glXGetProcAddressARB)(const GLubyte*);

static pfn_glXChooseFBConfig     p_glXChooseFBConfig;
static pfn_glXCreatePbuffer      p_glXCreatePbuffer;
static pfn_glXDestroyPbuffer     p_glXDestroyPbuffer;
static pfn_glXCreateNewContext   p_glXCreateNewContext;
static pfn_glXDestroyContext     p_glXDestroyContext;
static pfn_glXMakeContextCurrent p_glXMakeContextCurrent;
static pfn_XFree                 p_XFree;
static pfn_glXGetProcAddressARB  p_glXGetProcAddressARB;

#endif /* _WIN32 / Linux */

/* ------------------------------------------------------------------ */
/*  Resolved GL function pointers                                     */
/* ------------------------------------------------------------------ */

/* Base GL functions (from the library directly) */
static pfn_glGetIntegerv     pglGetIntegerv;
static pfn_glGetString       pglGetString;
static pfn_glFinish          pglFinish;

/* Extension / 4.3+ functions */
static pfn_glCreateShader       pglCreateShader;
static pfn_glShaderSource       pglShaderSource;
static pfn_glCompileShader      pglCompileShader;
static pfn_glGetShaderiv        pglGetShaderiv;
static pfn_glGetShaderInfoLog   pglGetShaderInfoLog;
static pfn_glCreateProgram      pglCreateProgram;
static pfn_glAttachShader       pglAttachShader;
static pfn_glLinkProgram        pglLinkProgram;
static pfn_glGetProgramiv       pglGetProgramiv;
static pfn_glGetProgramInfoLog  pglGetProgramInfoLog;
static pfn_glUseProgram         pglUseProgram;
static pfn_glDeleteShader       pglDeleteShader;
static pfn_glDeleteProgram      pglDeleteProgram;
static pfn_glDispatchCompute    pglDispatchCompute;
static pfn_glMemoryBarrier      pglMemoryBarrier;

static pfn_glGenBuffers         pglGenBuffers;
static pfn_glDeleteBuffers      pglDeleteBuffers;
static pfn_glBindBuffer         pglBindBuffer;
static pfn_glBindBufferBase     pglBindBufferBase;
static pfn_glBufferData         pglBufferData;
static pfn_glBufferSubData      pglBufferSubData;
static pfn_glGetBufferSubData   pglGetBufferSubData;
static pfn_glCopyBufferSubData  pglCopyBufferSubData;

/* ------------------------------------------------------------------ */
/*  Library handles                                                   */
/* ------------------------------------------------------------------ */

static void* g_gl_lib  = NULL;   /* opengl32.dll / libGL.so */
#ifndef _WIN32
static void* g_x11_lib = NULL;   /* libX11.so               */
#endif

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
static void*  g_hglrc;   /* HGLRC, but stored as void* to avoid type issues */

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

    g_hglrc = p_wglCreateContext(g_hdc);
    if (!g_hglrc) return -1;
    p_wglMakeCurrent(g_hdc, g_hglrc);
    return 0;
}

static void destroy_hidden_context(void) {
    if (g_hglrc) { p_wglMakeCurrent(NULL, NULL); p_wglDeleteContext(g_hglrc); g_hglrc = NULL; }
    if (g_hdc)   { ReleaseDC(g_hwnd, g_hdc); g_hdc = NULL; }
    if (g_hwnd)  { DestroyWindow(g_hwnd); g_hwnd = NULL; }
}

#else /* Linux / GLX */

static Display*    g_dpy;
static GLXContext  g_glx_ctx;
static GLXPbuffer  g_pbuf;

static int create_hidden_context(void) {
    g_dpy = p_XOpenDisplay(NULL);
    if (!g_dpy) return -1;

    /* Request an fbconfig that supports pbuffers and OpenGL */
    int fb_attribs[] = {
        GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
        GLX_RENDER_TYPE,   GLX_RGBA_BIT,
        GLX_RED_SIZE,      1,
        None
    };
    int nconfigs = 0;
    GLXFBConfig* configs = p_glXChooseFBConfig(g_dpy, p_XDefaultScreen(g_dpy),
                                                fb_attribs, &nconfigs);
    if (!configs || nconfigs == 0) {
        p_XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return -1;
    }

    /* Create 1x1 pbuffer (never displayed) */
    int pb_attribs[] = { GLX_PBUFFER_WIDTH, 1, GLX_PBUFFER_HEIGHT, 1, None };
    g_pbuf = p_glXCreatePbuffer(g_dpy, configs[0], pb_attribs);

    g_glx_ctx = p_glXCreateNewContext(g_dpy, configs[0], GLX_RGBA_TYPE, NULL, 1);
    p_XFree(configs);

    if (!g_glx_ctx) {
        p_glXDestroyPbuffer(g_dpy, g_pbuf);
        p_XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return -1;
    }

    p_glXMakeContextCurrent(g_dpy, g_pbuf, g_pbuf, g_glx_ctx);
    return 0;
}

static void destroy_hidden_context(void) {
    if (g_dpy) {
        p_glXMakeContextCurrent(g_dpy, None, None, NULL);
        if (g_glx_ctx)  { p_glXDestroyContext(g_dpy, g_glx_ctx); g_glx_ctx = NULL; }
        if (g_pbuf)     { p_glXDestroyPbuffer(g_dpy, g_pbuf); g_pbuf = 0; }
        p_XCloseDisplay(g_dpy);
        g_dpy = NULL;
    }
}

#endif /* _WIN32 / Linux */

/* ------------------------------------------------------------------ */
/*  Dynamic library loading                                           */
/* ------------------------------------------------------------------ */

static int load_platform_libraries(void) {
#ifdef _WIN32
    /* Load opengl32.dll */
    g_gl_lib = GL_DLOPEN(GL_LIB_NAME);
    if (!g_gl_lib) return -1;

    /* WGL functions live in opengl32.dll */
    *(void**)(&p_wglGetProcAddress) = (void*)GL_DLSYM(g_gl_lib, "wglGetProcAddress");
    *(void**)(&p_wglCreateContext)  = (void*)GL_DLSYM(g_gl_lib, "wglCreateContext");
    *(void**)(&p_wglMakeCurrent)    = (void*)GL_DLSYM(g_gl_lib, "wglMakeCurrent");
    *(void**)(&p_wglDeleteContext)  = (void*)GL_DLSYM(g_gl_lib, "wglDeleteContext");
    if (!p_wglGetProcAddress || !p_wglCreateContext || !p_wglMakeCurrent || !p_wglDeleteContext)
        return -1;

#else /* Linux */
    /* Load X11 */
    g_x11_lib = GL_DLOPEN(X11_LIB_NAME_2);
    if (!g_x11_lib) g_x11_lib = GL_DLOPEN(X11_LIB_NAME);
    if (!g_x11_lib) return -1;

    *(void**)(&p_XOpenDisplay)   = GL_DLSYM(g_x11_lib, "XOpenDisplay");
    *(void**)(&p_XCloseDisplay)  = GL_DLSYM(g_x11_lib, "XCloseDisplay");
    *(void**)(&p_XDefaultScreen) = GL_DLSYM(g_x11_lib, "XDefaultScreen");
    *(void**)(&p_XFree)          = GL_DLSYM(g_x11_lib, "XFree");
    if (!p_XOpenDisplay || !p_XCloseDisplay || !p_XDefaultScreen || !p_XFree)
        return -1;

    /* Load GL (which includes GLX on Linux) */
    g_gl_lib = GL_DLOPEN(GL_LIB_NAME_1);
    if (!g_gl_lib) g_gl_lib = GL_DLOPEN(GL_LIB_NAME_2);
    if (!g_gl_lib) return -1;

    /* GLX functions are exported from libGL.so */
    *(void**)(&p_glXChooseFBConfig)     = GL_DLSYM(g_gl_lib, "glXChooseFBConfig");
    *(void**)(&p_glXCreatePbuffer)      = GL_DLSYM(g_gl_lib, "glXCreatePbuffer");
    *(void**)(&p_glXDestroyPbuffer)     = GL_DLSYM(g_gl_lib, "glXDestroyPbuffer");
    *(void**)(&p_glXCreateNewContext)   = GL_DLSYM(g_gl_lib, "glXCreateNewContext");
    *(void**)(&p_glXDestroyContext)     = GL_DLSYM(g_gl_lib, "glXDestroyContext");
    *(void**)(&p_glXMakeContextCurrent) = GL_DLSYM(g_gl_lib, "glXMakeContextCurrent");
    *(void**)(&p_glXGetProcAddressARB)  = GL_DLSYM(g_gl_lib, "glXGetProcAddressARB");

    if (!p_glXChooseFBConfig || !p_glXCreatePbuffer || !p_glXDestroyPbuffer ||
        !p_glXCreateNewContext || !p_glXDestroyContext || !p_glXMakeContextCurrent ||
        !p_glXGetProcAddressARB)
        return -1;
#endif

    /* Base GL functions available directly from the library */
    *(void**)(&pglGetIntegerv) = (void*)GL_DLSYM(g_gl_lib, "glGetIntegerv");
    *(void**)(&pglGetString)   = (void*)GL_DLSYM(g_gl_lib, "glGetString");
    *(void**)(&pglFinish)      = (void*)GL_DLSYM(g_gl_lib, "glFinish");
    if (!pglGetIntegerv || !pglGetString || !pglFinish)
        return -1;

    return 0;
}

static void unload_platform_libraries(void) {
    if (g_gl_lib) { GL_DLCLOSE(g_gl_lib); g_gl_lib = NULL; }
#ifndef _WIN32
    if (g_x11_lib) { GL_DLCLOSE(g_x11_lib); g_x11_lib = NULL; }
#endif
}

/* ------------------------------------------------------------------ */
/*  Extension function loader                                         */
/*                                                                    */
/*  Must be called AFTER a context is current.  Uses the platform's   */
/*  extension loader to get 4.3+ compute entry points.                */
/* ------------------------------------------------------------------ */

static void* gl_get_proc(const char* name) {
#ifdef _WIN32
    /* wglGetProcAddress returns NULL for GL 1.1 functions, so fall back
     * to GetProcAddress on the GL library for those. */
    void* ptr = (void*)p_wglGetProcAddress(name);
    if (!ptr) ptr = (void*)GL_DLSYM(g_gl_lib, name);
    return ptr;
#else
    return (void*)p_glXGetProcAddressARB((const GLubyte*)name);
#endif
}

static int load_gl_functions(void) {
#define LOAD(ptr, name) do { \
    *(void**)(&ptr) = gl_get_proc(#name); \
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
    /* Step 1: dlopen the platform libraries */
    if (load_platform_libraries() != 0) {
        unload_platform_libraries();
        return -1;
    }

    /* Step 2: Create a hidden context (needs WGL/GLX functions) */
    if (create_hidden_context() != 0) {
        unload_platform_libraries();
        return -1;
    }

    /* Step 3: Query version — requires a current context */
    pglGetIntegerv(GL_MAJOR_VERSION, &g_gl_major);
    pglGetIntegerv(GL_MINOR_VERSION, &g_gl_minor);

    if (g_gl_major < 4 || (g_gl_major == 4 && g_gl_minor < 3)) {
        /* Below 4.3 — no compute shaders, reject. */
        destroy_hidden_context();
        unload_platform_libraries();
        return -1;
    }

    const char* renderer = (const char*)pglGetString(GL_RENDERER);
    if (renderer) {
        strncpy(g_gl_renderer, renderer, sizeof(g_gl_renderer) - 1);
    } else {
        snprintf(g_gl_renderer, sizeof(g_gl_renderer), "OpenGL %d.%d Device", g_gl_major, g_gl_minor);
    }

    /* Step 4: Load extension functions (needs current context) */
    if (load_gl_functions() != 0) {
        destroy_hidden_context();
        unload_platform_libraries();
        return -1;
    }

    return 0;
}

static void opengl_shutdown(void) {
    destroy_hidden_context();
    unload_platform_libraries();
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

    pglFinish();
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

    pglFinish();
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
    pglFinish();
}

static void opengl_kernel_destroy(void* kernel) {
    if (!kernel) return;
    OpenGLKernel* gl_kernel = (OpenGLKernel*)kernel;
    pglDeleteProgram(gl_kernel->program);
    free(gl_kernel);
}


/* -- Pipe ---------------------------------------------------------- */

typedef struct {
    int dummy; /* OpenGL is immediate-mode; no extra state needed */
} OpenGLPipe;

static void* opengl_pipe_create(void* dev) {
    (void)dev;

    OpenGLPipe* pipe = malloc(sizeof(OpenGLPipe));
    if (!pipe) return NULL;
    pipe->dummy = 0;

    return pipe;
}

static int opengl_pipe_add(void* pipe_ptr, void* kernel, void** inputs,
                            int input_count, void* output, int work_size) {
    (void)pipe_ptr;
    OpenGLKernel* gl_kernel = (OpenGLKernel*)kernel;

    pglUseProgram(gl_kernel->program);

    /* Bind input SSBOs */
    for (int i = 0; i < input_count; i++) {
        if (inputs[i]) {
            OpenGLBuffer* buf = (OpenGLBuffer*)inputs[i];
            pglBindBufferBase(GL_SHADER_STORAGE_BUFFER, (GLuint)i, buf->ssbo);
        }
    }

    /* Bind output SSBO */
    OpenGLBuffer* out_buf = (OpenGLBuffer*)output;
    pglBindBufferBase(GL_SHADER_STORAGE_BUFFER, (GLuint)input_count, out_buf->ssbo);

    /* Dispatch */
    GLuint wg_size = (GLuint)opengl_kernel_workgroup_size(kernel);
    GLuint groups = ((GLuint)work_size + wg_size - 1) / wg_size;
    pglDispatchCompute(groups, 1, 1);

    /* Barrier to ensure writes are visible to subsequent dispatches */
    pglMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    return 0;
}

static int opengl_pipe_execute(void* pipe_ptr) {
    (void)pipe_ptr;
    pglFinish();
    return 0;
}

static void opengl_pipe_destroy(void* pipe_ptr) {
    if (pipe_ptr) free(pipe_ptr);
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
    .pipe_create = opengl_pipe_create,
    .pipe_add = opengl_pipe_add,
    .pipe_execute = opengl_pipe_execute,
    .pipe_destroy = opengl_pipe_destroy,
    .viewport_attach = NULL,
    .viewport_present = NULL,
    .viewport_detach = NULL
};

mental_backend* opengl_backend = &g_opengl_backend;

#else
/* OpenGL compute not supported on this platform (macOS, etc.) */
#include "mental_internal.h"
#include <stddef.h>
mental_backend* opengl_backend = NULL;
#endif /* _WIN32 || __linux__ */
