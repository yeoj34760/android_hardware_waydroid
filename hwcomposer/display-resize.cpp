/*
 * Copyright (C) 2026 The waydroid-nvidia authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* The Android display follows the host window.
 *
 * A settled window size becomes the primary display's only config, and an
 * HWC1 hotplug(0, connected) makes SurfaceFlinger re-read it: Waydroid's
 * hwc2on1 "Handle primary hotplugs" patch re-runs populateConfigs() and
 * forwards a HWC2 re-connect, SurfaceFlinger reloads the display modes and
 * recreates the display (processDisplayHotplugEventsLocked ->
 * processDisplayChanged), and the framework follows with the new size.
 *
 * Where the hotplug is sent from decides whether that is safe:
 *
 *  - Never from inside hwc_set()/hwc_prepare(). Those run on the composer's
 *    binder thread while SurfaceFlinger's main thread waits for the
 *    presentDisplay/validateDisplay reply, so the callback is delivered to
 *    that waiting main thread, which processes a main-thread hotplug at once
 *    (onComposerHalHotplug) and erases the HWComposer DisplayData that
 *    presentAndGetReleaseFences() is still using. hwcomposer.cpp and
 *    wayland-hwc.cpp poison the identifier; this file is the only caller.
 *
 *  - From this file's own thread, SurfaceFlinger receives it on a binder
 *    thread, queues it and handles it on its main thread at the next commit,
 *    before that frame's composition. The composer side needs two things the
 *    stock composer 2.1 HAL does not give: the adapter must not take its
 *    state mutex and a Display's mutex in the opposite order to
 *    validate()/present(), and the HAL must keep the display's cached buffer
 *    handles until SurfaceFlinger has destroyed the old layers. A vendor
 *    image whose composer HAL does both declares
 *    ro.vendor.composer.safe_primary_reconnect=true; without it no hotplug is
 *    ever sent and the display keeps its calibrated size.
 *
 * The resize thread holds none of this module's locks while it calls into
 * the adapter or SurfaceFlinger (modes_mutex and resize_mutex are released
 * first), so it cannot be part of a lock cycle with a composer thread that is
 * inside hwc_set() holding the adapter's mutexes and windowsMutex.
 */

#include "wayland-hwc.h"

#include <algorithm>
#include <log/log.h>

static constexpr const char *kCapabilityProperty = "ro.vendor.composer.safe_primary_reconnect";

/* A window drag or an animated maximize walks through every intermediate size;
 * only the size it settles on goes to Android (same default and property as
 * Waydroid's hwcomposer on lineage-23.2, "Stop resizing the Android display so
 * much"). 0 sends each size as soon as the thread runs. */
static constexpr int32_t kResizeSettleDefaultMs = 250;

static thread_local bool t_is_resize_thread = false;

/* Resize thread only. */
static void
announce_display_size(struct display *display, int width, int height)
{
    LOG_ALWAYS_FATAL_IF(!t_is_resize_thread,
                        "display hotplug requested outside the resize thread");
    {
        std::scoped_lock lock(display->modes_mutex);
        if (display->phys_width == width && display->phys_height == height)
            return;
        display->modes.assign(1, {width, height});
        display->active_mode = 0;
        display->phys_width = width;
        display->phys_height = height;
    }
    /* A decorated window re-fits on the next frame (refit_windows_to_display). */
    display->modeChanged.store(true, std::memory_order_release);

    const hwc_procs_t *procs = display->procs;
    if (!procs || !procs->hotplug) {
        ALOGE("display resize: no HWC callbacks registered; Android keeps its display size "
              "until the next window resize");
        return;
    }
    ALOGI("display resize: Android display -> %dx%d (display reconnect)", width, height);
    procs->hotplug(procs, HWC_DISPLAY_PRIMARY, 1);
    if (procs->invalidate)
        procs->invalidate(procs);
}

static void
resize_worker(struct display *display)
{
    t_is_resize_thread = true;
    std::unique_lock<std::mutex> lock(display->resize_mutex);
    while (!display->resize_quit) {
        if (!display->resize_armed) {
            display->resize_cond.wait(lock);
            continue;
        }
        display->resize_cond.wait_until(lock, display->resize_deadline);
        if (display->resize_quit)
            break;
        /* Re-armed by a newer configure while waiting. */
        if (std::chrono::steady_clock::now() < display->resize_deadline)
            continue;
        display->resize_armed = false;
        const int width = display->resize_width;
        const int height = display->resize_height;
        lock.unlock();
        announce_display_size(display, width, height);
        lock.lock();
    }
}

void
display_resize_start(struct display *display)
{
    display->resizeFollowsWindow = property_get_bool(kCapabilityProperty, false);
    if (!display->resizeFollowsWindow) {
        ALOGI("display resize: the Android display keeps the size it was calibrated with; "
              "%s is not set, so this vendor image's composer HAL is not known to survive a "
              "display reconnect while frames are in flight, and none is sent",
              kCapabilityProperty);
        return;
    }
    ALOGI("display resize: the Android display follows the window (%s; settled after "
          "persist.waydroid.resize_settle_ms, default %d ms)",
          kCapabilityProperty, kResizeSettleDefaultMs);
    display->resize_armed = false;
    display->resize_quit = false;
    display->resize_thread = std::thread(resize_worker, display);
}

void
display_resize_arm(struct display *display, int width, int height)
{
    if (!display->resizeFollowsWindow)
        return;
    const auto settle = std::chrono::milliseconds(std::max(0, property_get_int32(
            "persist.waydroid.resize_settle_ms", kResizeSettleDefaultMs)));
    std::scoped_lock lock(display->resize_mutex);
    display->resize_width = width;
    display->resize_height = height;
    display->resize_deadline = std::chrono::steady_clock::now() + settle;
    display->resize_armed = true;
    display->resize_cond.notify_one();
}

void
display_resize_stop(struct display *display)
{
    if (!display->resize_thread.joinable())
        return;
    {
        std::scoped_lock lock(display->resize_mutex);
        display->resize_quit = true;
        display->resize_cond.notify_one();
    }
    display->resize_thread.join();
}
