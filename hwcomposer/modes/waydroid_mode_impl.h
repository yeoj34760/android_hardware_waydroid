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

#pragma once

#include "waydroid_mode.h"

template<class T>
class compositing_window_mode : public virtual waydroid_mode {
    skipped_layers_helper skipped_layers;

    /* Whether SurfaceFlinger's client target was attached this frame.
     * Reset in setup_set. */
    bool m_fbt_drawn {false};

    T *derived() {
        return static_cast<T *>(this);
    }

  public:
    int setup_prepare(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) override {
        skipped_layers.reset();
        for (size_t i = 0; i < contents->numHwLayers; ++i) {
            const auto &layer = contents->hwLayers[i];
            /* Layers whose buffer the compositor cannot import (format+
             * modifier not advertised via zwp_linux_dmabuf, e.g. LINEAR CPU
             * buffers on NVIDIA/KWin) must be composited by SurfaceFlinger:
             * direct attach would be a fatal wayland protocol error. */
            bool undisplayable = layer.handle
                && layer.compositionType != HWC_FRAMEBUFFER_TARGET
                && !(layer.flags & HWC_IS_CURSOR_LAYER)
                && !pdev->gralloc_handler.displayable_buffer(pdev->display, layer.handle);
            skipped_layers.setup_for_each_layer(i, layer, undisplayable);
        }
        return 0;
    }
    int prepare(hwc_layer_1 *hwc_layer, size_t hwc_layer_index) override {
        /* skipped layers have to be composited by SurfaceFlinger; so in order
         * to have correct z-ordering, we must ask SurfaceFlinger to composite
         * everything between the first and the last skipped layer. */
        if (skipped_layers.has_skipped_layers()) {
            if (skipped_layers.first_skipped() <= hwc_layer_index && hwc_layer_index <= skipped_layers.last_skipped()) {
                hwc_layer->compositionType = HWC_FRAMEBUFFER;
                return 0;
            }
        }

        if (hwc_layer->compositionType == HWC_FRAMEBUFFER) {
            hwc_layer->compositionType = HWC_OVERLAY;
        }
        // TODO: Handle HWC_SIDEBAND
        return 0;
    }

    int setup_set(waydroid_hwc_composer_device_1 *, hwc_display_contents_1_t *contents) override {
        skipped_layers.setup_set(contents->hwLayers, contents->numHwLayers);
        m_fbt_drawn = false;
        return 0;
    }
    int handle_layer(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t i) override {
        /* hwc_set only visits layers that have a buffer. Skipped layers can be
         * bufferless (e.g. solid-color letterbox layers around a pillarboxed
         * app get HWC_SKIP_LAYER from HWC2On1Adapter), so "draw the client
         * target exactly at i == first_skipped()" may never trigger: the
         * target was lost AND its acquire fence leaked every frame (one
         * sync_file per frame until EMFILE killed the composer). Instead,
         * attach the client target at the first *visited* layer at-or-after
         * the skipped run, which keeps it below any later direct layers. */
        if (skipped_layers.has_skipped_layers()) {
            const bool in_run = skipped_layers.first_skipped() <= i && i <= skipped_layers.last_skipped();
            const bool is_fbt = hwc_layer == skipped_layers.framebuffer_target_layer();
            if (!m_fbt_drawn && (in_run || i > skipped_layers.last_skipped())) {
                m_fbt_drawn = true;
                hwc_layer_1 *fbt = skipped_layers.framebuffer_target_layer();
                assert(fbt);
                window *window = derived()->get_window(pdev);
                int res = apply_hwc_layer_to_window(pdev, fbt, skipped_layers.framebuffer_target_index(), window);
                if (in_run || is_fbt) {
                    /* this layer's content is part of the client target */
                    if (!is_fbt && hwc_layer->acquireFenceFd != -1) {
                        close(hwc_layer->acquireFenceFd);
                    }
                    return res;
                }
                /* fall through: draw this non-skipped layer above the target */
            } else if (in_run || is_fbt) {
                /* composited into the (already attached) client target;
                 * the target's own fence was consumed when it was drawn */
                if (!is_fbt && hwc_layer->acquireFenceFd != -1) {
                    close(hwc_layer->acquireFenceFd);
                }
                return 0;
            }
        } else if (hwc_layer == skipped_layers.framebuffer_target_layer()) {
            /* no client composition happened: the target has no new content */
            if (hwc_layer->acquireFenceFd != -1) {
                close(hwc_layer->acquireFenceFd);
            }
            return 0;
        }

        if (hwc_layer->compositionType != HWC_OVERLAY) {
            ALOGW("Encountered unsupported compositionType: %" PRId32, hwc_layer->compositionType);
            // TODO: Support HWC_BACKGROUND
            if (hwc_layer->acquireFenceFd != -1) {
                close(hwc_layer->acquireFenceFd);
            }
            return 0;
        } else if (!hwc_layer->handle) {
            return 0;
        }

        window *window = derived()->get_window(pdev);
        return apply_hwc_layer_to_window(pdev, hwc_layer, i, window);
    }

    int post_processing(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) override {
        if (contents->flags & HWC_GEOMETRY_CHANGED) {
            neutralize_remaining_subsurfaces(pdev);
        }
        return 0;
    }
};
template<class T>
class non_compositing_window_mode : public virtual waydroid_mode {
    constexpr static size_t UNSET_VALUE = std::numeric_limits<size_t>::max();

    // When we should draw the framebuffer
    // It's at the first layer with type HWC_FRAMEBUFFER or HWC_FRAMEBUFFER_TARGET
    size_t m_draw_framebuffer_at {UNSET_VALUE};

    hwc_layer_1 *m_framebuffer_target {};
    size_t m_framebuffer_target_index {UNSET_VALUE};

    T *derived() {
        return static_cast<T *>(this);
    }

  public:
    int prepare(hwc_layer_1 *hwc_layer, size_t) override {
        hwc_layer->compositionType = HWC_FRAMEBUFFER;
        return 0;
    }

    int setup_set(waydroid_hwc_composer_device_1 *, hwc_display_contents_1_t *contents) override {
        static int dbg_count = 0;
        bool dbg = (dbg_count++ % 2000) == 0;
        for (size_t i = 0; i < contents->numHwLayers; ++i) {
            auto& layer = contents->hwLayers[i];
            if (dbg)
                ALOGI("noncomp setup_set: layer[%zu] type=%d handle=%p flags=0x%x",
                      i, layer.compositionType, layer.handle, layer.flags);
            /* hwc_set only forwards layers with a buffer to handle_layer, so
             * the framebuffer must be drawn at an index that will actually be
             * visited: the first FRAMEBUFFER-ish layer that has a handle
             * (bufferless dim/effect layers otherwise swallow the draw and
             * the window never gets a buffer nor gets created). */
            if (m_draw_framebuffer_at == UNSET_VALUE
                && layer.handle
                && (layer.compositionType == HWC_FRAMEBUFFER || layer.compositionType == HWC_FRAMEBUFFER_TARGET)) {
                m_draw_framebuffer_at = i;
            }
            if (layer.compositionType == HWC_FRAMEBUFFER_TARGET) {
                m_framebuffer_target = std::addressof(layer);
                m_framebuffer_target_index = i;
                break;
            }
        }
        return 0;
    }
    int handle_layer(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t i) override {
        /*
         * SurfaceFlinger did composite all layers except the cursor layer
         * To get the right Z-order we have to draw SurfaceFlinger's target framebuffer
         * at the first occurrence of a HWC_FRAMEBUFFER or HWC_FRAMEBUFFER_TARGET layer
         */
        int res = 0;
        if (i == m_draw_framebuffer_at) {
            assert(m_framebuffer_target->handle);
            window *window = derived()->get_window(pdev);

            res = apply_hwc_layer_to_window(pdev, m_framebuffer_target, m_framebuffer_target_index, window);
        }

        // The acquireFenceFd of HWC_FRAMEBUFFER_TARGET is closed in apply_hwc_layer_to_window
        if (i != m_framebuffer_target_index) {
            if (hwc_layer->acquireFenceFd != -1) {
                close(hwc_layer->acquireFenceFd);
            }
        }
        return res;
    }
};