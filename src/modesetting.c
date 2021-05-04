#define _XOPEN_SOURCE 700
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <alloca.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <event_loop.h>
#include <math.h>

#include <modesetting.h>

#ifdef HAS_FBDEV
#   define FBDEV_FORMAT_FIELD_INITIALIZER(r_length, r_offset, g_length, g_offset, b_length, b_offset, a_length, a_offset) \
        .fbdev_format = { \
            .r = {.length = r_length, .offset = r_offset, .msb_right = 0}, \
            .g = {.length = g_length, .offset = g_offset, .msb_right = 0}, \
            .b = {.length = b_length, .offset = b_offset, .msb_right = 0}, \
            .a = {.length = a_length, .offset = a_offset, .msb_right = 0}, \
        },
#else
#   define FBDEV_FORMAT_FIELD_INITIALIZER(r_length, r_offset, g_length, g_offset, b_length, b_offset, a_length, a_offset)
#endif

#ifdef HAS_GBM
#   define GBM_FORMAT_FIELD_INITIALIZER(_gbm_format) .gbm_format = _gbm_format,
#else
#   define GBM_FORMAT_FIELD_INITIALIZER(_gbm_format)
#endif

#ifdef HAS_KMS
#   define DRM_FORMAT_FIELD_INITIALIZER(_drm_format) .drm_format = _drm_format,
#else
#   define DRM_FORMAT_FIELD_INITIALIZER(_drm_format)
#endif

#define PIXFMT_MAPPING(_format, _bpp, _is_opaque, r_length, r_offset, g_length, g_offset, b_length, b_offset, a_length, a_offset, _gbm_format, _drm_format) \
    { \
        .format = _format, \
        .bpp = _bpp, \
        .is_opaque = _is_opaque, \
        FBDEV_FORMAT_FIELD_INITIALIZER(r_length, r_offset, g_length, g_offset, b_length, b_offset, a_length, a_offset) \
        GBM_FORMAT_FIELD_INITIALIZER(_gbm_format) \
        DRM_FORMAT_FIELD_INITIALIZER(_drm_format) \
    }

extern const struct pixfmt_info pixfmt_infos[] = {
    /**
     * 16-bit (high color)
     */
    PIXFMT_MAPPING(kRGB565, 16, true, 5, 11, 6, 5, 5, 0, 0, 0, GBM_FORMAT_RGB565, DRM_FORMAT_RGB565),
    
    /**
     * 24- or 32-bit (true color)
     */
    PIXFMT_MAPPING(kARGB8888, 32, false, 8, 16, 8, 8, 8, 0, 8, 24, GBM_FORMAT_ARGB8888, DRM_FORMAT_RGB565),
    PIXFMT_MAPPING(kBGRA8888, 32, false, 8, 8, 8, 16, 8, 24, 8, 0, GBM_FORMAT_BGRA8888, DRM_FORMAT_BGRA8888),
    PIXFMT_MAPPING(kRGBA8888, 32, false, 8, 24, 8, 16, 8, 8, 8, 0, GBM_FORMAT_RGBA8888, DRM_FORMAT_RGBA8888)
};

extern const size_t n_pixfmt_infos = sizeof(pixfmt_mappings) / sizeof(*pixfmt_mappings);


struct presenter_private;

struct presenter {
    struct presenter_private *private;
    int (*set_logical_zpos)(struct presenter *presenter, int logical_zpos);
    int (*get_zpos)(struct presenter *presenter);
    int (*set_scanout_callback)(struct presenter *presenter, presenter_scanout_callback_t cb, void *userdata);
#ifdef HAS_KMS
    int (*push_drm_fb_layer)(struct presenter *presenter, const struct drm_fb_layer *layer);
#endif
#ifdef HAS_GBM
    int (*push_gbm_bo_layer)(struct presenter *presenter, const struct gbm_bo_layer *layer);
#endif
    int (*push_sw_fb_layer)(struct presenter *presenter, const struct sw_fb_layer *layer);
    int (*push_placeholder_layer)(struct presenter *presenter, int n_reserved_layers);
    int (*push_display_buffer_layer)(struct presenter *presenter, const struct display_buffer_layer *layer);
    int (*flush)(struct presenter *presenter);
    void (*destroy)(struct presenter *presenter);
    struct display *display;
};


/*****************************************************************************
 *                               PRESENTERS                                  *
 *****************************************************************************/
struct display *presenter_get_display(struct presenter *presenter) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(presenter->display != NULL);
    return presenter->display;
}

/**
 * @brief Add a callback to all the layer for this CRTC are now present on screen.
 */
int presenter_set_scanout_callback(struct presenter *presenter, presenter_scanout_callback_t cb, void *userdata) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(presenter->set_scanout_callback != NULL);
    return presenter->set_scanout_callback(presenter, cb, userdata);
}

/**
 * @brief Sets the zpos (which will be used for any new pushed planes) to the zpos corresponding to @ref logical_zpos.
 * Only supported for KMS presenters.
 * 
 * Actual hardware zpos ranges are driver-specific, sometimes they range from 0 to 127, sometimes 1 to 256, etc.
 * The logical zpos always starts at 0 and ends at (hw_zpos_max - hw_zpos_min) inclusive.
 */
int presenter_set_logical_zpos(struct presenter *presenter, int logical_zpos) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(presenter->set_logical_zpos != NULL);
    return presenter->set_logical_zpos(presenter, logical_zpos);
}

/**
 * @brief Gets the current (actual) zpos. Only supported by KMS presenters, and even then not supported all the time.
 */
int presenter_get_zpos(struct presenter *presenter) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(presenter->set_logical_zpos != NULL);
    return presenter->get_zpos(presenter);
}

#ifdef HAS_KMS
/**
 * @brief Presents a DRM framebuffer, can be used only for KMS.
 */
int presenter_push_drm_fb_layer(
    struct presenter *presenter,
    const struct drm_fb_layer *layer
) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(layer != NULL);
    DEBUG_ASSERT(presenter->push_drm_fb_layer != NULL);
    return presenter->push_drm_fb_layer(presenter, layer);
}
#endif

#ifdef HAS_GBM
/**
 * @brief Presents a GBM buffer object, can be used for both fbdev and KMS.
 */
int presenter_push_gbm_bo_layer(
    struct presenter *presenter,
    const struct gbm_bo_layer *layer
) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(layer != NULL);
    DEBUG_ASSERT(presenter->push_gbm_bo_layer != NULL);
    return presenter->push_gbm_bo_layer(presenter, layer);
}
#endif

/**
 * @brief Presents a software framebuffer (i.e. some malloced memory).
 * 
 * Can only be used for fbdev presenting.
 */
int presenter_push_sw_fb_layer(
    struct presenter *presenter,
    const struct sw_fb_layer *layer
) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(layer != NULL);
    DEBUG_ASSERT(presenter->push_gbm_bo_layer != NULL);
    return presenter->push_sw_fb_layer(presenter, layer);
}

int presenter_push_display_buffer_layer(
    struct presenter *presenter,
    const struct display_buffer_layer *layer
) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(layer != NULL);
    DEBUG_ASSERT(presenter->push_display_buffer_layer);
    return presenter->push_display_buffer_layer(presenter, layer);
}

/**
 * @brief Push a placeholder layer. Increases the zpos by @ref n_reserved_layers for kms presenters.
 * Returns EOVERFLOW if the zpos before incrementing is higher than the maximum supported one by the hardware.
 */
int presenter_push_placeholder_layer(struct presenter *presenter, int n_reserved_layers) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(presenter->push_placeholder_layer != NULL);

    return presenter->push_placeholder_layer(presenter, n_reserved_layers);
}

/**
 * @brief Makes sure all the output operations are applied. This is NOT the "point of no return", that
 * point is way earlier.
 */
int presenter_flush(struct presenter *presenter) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(presenter->flush != NULL);
    return presenter->flush(presenter);
}

/**
 * @brief Destroy a presenter, freeing all the allocated resources.
 */
void presenter_destroy(struct presenter *presenter) {
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(presenter->destroy != NULL);

    return presenter->destroy(presenter);
}


struct display_buffer {
    struct display *display;
    struct display_buffer_backend backend;
    display_buffer_destroy_callback_t destroy_callback;
    void *userdata;
};

struct display_private;

struct display {
    struct display_private *private;

    void (*get_supported_formats)(struct display *display, const uint32_t **formats_out, size_t *n_formats_out);
    int (*make_mapped_buffer)(struct display_buffer *buffer);
    int (*import_sw_buffer)(struct display_buffer *buffer);
    int (*import_gbm_bo)(struct display_buffer *buffer);
    int (*import_gem_bo)(struct display_buffer *buffer);
    int (*import_egl_image)(struct display_buffer *buffer);
    struct presenter *(*create_presenter)(struct display *display);
    void (*destroy)(struct display *display);

    int width, height;
    double refresh_rate;
    bool has_dimensions;
    int width_mm, height_mm;
    bool supports_gbm;
    struct gbm_device *gbm_device;
    bool supported_buffer_types_for_import[kDisplayBufferTypeLast + 1];
};

void display_destroy(struct display *display) {
    DEBUG_ASSERT(display != NULL);
    DEBUG_ASSERT(display->destroy != NULL);
    return display->destroy(display);
}

void display_get_size(struct display *display, int *width_out, int *height_out) {
    DEBUG_ASSERT(display != NULL);
    
    if (width_out != NULL) {
        *width_out = display->width;
    }
    if (height_out != NULL) {
        *height_out = display->height;
    }
}

bool display_has_dimensions(struct display *display) {
    return display->has_dimensions;
}

void display_get_dimensions(struct display *display, int *width_mm_out, int *height_mm_out) {
    DEBUG_ASSERT(display->has_dimensions);
    if (width_mm_out) {
        *width_mm_out = display->width_mm;
    }
    if (height_mm_out) {
        *height_mm_out = display->height_mm;
    }
}

#ifdef HAS_GBM
bool display_supports_gbm(struct display *display) {
    DEBUG_ASSERT(display != NULL);
    return display->supports_gbm;
}

struct gbm_device *display_get_gbm_device(struct display *display) {
    DEBUG_ASSERT(display != NULL);
    DEBUG_ASSERT(display->supports_gbm);
    return display->gbm_device;
}
#endif

/**
 * @brief Get the list of fourcc formats supported by this presenter.
 */
void display_get_supported_formats(struct display *display, const uint32_t **formats_out, size_t *n_formats_out) {
    DEBUG_ASSERT(display != NULL);
    DEBUG_ASSERT(formats_out != NULL);
    DEBUG_ASSERT(n_formats_out != NULL);
    return display->get_supported_formats(display, formats_out, n_formats_out);
}

double display_get_refresh_rate(struct display *display) {
    return display->refresh_rate;
}

struct presenter *display_create_presenter(struct display *display) {
    DEBUG_ASSERT(display != NULL);
    DEBUG_ASSERT(display->create_presenter != NULL);
    return display->create_presenter(display);
}

bool display_supports_importing_buffer_type(struct display *display, enum display_buffer_type type) {
    DEBUG_ASSERT(display != NULL);
    return display->supported_buffer_types_for_import[type];
}

struct display_buffer *display_create_mapped_buffer(
    struct display *display,
    int width, int height, int stride,
    enum pixfmt format
) {
    struct display_buffer *buffer;
    int ok;

    DEBUG_ASSERT(display != NULL);

    buffer = malloc(sizeof *buffer);
    if (buffer == NULL) {
        return NULL;
    }
    
    buffer->display = display;
    buffer->backend.sw.width = width;
    buffer->backend.sw.height = height;
    buffer->backend.sw.format = format;

    ok = display->make_mapped_buffer(buffer);
    if (ok != 0) {
        free(buffer);
        return NULL;
    }

    return buffer;
}

struct display_buffer *display_import_buffer(
    struct display *display,
    const struct display_buffer_backend *source,
    display_buffer_destroy_callback_t destroy_callback,
    void *userdata
) {
    struct display_buffer *buffer;
    int ok;

    DEBUG_ASSERT(display != NULL);
    DEBUG_ASSERT(source != NULL);
    DEBUG_ASSERT(destroy_callback != NULL);
    DEBUG_ASSERT(display->supported_buffer_types_for_import[source->type]);

    buffer = malloc(sizeof *buffer);
    if (buffer == NULL) {
        return NULL;
    }

    buffer->display = display;
    buffer->backend = *source;
    buffer->destroy_callback = destroy_callback;
    buffer->userdata = userdata;

    if (source->type == kDisplayBufferTypeSw) {
        ok = display->import_sw_buffer(buffer);
    } else if (source->type == kDisplayBufferTypeGbmBo) {
        ok = display->import_gbm_bo(buffer);
    } else if (source->type == kDisplayBufferTypeGemBo) {
        ok = display->import_gem_bo(buffer);
    } else if (source->type == kDisplayBufferTypeEglImage) {
        ok = display->import_egl_image(buffer);
    }

    if (ok != 0) {
        free(buffer);
        return NULL;
    }

    return buffer;
}

const struct display_buffer_backend *display_buffer_get_backend(struct display_buffer *buffer) {
    return &buffer->backend;
}

void display_buffer_destroy(struct display_buffer *buffer) {
    DEBUG_ASSERT(buffer != NULL);
    DEBUG_ASSERT(buffer->destroy_callback != NULL);
    return buffer->destroy_callback(buffer->display, &buffer->backend, buffer->userdata);
}

#ifdef HAS_FBDEV
#define PRESENTER_PRIVATE_FBDEV(p_presenter) ((struct fbdev_presenter *) (p_presenter)->private)
#define DISPLAY_PRIVATE_FBDEV(p_display) ((struct fbdev_display *) (p_display)->private)

struct fbdev_display {
    int fd;
    
    /**
     * @brief fixinfo is the info that is fixed per framebuffer and can't be changed.
     */
    struct fb_fix_screeninfo fixinfo;

    /**
     * @brief varinfo is the info that can potentially be changed, but we don't do that and
     * instead rely on something like `fbset` configuring the fbdev for us.
     */
    struct fb_var_screeninfo varinfo;

    /**
     * @brief The physical width & height of the frame buffer and the pixel format as a fourcc code.
     */
    uint32_t width, height, format;
    
    /**
     * @brief The mapped video memory of this fbdev.
     * Basically the result of mmap(fd)
     */ 
    uint8_t *vmem;

    /**
     * @brief How many bytes of vmem we mapped.
     */
    size_t size_vmem;
};

struct fbdev_presenter {
    struct fbdev_display *fbdev_display;
    struct display *display;

    presenter_scanout_callback_t scanout_cb;
    void *scanout_cb_userdata;
};

static int fbdev_presenter_set_scanout_callback(struct presenter *presenter, presenter_scanout_callback_t cb, void *userdata) {
    struct fbdev_presenter *private = PRESENTER_PRIVATE_FBDEV(presenter);

    private->scanout_cb = cb;
    private->scanout_cb_userdata = userdata;
    
    return 0;
}

static int fbdev_presenter_set_logical_zpos(struct presenter *presenter, int logical_zpos) {
    (void) presenter;

    if ((logical_zpos == 0) || (logical_zpos == -1)) {
        return 0;
    } else {
        return EINVAL;
    }
}

static int fbdev_presenter_get_zpos(struct presenter *presenter) {
    (void) presenter;
    return 0;
}

static int fbdev_presenter_push_drm_fb_layer(struct presenter *presenter, const struct drm_fb_layer *layer) {
    (void) presenter;
    (void) layer;
    return EOPNOTSUPP;
}

static int fbdev_presenter_push_gbm_bo_layer(struct presenter *presenter, const struct gbm_bo_layer *layer) {
    (void) presenter;
    (void) layer;
    /// TODO: Implement
    return EINVAL;
}

static int fbdev_presenter_push_sw_fb_layer(struct presenter *presenter, const struct sw_fb_layer *layer) {
    struct fbdev_presenter *private = PRESENTER_PRIVATE_FBDEV(presenter);

    // formats should probably be handled by the code using this
    if (layer->format != private->fbdev_display->format) {
        return EINVAL;
    }

    if (layer->width != private->fbdev_display->width) {
        return EINVAL;
    }

    if (layer->height != private->fbdev_display->height) {
        return EINVAL;
    }

    // If the pitches don't match, we'd need to copy line by line.
    // We can support that in the future but it's probably unnecessary right now.
    if (private->fbdev_display->fixinfo.line_length != layer->pitch) {
        LOG_MODESETTING_ERROR("Rendering software framebuffers into fbdev with non-matching buffer pitches is not supported.\n");
        return EINVAL;
    }

    size_t offset = private->fbdev_display->varinfo.yoffset * private->fbdev_display->fixinfo.line_length + private->fbdev_display->varinfo.xoffset;

    memcpy(
        private->fbdev_display->vmem + offset,
        layer->vmem,
        min(private->fbdev_display->size_vmem - offset, layer->height * layer->pitch)
    );

    return 0;
}

static int fbdev_presenter_push_placeholder_layer(struct presenter *presenter, int n_reserved_layers) {
    (void) presenter;
    (void) n_reserved_layers;
    return EOPNOTSUPP;
}

static int fbdev_presenter_flush(struct presenter *presenter) {
    (void) presenter;
    /// TODO: Implement
    return 0;
}

static void fbdev_presenter_destroy(struct presenter *presenter) {
    free(presenter);
}

static inline bool fb_bitfield_equals(const struct fb_bitfield *a, const struct fb_bitfield *b) {
    return (a->offset == b->offset) && (a->length == b->length) && ((a->msb_right != 0) == (b->msb_right != 0));
}

static void fbdev_display_destroy(struct display *display) {
    struct fbdev_display *private;

    DEBUG_ASSERT(display != NULL);

    private = DISPLAY_PRIVATE_FBDEV(display);

    munmap(private->vmem, private->size_vmem);
    close(private->fd);
    free(private);
    free(display);
}

static void fbdev_display_get_supported_formats(struct display *display, const uint32_t **formats_out, size_t *n_formats_out) {
    struct fbdev_display *private = DISPLAY_PRIVATE_FBDEV(display);

    *formats_out = &private->format;
    *n_formats_out = 1;

    return 0;
}

static void fbdev_display_on_destroy_mapped_buffer(struct display *display, const struct display_buffer_backend *backend, void *userdata) {
    free(backend->sw.vmem);
}

static int fbdev_display_make_mapped_buffer(struct display_buffer *buffer) {
    struct fbdev_display *private = DISPLAY_PRIVATE_FBDEV(buffer->display);
    uint8_t *vmem;
    int bytes_per_pixel, stride;

    bytes_per_pixel = (get_pixfmt_info(buffer->backend.sw.format)->bits_per_pixel + 7) / 8;
    
    stride = buffer->backend.sw.width * bytes_per_pixel;
    vmem = malloc(buffer->backend.sw.height * stride);
    if (vmem == NULL) {
        return ENOMEM;
    }

    buffer->backend.sw.stride = stride;
    buffer->backend.sw.vmem = vmem;
    buffer->destroy_callback = fbdev_display_on_destroy_mapped_buffer;
    buffer->userdata = NULL;
    return 0;
}

static int fbdev_display_import_sw_buffer(struct display_buffer *buffer) {
    struct fbdev_display *private = DISPLAY_PRIVATE_FBDEV(buffer->display);
    
    if (buffer->backend.sw.width != private->width) {
        LOG_MODESETTING_ERROR("sw buffer has wrong width.\n");
        return EINVAL;
    }

    if (buffer->backend.sw.height != private->height) {
        LOG_MODESETTING_ERROR("sw buffer has wrong height.\n");
        return EINVAL;
    }

    if (buffer->backend.sw.pixel_format != private->format) {
        LOG_MODESETTING_ERROR("sw buffer has wrong pixel format.\n");
        return EINVAL;
    }

    return 0;
}

static struct presenter *fbdev_display_create_presenter(struct display *display) {
    struct fbdev_presenter *presenter_private;
    struct fbdev_display *private;
    struct presenter *presenter;

    DEBUG_ASSERT(display != NULL);
    private = DISPLAY_PRIVATE_FBDEV(display);

    presenter = malloc(sizeof *presenter);
    if (presenter == NULL) {
        return NULL;
    }

    presenter_private = malloc(sizeof *presenter_private);
    if (presenter_private == NULL) {
        free(presenter);
        return NULL;
    }

    presenter_private->fbdev_display = display;

    presenter->private = (struct presenter_private *) presenter_private;
    presenter->set_logical_zpos = fbdev_presenter_set_logical_zpos;
    presenter->get_zpos = fbdev_presenter_get_zpos;
    presenter->set_scanout_callback = fbdev_presenter_set_scanout_callback;
    presenter->push_drm_fb_layer = fbdev_presenter_push_drm_fb_layer;
    presenter->push_gbm_bo_layer = fbdev_presenter_push_gbm_bo_layer;
    presenter->push_sw_fb_layer = fbdev_presenter_push_sw_fb_layer;
    presenter->push_placeholder_layer = fbdev_presenter_push_placeholder_layer;
    presenter->flush = fbdev_presenter_flush;
    presenter->destroy = fbdev_presenter_destroy;
    
    return presenter;
}

struct display *fbdev_display_new_from_fd(int fd, const struct fbdev_display_config *config) {
    struct fbdev_display *private;
    struct display *display;
    unsigned int i;
    uint32_t format;
    void *vmem;
    int ok;

    DEBUG_ASSERT(config != NULL);

    display = malloc(sizeof *display);
    if (display == NULL) {
        goto fail_return_null;
    }

    private = malloc(sizeof *private);
    if (private == NULL) {
        goto fail_free_display;
    }

    ok = ioctl(fd, FBIOGET_FSCREENINFO, &private->fixinfo);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't get fbdev fix_screeninfo. ioctl: %s\n", strerror(-ok));
        goto fail_free_private;
    }

    ok = ioctl(fd, FBIOGET_VSCREENINFO, &private->varinfo);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't get fbdev var_screeninfo. ioctl: %s\n", strerror(-ok));
        goto fail_free_private;
    }

    if (private->fixinfo.visual != FB_TYPE_PACKED_PIXELS) {
        LOG_MODESETTING_ERROR("A fbdev type other than FB_TYPE_PACKED_PIXELS is not supported.\n");
        goto fail_free_private;
    }
    
    if (private->fixinfo.visual != FB_VISUAL_TRUECOLOR) {
        LOG_MODESETTING_ERROR("A fbdev visual other than FB_VISUAL_TRUECOLOR is not supported.\n");
        goto fail_free_private;
    }

    // look for the fourcc code for this fbdev format
    format = 0;
    for (i = 0; i < n_pixfmt_infos; i++) {
        if (fb_bitfield_equals(&private->varinfo.red, &pixfmt_infos[i].r) &&
            fb_bitfield_equals(&private->varinfo.green, &pixfmt_infos[i].g) &&
            fb_bitfield_equals(&private->varinfo.blue, &pixfmt_infos[i].b) &&
            fb_bitfield_equals(&private->varinfo.transp, &pixfmt_infos[i].a)
        ) {
            format = pixfmt_infos[i].fourcc;
            break;
        }
    }

    // if the format is still 0 we couldn't find a corresponding DRM fourcc format.
    if (format == 0) {
        LOG_MODESETTING_ERROR(
            "Didn't find a corresponding fourcc format for fbdev format rgba = %" PRIu32 "/%" PRIu32 ",%" PRIu32 "/%" PRIu32 ",%" PRIu32 "/%" PRIu32 ",%" PRIu32"/%" PRIu32 ".\n",
            private->varinfo.red.length, private->varinfo.red.offset,
            private->varinfo.green.length, private->varinfo.green.offset,
            private->varinfo.blue.length, private->varinfo.blue.offset,
            private->varinfo.transp.length, private->varinfo.transp.offset
        );
        goto fail_free_private;
    }

    vmem = mmap(
        NULL,
        private->varinfo.yres_virtual * private->fixinfo.line_length,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        private->fd,
        0
    );
    if (vmem == MAP_FAILED) {
        LOG_MODESETTING_ERROR("Couldn't map fbdev. mmap: %s\n", strerror(errno));
        goto fail_free_private;
    }

    private->fd = fd;
    private->width = private->varinfo.width;
    private->height = private->varinfo.height;
    private->format = format;
    private->vmem = vmem;
    display->private = (struct display_private*) private;
    display->get_supported_formats = fbdev_display_get_supported_formats;
    display->import_sw_buffer = fbdev_display_import_sw_buffer;
    display->create_presenter = fbdev_display_create_presenter;
    display->destroy = fbdev_display_destroy;
    display->width = private->varinfo.width;
    display->height = private->varinfo.height;
    display->has_dimensions = config->has_explicit_dimensions;
    display->width_mm = config->width_mm;
    display->height_mm = config->height_mm;
    display->supports_gbm = false;
    display->gbm_device = NULL;
    display->gbm_surface = NULL;
    display->supported_buffer_types_for_import[kDisplayBufferTypeSw] = true;
    display->supported_buffer_types_for_import[kDisplayBufferTypeGbmBo] = false;
    display->supported_buffer_types_for_import[kDisplayBufferTypeGemBo] = false;
    display->supported_buffer_types_for_import[kDisplayBufferTypeEglImage] = false;

    return display;


    fail_free_private:
    free(private);

    fail_free_display:
    free(display);

    fail_return_null:
    return NULL;
}

struct display *fbdev_display_new_from_path(const char *path, const struct fbdev_display_config *config) {
    struct display *display;
    int fd;

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOG_MODESETTING_ERROR("Couldn't open fb device. open: %s\n", strerror(errno));
        return NULL;
    }

    display = fbdev_display_new_from_fd(fd, config);
    if (display == NULL) {
        close(fd);
        return NULL;
    }

    return display;
}
#endif

#ifdef HAS_KMS
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

#define IS_VALID_ZPOS(kms_presenter, zpos) (((kms_presenter)->kms_display->crtc->min_zpos <= zpos) && ((kms_presenter)->kms_display->crtc->max_zpos >= zpos))
#define DEBUG_ASSERT_VALID_ZPOS(kms_presenter, zpos) DEBUG_ASSERT(IS_VALID_ZPOS(kms_presenter, zpos))
#define CHECK_VALID_ZPOS_OR_RETURN_EINVAL(kms_presenter, zpos) do { \
        if (IS_VALID_ZPOS(kms_presenter, zpos)) { \
            LOG_MODESETTING_ERROR("%s: Invalid zpos\n", __func__); \
            return EINVAL; \
        } \
    } while (false)

#define IS_VALID_CRTC_INDEX(kmsdev, crtc_index) (((crtc_index) >= 0) && ((crtc_index) < (kmsdev)->n_crtcs))
#define DEBUG_ASSERT_VALID_CRTC_INDEX(kmsdev, crtc_index) DEBUG_ASSERT(IS_VALID_CRTC_INDEX(kmsdev, crtc_index))

#define IS_VALID_CONNECTOR_INDEX(kmsdev, connector_index) (((connector_index) >= 0) && ((connector_index) < (kmsdev)->n_connectors))
#define DEBUG_ASSERT_VALID_CONNECTOR_INDEX(kmsdev, connector_index) DEBUG_ASSERT(IS_VALID_CONNECTOR_INDEX(kmsdev, connector_index))

#define PRESENTER_PRIVATE_KMS(p_presenter) ((struct kms_presenter *) (p_presenter)->private)
#define DISPLAY_PRIVATE_KMS(p_display) ((struct kms_display *) (p_display)->private)

struct drm_connector {
    drmModeConnector *connector;
	drmModeObjectProperties *props;
    drmModePropertyRes **props_info;
};

struct drm_encoder {
    drmModeEncoder *encoder;
};

struct drm_crtc {
    drmModeCrtc *crtc;
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;

    uint32_t bitmask;
    uint8_t index;

    int selected_connector_index;
    struct drm_connector *selected_connector;
    drmModeModeInfo selected_mode;
    uint32_t selected_mode_blob_id;

    bool supports_hardware_cursor;

    bool supports_zpos;
    int min_zpos, max_zpos;
    
    size_t n_formats;
    uint32_t *formats;

    presenter_scanout_callback_t scanout_cb;
    struct display *scanout_cb_display;
    void *scanout_cb_userdata;
};

struct drm_plane {
    drmModePlane *plane;
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;

    struct {
        uint32_t crtc_id, fb_id,
            src_x, src_y, src_w, src_h,
            crtc_x, crtc_y, crtc_w, crtc_h,
            rotation, zpos;
    } property_ids;

    int type;
    int min_zpos, max_zpos;
    uint32_t supported_rotations;
};

/**
 * @brief Helper structure that is attached to GBM BOs as userdata to track the DRM FB with which they can
 * be scanned out (and later destroyed) 
 */
struct drm_fb {
    struct kmsdev *dev;
    uint32_t fb_id;
};

struct kms_display {
    struct kmsdev *dev;

    uint32_t assigned_planes;
    uint32_t crtc_id;
    int crtc_index;
    struct drm_crtc *crtc;
};

struct kms_cursor {
    uint32_t handle;
    int width, height;
    int hot_x, hot_y;
};

struct kms_cursor_state {
    int x, y;
    bool enabled;
    struct kms_cursor *cursor;
};

struct kmsdev {
    int fd;

    /**
     * @brief Whether we can & should use atomic modesetting.
     */
    bool use_atomic_modesetting;

    /**
     * @brief Explicit fencing means every CRTC gets it's own fd
     * for waiting on the pageflip event (but also has to supply a input fence fd).
     */
    bool supports_explicit_fencing;

    size_t n_connectors;
    struct drm_connector *connectors;

    size_t n_encoders;
    struct drm_encoder *encoders;

    size_t n_crtcs;
    struct drm_crtc *crtcs;

    size_t n_planes;
    struct drm_plane *planes;

    drmModeRes *res;
    drmModePlaneRes *plane_res;

    /**
     * @brief The event loop we'll use to wait on the DRM fd (to get the page flip events)
     * With explicit fencing, every display can use its own event loop, but that's not
     * yet implemented.
     */
    struct event_loop *loop;
    
    /**
     * @brief Contains the pageflip event handler. There's a single pageflip event handler
     * for all displays. This pageflip handler will then call the crtc-specific pageflip
     * event handler in @ref scanout_callbacks.
     */
    drmEventContext pageflip_evctx;
    
    /**
     * @brief The cursor state (active cursor image, position, rotation) for each CRTC.
     */
    struct kms_cursor_state cursor_state[32];

    /**
     * @brief The configured displays for this KMS device. This is NULL and 0 on startup
     * and are only valid after @ref kmsdev_configure.
     */
    struct display **displays;
    size_t n_displays;

    struct gbm_device *gbm_device;
};

struct kms_presenter {
    struct kmsdev *dev;

    struct kms_display *kms_display;
    struct display *display;

    /**
     * @brief Bitmask of the planes that are currently free to use.
     */
    uint32_t free_planes;

    int current_zpos;
    drmModeAtomicReq *req;

    /**
     * @brief Used for legacy modesetting backwards-compatibility.
     * The lowest framebuffer layer in a legacy modesetting compatible format.
     */
    bool has_primary_layer;
    struct drm_fb_layer primary_layer;

    /**
     * If true, @ref on_present_layers will commit blockingly.
     * 
     * It will also schedule a simulated page flip event on the main thread
     * afterwards so the frame queue works.
     * 
     * If false, @ref on_present_layers will commit nonblocking using page flip events,
     * like usual.
     */
    bool do_blocking_atomic_commits;
};

static int fetch_connectors(struct kmsdev *dev, struct drm_connector **connectors_out, size_t *n_connectors_out) {
    struct drm_connector *connectors;
    int n_allocated_connectors;
    int ok;

    connectors = calloc(dev->res->count_connectors, sizeof *connectors);
    if (connectors == NULL) {
        ok = ENOMEM;
        goto fail_return_ok;
    }

    n_allocated_connectors = 0;
    for (int i = 0; i < dev->res->count_connectors; i++, n_allocated_connectors++) {
        drmModeObjectProperties *props;
        drmModePropertyRes **props_info;
        drmModeConnector *connector;

        connector = drmModeGetConnector(dev->fd, dev->res->connectors[i]);
        if (connector == NULL) {
            ok = errno;
            LOG_MODESETTING_ERROR("Could not get DRM device connector. drmModeGetConnector: %s\n", strerror(errno));
            goto fail_free_connectors;
        }

        props = drmModeObjectGetProperties(dev->fd, dev->res->connectors[i], DRM_MODE_OBJECT_CONNECTOR);
        if (props == NULL) {
            ok = errno;
            LOG_MODESETTING_ERROR("Could not get DRM device connectors properties. drmModeObjectGetProperties: %s\n", strerror(errno));
            drmModeFreeConnector(connector);
            goto fail_free_connectors;
        }

        props_info = calloc(props->count_props, sizeof *props_info);
        if (props_info == NULL) {
            ok = ENOMEM;
            drmModeFreeObjectProperties(props);
            drmModeFreeConnector(connector);
            goto fail_free_connectors;
        }

        for (unsigned int j = 0; j < props->count_props; j++) {
            props_info[j] = drmModeGetProperty(dev->fd, props->props[j]);
            if (props_info[j] == NULL) {
                ok = errno;
                LOG_MODESETTING_ERROR("Could not get DRM device connector properties' info. drmModeGetProperty: %s\n", strerror(errno));
                for (unsigned int k = 0; k < (j-1); k++)
                    drmModeFreeProperty(props_info[j]);
                free(props_info);
                drmModeFreeObjectProperties(props);
                drmModeFreeConnector(connector);
                goto fail_free_connectors;
            }
        }

        connectors[i].connector = connector;
        connectors[i].props = props;
        connectors[i].props_info = props_info;
    }

    *connectors_out = connectors;
    *n_connectors_out = dev->res->count_connectors;

    return 0;

    fail_free_connectors:
    for (int i = 0; i < n_allocated_connectors; i++) {
        for (unsigned int j = 0; j < connectors[i].props->count_props; j++)
            drmModeFreeProperty(connectors[i].props_info[j]);
        free(connectors[i].props_info);
        drmModeFreeObjectProperties(connectors[i].props);
        drmModeFreeConnector(connectors[i].connector);
    }

    free(connectors);

    fail_return_ok:
    *connectors_out = NULL;
    *n_connectors_out = 0;
    return ok;
}

static int free_connectors(struct drm_connector *connectors, size_t n_connectors) {
    for (unsigned int i = 0; i < n_connectors; i++) {
        for (unsigned int j = 0; j < connectors[i].props->count_props; j++)
            drmModeFreeProperty(connectors[i].props_info[j]);
        free(connectors[i].props_info);
        drmModeFreeObjectProperties(connectors[i].props);
        drmModeFreeConnector(connectors[i].connector);
    }

    free(connectors);

    return 0;
}

static int fetch_encoders(struct kmsdev *dev, struct drm_encoder **encoders_out, size_t *n_encoders_out) {
    struct drm_encoder *encoders;
    int n_allocated_encoders;
    int ok;

    encoders = calloc(dev->res->count_encoders, sizeof *encoders);
    if (encoders == NULL) {
        *encoders_out = NULL;
        *n_encoders_out = 0;
        return ENOMEM;
    }

    n_allocated_encoders = 0;
    for (int i = 0; i < dev->res->count_encoders; i++, n_allocated_encoders++) {
        drmModeEncoder *encoder;

        encoder = drmModeGetEncoder(dev->fd, dev->res->encoders[i]);
        if (encoder == NULL) {
            ok = errno;
            LOG_MODESETTING_ERROR("Could not get DRM device encoder. drmModeGetEncoder: %s\n", strerror(errno));
            goto fail_free_encoders;
        }

        encoders[i].encoder = encoder;
    }

    *encoders_out = encoders;
    *n_encoders_out = dev->res->count_encoders;

    return 0;

    fail_free_encoders:
    for (int i = 0; i < n_allocated_encoders; i++) {
        drmModeFreeEncoder(encoders[i].encoder);
    }
    free(encoders);

    *encoders_out = NULL;
    *n_encoders_out = 0;
    return ok;
}

static int free_encoders(struct drm_encoder *encoders, size_t n_encoders) {
    for (unsigned int i = 0; i < n_encoders; i++) {
        drmModeFreeEncoder(encoders[i].encoder);
    }

    free(encoders);

    return 0;
}

static int fetch_crtcs(struct kmsdev *dev, struct drm_crtc **crtcs_out, size_t *n_crtcs_out) {
    struct drm_crtc *crtcs;
    int n_allocated_crtcs;
    int ok;

    crtcs = calloc(dev->res->count_crtcs, sizeof *crtcs);
    if (crtcs == NULL) {
        *crtcs_out = NULL;
        return ENOMEM;
    }

    n_allocated_crtcs = 0;
    for (int i = 0; i < dev->res->count_crtcs; i++, n_allocated_crtcs++) {
        drmModeObjectProperties *props;
        drmModePropertyRes **props_info;
        drmModeCrtc *crtc;

        crtc = drmModeGetCrtc(dev->fd, dev->res->crtcs[i]);
        if (crtc == NULL) {
            ok = errno;
            LOG_MODESETTING_ERROR("Could not get DRM device CRTC. drmModeGetCrtc: %s\n", strerror(errno));
            goto fail_free_crtcs;
        }

        props = drmModeObjectGetProperties(dev->fd, dev->res->crtcs[i], DRM_MODE_OBJECT_CRTC);
        if (props == NULL) {
            ok = errno;
            LOG_MODESETTING_ERROR("Could not get DRM device CRTCs properties. drmModeObjectGetProperties: %s\n", strerror(errno));
            drmModeFreeCrtc(crtc);
            goto fail_free_crtcs;
        }

        props_info = calloc(props->count_props, sizeof *props_info);
        if (props_info == NULL) {
            ok = ENOMEM;
            drmModeFreeObjectProperties(props);
            drmModeFreeCrtc(crtc);
            goto fail_free_crtcs;
        }

        for (unsigned int j = 0; j < props->count_props; j++) {
            props_info[j] = drmModeGetProperty(dev->fd, props->props[j]);
            if (props_info[j] == NULL) {
                ok = errno;
                LOG_MODESETTING_ERROR("Could not get DRM device CRTCs properties' info. drmModeGetProperty: %s\n", strerror(errno));
                for (unsigned int k = 0; k < (j-1); k++)
                    drmModeFreeProperty(props_info[j]);
                free(props_info);
                drmModeFreeObjectProperties(props);
                drmModeFreeCrtc(crtc);
                goto fail_free_crtcs;
            }
        }
        
        crtcs[i].crtc = crtc;
        crtcs[i].props = props;
        crtcs[i].props_info = props_info;
        
        crtcs[i].index = i;
        crtcs[i].bitmask = 1 << i;

        crtcs[i].selected_connector_index = -1;
        crtcs[i].selected_connector = NULL;
        memset(&crtcs[i].selected_mode, 0, sizeof(drmModeModeInfo));
        crtcs[i].selected_mode_blob_id = 0;
        
        // for the other ones, set some reasonable defaults.
        // these depend on the plane data
        crtcs[i].supports_hardware_cursor = false;

        crtcs[i].supports_zpos = false;
        crtcs[i].min_zpos = 0;
        crtcs[i].max_zpos = 0;
        
        crtcs[i].n_formats = 0;
        crtcs[i].formats = NULL;

        crtcs[i].scanout_cb = NULL;
        crtcs[i].scanout_cb_userdata = NULL;
    }

    *crtcs_out = crtcs;
    *n_crtcs_out = dev->res->count_crtcs;

    return 0;


    fail_free_crtcs:
    for (int i = 0; i < n_allocated_crtcs; i++) {
        for (unsigned int j = 0; j < crtcs[i].props->count_props; j++)
            drmModeFreeProperty(crtcs[i].props_info[j]);
        free(crtcs[i].props_info);
        drmModeFreeObjectProperties(crtcs[i].props);
        drmModeFreeCrtc(crtcs[i].crtc);
    }

    free(crtcs);

    *crtcs_out = NULL;
    *n_crtcs_out = 0;
    return ok;
}

static int free_crtcs(struct drm_crtc *crtcs, size_t n_crtcs) {
    for (unsigned int i = 0; i < n_crtcs; i++) {
        for (unsigned int j = 0; j < crtcs[i].props->count_props; j++)
            drmModeFreeProperty(crtcs[i].props_info[j]);
        free(crtcs[i].props_info);
        drmModeFreeObjectProperties(crtcs[i].props);
        drmModeFreeCrtc(crtcs[i].crtc);
    }

    free(crtcs);

    return 0;
}

static int fetch_planes(struct kmsdev *dev, struct drm_plane **planes_out, size_t *n_planes_out) {
    struct drm_plane *planes;
    int n_allocated_planes;
    int ok;

    planes = calloc(dev->plane_res->count_planes, sizeof *planes);
    if (planes == NULL) {
        *planes_out = NULL;
        return ENOMEM;
    }

    n_allocated_planes = 0;
    for (unsigned int i = 0; i < dev->plane_res->count_planes; i++, n_allocated_planes++) {
        drmModeObjectProperties *props;
        drmModePropertyRes **props_info;
        drmModePlane *plane;

        memset(&planes[i].property_ids, 0xFF, sizeof(planes[i].property_ids));

        plane = drmModeGetPlane(dev->fd, dev->plane_res->planes[i]);
        if (plane == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device plane. drmModeGetPlane");
            goto fail_free_planes;
        }

        props = drmModeObjectGetProperties(dev->fd, dev->plane_res->planes[i], DRM_MODE_OBJECT_PLANE);
        if (props == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device planes' properties. drmModeObjectGetProperties");
            drmModeFreePlane(plane);
            goto fail_free_planes;
        }

        props_info = calloc(props->count_props, sizeof *props_info);
        if (props_info == NULL) {
            ok = ENOMEM;
            drmModeFreeObjectProperties(props);
            drmModeFreePlane(plane);
            goto fail_free_planes;
        }

        for (unsigned int j = 0; j < props->count_props; j++) {
            props_info[j] = drmModeGetProperty(dev->fd, props->props[j]);
            if (props_info[j] == NULL) {
                ok = errno;
                perror("[modesetting] Could not get DRM device planes' properties' info. drmModeGetProperty");
                for (unsigned int k = 0; k < (j-1); k++)
                    drmModeFreeProperty(props_info[j]);
                free(props_info);
                drmModeFreeObjectProperties(props);
                drmModeFreePlane(plane);
                goto fail_free_planes;
            }

            if (strcmp(props_info[j]->name, "type") == 0) {
                planes[i].type = 0;
                for (uint32_t k = 0; k < props->count_props; k++) {
                    if (props->props[k] == props_info[j]->prop_id) {
                        planes[i].type = props->prop_values[k];
                        break;
                    }
                }
            } else if (strcmp(props_info[j]->name, "CRTC_ID") == 0) {
                planes[i].property_ids.crtc_id = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "FB_ID") == 0) {
                planes[i].property_ids.fb_id = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "SRC_X") == 0) {
                planes[i].property_ids.src_x = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "SRC_Y") == 0) {
                planes[i].property_ids.src_y = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "SRC_W") == 0) {
                planes[i].property_ids.src_w = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "SRC_H") == 0) {
                planes[i].property_ids.src_h = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "CRTC_X") == 0) {
                planes[i].property_ids.crtc_x = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "CRTC_Y") == 0) {
                planes[i].property_ids.crtc_y = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "CRTC_W") == 0) {
                planes[i].property_ids.crtc_w = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "CRTC_H") == 0) {
                planes[i].property_ids.crtc_h = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "zpos") == 0) {
                planes[i].property_ids.zpos = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "rotation") == 0) {
                planes[i].property_ids.rotation = props_info[j]->prop_id;
            }
        }

        planes[i].plane = plane;
        planes[i].props = props;
        planes[i].props_info = props_info;
    }

    *planes_out = planes;
    *n_planes_out = dev->plane_res->count_planes;

    return 0;


    fail_free_planes:
    for (int i = 0; i < n_allocated_planes; i++) {
        for (unsigned int j = 0; j < planes[i].props->count_props; j++)
            drmModeFreeProperty(planes[i].props_info[j]);
        free(planes[i].props_info);
        drmModeFreeObjectProperties(planes[i].props);
        drmModeFreePlane(planes[i].plane);
    }

    free(planes);

    *planes_out = NULL;
    *n_planes_out = 0;
    return ok;
}

/*
static int free_planes(struct drm_plane *planes, size_t n_planes) {
    for (size_t i = 0; i < n_planes; i++) {
        for (uint32_t j = 0; j < planes[i].props->count_props; j++)
            drmModeFreeProperty(planes[i].props_info[j]);
        free(planes[i].props_info);
        drmModeFreeObjectProperties(planes[i].props);
        drmModeFreePlane(planes[i].plane);
    }

    free(planes);

    return 0;
}
*/

bool fd_is_kmsfd(int fd) {
    drmModeRes *res;

    res = drmModeGetResources(fd);
    if (res == NULL) {
        return false;
    }

    drmModeFreeResources(res);

    return true;
}

void on_pageflip(
    int fd,
    unsigned int sequence,
    unsigned int tv_sec,
    unsigned int tv_usec,
    unsigned int crtc_id,
    void *userdata
) {
    struct drm_crtc *crtc;
    struct kmsdev *dev;
    unsigned int i;

    (void) fd;
    (void) sequence;
    
    DEBUG_ASSERT(userdata != NULL);

    dev = userdata;

    for (i = 0; i < dev->n_crtcs; i++) {
        if (dev->crtcs[i].crtc->crtc_id == crtc_id) {
            break;
        }
    }

    DEBUG_ASSERT(i < dev->n_crtcs);

    crtc = dev->crtcs + i;

    if (crtc->scanout_cb != NULL) {
        crtc->scanout_cb(crtc->scanout_cb_display, tv_sec * 1000000000ull + tv_usec * 1000ull, crtc->scanout_cb_userdata);
        crtc->scanout_cb = NULL;
        crtc->scanout_cb_display = NULL;
        crtc->scanout_cb_userdata = NULL;
    }
}

struct kmsdev *kmsdev_new_from_fd(struct event_loop *loop, int fd) {
    struct kmsdev *dev;
    int ok;

    dev = malloc(sizeof *dev);
    if (dev == NULL) {
        return NULL;
    }

    ok = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1ull);
    if ((ok < 0) && (errno == EOPNOTSUPP)) {
        ok = errno;
        goto fail_free_dev;
    } else {
        ok = errno;
        LOG_MODESETTING_ERROR("Could not set DRM client universal planes capable. drmSetClientCap: %s\n", strerror(ok));
        goto fail_free_dev;
    }
    
    ok = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1ull);
    if ((ok < 0) && (errno == EOPNOTSUPP)) {
        dev->use_atomic_modesetting = false;
    } else if (ok < 0) {
        ok = errno;
        LOG_MODESETTING_ERROR("Could not set DRM client atomic capable. drmSetClientCap: %s\n", strerror(ok));
        goto fail_close_fd;
    } else {
        dev->use_atomic_modesetting = true;
    }

    /*
    dev-> = gbm_create_device(fd);
    if (dev == NULL) {
        ok = errno;
        goto fail_close_fd;
    }
    */

    dev->res = drmModeGetResources(fd);
    if (dev->res == NULL) {
        ok = errno;
        perror("[modesetting] Could not get DRM device resources. drmModeGetResources");
        goto fail_close_fd;
    }

    dev->plane_res = drmModeGetPlaneResources(dev->fd);
    if (dev->plane_res == NULL) {
        ok = errno;
        perror("[modesetting] Could not get DRM device planes resources. drmModeGetPlaneResources");
        goto fail_free_resources;
    }

    ok = fetch_connectors(dev, &dev->connectors, &dev->n_connectors);
    if (ok != 0) {
        goto fail_free_plane_resources;
    }

    ok = fetch_encoders(dev, &dev->encoders, &dev->n_encoders);
    if (ok != 0) {
        goto fail_free_connectors;
    }

    ok = fetch_crtcs(dev, &dev->crtcs, &dev->n_crtcs);
    if (ok != 0) {
        goto fail_free_encoders;
    }

    ok = fetch_planes(dev, &dev->planes, &dev->n_planes);
    if (ok != 0) {
        goto fail_free_crtcs;
    }

    dev->fd = fd;
    dev->supports_explicit_fencing = false;
    dev->pageflip_evctx = (drmEventContext) {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .vblank_handler = NULL,
        .page_flip_handler = NULL,
        .page_flip_handler2 = on_pageflip,
        .sequence_handler = NULL
    };
    dev->loop = loop;

    return dev;


    fail_free_crtcs:
    free_crtcs(dev->crtcs, dev->n_crtcs);

    fail_free_encoders:
    free_encoders(dev->encoders, dev->n_encoders);

    fail_free_connectors:
    free_connectors(dev->connectors, dev->n_connectors);

    fail_free_plane_resources:
    drmModeFreePlaneResources(dev->plane_res);

    fail_free_resources:
    drmModeFreeResources(dev->res);

    fail_close_fd:
    close(dev->fd);

    fail_free_dev:
    free(dev);
    return NULL;
}

struct kmsdev *kmsdev_new_from_path(struct event_loop *loop, const char *path) {
    struct kmsdev *dev;
    int fd;

    fd = open(path, O_RDWR);
    if (fd < 0) {
        LOG_MODESETTING_ERROR("Couldn't open drm device. open: %s\n", strerror(errno));
        return NULL;
    }

    dev = kmsdev_new_from_fd(loop, fd);
    if (dev == NULL) {
        close(fd);
        return NULL;
    }

    return dev;
}

struct kmsdev *kmsdev_new_auto(struct event_loop *loop) {
    struct kmsdev *dev;
    int n_devices, ok, fd;

    ok = drmGetDevices2(0, NULL, 0);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't get the number of attached DRM devices. drmGetDevices2: %s\n", strerror(-ok));
        return NULL;
    }

    n_devices = ok;

    drmDevice devices[n_devices];
    ok = drmGetDevices2(0, devices, n_devices);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't query the attached DRM devices. drmGetDevices2: %s\n", strerror(-ok));
        return NULL;
    }

    n_devices = ok;
    for (int i = 0; i < n_devices; i++) {
        if (devices[i].available_nodes & (1 << DRM_NODE_PRIMARY)) {
            ok = open(devices[i].nodes[DRM_NODE_PRIMARY], O_CLOEXEC);
            if (ok < 0) {
                LOG_MODESETTING_ERROR("Couldn't open DRM device. open: %s\n", strerror(errno));
                continue;
            }

            fd = ok;

            if (fd_is_kmsfd(fd)) {
                dev = kmsdev_new_from_fd(loop, fd);
                if (dev != NULL) {
                    return dev;
                }

                close(fd);
            }
        }
    }

    return NULL;
}

void kmsdev_destroy(struct kmsdev *dev) {
    if (dev->fd > 0) {
        close(dev->fd);
    }
    free(dev);
}

int kmsdev_get_n_crtcs(struct kmsdev *dev) {
    DEBUG_ASSERT(dev != NULL);
    return dev->n_crtcs;
}

int kmsdev_get_n_connectors(struct kmsdev *dev) {
    DEBUG_ASSERT(dev != NULL);
    return dev->n_connectors;
}

bool kmsdev_is_connector_connected(struct kmsdev *dev, int connector_index) {
    DEBUG_ASSERT(dev != NULL);
    DEBUG_ASSERT_VALID_CONNECTOR_INDEX(dev, connector_index);
    
    return dev->connectors[connector_index].connector->connection == DRM_MODE_CONNECTED;
}

int kmsdev_configure_crtc(
    struct kmsdev *dev,
    int crtc_index,
    int connector_index,
    drmModeModeInfo *mode
) {
    struct drm_crtc *crtc;
    uint32_t blob_id;
    int ok;

    DEBUG_ASSERT(dev != NULL);
    DEBUG_ASSERT_VALID_CRTC_INDEX(dev, crtc_index);
    DEBUG_ASSERT_VALID_CONNECTOR_INDEX(dev, connector_index);
    DEBUG_ASSERT(mode != NULL);

    crtc = dev->crtcs + crtc_index;

    blob_id = 0;
    if (dev->use_atomic_modesetting) {
        ok = drmModeCreatePropertyBlob(dev->fd, mode, sizeof(mode), &blob_id);
        if (ok < 0) {
            ok = errno;
            LOG_MODESETTING_ERROR("Couldn't upload video mode to KMS. drmModeCreatePropertyBlob: %s\n", strerror(errno));
            goto fail_return_ok;
        }
    } else {
        ok = drmModeSetCrtc(
            dev->fd,
            dev->crtcs[crtc_index].crtc->crtc_id,
            0,
            0, 0,
            &(dev->connectors[connector_index].connector->connector_id),
            1,
            mode
        );
        if (ok < 0) {
            ok = errno;
            LOG_MODESETTING_ERROR("Couldn't set mode on CRTC. drmModeSetCrtc: %s\n", strerror(errno));
            goto fail_return_ok;
        }
    }

    if (crtc->selected_mode_blob_id != 0) {
        ok = drmModeDestroyPropertyBlob(dev->fd, crtc->selected_mode_blob_id);
        if (ok < 0) {
            LOG_MODESETTING_ERROR("Couldn't delete old video mode. drmModeDestroyPropertyBlob: %s\n", strerror(errno));
            goto fail_maybe_delete_property_blob;
        }
    }

    crtc->selected_connector_index = connector_index;
    crtc->selected_connector = dev->connectors + connector_index;
    crtc->selected_mode = *mode;
    crtc->selected_mode_blob_id = blob_id;

    return 0;

    fail_maybe_delete_property_blob:
    if (blob_id != 0) {
        DEBUG_ASSERT(drmModeDestroyPropertyBlob(dev->fd, blob_id) >= 0);
    }

    fail_return_ok:
    return ok;
}

int kmsdev_configure_crtc_with_preferences(
    struct kmsdev *dev,
    int crtc_index,
    int connector_index,
    const enum kmsdev_mode_preference *preferences
) {
    const enum kmsdev_mode_preference *cursor;
    enum kmsdev_mode_preference pref;
    drmModeModeInfo *stored_mode, *mode;

    DEBUG_ASSERT(dev != NULL);
    DEBUG_ASSERT_VALID_CRTC_INDEX(dev, crtc_index);
    DEBUG_ASSERT_VALID_CONNECTOR_INDEX(dev, connector_index);
    DEBUG_ASSERT(preferences != NULL);
    DEBUG_ASSERT(*preferences != kKmsdevModePreferenceNone);

    stored_mode = NULL;
    for (int i = 0; i < dev->connectors[connector_index].connector->count_modes; i++) {
        mode = dev->connectors[connector_index].connector->modes + i;

        if (stored_mode == NULL) {
            continue;
        }

        cursor = preferences;
        do {
            pref = *cursor;

            if (pref == kKmsdevModePreferencePreferred) {
                if (mode_is_preferred(stored_mode) != mode_is_preferred(mode)) {
                    if (mode_is_preferred(mode)) {
                        stored_mode = mode;
                    }
                    break;
                }
            } else if ((pref == kKmsdevModePreferenceHighestResolution) || (pref == kKmsdevModePreferenceLowestResolution)) {
                int area1 = mode_get_display_area(stored_mode);
                int area2 = mode_get_display_area(mode);

                if (area1 != area2) {
                    if ((pref == kKmsdevModePreferenceHighestResolution) && (area2 > area1)) {
                        stored_mode = mode;
                    } else if ((pref == kKmsdevModePreferenceLowestResolution) && (area2 < area1)) {
                        stored_mode = mode;
                    }
                    break;
                }
            } else if ((pref == kKmsdevModePreferenceHighestRefreshrate) || (pref == kKmsdevModePreferenceLowestRefreshrate)) {
                int refreshrate1 = round(mode_get_vrefresh(stored_mode));
                int refreshrate2 = round(mode_get_vrefresh(mode));

                if (refreshrate1 != refreshrate2) {
                    if ((pref == kKmsdevModePreferenceHighestRefreshrate) && (refreshrate2 > refreshrate1)) {
                        stored_mode = mode;
                    } else if ((pref == kKmsdevModePreferenceLowestRefreshrate) && (refreshrate2 < refreshrate1)) {
                        stored_mode = mode;
                    }
                    break;
                }
            } else if ((pref == kKmsdevModePreferenceInterlaced) || (pref == kKmsdevModePreferenceProgressive)) {
                if (mode_is_interlaced(stored_mode) != mode_is_interlaced(mode)) {
                    if ((pref == kKmsdevModePreferenceProgressive) && (!mode_is_interlaced(mode))) {
                        stored_mode = mode;
                    } else if ((pref == kKmsdevModePreferenceInterlaced) && (mode_is_interlaced(mode))) {
                        stored_mode = mode;
                    }
                    break;
                }
            } else {
                // we shouldn't ever reach this point.
                DEBUG_ASSERT(false);
            }
        } while (*(cursor++) != kKmsdevModePreferenceNone);
    }

    return kmsdev_configure_crtc(dev, crtc_index, connector_index, stored_mode);
}

#ifdef HAS_GBM
static void destroy_gbm_bo(struct gbm_bo *bo, void *userdata) {
	struct drm_fb *fb = userdata;

    (void) bo;
    DEBUG_ASSERT(fb != NULL);

    kmsdev_destroy_fb(fb->dev, fb->fb_id);
	free(fb);
}

int kmsdev_add_gbm_bo(
    struct kmsdev *dev,
    struct gbm_bo *bo,
    uint32_t *fb_id_out
) {
    struct drm_fb *fb;
	uint64_t modifiers[4] = {0};
	uint32_t width, height, format;
	uint32_t strides[4] = {0};
	uint32_t handles[4] = {0};
	uint32_t offsets[4] = {0};
	uint32_t flags = 0;
	int num_planes;
	int ok = -1;
	
	fb = gbm_bo_get_user_data(bo);
	/// TODO: Invalidate the drm_fb in the case that the drmdev changed (although that should never happen)
	// If the buffer object already has some userdata associated with it,
	// it's the drm_fb we allocated.
	if (fb != NULL) {
        *fb_id_out = fb->fb_id;
		return 0;
	}

	// If there's no framebuffer for the BO, we need to create one.
	fb = malloc(sizeof *fb);
	if (fb == NULL) {
		return ENOMEM;
	}

	fb->dev = dev;

	width = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);
	format = gbm_bo_get_format(bo);

	modifiers[0] = gbm_bo_get_modifier(bo);
	num_planes = gbm_bo_get_plane_count(bo);

	for (int i = 0; i < num_planes; i++) {
		strides[i] = gbm_bo_get_stride_for_plane(bo, i);
		handles[i] = gbm_bo_get_handle(bo).u32;
		offsets[i] = gbm_bo_get_offset(bo, i);
		modifiers[i] = modifiers[0];
	}

	if (modifiers[0]) {
		flags = DRM_MODE_FB_MODIFIERS;
	}

	ok = kmsdev_add_fb(
		dev,
		width,
		height,
		format,
		handles,
		strides,
		offsets,
		modifiers,
		&fb->fb_id,
		flags
	);
	if (ok != 0) {
		LOG_MODESETTING_ERROR("Couldn't add GBM BO as DRM framebuffer. kmsdev_add_fb: %s\n", strerror(ok));
		free(fb);
		return ok;
	}

	gbm_bo_set_user_data(bo, fb, destroy_gbm_bo);

    *fb_id_out = fb->fb_id;

	return 0;
}
#endif

int kmsdev_add_fb(
    struct kmsdev *dev,
    uint32_t width, uint32_t height,
    uint32_t pixel_format,
    const uint32_t bo_handles[4],
    const uint32_t pitches[4],
    const uint32_t offsets[4],
    const uint64_t modifier[4],
    uint32_t *buf_id,
    uint32_t flags
) {
    int ok;
    
    ok = drmModeAddFB2WithModifiers(
        dev->fd,
        width, height,
        pixel_format,
        bo_handles,
        pitches,
        offsets,
        modifier,
        buf_id,
        flags
    );

    return -ok;
}

int kmsdev_destroy_fb(
    struct kmsdev *dev,
    uint32_t buf_id
) {
    return -drmModeRmFB(dev->fd, buf_id);
}

const drmModeModeInfo *kmsdev_get_selected_mode(struct kmsdev *dev, int crtc_index) {
    DEBUG_ASSERT(dev != NULL);
    DEBUG_ASSERT_VALID_CRTC_INDEX(dev, crtc_index);

    return &dev->crtcs[crtc_index].selected_mode;
}

static inline int set_cursor(struct kmsdev *dev, int crtc_index, struct kms_cursor *cursor) {
    int ok;
    
    if (cursor != NULL) {
        ok = drmModeSetCursor2(
            dev->fd,
            dev->crtcs[crtc_index].crtc->crtc_id,
            cursor->handle,
            cursor->width,
            cursor->height,
            cursor->hot_x,
            cursor->hot_y
        );
    } else {
        ok = drmModeSetCursor2(
            dev->fd,
            dev->crtcs[crtc_index].crtc->crtc_id,
            0,
            64, 64,
            0, 0
        );
    }
    
    if (ok < 0) {
        ok = errno;
        LOG_MODESETTING_ERROR("Couldn't set cursor buffer. drmModeSetCursor2: %s\n", strerror(errno));
        return ok;
    }

    return 0;
}

int kmsdev_set_cursor(struct kmsdev *dev, int crtc_index, struct kms_cursor *cursor) {
    struct kms_cursor_state *state;
    int ok;

    DEBUG_ASSERT_VALID_CRTC_INDEX(dev, crtc_index);
    state = dev->cursor_state + crtc_index;

    if (state->cursor != cursor) {
        ok = set_cursor(dev, crtc_index, cursor);
        if (ok != 0) {
            return ok;
        }

        state->cursor = cursor;
    }

    return 0;
}

int kmsdev_move_cursor(struct kmsdev *dev, int crtc_index, int x, int y) {
    struct kms_cursor_state *state;
    int ok;

    DEBUG_ASSERT_VALID_CRTC_INDEX(dev, crtc_index);

    state = dev->cursor_state + crtc_index;

    if (state->cursor == NULL) {
        // if the cursor isn't enabled for this CRTC, don't do anything
        return 0;
    }

	ok = drmModeMoveCursor(dev->fd, dev->crtcs[crtc_index].crtc->crtc_id, x - state->cursor->hot_x, y - state->cursor->hot_y);
	if (ok < 0) {
        ok = errno;
		LOG_MODESETTING_ERROR("Could not move cursor. drmModeMoveCursor: %s", strerror(errno));
		return ok;
	}

	state->x = x;
	state->y = y;

	return 0;
}

/**
 * @brief Load raw cursor data into a cursor that can be used by KMS.
 */
struct kms_cursor *kmsdev_load_cursor(
    struct kmsdev *dev,
    int width, int height,
    uint32_t format,
    int hot_x, int hot_y,
    const uint8_t *data
) {
    struct drm_mode_destroy_dumb destroy_req;
    struct drm_mode_create_dumb create_req;
	struct drm_mode_map_dumb map_req;
    struct kms_cursor *cursor;
	uint32_t *buffer;
	uint64_t cap;
	int ok;

    // ARGB8888 is the format that'll implicitly be used when
    // uploading a cursor BO using drmModeSetCursor2.
    if (format != GBM_FORMAT_ARGB8888) {
        LOG_MODESETTING_ERROR("A format other than ARGB8888 is not supported for cursor images right now.\n");
        goto fail_return_null;
    }

    // first, find out if we can actually use dumb buffers.
    ok = drmGetCap(dev->fd, DRM_CAP_DUMB_BUFFER, &cap);
	if (ok < 0) {
		ok = errno;
        LOG_MODESETTING_ERROR("Could not query GPU driver support for dumb buffers. drmGetCap: %s\n", strerror(ok));
		goto fail_return_null;
	}

	if (cap == 0) {
        ok = ENOTSUP;
        LOG_MODESETTING_ERROR("Kernel / GPU Driver does not support dumb DRM buffers. Mouse cursor will not be displayed.\n");
		goto fail_return_null;
	}

	memset(&create_req, 0, sizeof create_req);
	create_req.width = width;
	create_req.height = height;
	create_req.bpp = 32;
	create_req.flags = 0;

	ok = ioctl(dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req);
	if (ok < 0) {
		ok = errno;
		LOG_MODESETTING_ERROR("Could not create a dumb buffer for the hardware cursor. ioctl: %s\n", strerror(errno));
		goto fail_return_null;
	}

	memset(&map_req, 0, sizeof map_req);
	map_req.handle = create_req.handle;

	ok = ioctl(dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req);
	if (ok < 0) {
		ok = errno;
		LOG_MODESETTING_ERROR("Could not prepare dumb buffer mmap for uploading the hardware cursor image. ioctl: %s\n", strerror(errno));
		goto fail_destroy_dumb_buffer;
	}

	buffer = mmap(0, create_req.size, PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd, map_req.offset);
	if (buffer == MAP_FAILED) {
		ok = errno;
		LOG_MODESETTING_ERROR("Could not mmap dumb buffer for uploading the hardware cursor icon. mmap: %s\n", strerror(errno));
        goto fail_destroy_dumb_buffer;
	}

    memcpy(buffer, data, create_req.size);

    ok = munmap(buffer, create_req.size);
    assert(ok >= 0); // likely a programming error

    cursor = malloc(sizeof *cursor);
    if (cursor == NULL) {
        goto fail_destroy_dumb_buffer;
    }

    cursor->handle = create_req.handle;
    cursor->width = width;
    cursor->height = height;
    cursor->hot_x = hot_x;
    cursor->hot_y = hot_y;

    return cursor;


	fail_destroy_dumb_buffer:
	memset(&destroy_req, 0, sizeof destroy_req);
	destroy_req.handle = create_req.handle;
	ioctl(dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);

	fail_return_null:
	return NULL;
}

/**
 * @brief Dispose this cursor, freeing all associated resources. Make sure
 * the cursor is now longer used on any crtc before disposing it.
 */
void kmsdev_dispose_cursor(struct kmsdev *dev, struct kms_cursor *cursor) {
    struct drm_mode_destroy_dumb destroy_req;
    int ok;

	memset(&destroy_req, 0, sizeof destroy_req);
	destroy_req.handle = cursor->handle;

	ok = ioctl(dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
    assert(ok >= 0); // likely a programming error
    
    free(cursor);
}

static int kms_presenter_set_scanout_callback(struct presenter *presenter, presenter_scanout_callback_t cb, void *userdata) {
    struct kms_presenter *private = PRESENTER_PRIVATE_KMS(presenter);

    private->scanout_callbacks[private->kms_display->crtc_index].callback = cb;
    private->scanout_callbacks[private->kms_display->crtc_index].userdata = userdata;

    private->kms_display->crtc->scanout_cb = cb;
    private->kms_display->crtc->scanout_cb_userdata = userdata;
    
    return 0;
}

static int kms_presenter_set_logical_zpos(struct presenter *presenter, int logical_zpos) {
    struct kms_presenter *private = PRESENTER_PRIVATE_KMS(presenter);
    if (logical_zpos >= 0) {
        logical_zpos = logical_zpos + private->kms_display->crtc->min_zpos;
    } else {
        logical_zpos = private->kms_display->crtc->max_zpos - logical_zpos + 1;
    }

    if (!IS_VALID_ZPOS(private, logical_zpos)) {
        return EINVAL;
    }

    private->current_zpos = logical_zpos;

    return 0;
}

static int kms_presenter_get_zpos(struct presenter *presenter) {
    struct kms_presenter *kms = PRESENTER_PRIVATE_KMS(presenter);
    return kms->current_zpos;
}

/**
 * @brief Tries to find an unused DRM plane and returns its index in kmsdev->planes. Otherwise returns -1.
 */
static inline int reserve_plane(struct kms_presenter *presenter, int zpos_hint) {
    (void) zpos_hint;
    for (unsigned int i = 0; i < (sizeof(presenter->free_planes) * CHAR_BIT); i++) {
        if (presenter->free_planes & (1 << i)) {
            presenter->free_planes &= ~(1 << i);
            return i;
        }
    }
    return -1;
}

/**
 * @brief Unreserves a plane index so it may be reserved using reserve_plane again.
 */
static inline void unreserve_plane(struct kms_presenter *presenter, int plane_index) {
    presenter->free_planes |= (1 << plane_index);
}

static int kms_presenter_push_drm_fb_layer(struct presenter *presenter, const struct drm_fb_layer *layer) {
    struct kms_presenter *private;
    struct drm_plane *plane;
    uint32_t plane_id;
    int ok, plane_index;
    
    DEBUG_ASSERT(presenter != NULL);
    DEBUG_ASSERT(layer != NULL);
    private = PRESENTER_PRIVATE_KMS(presenter);
    CHECK_VALID_ZPOS_OR_RETURN_EINVAL(private, private->current_zpos);

    plane_index = reserve_plane(private, private->has_primary_layer? 1 : 0);
    if (plane_index < 0) {
        LOG_MODESETTING_ERROR("Couldn't find unused plane for framebuffer layer.\n");
        ok = EINVAL;
        goto fail_return_ok;
    }

    plane = &private->dev->planes[plane_index];
    plane_id = plane->plane->plane_id;

    if (layer && (plane->property_ids.rotation == DRM_NO_PROPERTY_ID)) {
        LOG_MODESETTING_ERROR("Rotation was requested but is not supported.\n");
        ok = EINVAL;
        goto fail_unreserve_plane;
    }

    if ((plane->property_ids.zpos == DRM_NO_PROPERTY_ID) && private->has_primary_layer) {
        LOG_MODESETTING_ERROR("Plane doesn't support zpos but that's necessary for overlay planes.\n");
        ok = EINVAL;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.fb_id, layer->fb_id);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.crtc_id, private->kms_display->crtc->crtc->crtc_id);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.src_x, layer->src_x);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.src_y, layer->src_y);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.src_w, layer->src_w);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.src_h, layer->src_h);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.crtc_x, layer->crtc_x);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.crtc_y, layer->crtc_y);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }
    
    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.crtc_w, layer->crtc_w);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.crtc_h, layer->crtc_h);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    if (plane->property_ids.zpos != DRM_NO_PROPERTY_ID) {
        ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.zpos, private->current_zpos);
        if (ok < 0) {
            LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
            ok = -ok;
            goto fail_unreserve_plane;
        }
    }

    if (layer->has_rotation) {
        ok = drmModeAtomicAddProperty(private->req, plane_id, plane->property_ids.rotation, layer->rotation);
        if (ok < 0) {
            LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
            ok = -ok;
            goto fail_unreserve_plane;
        }
    }

    if (private->has_primary_layer == false) {
        memcpy(&private->primary_layer, layer, sizeof(struct drm_fb_layer));
    }

    return 0;


    fail_unreserve_plane:
    unreserve_plane(private, plane_index);

    fail_return_ok:
    return ok;
}

static int kms_presenter_push_gbm_bo_layer(struct presenter *presenter, const struct gbm_bo_layer *layer) {
    (void) presenter;
    (void) layer;
    /// TODO: Implement
    return EINVAL;
}

static int kms_presenter_push_sw_fb_layer(struct presenter *presenter, const struct sw_fb_layer *layer) {
    (void) presenter;
    (void) layer;
    /// NOTE: We _could_ copy the software framebuffer contents into a real framebuffer here.
    /// But really, the code using this should rather create a GBM BO, map it, and render into that directly.
    return ENOTSUP;
}

static int kms_presenter_push_placeholder_layer(struct presenter *presenter, int n_reserved_layers) {
    struct kms_presenter *private = PRESENTER_PRIVATE_KMS(presenter);

    if (!IS_VALID_ZPOS(private, private->current_zpos)) {
        return EOVERFLOW;
    }

    private->current_zpos += n_reserved_layers;

    return 0;
}

static int kms_presenter_flush(struct presenter *presenter) {
    struct kms_presenter *private = PRESENTER_PRIVATE_KMS(presenter);

    /// TODO: hookup callback
    if (private->dev->use_atomic_modesetting) {
        return drmModeAtomicCommit(
            private->dev->fd,
            private->req,
            DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_ATOMIC_ALLOW_MODESET,
            NULL
        );
    } else {
        return drmModePageFlip(
            private->dev->fd,
            private->kms_display->crtc->crtc->crtc_id,
            private->primary_layer.fb_id,
            DRM_MODE_PAGE_FLIP_EVENT,
            NULL
        );
    }
}

static void kms_presenter_destroy(struct presenter *presenter) {
    free(presenter);
}


static void kms_display_destroy(struct display *display) {
    struct kms_display *private;

    DEBUG_ASSERT(display != NULL);

    private = DISPLAY_PRIVATE_KMS(display);

    /// TODO: Implement

    free(private);
    free(display);
}

static void kms_display_on_destroy_mapped_buffer(struct display *display, const struct display_buffer_backend *backend, void *userdata) {
    /// TODO: Implement
}

static int kms_display_make_mapped_buffer(struct display_buffer *buffer) {
    struct kms_display *private = DISPLAY_PRIVATE_KMS(buffer->display);
    uint8_t *vmem;
    int bytes_per_pixel, stride;

    bytes_per_pixel = (get_pixfmt_info(buffer->backend.sw.format)->bits_per_pixel + 7) / 8;
    
    /// TODO: Implement

    stride = buffer->backend.sw.width * bytes_per_pixel;
    vmem = malloc(buffer->backend.sw.height * stride);
    if (vmem == NULL) {
        return ENOMEM;
    }

    buffer->backend.sw.stride = stride;
    buffer->backend.sw.vmem = vmem;
    buffer->destroy_callback = kms_display_on_destroy_mapped_buffer;
    buffer->userdata = NULL;
    return 0;
}

static void kms_display_get_supported_formats(struct display *display, const uint32_t **formats_out, size_t *n_formats_out) {
    struct kms_display *private = DISPLAY_PRIVATE_KMS(display);

    *formats_out = private->dev->crtcs[private->crtc_index].formats;
    *n_formats_out = private->dev->crtcs[private->crtc_index].n_formats;

    return 0;
}

static struct presenter *kms_display_create_presenter(struct display *display) {
    struct kms_display *display_private;
    struct kms_presenter *private;
    struct presenter *presenter;
    drmModeAtomicReq *req;
    struct kmsdev *dev;
    uint32_t free_planes;

    DEBUG_ASSERT(display != NULL);
    display_private = DISPLAY_PRIVATE_KMS(display);
    dev = display_private->dev;

    presenter = malloc(sizeof *presenter);
    if (presenter == NULL) {
        return NULL;
    }

    private = malloc(sizeof *private);
    if (private == NULL) {
        free(presenter);
        return NULL;
    }

    req = drmModeAtomicAlloc();
    if (req == NULL) {
        free(private);
        free(presenter);
        return NULL;
    }

    for (unsigned int i = 0; i < dev->n_planes; i++) {
        if ((dev->planes[i].type == DRM_PLANE_TYPE_PRIMARY) || (dev->planes[i].type == DRM_PLANE_TYPE_OVERLAY)) {
            free_planes |= (1 << i);
        }
    }

    private->dev = dev;
    private->free_planes = free_planes;
    private->req = req;
    private->has_primary_layer = false;
    memset(&private->primary_layer, 0, sizeof(private->primary_layer));
    memset(private->scanout_callbacks, 0, sizeof(private->scanout_callbacks));

    presenter->private = (struct presenter_private*) private;
    presenter->set_logical_zpos = kms_presenter_set_logical_zpos;
    presenter->get_zpos = kms_presenter_get_zpos;
    presenter->set_scanout_callback = kms_presenter_set_scanout_callback;
    presenter->push_drm_fb_layer = kms_presenter_push_drm_fb_layer;
    presenter->push_gbm_bo_layer = kms_presenter_push_gbm_bo_layer;
    presenter->push_sw_fb_layer = kms_presenter_push_sw_fb_layer;
    presenter->push_placeholder_layer = kms_presenter_push_placeholder_layer;
    presenter->flush = kms_presenter_flush;
    presenter->destroy = kms_presenter_destroy;

    return presenter;
}

static struct display *create_kms_display(
    struct kmsdev *dev,
    int crtc_index,
    bool has_dimensions,
    int width_mm, int height_mm
) {
    struct drm_connector *connector;
    struct kms_display *private;
    struct drm_crtc *crtc;
    drmModeModeInfo *mode;
    struct display *display;
    uint32_t planes;

    DEBUG_ASSERT_VALID_CRTC_INDEX(dev, crtc_index);
    
    display = malloc(sizeof *display);
    if (display == NULL) {
        return NULL;
    }

    private = malloc(sizeof *private);
    if (private = NULL) {
        free(display);
        return NULL;
    }

    crtc = dev->crtcs + crtc_index;
    mode = &crtc->selected_mode;
    connector = crtc->selected_connector;

    planes = 0;
    for (int i = 0; i < dev->n_planes; i++) {
        if (dev->planes[i].plane->possible_crtcs & crtc->bitmask) {
            planes |= (1 << i);
        }
    }

    if (has_dimensions == false) {
        if ((connector->connector->connector_type == DRM_MODE_CONNECTOR_DSI) &&
            (connector->connector->mmWidth == 0) &&
            (connector->connector->mmHeight == 0)) {
            // if it's connected via DSI, and the width & height are 0,
            //   it's probably the official 7 inch touchscreen.
            has_dimensions = true;
            width_mm = 155;
            height_mm = 86;
        } else if (((connector->connector->mmWidth % 10) != 0) || ((connector->connector->mmHeight % 10) != 0)) {
            has_dimensions = true;
            width_mm = connector->connector->mmWidth;
            height_mm = connector->connector->mmHeight;
        }
    }

    private->dev = dev;
    private->crtc_index = crtc_index;
    private->crtc = dev->crtcs + crtc_index;
    private->assigned_planes = planes;
    display->private = private;
    display->create_presenter = kms_display_create_presenter;
    display->destroy = kms_display_destroy;
    display->width = mode->hdisplay;
    display->height = mode->vdisplay;
    display->has_dimensions = has_dimensions;
    display->width_mm = width_mm;
    display->height_mm = height_mm;
    display->supports_gbm = true;
    display->gbm_device = dev->gbm_device;

    return display;
}

/**
 * @brief Configure the display setup.
 * This will choose all for all connected CRTCs:
 *   - whether they should be enabled
 *   - how many planes they'll get assigned (if the CRTC is disabled it'll probably get as few planes as possible)
 *   - what output mode to use
 *   - other things like cloning, spanning, etc
 */
int kmsdev_configure(
    struct kmsdev *dev,
    const struct kms_config *config
) {
    struct display **displays, *display;
    size_t n_displays;
    int ok, n_allocated_displays;

    DEBUG_ASSERT(dev != NULL);
    DEBUG_ASSERT(config != NULL);
    DEBUG_ASSERT(config->display_configs != NULL);
    DEBUG_ASSERT(config->n_display_configs != 0);
    
    n_displays = config->n_display_configs;

    if (dev->n_crtcs < n_displays) {
        return NULL;
    }

    displays = malloc(n_displays * (sizeof *displays));
    if (displays == NULL) {
        ok = ENOMEM;
        goto fail_return_ok;
    }

    n_allocated_displays = 0;
    for (int i = 0; i < n_displays; i++) {
        display = create_kms_display(
            dev,
            i,
            config->display_configs[i].has_explicit_dimensions,
            config->display_configs[i].width_mm,
            config->display_configs[i].height_mm
        );
        if (display == NULL) {
            goto fail_destroy_displays;
        }

        n_allocated_displays++;
    }

    dev->displays = displays;
    dev->n_displays = n_displays;

    return 0;


    fail_destroy_displays:
    for (int i = n_allocated_displays; i >= 0; i--) display_destroy(displays[i]);

    fail_free_displays:
    free(displays);

    fail_return_ok:
    return ok;
}

struct display *kmsdev_get_display(struct kmsdev *dev, int display_index) {
    DEBUG_ASSERT(dev != NULL);
    DEBUG_ASSERT(dev->displays != NULL);
    DEBUG_ASSERT(display_index < dev->n_displays);
    return dev->displays[display_index];
}

void kmsdev_get_displays(struct kmsdev *dev, struct display ***displays_out, size_t *n_displays_out) {
    DEBUG_ASSERT(dev != NULL);
    DEBUG_ASSERT(displays_out != NULL);
    *displays_out = dev->displays;
    *n_displays_out = dev->n_displays;
}
#endif