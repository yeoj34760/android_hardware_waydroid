/*
 * Copyright © 2025 Waydroid Project.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "gralloc_handler.h"

#include <algorithm>
#include <cstdint>
#include <memory>

#include <linux/memfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <drm_fourcc.h>
#include <wayland-client.h>

#include <log/log.h>
#include <ui/Rect.h>
#include <ui/GraphicBufferMapper.h>
#include <gralloc_handle.h>
#include <cros_gralloc/cros_gralloc_handle.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "wayland-android-client-protocol.h"

#include "egl-tools.h"
#include "prime-copy.h"
#include "wayland-hwc.h"

namespace {
    uint32_t ConvertHalFormatToShm(uint32_t hal_format) {
        uint32_t fmt;

        switch (hal_format) {
            case HAL_PIXEL_FORMAT_RGBX_8888:
                fmt = WL_SHM_FORMAT_XRGB8888;
                break;
            case HAL_PIXEL_FORMAT_RGBA_8888:
                fmt = WL_SHM_FORMAT_ARGB8888;
                break;
            default:
                ALOGE("Cannot convert hal format to shm format %u", hal_format);
                return -EINVAL;
        }
        return fmt;
    }

    const wl_buffer_listener buffer_listener {
        [](void *, struct wl_buffer *) {}
    };
}
std::unique_ptr<buffer> create_shm_wl_buffer(display *display, const buffer_metadata& metadata, buffer_handle_t handle) {
    std::unique_ptr<buffer> buf { new buffer() };

    // Assume 4bpp formats or none of this is going to work
    int shm_stride = metadata.width * 4;
    int size = shm_stride * metadata.height;

    buf->metadata = metadata;
    buf->handle = handle;

    buf->isShm = true;
    buf->size = size;

    /* ConvertHalFormatToShm() returns -EINVAL as uint32_t, so the old assert(shm_format >= 0)
     * never fired and a YUV layer (multi-window mode has no client target to compose it into)
     * became a wl_shm buffer with format 0xffffffea: invalid_format, a fatal protocol error.
     * Such a layer is not shown instead (logged). */
    const uint32_t shm_format = ConvertHalFormatToShm(metadata.format);
    if (shm_format == (uint32_t)-EINVAL) {
        ALOGE("layer format %u has no wl_shm equivalent; the layer is not shown", metadata.format);
        return nullptr;
    }

    int fd = syscall(SYS_memfd_create, "buffer", MFD_ALLOW_SEALING);
    ftruncate(fd, size);
    buf->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf->shm_data == MAP_FAILED) {
        ALOGE("mmap failed");
        close(fd);
        return nullptr;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(display->shm, fd, size);
    buf->wl_buffer = wl_shm_pool_create_buffer(pool, 0, metadata.width, metadata.height, shm_stride, shm_format);
    wl_buffer_add_listener(buf->wl_buffer, &buffer_listener, nullptr);
    wl_shm_pool_destroy(pool);
    close(fd);

    return buf;
}

namespace {
    /* The 'failed' event is the compositor's non-fatal way of rejecting an import (the other
     * is an invalid_wl_buffer protocol error, which ends the connection and is handled by
     * hwc_wayland_thread()). The buffer will not show; the connection and the HAL go on. */
    void create_failed(void *data __unused, zwp_linux_buffer_params_v1 *params __unused)
    {
        static int failures = 0;
        if ((failures++ % 100) == 0)
            ALOGE("%s: the compositor rejected a dma-buf import (zwp_linux_buffer_params "
                  "'failed', %d so far); that buffer is not shown", __func__, failures);
    }

    const struct zwp_linux_buffer_params_v1_listener params_listener = {
            [](void *, zwp_linux_buffer_params_v1 *, wl_buffer *) {},
            create_failed
    };

    bool isFormatSupported(struct display *display, uint32_t format) {
        std::lock_guard<std::mutex> lock(display->formats_mutex);
        return display->formats.count(format) == 1;
    }

    uint32_t ConvertHalFormatToDrm(struct display *display, uint32_t hal_format) {
        uint32_t fmt;

        switch (hal_format) {
            case HAL_PIXEL_FORMAT_RGB_888:
                fmt = DRM_FORMAT_BGR888;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_RGB888;
                break;
            case HAL_PIXEL_FORMAT_BGRA_8888:
                fmt = DRM_FORMAT_ARGB8888;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_ABGR8888;
                break;
            case HAL_PIXEL_FORMAT_RGBX_8888:
                fmt = DRM_FORMAT_XBGR8888;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_XRGB8888;
                break;
            case HAL_PIXEL_FORMAT_RGBA_8888:
                fmt = DRM_FORMAT_ABGR8888;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_ARGB8888;
                break;
            case HAL_PIXEL_FORMAT_RGB_565:
                fmt = DRM_FORMAT_BGR565;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_RGB565;
                break;
            case HAL_PIXEL_FORMAT_YV12:
                fmt = DRM_FORMAT_YVU420;
                if (!isFormatSupported(display, fmt))
                    fmt = DRM_FORMAT_GR88;
                break;
            default:
                ALOGE("Cannot convert hal format to drm format %u", hal_format);
                return -EINVAL;
        }
        if (!isFormatSupported(display, fmt)) {
            ALOGE("Current wayland display doesn't support hal format %u", hal_format);
            return -EINVAL;
        }
        return fmt;
    }
}
namespace {
bool compositor_supports(display *display, uint32_t drm_format, uint64_t modifier) {
    if (modifier == DRM_FORMAT_MOD_INVALID)
        return false;
    /* Compositor on another GPU (prime-copy.h): it cannot import NVIDIA block-linear, and an
     * advertised LINEAR does not help either, because gralloc's LINEAR buffers live in NVIDIA
     * video memory, which nvidia-drm exports without pages (foreign attach fails). Every layer
     * is composed by SurfaceFlinger (or copied into wl_shm); only the client target reaches the
     * compositor, through the system-memory copy. */
    if (display->present_path.load() == (int)present_path::cross_gpu) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            ALOGI("cross-GPU compositor: no layer buffer is attached directly; layers are composed "
                  "by SurfaceFlinger and the client target is copied into system memory");
        return false;
    }
    /* Search under the lock: the Wayland thread may reallocate this vector at any time. */
    std::lock_guard<std::mutex> lock(display->formats_mutex);
    /* NVIDIA's EGL imports a LINEAR dma-buf only as GL_TEXTURE_EXTERNAL_OES (eglQueryDmaBuf-
     * ModifiersEXT external_only = TRUE). A compositor that also lists NVIDIA block-linear
     * modifiers for this format imports it through NVIDIA's EGL, and if it samples client
     * buffers as GL_TEXTURE_2D (Mutter/Cogl) an advertised LINEAR is a fatal
     * zwp_linux_buffer_params error 7 ("Could not bind the given EGLImage to a
     * CoglTexture2D", waydroid-nvidia #11). So LINEAR is refused for such a format and the
     * layer is composed by SurfaceFlinger instead. Per format, not global: KWin on NVIDIA lists
     * LINEAR only for YUV formats that have no NVIDIA modifier (it handles those as external
     * textures), and hybrid iGPU compositors list no NVIDIA modifiers, so both keep LINEAR. */
    if (modifier == DRM_FORMAT_MOD_LINEAR && display->nvidia_modifier_formats.count(drm_format)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            ALOGI("compositor imports format 0x%x through NVIDIA EGL; LINEAR buffers of such "
                  "formats are composed by SurfaceFlinger", drm_format);
        return false;
    }
    auto it = display->modifiers.find(drm_format);
    if (it == display->modifiers.end())
        return false;
    const auto &mods = it->second;
    return std::find(mods.begin(), mods.end(), modifier) != mods.end();
}

/* Alpha-carrying fourcc -> layout-identical alpha-ignoring fourcc. Used for
 * HWC_BLENDING_NONE layers: their buffers' alpha channel is undefined and the
 * compositor must not blend it (SurfaceFlinger doesn't). */
uint32_t drm_format_strip_alpha(uint32_t format) {
    switch (format) {
        case DRM_FORMAT_ABGR8888:       return DRM_FORMAT_XBGR8888;
        case DRM_FORMAT_ARGB8888:       return DRM_FORMAT_XRGB8888;
        case DRM_FORMAT_ABGR2101010:    return DRM_FORMAT_XBGR2101010;
        case DRM_FORMAT_ARGB2101010:    return DRM_FORMAT_XRGB2101010;
        case DRM_FORMAT_ABGR16161616F:  return DRM_FORMAT_XBGR16161616F;
        default:                        return format;
    }
}
}

std::unique_ptr<buffer> create_dmabuf_wl_buffer(display *display, const buffer_metadata& metadata,
                                                                const dmabuf_plane *planes, unsigned num_planes,
                                                                int drm_format, uint64_t modifier, buffer_handle_t handle)
{
    assert(num_planes >= 1 && planes[0].fd >= 0);

    std::unique_ptr<buffer> buf { new buffer() };

    buf->metadata = metadata;
    buf->handle = handle;

    if (drm_format < 0) {
        drm_format = ConvertHalFormatToDrm(display, metadata.format);
    }
    assert(drm_format >= 0);

    if (metadata.opaque) {
        uint32_t stripped = drm_format_strip_alpha((uint32_t)drm_format);
        /* only if the compositor advertises the X-variant with this modifier */
        if (stripped != (uint32_t)drm_format
            && compositor_supports(display, stripped, modifier)) {
            drm_format = (int)stripped;
        }
    }

    zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(display->dmabuf);
    for (unsigned i = 0; i < num_planes; i++)
        zwp_linux_buffer_params_v1_add(params, planes[i].fd, i, planes[i].offset, planes[i].stride,
                                       modifier >> 32, modifier & 0xffffffff);
    zwp_linux_buffer_params_v1_add_listener(params, &params_listener, nullptr);

    buf->wl_buffer = zwp_linux_buffer_params_v1_create_immed(params, buf->metadata.width, buf->metadata.height, drm_format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    wl_buffer_add_listener(buf->wl_buffer, &buffer_listener, nullptr);

    return buf;
}

std::unique_ptr<buffer> create_prime_wl_buffer(display *display, const buffer_metadata& metadata, buffer_handle_t handle)
{
    if (display->gtype == GrallocType::GRALLOC_CROS) {
        auto *h = reinterpret_cast<const cros_gralloc_handle *>(handle);
        return prime_create_buffer(display, metadata, h->fds[0], h->format, h->strides[0],
                                   h->offsets[0], h->format_modifier, handle);
    }
    if (display->gtype == GrallocType::GRALLOC_GBM) {
        auto *h = reinterpret_cast<const gralloc_handle_t *>(handle);
        int drm_format = ConvertHalFormatToDrm(display, h->format);
        if (drm_format < 0)
            return nullptr;
        return prime_create_buffer(display, metadata, h->prime_fd, (uint32_t)drm_format, h->stride,
                                   0, h->modifier, handle);
    }
    ALOGE("cross-GPU copy needs a dma-buf gralloc (gbm or cros), this one is type %d; the client "
          "target is not shown", (int)display->gtype);
    return nullptr;
}

std::unique_ptr<buffer> create_android_wl_buffer(display *display, const buffer_metadata& metadata, buffer_handle_t handle)
{
    std::unique_ptr<buffer> buf { new buffer() };

    buf->metadata = metadata;
    buf->handle = handle;

    struct wl_array ints;
    wl_array_init(&ints);
    int *the_ints = (int *)wl_array_add(&ints, handle->numInts * sizeof(int));
    memcpy(the_ints, handle->data + handle->numFds, handle->numInts * sizeof(int));
    android_wlegl_handle *wlegl_handle = android_wlegl_create_handle(display->android_wlegl, handle->numFds, &ints);
    wl_array_release(&ints);

    for (int i = 0; i < handle->numFds; i++) {
        android_wlegl_handle_add_fd(wlegl_handle, handle->data[i]);
    }

    buf->wl_buffer = android_wlegl_create_buffer(display->android_wlegl, buf->metadata.width, buf->metadata.height, buf->metadata.pixel_stride, metadata.format, GRALLOC_USAGE_HW_RENDER, wlegl_handle);
    android_wlegl_handle_destroy(wlegl_handle);

    wl_buffer_add_listener(buf->wl_buffer, &buffer_listener, nullptr);

    return buf;
}

namespace {
buffer_metadata get_buffer_metadata_generic(display *display, hwc_layer_1_t *layer, size_t pos) {
    uint32_t format, pixel_stride, width, height;
    if (layer->compositionType == HWC_FRAMEBUFFER_TARGET) {
        format = display->target_layer_handle_ext.format;
        pixel_stride = display->target_layer_handle_ext.stride;
        width = display->target_layer_handle_ext.width;
        height = display->target_layer_handle_ext.height;
    } else {
        format = display->layer_handles_ext[pos].format;
        pixel_stride = display->layer_handles_ext[pos].stride;
        width = display->layer_handles_ext[pos].width;
        height = display->layer_handles_ext[pos].height;
    }

    if (!width)
        width = layer->displayFrame.right - layer->displayFrame.left;
    if (!height)
        height = layer->displayFrame.bottom - layer->displayFrame.top;

    return {height, width, pixel_stride, format};
}
/* inode of a dma-buf fd: unique per dma-buf for the kernel's lifetime (see buffer_metadata::ident) */
uint64_t dmabuf_ident(int fd) {
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        static int failures = 0;
        if ((failures++ % 500) == 0)
            ALOGE("cannot identify buffer: fstat(dma-buf fd %d) failed: %s (%d times); its cached "
                  "wl_buffer may be reused for another buffer", fd, fd < 0 ? "no fd" : strerror(errno),
                  failures);
        return 0;
    }
    return (uint64_t)st.st_ino;
}
buffer_metadata get_buffer_metadata_gbm(display *, hwc_layer_1_t *layer, size_t) {
    auto handle = reinterpret_cast<const gralloc_handle_t *>(layer->handle);
    buffer_metadata m {
            handle->height,
            handle->width,
            handle->stride,
            handle->format
    };
    m.ident = dmabuf_ident(handle->prime_fd);
    return m;
}
buffer_metadata get_buffer_metadata_cros(display *, hwc_layer_1_t *layer, size_t) {
    auto handle = reinterpret_cast<const cros_gralloc_handle *>(layer->handle);
    buffer_metadata m {
            handle->height,
            handle->width,
            handle->pixel_stride,
            static_cast<uint32_t>(handle->droid_format)
    };
    m.ident = dmabuf_ident(handle->fds[0]);
    return m;
}

std::unique_ptr<buffer> create_buffer_generic(display *display, const buffer_metadata& metadata, buffer_handle_t handle) {
    return create_shm_wl_buffer(display, metadata, handle);
}
std::unique_ptr<buffer> create_buffer_gbm(display *display, const buffer_metadata& metadata, buffer_handle_t handle) {
    auto *drm_handle = reinterpret_cast<const gralloc_handle_t *>(handle);
    /* gbm_gralloc's handle carries one fd/stride and no plane offsets; displayable_buffer_gbm()
     * admits only single-plane formats here. */
    const dmabuf_plane plane { drm_handle->prime_fd, 0, (uint32_t)drm_handle->stride };
    return create_dmabuf_wl_buffer(display, metadata, &plane, 1, -1 /* compute drm format */, drm_handle->modifier, handle);
}
std::unique_ptr<buffer> create_buffer_cros(display *display, const buffer_metadata& metadata, buffer_handle_t handle) {
    auto *cros_handle = reinterpret_cast<const cros_gralloc_handle *>(handle);
    /* displayable_buffer_cros() admitted it, so num_planes is the format's plane count and
     * every plane has an fd */
    const unsigned n = cros_handle->num_planes;
    dmabuf_plane planes[DRV_MAX_PLANES];
    for (unsigned i = 0; i < n; i++)
        planes[i] = { cros_handle->fds[i], cros_handle->offsets[i], cros_handle->strides[i] };
    if (n == 1)
        ALOGI("attach dmabuf: %ux%u fmt 0x%x stride %u mod 0x%llx usage 0x%x",
              metadata.width, metadata.height, cros_handle->format,
              cros_handle->strides[0],
              (unsigned long long)cros_handle->format_modifier,
              cros_handle->usage);
    else
        ALOGI("attach dmabuf: %ux%u fmt 0x%x planes %u (offset/stride %u/%u, %u/%u, %u/%u) "
              "mod 0x%llx usage 0x%x",
              metadata.width, metadata.height, cros_handle->format, n,
              planes[0].offset, planes[0].stride, planes[1].offset, planes[1].stride,
              n > 2 ? planes[2].offset : 0, n > 2 ? planes[2].stride : 0,
              (unsigned long long)cros_handle->format_modifier, cros_handle->usage);
    return create_dmabuf_wl_buffer(display, metadata, planes, n, cros_handle->format, cros_handle->format_modifier, handle);
}
std::unique_ptr<buffer> create_buffer_android(display *display, const buffer_metadata& metadata, buffer_handle_t handle) {
    return create_android_wl_buffer(display, metadata, handle);
}

void update_shm_buffer_generic(display *display, buffer *buffer) {
    /* egl_render_to_pixels() names the failing step itself (rate-limited); there is no other
     * source for these pixels, so a failed read-back is reported, not replaced */
    display->egl_work_queue.emplace_back([display, buffer] { egl_render_to_pixels(display, buffer); });
    sem_post(&display->egl_go);
    sem_wait(&display->egl_done);
}
void update_shm_buffer_default(display *, buffer *buffer) {
    void *data;
    uint32_t shm_stride, src_stride;
    android::Rect bounds(buffer->metadata.width, buffer->metadata.height);
    if (android::GraphicBufferMapper::get().lock(buffer->handle, GRALLOC_USAGE_SW_READ_OFTEN, bounds, &data) == 0) {
        src_stride = buffer->metadata.pixel_stride;
        shm_stride = buffer->metadata.width;
        for (int i = 0; i < buffer->metadata.height; i++) {
            uint32_t* source = (uint32_t*)data + (i * src_stride);
            uint32_t* dist = (uint32_t*)buffer->shm_data + (i * shm_stride);
            uint32_t* end = dist + shm_stride;

            while (dist < end) {
                uint32_t c = *source;
                *dist = (c & 0xFF00FF00) | ((c & 0xFF0000) >> 16) | ((c & 0xFF) << 16);
                source++;
                dist++;
            }
        }
        android::GraphicBufferMapper::get().unlock(buffer->handle);
    }
}
}

gralloc_handler::get_buffer_metadata_func gralloc_handler::select_get_buffer_metadata_impl(GrallocType gralloc_type) {
    switch (gralloc_type) {
        case GrallocType::GRALLOC_GBM:
            return get_buffer_metadata_gbm;
        case GrallocType::GRALLOC_CROS:
            return get_buffer_metadata_cros;
        case GrallocType::GRALLOC_ANDROID:
        case GrallocType::GRALLOC_DEFAULT:
            return get_buffer_metadata_generic;
        default:
            /* unreachable: every GrallocType is handled above */
            ALOGE("unknown gralloc type %d; using the generic (wl_shm) buffer path",
                  (int)gralloc_type);
            return get_buffer_metadata_generic;
    }
}

gralloc_handler::create_buffer_func gralloc_handler::select_create_buffer_impl(display *display, GrallocType gralloc_type) {
    ALOGI("create_buffer impl: gtype %d dmabuf %p wlegl %p",
          (int)gralloc_type, display->dmabuf, display->android_wlegl);
    if (gralloc_type == GrallocType::GRALLOC_GBM && display->dmabuf) {
        return create_buffer_gbm;
    } else if (gralloc_type == GrallocType::GRALLOC_CROS && display->dmabuf) {
        return create_buffer_cros;
    } else if (gralloc_type == GrallocType::GRALLOC_ANDROID && display->android_wlegl) {
        return create_buffer_android;
    } else {
        return create_buffer_generic;
    }
}

gralloc_handler::update_shm_buffer_func gralloc_handler::select_update_shm_buffer_impl(GrallocType gralloc_type) {
    if (gralloc_type == GrallocType::GRALLOC_DEFAULT) {
        return update_shm_buffer_default;
    } else {
        return update_shm_buffer_generic;
    }
}

namespace {
/* Memory planes of a DRM format as zwp_linux_buffer_params_v1 needs them: planar YUV has 2 or
 * 3, every packed YUV / RGB / single-channel format 1. */
unsigned drm_format_planes(uint32_t format) {
    switch (format) {
        case DRM_FORMAT_NV12: case DRM_FORMAT_NV21: case DRM_FORMAT_NV16: case DRM_FORMAT_NV61:
        case DRM_FORMAT_NV24: case DRM_FORMAT_NV42: case DRM_FORMAT_P010: case DRM_FORMAT_P012:
        case DRM_FORMAT_P016: case DRM_FORMAT_P210:
            return 2;
        case DRM_FORMAT_YUV420: case DRM_FORMAT_YVU420: case DRM_FORMAT_YUV422:
        case DRM_FORMAT_YVU422: case DRM_FORMAT_YUV444: case DRM_FORMAT_YVU444:
            return 3;
        default:
            return 1;
    }
}

bool is_yuv(uint32_t format) {
    switch (format) {
        case DRM_FORMAT_NV12: case DRM_FORMAT_NV21: case DRM_FORMAT_NV16: case DRM_FORMAT_NV61:
        case DRM_FORMAT_NV24: case DRM_FORMAT_NV42: case DRM_FORMAT_P010: case DRM_FORMAT_P012:
        case DRM_FORMAT_P016: case DRM_FORMAT_P210:
        case DRM_FORMAT_YUV420: case DRM_FORMAT_YVU420: case DRM_FORMAT_YUV422:
        case DRM_FORMAT_YVU422: case DRM_FORMAT_YUV444: case DRM_FORMAT_YVU444:
        case DRM_FORMAT_YUYV: case DRM_FORMAT_YVYU: case DRM_FORMAT_UYVY: case DRM_FORMAT_VYUY:
        case DRM_FORMAT_AYUV: case DRM_FORMAT_XYUV8888: case DRM_FORMAT_Y210:
        case DRM_FORMAT_Y410:
            return true;
        default:
            return false;
    }
}

bool displayable_buffer_cros(display *display, buffer_handle_t handle) {
    auto *cros_handle = reinterpret_cast<const cros_gralloc_handle *>(handle);
    const uint32_t format = cros_handle->format;
    const unsigned want = drm_format_planes(format);
    const unsigned have = cros_handle->num_planes;
    bool fds_ok = have >= 1 && have <= DRV_MAX_PLANES;
    for (unsigned i = 0; fds_ok && i < have; i++)
        fds_ok = cros_handle->fds[i] >= 0;
    if (!fds_ok || have != want) {
        static int logged = 0;
        if ((logged++ % 500) == 0)
            ALOGW("buffer fmt 0x%x has %u planes in its gralloc handle, the format needs %u (or a "
                  "plane fd is missing); composed by SurfaceFlinger (%d times)", format, have, want,
                  logged);
        return false;
    }
    /* YUV layers are composed by SurfaceFlinger, never attached:
     * - hwc1 carries no dataspace (HWC2On1Adapter drops setLayerDataspace), so a compositor
     *   would have to guess matrix, range and primaries; SurfaceFlinger knows the layer's
     *   dataspace and converts correctly;
     * - KWin 6.7 on NVIDIA advertises NV12 + LINEAR but rejects exactly that import (NV12
     *   3840x2160, 2 planes, offsets 0/8294400, pitch 3840: invalid_wl_buffer, fatal), so an
     *   advertised YUV pair is no proof the import works. */
    if (is_yuv(format)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            ALOGI("YUV layer (fmt 0x%x, %u planes): composed by SurfaceFlinger (hwc1 has no "
                  "dataspace to give the compositor)", format, have);
        return false;
    }
    return compositor_supports(display, format, cros_handle->format_modifier);
}

bool displayable_buffer_gbm(display *display, buffer_handle_t handle) {
    auto *drm_handle = reinterpret_cast<const gralloc_handle_t *>(handle);
    int drm_format = ConvertHalFormatToDrm(display, drm_handle->format);
    if (drm_format < 0)
        return false;
    /* the gbm_gralloc handle has one fd and no plane offsets: multi-planar formats
     * (YV12 -> YVU420) cannot be described to the compositor */
    if (drm_format_planes((uint32_t)drm_format) != 1 || is_yuv((uint32_t)drm_format))
        return false;
    return compositor_supports(display, (uint32_t)drm_format, drm_handle->modifier);
}

/* android_wlegl / wl_shm transports have no dmabuf format negotiation */
bool displayable_buffer_always(display *, buffer_handle_t) {
    return true;
}
}

gralloc_handler::displayable_buffer_func gralloc_handler::select_displayable_buffer_impl(display *display, GrallocType gralloc_type) {
    if (gralloc_type == GrallocType::GRALLOC_GBM && display->dmabuf) {
        return displayable_buffer_gbm;
    } else if (gralloc_type == GrallocType::GRALLOC_CROS && display->dmabuf) {
        return displayable_buffer_cros;
    } else {
        return displayable_buffer_always;
    }
}

gralloc_handler::gralloc_handler(display *display)
    : get_buffer_metadata_impl(select_get_buffer_metadata_impl(display->gtype))
    , create_buffer_impl(select_create_buffer_impl(display, display->gtype))
    , update_shm_buffer_impl(select_update_shm_buffer_impl(display->gtype))
    , displayable_buffer_impl(select_displayable_buffer_impl(display, display->gtype)) { }