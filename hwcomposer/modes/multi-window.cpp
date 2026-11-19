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

#include "multi-window.h"

#include <log/log.h>
#include <cutils/properties.h>

constexpr hwc_color_t color_transparent = {0, 0, 0, 0};

window *multi_window_mode::get_window(waydroid_hwc_composer_device_1 *pdev, layer_info &layer_info) {
    auto &windows = pdev->display->windows;
    if (layer_info.type == LayerSplitType::TID) {
        // Create windows based on Task ID in layer name
        if (is_blacklisted(pdev, layer_info.aid, layer_info.component))
            return nullptr;

        auto it = windows.find(layer_info.tid);
        if (it != windows.end()) {
            return it->second.get();
        } else {
            return windows.add(pdev, layer_info.tid, layer_info.aid, layer_info.tid, color_transparent);
        }
    } else if (layer_info.type == LayerSplitType::RawName) {
        if (layer_info.aid == "InputMethod") {
            auto it = windows.find("InputMethod");
            if (it != windows.end()) {
                return it->second.get();
            } else {
                return windows.add(pdev, "InputMethod", "InputMethod", "none", color_transparent);
            }
        }
    }
    return nullptr;
}

bool multi_window_mode::can_handle_layer(const hwc_layer_1& layer) {
    return layer.compositionType == HWC_OVERLAY && !(layer.flags & HWC_SKIP_LAYER);
}

int multi_window_mode::prepare(hwc_layer_1* hwc_layer, size_t) {
    if (hwc_layer->compositionType == HWC_FRAMEBUFFER) {
        hwc_layer->compositionType = HWC_OVERLAY;
    }
    // TODO: Handle HWC_SIDEBAND
    return 0;
}

void multi_window_mode::route_input_method(waydroid_hwc_composer_device_1 *pdev,
                                                hwc_display_contents_1_t *contents) {
    const int focused = pdev->display->focused_task.load(std::memory_order_relaxed);
    const std::string focused_tid = std::to_string(focused);
    const layer_info *owner = nullptr;
    for (size_t i = 0; i < contents->numHwLayers; ++i) {
        const auto &info = layer_infos[i];
        const auto &layer = contents->hwLayers[i];
        if (info.type != LayerSplitType::TID || !can_handle_layer(layer) || !layer.handle
            || (layer.flags & HWC_IS_CURSOR_LAYER)
            || is_blacklisted(pdev, info.aid, info.component))
            continue;
        if (focused >= 0 && info.tid != focused_tid)
            continue;
        if (owner && owner->tid != info.tid)
            return; // no host focus yet and more than one possible app
        owner = &info;
    }
    if (!owner)
        return; // retain the existing standalone fallback without a known owner
    for (size_t i = 0; i < contents->numHwLayers; ++i) {
        auto &info = layer_infos[i];
        if (info.type == LayerSplitType::RawName && info.aid == "InputMethod")
            info = *owner;
    }
}

int multi_window_mode::setup_set(waydroid_hwc_composer_device_1* pdev, hwc_display_contents_1_t* contents) {
    layer_infos.setup(pdev, contents->hwLayers, contents->numHwLayers);
    content_frames.clear();
    if (pdev->display->resizeFollowsWindow && !pdev->display->resizeUsesReconnect)
        display_resize_arm(pdev->display, 0, 0); // clear an earlier single-window override
    if (pdev->display->wantDecorations) {
        /* Freeform windows use the output scale, independently of the last full UI window. */
        pdev->display->scale = pdev->display->output_scale;
        pdev->display->present_x = pdev->display->present_y = 0;
        for (size_t i = 0; i < contents->numHwLayers; ++i) {
            const auto &layer = contents->hwLayers[i];
            if (!can_handle_layer(layer) || !layer.handle || (layer.flags & HWC_IS_CURSOR_LAYER)
                || layer_infos[i].type != LayerSplitType::TID)
                continue;
            const auto &frame = layer.displayFrame;
            if (frame.right <= frame.left || frame.bottom <= frame.top)
                continue;
            auto result = content_frames.emplace(layer_infos[i].tid, frame);
            auto &bounds = result.first->second;
            bounds.left = std::min(bounds.left, frame.left);
            bounds.top = std::min(bounds.top, frame.top);
            bounds.right = std::max(bounds.right, frame.right);
            bounds.bottom = std::max(bounds.bottom, frame.bottom);
        }
    }
    // Overlay buffers must not contribute their display-sized bounds to the app.
    route_input_method(pdev, contents);
    return 0;
}

int multi_window_mode::cleanup_stale_windows(waydroid_hwc_composer_device_1* pdev,
                                             hwc_display_contents_1_t* contents) {
    pdev->display->windows.erase_if([&](const auto& it){
        const auto& key = it.first;
        for (size_t i = 0; i < contents->numHwLayers; ++i) {
            const auto &layer_info = layer_infos[i];
            const auto &layer = contents->hwLayers[i];
            if (can_handle_layer(layer)
                && layer_info.type != LayerSplitType::Empty && layer_info.key() == key) {
                return false;
            }
        }
        /* SurfaceFlinger can omit every task layer while Android relays out
         * a resized app. Keep its native window, position and focus through
         * that gap instead of destroying and recreating the toplevel. */
        return !it.second->awaiting_task_relayout();
    });

    pdev->display->ignored_apps.clear();

    return 0;
}

int multi_window_mode::handle_layer(waydroid_hwc_composer_device_1* pdev, hwc_layer_1* hwc_layer, size_t i) {
    if (!can_handle_layer(*hwc_layer)) {
        if (!(hwc_layer->flags & HWC_SKIP_LAYER) && hwc_layer->compositionType != HWC_FRAMEBUFFER_TARGET) {
            // TODO: Support HWC_BACKGROUND
            ALOGW("Encountered unsupported compositionType: %" PRId32, hwc_layer->compositionType);
        }
        if (hwc_layer->acquireFenceFd != -1) {
            close(hwc_layer->acquireFenceFd);
        }
        return 0;
    }

    assert(hwc_layer->handle);
    window *window = get_window(pdev, layer_infos[i]);

    if (window) {
        auto frame = content_frames.find(layer_infos[i].tid);
        if (window->uses_task_geometry() && window->lastLayer == 0 && frame != content_frames.end())
            window->set_content_frame(frame->second);
        return apply_hwc_layer_to_window(pdev, hwc_layer, i, window);
    } else {
        if (hwc_layer->acquireFenceFd != -1) {
            close(hwc_layer->acquireFenceFd);
        }
        return 0;
    }
}

int multi_window_mode::post_processing(waydroid_hwc_composer_device_1 *pdev, hwc_display_contents_1_t *contents) {
    (void)contents;
    // Focus can move the IME between apps without changing HWC layer geometry.
    neutralize_remaining_subsurfaces(pdev);
    return 0;
}