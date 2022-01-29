#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <gst/video/video.h>
#include <gst/allocators/allocators.h>

#include <flutter-pi.h>
#include <texture_registry.h>
#include <plugins/gstreamer_video_player.h>

#define LOG_ERROR(...) fprintf(stderr, "[gstreamer video player] " __VA_ARGS__)

#define MAX_N_PLANES 4

struct video_frame {
    GstBuffer *buffer;

    struct frame_interface *interface;

    uint32_t drm_format;

    int n_dmabuf_fds;
    int dmabuf_fds[MAX_N_PLANES];

    EGLImage image;
    size_t width, height;

    struct gl_texture_frame gl_frame;
};

struct frame_interface *frame_interface_new(struct flutterpi *flutterpi) {
    struct frame_interface *interface;
    EGLBoolean egl_ok;
    EGLContext context;
    EGLDisplay display;

    interface = malloc(sizeof *interface);
    if (interface == NULL) {
        return NULL;
    }

    display = flutterpi_get_egl_display(flutterpi);
    if (display == EGL_NO_DISPLAY) {
        goto fail_free;
    }

    context = flutterpi_create_egl_context(flutterpi);
    if (context == EGL_NO_CONTEXT) {
        goto fail_free;
    }

    PFNEGLCREATEIMAGEKHRPROC create_image = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
    if (create_image == NULL) {
        LOG_ERROR("Could not resolve eglCreateImageKHR egl procedure.\n");
        goto fail_destroy_context;
    }

    PFNEGLDESTROYIMAGEKHRPROC destroy_image = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
    if (destroy_image == NULL) {
        LOG_ERROR("Could not resolve eglDestroyImageKHR egl procedure.\n");
        goto fail_destroy_context;
    }

    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_egl_image_target_texture2d = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (gl_egl_image_target_texture2d == NULL) {
        LOG_ERROR("Could not resolve glEGLImageTargetTexture2DOES egl procedure.\n");
        goto fail_destroy_context;
    }

    // These two are optional.
    // Might be useful in the future.
    PFNEGLQUERYDMABUFFORMATSEXTPROC egl_query_dmabuf_formats = (PFNEGLQUERYDMABUFFORMATSEXTPROC) eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC egl_query_dmabuf_modifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC) eglGetProcAddress("eglQueryDmaBufModifiersEXT");

    interface->gbm_device = flutterpi_get_gbm_device(flutterpi);
    interface->display = display;
    pthread_mutex_init(&interface->context_lock, NULL); 
    interface->context = context;
    interface->eglCreateImageKHR = create_image;
    interface->eglDestroyImageKHR = destroy_image;
    interface->glEGLImageTargetTexture2DOES = gl_egl_image_target_texture2d;
    interface->supports_extended_imports = false;
    interface->eglQueryDmaBufFormatsEXT = egl_query_dmabuf_formats;
    interface->eglQueryDmaBufModifiersEXT = egl_query_dmabuf_modifiers;
    interface->n_refs = REFCOUNT_INIT_1;
    return interface;

    fail_destroy_context:
    egl_ok = eglDestroyContext(display, context);
    DEBUG_ASSERT_EGL_TRUE(egl_ok);

    fail_free:
    free(interface);
    return NULL;
}

void frame_interface_destroy(struct frame_interface *interface) {
    EGLBoolean egl_ok;

    pthread_mutex_destroy(&interface->context_lock);
    egl_ok = eglDestroyContext(interface->display, interface->context);
    DEBUG_ASSERT_EGL_TRUE(egl_ok);
    free(interface);
}

DEFINE_REF_OPS(frame_interface, n_refs)

/**
 * @brief Create a dmabuf fd from the given GstBuffer.
 * 
 * Calls gst_buffer_map on the buffer, so buffer could have changed after the call.
 * 
 */
int dup_gst_buffer_as_dmabuf(struct gbm_device *gbm_device, GstBuffer *buffer) {
    struct gbm_bo *bo;
    GstMapInfo map_info;
    uint32_t stride;
    gboolean gst_ok;
    void *map, *map_data;
    int fd;
    
    gst_ok = gst_buffer_map(buffer, &map_info, GST_MAP_READ);
    if (gst_ok == FALSE) {
        LOG_ERROR("Couldn't map gstreamer video frame buffer to copy it into a dma buffer.\n");
        return -1;
    }

    bo = gbm_bo_create(gbm_device, map_info.size, 1, GBM_FORMAT_R8, GBM_BO_USE_LINEAR);
    if (bo == NULL) {
        LOG_ERROR("Couldn't create GBM BO to copy video frame into.\n");
        goto fail_unmap_buffer;
    }

    map_data = NULL;
    map = gbm_bo_map(bo, 0, 0, map_info.size, 1, GBM_BO_TRANSFER_WRITE, &stride, &map_data);
    if (map == NULL) {
        LOG_ERROR("Couldn't mmap GBM BO to copy video frame into it.\n");
        goto fail_destroy_bo;
    }

    memcpy(map, map_info.data, map_info.size);

    gbm_bo_unmap(bo, map_data);

    fd = gbm_bo_get_fd(bo);
    if (fd < 0) {
        LOG_ERROR("Couldn't filedescriptor of video frame GBM BO.\n");
        goto fail_destroy_bo;
    }

    /// TODO: Should we dup the fd before we destroy the bo? 
    gbm_bo_destroy(bo);
    gst_buffer_unmap(buffer, &map_info);
    return fd;

    fail_destroy_bo:
    gbm_bo_destroy(bo);

    fail_unmap_buffer:
    gst_buffer_unmap(buffer, &map_info);
    return -1;
}

struct video_frame *frame_new(
    struct frame_interface *interface,
    const struct frame_info *info,
    GstBuffer *buffer
) {
#   define PUT_ATTR(_key, _value) do { *attr_cursor++ = _key; *attr_cursor++ = _value; } while (false)
    struct video_frame *frame;
    GstVideoMeta *meta;
    EGLBoolean egl_ok;
    EGLImage egl_image;
    GstMemory *memory;
    GLenum gl_error;
    EGLint attributes[2*7 + MAX_N_PLANES*2*5 + 1], *attr_cursor;
    GLuint texture;
    EGLint egl_error;
    bool is_dmabuf_memory;
    int dmabuf_fd, n_mems, n_planes, width, height;

    struct {
        int fd;
        int offset;
        int pitch;
        bool has_modifier;
        uint64_t modifier;
    } planes[MAX_N_PLANES];

    frame = malloc(sizeof *frame);
    if (frame == NULL) {
        goto fail_unref_buffer;
    }

    memory = gst_buffer_peek_memory(buffer, 0);
    is_dmabuf_memory = gst_is_dmabuf_memory(memory);
    n_mems = gst_buffer_n_memory(buffer);

    if (is_dmabuf_memory) {
        dmabuf_fd = dup(gst_dmabuf_memory_get_fd(memory));
    } else {
        dmabuf_fd = dup_gst_buffer_as_dmabuf(interface->gbm_device, buffer);
        
        //LOG_ERROR("Only dmabuf memory is supported for video frame buffers right now, but gstreamer didn't provide a dmabuf memory buffer.\n");
        //goto fail_free_frame;
    }

    if (n_mems > 1) {
        LOG_ERROR("Multiple dmabufs for a single frame buffer is not supported right now.\n");
        goto fail_free_frame;
    }

    width = GST_VIDEO_INFO_WIDTH(info->gst_info);
    height = GST_VIDEO_INFO_HEIGHT(info->gst_info);
    n_planes = GST_VIDEO_INFO_N_PLANES(info->gst_info);

    meta = gst_buffer_get_video_meta(buffer);
    if (meta != NULL) {
        for (int i = 0; i < n_planes; i++) {
            planes[i].fd = dmabuf_fd;
            planes[i].offset = meta->offset[i];
            planes[i].pitch = meta->stride[i];
            planes[i].has_modifier = false;
            planes[i].modifier = DRM_FORMAT_MOD_LINEAR;
        }
    } else {
        for (int i = 0; i < n_planes; i++) {
            planes[i].fd = dmabuf_fd;
            planes[i].offset = GST_VIDEO_INFO_PLANE_OFFSET(info->gst_info, i);
            planes[i].pitch = GST_VIDEO_INFO_PLANE_STRIDE(info->gst_info, i);
            planes[i].has_modifier = false;
            planes[i].modifier = DRM_FORMAT_MOD_LINEAR;
        }
    }

    attr_cursor = attributes;

    // first, put some of our basic attributes like
    // frame size and format
    PUT_ATTR(EGL_WIDTH, width);
    PUT_ATTR(EGL_HEIGHT, height);
    PUT_ATTR(EGL_LINUX_DRM_FOURCC_EXT, info->drm_format);

    // if we have a color space, put that too
    // could be one of EGL_ITU_REC601_EXT, EGL_ITU_REC709_EXT or EGL_ITU_REC2020_EXT
    if (info->egl_color_space != EGL_NONE) {
        PUT_ATTR(EGL_YUV_COLOR_SPACE_HINT_EXT, info->egl_color_space);
    }

    // if we have information about the sample range, put that into the attributes too
    if (GST_VIDEO_INFO_COLORIMETRY(info->gst_info).range == GST_VIDEO_COLOR_RANGE_0_255) {
        PUT_ATTR(EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_FULL_RANGE_EXT);
    } else if (GST_VIDEO_INFO_COLORIMETRY(info->gst_info).range == GST_VIDEO_COLOR_RANGE_16_235) {
        PUT_ATTR(EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT);
    }

    // Check that we can actually represent that siting info using the attributes EGL gives us.
    // For example, we can't represent GST_VIDEO_CHROMA_SITE_ALT_LINE.
    if ((GST_VIDEO_INFO_CHROMA_SITE(info->gst_info) & ~(GST_VIDEO_CHROMA_SITE_H_COSITED | GST_VIDEO_CHROMA_SITE_V_COSITED)) == 0) {
        if (GST_VIDEO_INFO_CHROMA_SITE(info->gst_info) & GST_VIDEO_CHROMA_SITE_H_COSITED) {
            PUT_ATTR(EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_EXT);
        } else {
            PUT_ATTR(EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_5_EXT);
        }
        if (GST_VIDEO_INFO_CHROMA_SITE(info->gst_info) & GST_VIDEO_CHROMA_SITE_V_COSITED) {
            PUT_ATTR(EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_EXT);
        } else {
            PUT_ATTR(EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT, EGL_YUV_CHROMA_SITING_0_5_EXT);
        }
    }
    
    // now begin with putting in information about plane memory
    PUT_ATTR(EGL_DMA_BUF_PLANE0_FD_EXT, planes[0].fd);
    PUT_ATTR(EGL_DMA_BUF_PLANE0_OFFSET_EXT, planes[0].offset);
    PUT_ATTR(EGL_DMA_BUF_PLANE0_PITCH_EXT, planes[0].pitch);
    if (planes[0].has_modifier) {
        if (interface->supports_extended_imports) {
            PUT_ATTR(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, uint32_to_int32(planes[0].modifier & 0xFFFFFFFFlu));
            PUT_ATTR(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, uint32_to_int32(planes[0].modifier >> 32));
        } else {
            LOG_ERROR("video frame buffer uses modified format but EGL doesn't support the EGL_EXT_image_dma_buf_import_modifiers extension.\n");
            goto fail_close_dmabuf_fd;
        }
    }

    if (n_planes >= 2) {
        PUT_ATTR(EGL_DMA_BUF_PLANE1_FD_EXT, planes[1].fd);
        PUT_ATTR(EGL_DMA_BUF_PLANE1_OFFSET_EXT, planes[1].offset);
        PUT_ATTR(EGL_DMA_BUF_PLANE1_PITCH_EXT, planes[1].pitch);
        if (planes[1].has_modifier) {
            if (interface->supports_extended_imports) {
                PUT_ATTR(EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, uint32_to_int32(planes[1].modifier & 0xFFFFFFFFlu));
                PUT_ATTR(EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, uint32_to_int32(planes[1].modifier >> 32));
            } else {
                LOG_ERROR("video frame buffer uses modified format but EGL doesn't support the EGL_EXT_image_dma_buf_import_modifiers extension.\n");
                goto fail_close_dmabuf_fd;
            }
        }
    }

    if (n_planes >= 3) {
        PUT_ATTR(EGL_DMA_BUF_PLANE2_FD_EXT, planes[2].fd);
        PUT_ATTR(EGL_DMA_BUF_PLANE2_OFFSET_EXT, planes[2].offset);
        PUT_ATTR(EGL_DMA_BUF_PLANE2_PITCH_EXT, planes[2].pitch);
        if (planes[2].has_modifier) {
            if (interface->supports_extended_imports) {
                PUT_ATTR(EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, uint32_to_int32(planes[2].modifier & 0xFFFFFFFFlu));
                PUT_ATTR(EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, uint32_to_int32(planes[2].modifier >> 32));
            } else {
                LOG_ERROR("video frame buffer uses modified format but EGL doesn't support the EGL_EXT_image_dma_buf_import_modifiers extension.\n");
                goto fail_close_dmabuf_fd;
            }
        }
    }

    if (n_planes >= 4) {
        if (!interface->supports_extended_imports) {
            LOG_ERROR("The video frame has more than 3 planes but that can't be imported as a GL texture if EGL doesn't support the EGL_EXT_image_dma_buf_import_modifiers extension.\n");
            goto fail_close_dmabuf_fd;
        }

        PUT_ATTR(EGL_DMA_BUF_PLANE3_FD_EXT, planes[3].fd);
        PUT_ATTR(EGL_DMA_BUF_PLANE3_OFFSET_EXT, planes[3].offset);
        PUT_ATTR(EGL_DMA_BUF_PLANE3_PITCH_EXT, planes[3].pitch);
        if (planes[3].has_modifier) {
            if (interface->supports_extended_imports) {
                PUT_ATTR(EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, uint32_to_int32(planes[3].modifier & 0xFFFFFFFFlu));
                PUT_ATTR(EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT, uint32_to_int32(planes[3].modifier >> 32));
            } else {
                LOG_ERROR("video frame buffer uses modified format but EGL doesn't support the EGL_EXT_image_dma_buf_import_modifiers extension.\n");
                goto fail_close_dmabuf_fd;
            }
        }
    }

    // add a EGL_NONE to mark the end of the buffer
    *attr_cursor++ = EGL_NONE;

    egl_image = interface->eglCreateImageKHR(interface->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attributes);
    if (egl_image == EGL_NO_IMAGE_KHR) {
        goto fail_close_dmabuf_fd;
    }

    frame_interface_lock(interface);

    egl_ok = eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, interface->context);
    if (egl_ok == EGL_FALSE) {
        egl_error = eglGetError();
        LOG_ERROR("Could not make EGL context current. eglMakeCurrent: %" PRId32 "\n", egl_error);
        goto fail_unlock_interface;
    }

    glGenTextures(1, &texture);
    if (texture == 0) {
        gl_error = glGetError();
        LOG_ERROR("Could not create GL texture. glGenTextures: %" PRIu32 "\n", gl_error);
        goto fail_clear_context;
    }

    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    interface->glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, egl_image);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    egl_ok = eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_ok == EGL_FALSE) {
        egl_error = eglGetError();
        LOG_ERROR("Could not clear EGL context. eglMakeCurrent: %" PRId32 "\n", egl_error);
        goto fail_delete_texture;
    }

    frame_interface_unlock(interface);

    /// TODO: The examples do this, but I'm not sure it's a good idea.
    /// What if, instead of closing the underlying dmabuf fd, gstreamer decides to put
    /// it in a pool of unused dmabufs and reuse it later?
    gst_buffer_unref(buffer);

    frame->buffer = buffer;
    frame->interface = frame_interface_ref(interface);
    frame->drm_format = info->drm_format;
    frame->n_dmabuf_fds = 1;
    frame->dmabuf_fds[0] = dmabuf_fd;
    frame->image = egl_image;
    frame->gl_frame.target = GL_TEXTURE_EXTERNAL_OES;
    frame->gl_frame.name = texture;
    frame->gl_frame.format = GL_RGBA8_OES;
    frame->gl_frame.width = 0;
    frame->gl_frame.height = 0;

    return frame;

    fail_delete_texture:
    glDeleteTextures(1, &texture);

    fail_clear_context:
    eglMakeCurrent(interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    fail_unlock_interface:
    frame_interface_unlock(interface);
    interface->eglDestroyImageKHR(interface->display, egl_image);

    fail_close_dmabuf_fd:
    close(dmabuf_fd);

    fail_free_frame:
    free(frame);

    fail_unref_buffer:
    gst_buffer_unref(buffer);
    return NULL;

#   undef PUT_ATTR
}

void frame_destroy(struct video_frame *frame) {
    EGLBoolean egl_ok;
    int ok;
    /// TODO: See TODO in frame_new 
    // gst_buffer_unref(frame->buffer);

    frame_interface_lock(frame->interface);
    egl_ok = eglMakeCurrent(frame->interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, frame->interface->context);
    DEBUG_ASSERT_EGL_TRUE(egl_ok);
    glDeleteTextures(1, &frame->gl_frame.name);
    DEBUG_ASSERT(GL_NO_ERROR == glGetError());
    egl_ok = eglMakeCurrent(frame->interface->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    DEBUG_ASSERT_EGL_TRUE(egl_ok);
    frame_interface_unlock(frame->interface);
    
    egl_ok = frame->interface->eglDestroyImageKHR(frame->interface->display, frame->image);
    DEBUG_ASSERT_EGL_TRUE(egl_ok);
    frame_interface_unref(frame->interface);
    for (int i = 0; i < frame->n_dmabuf_fds; i++) {
        ok = close(frame->dmabuf_fds[i]);
        DEBUG_ASSERT(ok == 0);
    }
    free(frame);
}

const struct gl_texture_frame *frame_get_gl_frame(struct video_frame *frame) {
    return &frame->gl_frame;
}
