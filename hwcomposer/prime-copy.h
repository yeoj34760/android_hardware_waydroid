/*
 * Cross-GPU ("PRIME") presentation of the client target.
 *
 * When the Wayland compositor renders on another GPU than the NVIDIA GPU the guest renders on
 * (hybrid laptops: KWin/Mutter on the Intel or AMD iGPU), it cannot import NVIDIA block-linear
 * dma-bufs (zwp_linux_buffer_params error 7), and it cannot import NVIDIA video memory at all
 * (nvidia-drm exports NVKMS video memory without pages). NVIDIA's own Wayland WSI handles the same
 * case by copying each presented image, after rendering, on the copy engine into a system-memory
 * LINEAR buffer with a 256-byte aligned pitch and presenting that buffer with
 * DRM_FORMAT_MOD_LINEAR; without explicit sync the copy is waited for on the CPU before the
 * commit. This file does the same for the client target, through the guest's Vulkan driver.
 *
 * Copyright 2026 waydroid-nvidia-prolapse. Apache-2.0.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <sys/types.h>

#include <cutils/native_handle.h>

struct display;
struct buffer;
struct buffer_metadata;

/* Where the client target goes. Decided from zwp_linux_dmabuf_v1 v4 default feedback. */
enum class present_path : int {
    unknown = 0,     /* no feedback (dmabuf < v4) or not decided yet: direct, as before */
    direct = 1,      /* compositor on the NVIDIA GPU: block-linear buffers attached directly */
    cross_gpu = 2,   /* compositor on another GPU: client target copied into sysmem LINEAR */
};

/* The NVIDIA DRM device of this guest (render node, and its primary node when sysfs shows it).
 * Returns false (and logs why) when no nvidia-drm render node is visible. */
bool prime_find_nvidia_devices(dev_t *render, dev_t *primary);

/* A client-target buffer presented through a copy. buf->wl_buffer is borrowed from the copy
 * target pool and changes every frame; prime_copy() selects it. */
std::unique_ptr<buffer> prime_create_buffer(display *display, const buffer_metadata &metadata,
                                            int dma_fd, uint32_t drm_format, uint32_t stride,
                                            uint32_t offset, uint64_t modifier,
                                            buffer_handle_t handle);
/* Copy buf's source into a free sysmem LINEAR target and point buf->wl_buffer at it. Call after
 * the layer's acquire fence has signalled and before attach. Returns 0 on success. */
int prime_copy(display *display, buffer *buf);
/* Called from ~buffer for prime buffers. */
void prime_release_source(void *source);
