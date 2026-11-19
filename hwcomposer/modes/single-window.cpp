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

#include "single-window.h"

#include <log/log.h>
#include <cutils/properties.h>

bool single_window_mode_base::should_show() const {
    return !target_layer_tid.empty();
}

window *single_window_mode_base::get_window(waydroid_hwc_composer_device_1 *pdev) const {
    assert(should_show());

    auto &windows = pdev->display->windows;
    auto it = windows.find(target_layer_tid);
    if (it != windows.end()) {
        return it->second.get();
    } else {
        return windows.add(pdev, target_layer_tid, target_layer_aid, target_layer_tid);
    }
}

int single_window_mode_base::setup_set(waydroid_hwc_composer_device_1* pdev, hwc_display_contents_1_t* contents) {
    layer_infos.setup(pdev, contents->hwLayers, contents->numHwLayers);
    return 0;
}

int single_window_mode_base::cleanup_stale_windows(waydroid_hwc_composer_device_1* pdev,
                                                   hwc_display_contents_1_t*) {
    /* Pick the first TID layer that is not blacklisted. The launcher layer
     * (blacklisted by default) legitimately sits below non-fullscreen app
     * windows in SF's bottom-to-top layer list; treating "first TID layer is
     * blacklisted" as "close everything" made any portrait/windowed app
     * (e.g. games with pillarboxing) show NO window at all. Only close the
     * windows when every TID layer on screen is blacklisted.
     */
    auto tid_layers_end = layer_infos.container().cend();
    auto first_tid_layer = tid_layers_end;
    bool saw_tid_layer = false;
    for (auto it2 = layer_infos.container().cbegin(); it2 != tid_layers_end; ++it2) {
        if (it2->type != LayerSplitType::TID)
            continue;
        saw_tid_layer = true;
        if (!is_blacklisted(pdev, it2->aid, it2->component)) {
            first_tid_layer = it2;
            break;
        }
    }
    if (first_tid_layer != tid_layers_end) {
        if (pdev->display->ignored_apps.count(first_tid_layer->tid) == 0) {
            target_layer_tid = first_tid_layer->tid;
            target_layer_aid = first_tid_layer->aid;
        } else {
            static int ign_count = 0;
            if ((ign_count++ % 500) == 0)
                ALOGI("single-window: tid %s (%s) in ignored_apps -> no window",
                      first_tid_layer->tid.c_str(), first_tid_layer->aid.c_str());
        }
        static int tgt_count = 0;
        if ((tgt_count++ % 2000) == 0)
            ALOGI("single-window: target tid='%s' aid='%s' should_show=%d",
                  target_layer_tid.c_str(), target_layer_aid.c_str(),
                  (int)should_show());
    } else if (saw_tid_layer) {
        ALOGI("single-window: all TID layers blacklisted -> no window");
        clear_open_windows(pdev);
        return 0;
    } else {
        static int no_tid_count = 0;
        if ((no_tid_count++ % 500) == 0) {
            ALOGI("single-window: NO TID layer among %zu layers (window stays closed)",
                  layer_infos.container().size());
            for (size_t i = 0; i < pdev->display->layer_names.size() && i < 8; i++)
                ALOGI("  layer_name[%zu] = '%s'", i, pdev->display->layer_names[i].c_str());
        }
    }

    // Close leftover window from full-ui mode
    pdev->display->windows.erase("Waydroid");

    // Snapshot non-active windows
    if (should_show()) {
        for (auto const& [layer_tid, window] : pdev->display->windows) {
            // Replace inactive app window buffer with snapshot
            if (layer_tid != target_layer_tid && !window->snapshot_buffer) {
                pdev->display->egl_work_queue.push_back(std::bind(snapshot_inactive_app_window, pdev->display, window.get()));
            }
        }
        if (!pdev->display->egl_work_queue.empty()) {
            sem_post(&pdev->display->egl_go);
            sem_wait(&pdev->display->egl_done);
        }
    }

    // Clear ignored apps that are closed now
    auto &ignored_apps = pdev->display->ignored_apps;
    auto it = ignored_apps.begin();
    while (it != ignored_apps.end()) {
        bool still_shown = std::any_of(layer_infos.container().begin(), layer_infos.container().end(), [&it](const auto &layer_info){
            return layer_info.type == LayerSplitType::TID && layer_info.tid == *it;
        });

        if (still_shown) {
            ++it;
        } else {
            it = pdev->display->ignored_apps.erase(it);
        }
    }

    return 0;
}


int non_compositing_single_window_mode::setup_set(waydroid_hwc_composer_device_1* pdev, hwc_display_contents_1_t* contents) {
    int res = Base::setup_set(pdev, contents);
    if (res != 0)
        return res;
    return single_window_mode_base::setup_set(pdev, contents);
}

int non_compositing_single_window_mode::handle_layer(waydroid_hwc_composer_device_1* pdev, hwc_layer_1* hwc_layer, size_t i) {
    if (!should_show()) {
        if (hwc_layer->acquireFenceFd != -1) {
            close(hwc_layer->acquireFenceFd);
        }
        return 0;
    }

    return Base::handle_layer(pdev, hwc_layer, i);
}


int compositing_single_window_mode::setup_set(waydroid_hwc_composer_device_1* pdev, hwc_display_contents_1_t* contents) {
    int res = Base::setup_set(pdev, contents);
    if (res != 0)
        return res;
    return single_window_mode_base::setup_set(pdev, contents);
}

int compositing_single_window_mode::handle_layer(waydroid_hwc_composer_device_1* pdev, hwc_layer_1* hwc_layer, size_t i) {
    if (!should_show()) {
        if (hwc_layer->acquireFenceFd != -1) {
            close(hwc_layer->acquireFenceFd);
        }
        return 0;
    }

    return Base::handle_layer(pdev, hwc_layer, i);
}