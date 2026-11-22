#include "window_output.h"
#include "protocol.h"
#include "vulkan_output.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "AnlandWindow", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AnlandWindow", __VA_ARGS__)

enum { ANLAND_RGBA_8888 = 1, RGBA_BYTES = 4, ROW_ALIGN = 64 };

struct window_output {
    pthread_t owner;
    ANativeWindow *window;
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    PFNEGLGETNEXTFRAMEIDANDROIDPROC next_frame_id;
    PFNEGLGETFRAMETIMESTAMPSANDROIDPROC frame_timestamps;
    GLint max_viewport[2];
    struct buf_info info;
    int fd;
    size_t length;
    const void *pixels;
    struct vulkan_output *vulkan;
};

static EGLDisplay get_display(void)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static EGLDisplay shared = EGL_NO_DISPLAY;

    /* Hold one process-lifetime initialization of the default display. Do
     * not eglTerminate on window teardown: other windows and Android HWUI
     * may be using the same display. */
    pthread_mutex_lock(&lock);
    if (shared == EGL_NO_DISPLAY) {
        EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display != EGL_NO_DISPLAY && eglInitialize(display, NULL, NULL))
            shared = display;
        else
            LOGE("EGL display initialization failed: 0x%x", eglGetError());
    }
    EGLDisplay display = shared;
    pthread_mutex_unlock(&lock);
    return display;
}

static EGLConfig choose_config(EGLDisplay display)
{
    const EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLint count;
    if (!eglChooseConfig(display, attrs, NULL, 0, &count) ||
        count <= 0 || (uint64_t)count > SIZE_MAX / sizeof(EGLConfig))
        return NULL;

    EGLConfig config = NULL;
    EGLConfig *configs = malloc((size_t)count * sizeof(*configs));
    if (!configs)
        return NULL;

    /* eglChooseConfig treats component sizes as minima and ignores the
     * native visual ID, so explicitly find the RGBA8888 window visual. */
    if (eglChooseConfig(display, attrs, configs, count, &count)) {
        for (EGLint i = 0; i < count; i++) {
            EGLint visual, bits;
            if (eglGetConfigAttrib(display, configs[i], EGL_NATIVE_VISUAL_ID, &visual) &&
                visual == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM &&
                eglGetConfigAttrib(display, configs[i], EGL_BUFFER_SIZE, &bits) && bits == 32) {
                config = configs[i];
                break;
            }
        }
    }
    free(configs);
    return config;
}

static bool has_extension(const char *extensions, const char *name)
{
    if (!extensions)
        return false;
    size_t length = strlen(name);
    for (const char *p = extensions; (p = strstr(p, name)); p += length) {
        if ((p == extensions || p[-1] == ' ') && (p[length] == ' ' || p[length] == '\0'))
            return true;
    }
    return false;
}

static void enable_timestamps(struct window_output *output)
{
    EGLDisplay display = output->display;
    EGLSurface surface = output->surface;
    if (!has_extension(eglQueryString(display, EGL_EXTENSIONS),
                       "EGL_ANDROID_get_frame_timestamps")) {
        LOGI("DISPLAY_PRESENT timestamps unavailable: EGL_ANDROID_get_frame_timestamps not advertised");
        return;
    }

    PFNEGLGETNEXTFRAMEIDANDROIDPROC next_frame_id =
        (PFNEGLGETNEXTFRAMEIDANDROIDPROC)eglGetProcAddress("eglGetNextFrameIdANDROID");
    PFNEGLGETFRAMETIMESTAMPSANDROIDPROC frame_timestamps =
        (PFNEGLGETFRAMETIMESTAMPSANDROIDPROC)eglGetProcAddress("eglGetFrameTimestampsANDROID");
    PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC supported =
        (PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC)eglGetProcAddress("eglGetFrameTimestampSupportedANDROID");
    if (!next_frame_id || !frame_timestamps || !supported) {
        LOGI("DISPLAY_PRESENT timestamps unavailable: missing EGL extension entry points");
        return;
    }
    if (!supported(display, surface, EGL_DISPLAY_PRESENT_TIME_ANDROID)) {
        LOGI("DISPLAY_PRESENT timestamps unsupported on this EGL window (EGL 0x%x)", eglGetError());
        return;
    }
    if (!eglSurfaceAttrib(display, surface, EGL_TIMESTAMPS_ANDROID, EGL_TRUE)) {
        LOGE("DISPLAY_PRESENT timestamps unavailable: eglSurfaceAttrib failed (EGL 0x%x)", eglGetError());
        return;
    }
    output->next_frame_id = next_frame_id;
    output->frame_timestamps = frame_timestamps;
    LOGI("DISPLAY_PRESENT timestamps available on this EGL window");
}

static bool allocate_slot(struct window_output *output, int width, int height)
{
    const uint64_t stride = ((uint64_t)width * RGBA_BYTES + ROW_ALIGN - 1) & ~(uint64_t)(ROW_ALIGN - 1);
    if (stride > UINT32_MAX || (uint64_t)height > PTRDIFF_MAX / stride) {
        LOGE("staging slot size out of range: %dx%d", width, height);
        return false;
    }
    output->length = (size_t)(stride * (uint64_t)height);

    /* Samsung/vendored kernels often lack the generic AOSP dmabuf heap but do
     * expose legacy ION. Try dmabuf heap first, then fall back to ION's system
     * heap (heap_id_mask 0x1), the same heap Mesa/kbase uses on this device.
     * ABI from device kernel include/uapi/linux/ion.h. */
    int fd = -1;
    int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (heap >= 0) {
        struct dma_heap_allocation_data allocation = {
            .len = output->length,
            .fd_flags = O_RDWR | O_CLOEXEC,
        };
        int ret = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &allocation);
        int err = errno;
        close(heap);
        if (ret == 0)
            fd = (int)allocation.fd;
        else
            LOGE("DMA_HEAP_IOCTL_ALLOC failed: %s", strerror(err));
    } else {
        LOGE("open /dev/dma_heap/system failed: %s (trying /dev/ion)", strerror(errno));
    }

    if (fd < 0) {
        struct ion_allocation_data {
            __u64 len;
            __u32 heap_id_mask;
            __u32 flags;
            __u32 fd;
            __u32 unused;
        };
        #define ION_IOC_MAGIC 'I'
        #define ION_IOC_ALLOC _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data)
        int ion = open("/dev/ion", O_RDONLY | O_CLOEXEC);
        if (ion < 0) {
            LOGE("open /dev/ion failed: %s", strerror(errno));
            return false;
        }
        struct ion_allocation_data alloc = {
            .len = output->length,
            .heap_id_mask = 0x1, /* ION_HEAP_TYPE_SYSTEM */
            .flags = 0,
        };
        int ret = ioctl(ion, ION_IOC_ALLOC, &alloc);
        int err = errno;
        close(ion);
        if (ret < 0) {
            LOGE("ION_IOC_ALLOC failed: %s", strerror(err));
            return false;
        }
        fd = (int)alloc.fd;
        LOGI("staging slot allocated via /dev/ion system heap");
    }
    output->fd = fd;
    output->info = (struct buf_info){
        .stride = (uint32_t)stride,
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .format = ANLAND_RGBA_8888, /* Protocol v5: memory byte order R, G, B, A. */
        .modifier = 0, /* DRM_FORMAT_MOD_LINEAR */
        .offset = 0,
    };
    return true;
}

static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    if (!shader) {
        LOGE("glCreateShader failed: 0x%x", glGetError());
        return 0;
    }
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE)
        return shader;

    char message[512] = {0};
    glGetShaderInfoLog(shader, sizeof(message), NULL, message);
    LOGE("shader compilation failed: %s", message);
    glDeleteShader(shader);
    return 0;
}

static bool gl_ok(const char *operation)
{
    GLenum error = glGetError();
    if (error == GL_NO_ERROR)
        return true;
    LOGE("%s failed: GLES 0x%x", operation, error);
    return false;
}

static bool init_blit(const struct window_output *output)
{
    /* Producer row 0 is the logical screen top. GLES texture row 0 (v=0)
     * therefore belongs at the top of the EGL surface (clip y=+1). */
    const char *vs = "#version 300 es\n"
        "out highp vec2 uv;\n"
        "void main() {\n"
        "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "    uv = vec2(p.x, 1.0 - p.y);\n"
        "}\n";
    const char *fs = "#version 300 es\n"
        "in highp vec2 uv;\n"
        "uniform highp sampler2D src_texture;\n"
        "out highp vec4 color;\n"
        "void main() { color = texture(src_texture, uv); }\n";
    GLuint vert = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, fs);
    GLuint program = (vert && frag) ? glCreateProgram() : 0;
    if (!program) {
        if (vert && frag)
            LOGE("glCreateProgram failed: 0x%x", glGetError());
        return false;
    }
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);
    glDeleteShader(vert);
    glDeleteShader(frag);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char message[512] = {0};
        glGetProgramInfoLog(program, sizeof(message), NULL, message);
        LOGE("blit program link failed: %s", message);
        return false;
    }

    /* The unshared context is private to this renderer and retains this
     * state. Destroying it reclaims the program, texture and vertex array. */
    GLuint texture = 0, vao = 0;
    glUseProgram(program); /* Sampler defaults to unit 0. */
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, output->info.width, output->info.height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(output->info.stride / RGBA_BYTES));
    glDisable(GL_DITHER);
    return gl_ok("blit initialization");
}

void window_output_destroy(struct window_output *output)
{
    if (!output)
        return;
    if (!pthread_equal(output->owner, pthread_self())) {
        LOGE("window_output_destroy called outside the owning render thread");
        return;
    }
    vulkan_output_destroy(output->vulkan);
    EGLDisplay display = output->display;
    if (output->context != EGL_NO_CONTEXT && eglGetCurrentContext() == output->context &&
        !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
        LOGE("EGL context unbind failed: 0x%x", eglGetError());
    if (output->surface != EGL_NO_SURFACE && !eglDestroySurface(display, output->surface))
        LOGE("eglDestroySurface failed: 0x%x", eglGetError());
    if (output->context != EGL_NO_CONTEXT && !eglDestroyContext(display, output->context))
        LOGE("eglDestroyContext failed: 0x%x", eglGetError());
    if (output->pixels != MAP_FAILED)
        munmap((void *)output->pixels, output->length);
    if (output->fd >= 0)
        close(output->fd);
    ANativeWindow_release(output->window);
    free(output);
}

static struct window_output *allocate_output(ANativeWindow *window, int width, int height)
{
    if (!window || width <= 0 || height <= 0)
        return NULL;

    struct window_output *output = calloc(1, sizeof(*output));
    if (!output)
        return NULL;
    output->owner = pthread_self();
    output->fd = -1;
    output->pixels = MAP_FAILED;
    output->window = window;
    ANativeWindow_acquire(window);
    return output;
}

struct window_output *window_output_create_gpu(ANativeWindow *window, int width, int height)
{
    struct window_output *output = allocate_output(window, width, height);
    if (!output)
        return NULL;
    if (allocate_slot(output, width, height))
        output->vulkan = vulkan_output_create(window, output->fd, &output->info);
    if (output->vulkan)
        return output;
    window_output_destroy(output);
    return NULL;
}

struct window_output *window_output_create(ANativeWindow *window, int width, int height)
{
    struct window_output *output = allocate_output(window, width, height);
    if (!output)
        return NULL;

    EGLDisplay display = get_display();
    output->display = display;
    if (display == EGL_NO_DISPLAY)
        goto fail;
    EGLConfig config = choose_config(display);
    if (!config) {
        LOGE("no matching EGL window config for RGBA8888 and GLES3 (EGL 0x%x)", eglGetError());
        goto fail;
    }
    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        LOGE("eglBindAPI failed: 0x%x", eglGetError());
        goto fail;
    }
    output->context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attrs);
    if (output->context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        goto fail;
    }

    /* Keep the window's base geometry; the staging resolution is independent.
     * Match the EGL config's native visual without inspecting its buffers. */
    int ret = ANativeWindow_setBuffersGeometry(window, 0, 0, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    if (ret != 0) {
        LOGE("ANativeWindow_setBuffersGeometry failed: %d", ret);
        goto fail;
    }
    output->surface = eglCreateWindowSurface(display, config, window, NULL);
    if (output->surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
        goto fail;
    }
    if (!eglMakeCurrent(display, output->surface, output->surface, output->context)) {
        LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        goto fail;
    }

    GLint max_texture = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, output->max_viewport);
    if (!gl_ok("GLES limits query"))
        goto fail;
    if (width > max_texture || height > max_texture) {
        LOGE("staging size %dx%d exceeds GL_MAX_TEXTURE_SIZE %d", width, height, max_texture);
        goto fail;
    }
    if (!allocate_slot(output, width, height))
        goto fail;
    output->pixels = mmap(NULL, output->length, PROT_READ, MAP_SHARED, output->fd, 0);
    if (output->pixels == MAP_FAILED) {
        LOGE("staging slot mmap failed: %s", strerror(errno));
        goto fail;
    }
    if (!init_blit(output))
        goto fail;
    enable_timestamps(output);
    return output;

fail:
    window_output_destroy(output);
    return NULL;
}

int window_output_get_buffer(const struct window_output *output, struct buf_info *info)
{
    if (!output || !info)
        return -1;
    *info = output->info;
    return output->fd;
}

static bool sync_read(int fd, uint64_t phase)
{
    struct dma_buf_sync sync = { .flags = phase | DMA_BUF_SYNC_READ };
    int ret;
    do {
        ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (ret < 0 && (errno == EINTR || errno == EAGAIN));
    if (ret < 0)
        LOGE("DMA_BUF_IOCTL_SYNC %s READ failed: %s", phase == DMA_BUF_SYNC_START ? "START" : "END",
             strerror(errno));
    return ret == 0;
}

int window_output_upload(struct window_output *output)
{
    if (!output || output->vulkan || !pthread_equal(output->owner, pthread_self()))
        return -1;

    if (eglGetCurrentContext() != output->context &&
        !eglMakeCurrent(output->display, output->surface, output->surface, output->context)) {
        LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        return -1;
    }

    if (!sync_read(output->fd, DMA_BUF_SYNC_START))
        return -1;
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, output->info.width, output->info.height,
                   GL_RGBA, GL_UNSIGNED_BYTE, output->pixels);
    /* No unpack PBO: GLES has consumed the client pixels on return. The GPU
     * samples an independent texture, so the slot needs no GLES fence. */
    bool synced = sync_read(output->fd, DMA_BUF_SYNC_END);
    return gl_ok("glTexSubImage2D") && synced ? 0 : -1;
}

int window_output_draw_gpu(struct window_output *output, int producer_fd, int *release_fd)
{
    if (output && output->vulkan && pthread_equal(output->owner, pthread_self()))
        return vulkan_output_draw(output->vulkan, producer_fd, release_fd);
    if (producer_fd >= 0)
        close(producer_fd);
    return -1;
}

int window_output_present(struct window_output *output, struct window_frame *frame)
{
    if (!frame)
        return -1;
    *frame = (struct window_frame){0};
    if (!output || !pthread_equal(output->owner, pthread_self()))
        return -1;
    if (output->vulkan)
        return vulkan_output_present(output->vulkan, frame);
    if (eglGetCurrentContext() != output->context)
        return -1;

    EGLDisplay display = output->display;
    EGLSurface surface = output->surface;
    EGLint width, height;
    if (!eglQuerySurface(display, surface, EGL_WIDTH, &width) ||
        !eglQuerySurface(display, surface, EGL_HEIGHT, &height)) {
        LOGE("eglQuerySurface size failed: 0x%x", eglGetError());
        return -1;
    }
    if (width <= 0 || height <= 0 || width > output->max_viewport[0] || height > output->max_viewport[1]) {
        LOGE("EGL window size %dx%d out of viewport range", width, height);
        return -1;
    }

    glViewport(0, 0, width, height);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (!gl_ok("fullscreen blit"))
        return -1;

    EGLuint64KHR id = 0;
    bool tracked = output->next_frame_id && output->next_frame_id(display, surface, &id);
    if (output->next_frame_id && !tracked)
        LOGE("eglGetNextFrameIdANDROID failed: 0x%x", eglGetError());
    if (!eglSwapBuffers(display, surface)) {
        LOGE("eglSwapBuffers failed: 0x%x", eglGetError());
        return -1;
    }
    if (tracked)
        *frame = (struct window_frame){ .id = id, .tracked = true };
    return 0;
}

int window_output_presentation_time(struct window_output *output, uint64_t id, uint64_t *ns)
{
    if (!output || !ns || !pthread_equal(output->owner, pthread_self()))
        return -1;
    if (output->vulkan)
        return vulkan_output_presentation_time(output->vulkan, id, ns);
    if (!output->frame_timestamps)
        return -1;

    const EGLint name = EGL_DISPLAY_PRESENT_TIME_ANDROID;
    EGLnsecsANDROID time = EGL_TIMESTAMP_INVALID_ANDROID;
    if (!output->frame_timestamps(output->display, output->surface, id, 1, &name, &time)) {
        (void)eglGetError(); /* A dropped or evicted frame may no longer be queryable. */
        return -1;
    }
    if (time == EGL_TIMESTAMP_PENDING_ANDROID)
        return 0;
    if (time <= 0)
        return -1;
    *ns = (uint64_t)time;
    return 1;
}
