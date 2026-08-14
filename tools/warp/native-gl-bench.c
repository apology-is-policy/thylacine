// native-gl-bench.c -- the HW-GL exit bar's NATIVE anchor (GPU-DESIGN.md §13).
//
// A surfaceless-EGL + FBO GLES2 benchmark: the ONLY headless path that
// reaches real V3D on a display-less Pi (kmsdrm/X/SDL-offscreen all demand a
// native display; eglinfo proves EGL_PLATFORM_SURFACELESS_MESA works), AND
// the faithful match to the guest's rendering model -- virglrenderer renders
// into an FBO-backed virtio-gpu resource, never a window.
//
// The workload is DRAW-CALL-HEAVY by design: ~DRAWS_PER_FRAME small textured,
// alpha-blended triangle batches per frame, to mirror GLQuake's ~1000
// draws/frame. That is the axis most likely to expose per-submit virt
// overhead (#215), so the native HW:SW ratio measured here is the reference
// the guest ratio is held against.
//
// Renderer selection is by environment, not code: default = V3D; force
// software with GALLIUM_DRIVER=llvmpipe LIBGL_ALWAYS_SOFTWARE=1. The harness
// (native-gl-bench.sh) runs it twice and reports HW, SW, and HW/SW.
//
// Build (on the Pi): cc -O2 native-gl-bench.c -lEGL -lGLESv2 -lm -o native-gl-bench
// Prints one machine-readable line: NGB <renderer> frames <n> secs <s> fps <f> draws_per_s <d>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define FB_W 640
#define FB_H 480
#define DRAWS_PER_FRAME 800
#define RUN_SECS 20.0

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void die(const char *m) { fprintf(stderr, "native-gl-bench: %s\n", m); exit(2); }

static const char *VS =
    "attribute vec2 p;\n"
    "attribute vec2 uv;\n"
    "varying vec2 v_uv;\n"
    "uniform vec2 off;\n"
    "void main(){ v_uv=uv; gl_Position=vec4(p+off,0.0,1.0); }\n";

static const char *FS =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D tex;\n"
    "uniform vec4 tint;\n"
    "void main(){ gl_FragColor = texture2D(tex, v_uv) * tint; }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        die("shader compile failed");
    }
    return s;
}

int main(void) {
    // Surfaceless EGL: the platform eglinfo confirmed. No native display.
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (getPlatformDisplay)
        dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY)
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) die("no EGL display");

    if (!eglInitialize(dpy, NULL, NULL)) die("eglInitialize");
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1)
        die("eglChooseConfig");

    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) die("eglCreateContext");

    // Surfaceless: no draw/read surface -- render only to our own FBO.
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
        die("eglMakeCurrent (surfaceless)");

    const char *renderer = (const char *)glGetString(GL_RENDERER);
    if (!renderer) renderer = "unknown";

    // A 640x480 RGBA FBO -- the guest resource analog.
    GLuint tex_color;
    glGenTextures(1, &tex_color);
    glBindTexture(GL_TEXTURE_2D, tex_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FB_W, FB_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_color, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        die("FBO incomplete");
    glViewport(0, 0, FB_W, FB_H);

    // A small source texture, sampled every draw (Quake lightmapped surfaces).
    unsigned char px[16 * 16 * 4];
    for (int i = 0; i < 16 * 16; i++) {
        px[i * 4 + 0] = (i * 7) & 0xff; px[i * 4 + 1] = (i * 13) & 0xff;
        px[i * 4 + 2] = (i * 29) & 0xff; px[i * 4 + 3] = 0xff;
    }
    GLuint src_tex;
    glGenTextures(1, &src_tex);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
    glBindAttribLocation(prog, 0, "p");
    glBindAttribLocation(prog, 1, "uv");
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) die("link failed");
    glUseProgram(prog);
    GLint u_off = glGetUniformLocation(prog, "off");
    GLint u_tint = glGetUniformLocation(prog, "tint");

    // A single small triangle-pair the draw loop re-tints + re-offsets, so
    // every iteration is a real state-change + draw (the per-draw axis), not
    // one fat batched draw.
    float verts[] = {
        -0.08f, -0.08f, 0.0f, 0.0f,
         0.08f, -0.08f, 1.0f, 0.0f,
         0.08f,  0.08f, 1.0f, 1.0f,
        -0.08f, -0.08f, 0.0f, 0.0f,
         0.08f,  0.08f, 1.0f, 1.0f,
        -0.08f,  0.08f, 0.0f, 1.0f,
    };
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);

    // Warmup: one full frame + finish, so shader/pipe compile is off the clock.
    glClear(GL_COLOR_BUFFER_BIT);
    for (int d = 0; d < DRAWS_PER_FRAME; d++) {
        glUniform2f(u_off, 0.0f, 0.0f);
        glUniform4f(u_tint, 1.0f, 1.0f, 1.0f, 0.5f);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glFinish();

    long frames = 0, draws = 0;
    double t0 = now_s(), t = t0;
    while ((t = now_s()) - t0 < RUN_SECS) {
        glClear(GL_COLOR_BUFFER_BIT);
        for (int d = 0; d < DRAWS_PER_FRAME; d++) {
            float a = (float)d / DRAWS_PER_FRAME;
            glUniform2f(u_off, sinf(a * 6.283f) * 0.8f, cosf(a * 6.283f) * 0.8f);
            glUniform4f(u_tint, a, 1.0f - a, 0.5f, 0.5f);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            draws++;
        }
        glFinish();  // the guest fences every submit; match that serialization
        frames++;
    }
    double secs = now_s() - t0;

    printf("NGB %s frames %ld secs %.2f fps %.2f draws_per_s %.0f\n",
           renderer, frames, secs, frames / secs, draws / secs);
    return 0;
}
