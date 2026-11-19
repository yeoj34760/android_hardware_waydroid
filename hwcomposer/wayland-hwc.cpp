/*
 * Copyright © 2011 Benjamin Franzke
 * Copyright © 2010 Intel Corporation
 * Copyright © 2014 Collabora Ltd.
 * Copyright © 2021 Waydroid Project.
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

#include "wayland-hwc.h"

#include "hwcomposer.h"
#include "egl-tools.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <spawn.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/memfd.h>
#include <drm_fourcc.h>
#include <system/graphics.h>
#include <syscall.h>
#include <cmath>
#include <algorithm>

#include <libsync/sw_sync.h>
#include <sync/sync.h>
#include <hardware/gralloc.h>
#include <log/log.h>
#include <sys/resource.h>

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <cutils/trace.h>
#include <cutils/properties.h>

#include <xkbcommon/xkbcommon.h>
#include <cinttypes>

#include "gralloc_handler.h"

#include <wayland-client.h>
#include <wayland-android-client-protocol.h>
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "tablet-unstable-v2-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

using ::android::hardware::hidl_string;

/* See display::last_input_serial: keyboard and pointer events do not reach the
 * client in serial order, so only ever move it forward. */
static void note_input_serial(struct display *display, uint32_t serial)
{
    if (serial > display->last_input_serial) {
        display->last_input_serial = serial;
        CLIPLOG("clipboard: input serial advanced to %u", serial);
    }
}

struct buffer;

buffer::~buffer() {
    wl_buffer_destroy(wl_buffer);
    if (isShm)
        munmap(shm_data, size);
}

// Call me from egl_worker_thread only!
static void build_display_modes(struct display *d);
static void fit_guest_display_to_window(struct display *display);

void
refit_windows_to_display(struct waydroid_hwc_composer_device_1 *pdev)
{
    struct display *display = pdev->display;
    if (!display->wantDecorations)
        return;

    fit_guest_display_to_window(display);
    ALOGI("refit to %dx%d guest, window %dx%d, scale %.3f, offset %d,%d",
          display->phys_width, display->phys_height,
          display->width, display->height,
          display->scale, display->present_x, display->present_y);

    std::scoped_lock lock(display->windowsMutex);
    for (auto &entry : display->windows) {
        struct window *w = entry.second.get();
        if (!w)
            continue;
        if (display->wm_base && w->xdg_surface)
            xdg_surface_set_window_geometry(w->xdg_surface, 0, 0,
                                            display->width, display->height);
        if (w->dedicated_background_surface && w->viewport && w->surface) {
            wp_viewport_set_destination(w->viewport, display->width, display->height);
            struct wl_region *opaque = wl_compositor_create_region(display->compositor);
            wl_region_add(opaque, 0, 0, display->width, display->height);
            wl_surface_set_opaque_region(w->surface, opaque);
            wl_region_destroy(opaque);
            wl_surface_commit(w->surface);
        }
    }
    wl_display_flush(display->display);
}

void snapshot_inactive_app_window(struct display *display, struct window *window) {
    if (!window->layers[0].surface || !window->last_layer_buffer
        || window->last_layer_buffer->isShm || window->snapshot_buffer) {
        // Need a surface to draw and a non-SHM buffer to make snapshot from
        return;
    }

    ALOGI("Making inactive window snapshot for %s", window->taskID.c_str());

    struct buffer *old_buf = window->last_layer_buffer;

    window->snapshot_buffer = create_shm_wl_buffer(display, old_buf->metadata, old_buf->handle);
    if (!window->snapshot_buffer) {
        ALOGE("failed to create a wayland buffer for window snapshot");
        return;
    }

    // FIXME won't work as expected if there are multiple surfaces
    struct wl_surface *surface = window->layers[0].surface;

    egl_render_to_pixels(display, window->snapshot_buffer.get());

    wl_surface_attach(surface, window->snapshot_buffer->wl_buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(surface);

    window->last_layer_buffer = nullptr;
}

static void
xdg_surface_handle_configure(void *data, struct xdg_surface *surface,
                 uint32_t serial)
{
    auto window = static_cast<struct window *>(data);

    xdg_surface_ack_configure(surface, serial);

    window->configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    xdg_surface_handle_configure,
};

void choose_width_height(struct display* display, int32_t hint_width, int32_t hint_height) {
    char property[PROPERTY_VALUE_MAX];
    int width = hint_width;
    int height = hint_height;

    // Ignore hint it requested
    if (property_get("persist.waydroid.width", property, nullptr) > 0) {
        display->isMaximized = false;
        width = atoi(property);
    }

    if (property_get("persist.waydroid.height", property, nullptr) > 0) {
        display->isMaximized = false;
        height = atoi(property);
    }

    display->width = width;
    display->height = height;
}

static void build_display_modes(struct display *d);

void
finished_calibrating(struct display *d)
{
    char property[PROPERTY_VALUE_MAX];
    int default_density = 180;
    std::string display_scale = std::to_string(d->scale);
    property_set("waydroid.display_scale", display_scale.c_str());
    if (property_get("ro.sf.lcd_density", property, nullptr) <= 0) {
        std::string lcd_density = std::to_string(int(default_density * d->scale));
        property_set("ro.sf.lcd_density", lcd_density.c_str());
    }

    choose_width_height(d, d->req_width, d->req_height);
    /* The guest is already this size, so the first configure after calibration
     * must not spawn a pointless resize. */
    d->forced_width = d->width;
    d->forced_height = d->height;
    /* Android's display size. SurfaceFlinger keeps projecting onto this no
     * matter what the logical size becomes, so every later window size is
     * expressed as a scale of it rather than as a new display. */
    d->phys_width = floor(d->width * d->scale);
    d->phys_height = floor(d->height * d->scale);

    if (d->modes.empty()) {
        build_display_modes(d);
        d->active_mode = 0;
    }
}

void
do_hotplug(struct display *display) {
    if (display->touch) {
        char property[PROPERTY_VALUE_MAX];
        int width = floor(display->width * display->scale);
        int height = floor(display->height * display->scale);

        if (property_get("persist.waydroid.width_padding", property, nullptr) > 0)
            width -= atoi(property);
        std::string width_str = std::to_string(width);
        property_set("waydroid.display_width", width_str.c_str());

        if (property_get("persist.waydroid.height_padding", property, nullptr) > 0)
            height -= atoi(property);
        std::string height_str = std::to_string(height);
        property_set("waydroid.display_height", height_str.c_str());

        display->input_fd[INPUT_TOUCH] = -1;
        remove(INPUT_PIPE_NAME[INPUT_TOUCH]);
        mkfifo(INPUT_PIPE_NAME[INPUT_TOUCH], S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
        chown(INPUT_PIPE_NAME[INPUT_TOUCH], 1000, 1000);
    }
    if (display->procs && display->procs->invalidate) {
        display->needHotplug = true;
        /* See display::creatingWindow — calling into SurfaceFlinger here while
         * the composer thread is blocked in window::create() deadlocks both. */
        if (!display->creatingWindow.load(std::memory_order_acquire))
            display->procs->invalidate(display->procs);
    }
}

static bool
toplevel_has_state(struct wl_array *states, uint32_t wanted)
{
    if (!states)
        return false;
    /* Not wl_array_for_each(): its `pos = array->data` does not compile in
     * C++ (void* needs an explicit cast). */
    const uint32_t *begin = static_cast<const uint32_t *>(states->data);
    const size_t count = states->size / sizeof(uint32_t);
    for (size_t i = 0; i < count; i++) {
        if (begin[i] == wanted)
            return true;
    }
    return false;
}

/* Resize the guest display to match the window.
 *
 * Android hot-resizes its display all the time — it is how foldables switch
 * panels (DisplayFoldController -> WindowManagerInternal::setForcedDisplaySize).
 * What is NOT available to us is the hwc route: do_hotplug()'s
 * procs->hotplug(0, 1) would make SurfaceFlinger re-read the display
 * attributes, but the HWC2On1Adapter discards hotplug for display 0
 * ("Received hotplug for non-external display"). WindowManager's shell command
 * is the entry point a vendor HAL can actually reach; setForcedDisplaySize
 * requires WRITE_SECURE_SETTINGS, which this process (uid 1000, system) holds.
 *
 * Without this the window is still resizable, but a smaller window can only
 * CROP the guest screen — the buffers keep arriving at the old size.
 */
static void
resize_guest_display(struct display *display, int32_t width, int32_t height)
{
    char arg[32];
    if (width == display->forced_width && height == display->forced_height)
        return;
    display->forced_width = width;
    display->forced_height = height;

    if (width > 0 && height > 0)
        snprintf(arg, sizeof(arg), "%dx%d", width, height);
    else
        snprintf(arg, sizeof(arg), "reset");
    ALOGI("resizing guest display to %s", arg);

    /* posix_spawn, NOT fork(): this runs on the Wayland dispatch thread of a
     * heavily threaded HAL, and a forked child that touches any lock another
     * thread held at fork time deadlocks. That is not theoretical — a fork()
     * here wedged window creation outright. posix_spawn goes straight to
     * clone+exec without running atfork handlers.
     * The child is reaped by SIGCHLD/SIG_DFL + a non-blocking waitpid sweep on
     * the next call; we never block waiting for WindowManager to answer. */
    static pid_t last_child = -1;
    if (last_child > 0)
        waitpid(last_child, nullptr, WNOHANG);

    char *const argv[] = { (char *)"cmd", (char *)"window", (char *)"size", arg, nullptr };
    extern char **environ;
    pid_t pid;
    int err = posix_spawn(&pid, "/system/bin/cmd", nullptr, nullptr, argv, environ);
    if (err != 0) {
        ALOGE("could not spawn `cmd window size %s`: %s", arg, strerror(err));
        return;
    }
    last_child = pid;
}

/* Present the whole guest display inside whatever size the window now is.
 *
 * The Android-side resolution is fixed (SurfaceFlinger projects onto
 * phys_width x phys_height and letterboxes anything else into it), so a
 * resized window is a *scaled view* of that space, not a different display.
 * display->scale is exactly that mapping: every layer position and viewport
 * destination is divided by it, and hwc_get_display_attributes reports
 * width*scale, which keeps reading back as the unchanged physical size.
 *
 * The height is snapped to the guest's aspect so the mapping stays uniform —
 * a non-uniform fit cannot be expressed by a single scale, and trying to fake
 * it is what produced cropped and offset output.
 */
/* Build the list of display modes offered to SurfaceFlinger.
 *
 * SF snapshots this once at startup and hwc cannot add to it later, so whatever
 * shapes we might ever want to switch to have to be here from the start. Since
 * hwc can also scale its output into the window, a mode only has to match the
 * window's ASPECT closely — the leftover is taken up by scaling — so the ladder
 * spans aspect ratios rather than exact sizes.
 *
 * Mode 0 must be the native one: it is what SF makes active at boot.
 */
static void
build_display_modes(struct display *d)
{
    /* HWC2On1Adapter::Display::populateConfigs() reads at most MAX_NUM_CONFIGS
     * = 128 configs from us, and SurfaceFlinger snapshots the list once at
     * startup, so this budget is both hard and one-shot. Measured on a real
     * guest: advertise 200, exactly 128 arrive (ids 0..127). */
    const size_t MAX_DISPLAY_MODES = 128;
    size_t budget = MAX_DISPLAY_MODES;
    const int override_modes = property_get_int32("persist.waydroid.display_modes", 0);
    if (override_modes > 1 && (size_t)override_modes < budget)
        budget = (size_t)override_modes;   /* testing only; never raises the cap */

    /* Mode 0 is the native one and must stay first: SurfaceFlinger makes the
     * first config active at boot. */
    d->modes.push_back({d->phys_width, d->phys_height});

    /* Every mode is a RESHAPE of the native pixel budget, never a fixed
     * resolution — the ladder has to be as correct on a 1366x768 laptop as on
     * a 5120x1440 ultrawide. Constant area also means every shape costs the
     * guest the same to render as the native one does. */
    const double native_aspect = (double)d->phys_width / d->phys_height;
    const double area = (double)d->phys_width * (double)d->phys_height;

    /* Shape space, not resolution space, so this range is machine-independent:
     * 0.25 covers stacked/rotated portrait monitors (9:32 is 0.28), 5.60 covers
     * 32:9 superwide (3.56) and a triple 16:9 span (5.33). Geometric steps keep
     * the RELATIVE aspect error constant across the whole range instead of
     * bunching resolution near 1:1. */
    const double lo = 0.25, hi = 5.60;
    const size_t steps = budget - 1;
    size_t dropped = 0;

    for (size_t i = 0; i < steps; i++) {
        const double a = lo * pow(hi / lo, (double)i / (double)(steps - 1));
        if (fabs(a - native_aspect) / native_aspect < 0.01)
            continue;                       /* native already covers this shape */

        int w = (int)lround(sqrt(area * a));
        int h = (int)lround(sqrt(area / a));
        w &= ~1; h &= ~1;                   /* even dimensions */

        /* Android's smallest sane display; only reachable at an extreme aspect
         * on an already-small guest, and dropping is better than offering a
         * mode the framework will refuse. */
        if (w < 320 || h < 320) {
            dropped++;
            continue;
        }
        d->modes.push_back({w, h});
    }

    /* Worst-case aspect error a window can be left with, i.e. the letterbox the
     * scale path then has to absorb. Logged because it is the number that says
     * whether the ladder is dense enough on THIS machine. */
    const double step_ratio = pow(hi / lo, 1.0 / (double)(steps - 1));
    ALOGI("display modes: %zu of %zu budget, native %dx%d, aspect %.2f..%.2f, "
          "worst-case aspect error %.2f%%%s",
          d->modes.size(), budget, d->phys_width, d->phys_height, lo, hi,
          (step_ratio - 1.0) * 50.0,
          dropped ? " (some dropped: guest too small for extreme aspects)" : "");
}

/* Ask the framework to switch the guest display to a given mode.
 *
 * hwc cannot switch modes itself — SurfaceFlinger owns the active config and
 * calls setActiveConfig down to us — so the request has to come from the
 * framework side. This is the same path the Settings "screen resolution"
 * toggle uses. Fire and forget; the switch arrives later as setActiveConfig.
 */
static void
request_display_mode(struct display *display, int w, int h)
{
    char aw[16], ah[16], ar[32];
    snprintf(aw, sizeof(aw), "%d", w);
    snprintf(ah, sizeof(ah), "%d", h);
    snprintf(ar, sizeof(ar), "%.2f", display->refresh_hz > 1.0 ? display->refresh_hz : 60.0);
    ALOGI("requesting display mode %sx%s@%s", aw, ah, ar);

    static pid_t last_child = -1;
    if (last_child > 0)
        waitpid(last_child, nullptr, WNOHANG);
    char *const argv[] = { (char *)"cmd", (char *)"display",
                           (char *)"set-user-preferred-display-mode",
                           aw, ah, ar, nullptr };
    extern char **environ;
    pid_t pid;
    /* posix_spawn, never fork() — see resize_guest_display(). */
    int err = posix_spawn(&pid, "/system/bin/cmd", nullptr, nullptr, argv, environ);
    if (err != 0) {
        ALOGE("could not spawn `cmd display set-user-preferred-display-mode`: %s",
              strerror(err));
        return;
    }
    last_child = pid;
}

/* Pick the advertised mode whose aspect is closest to the window's. Only the
 * aspect matters: hwc scales the chosen mode onto the window, so the size is
 * absorbed and a near-exact aspect means near-zero letterbox. */
static void
match_display_mode_to_window(struct display *display)
{
    if (display->modes.empty() || display->width <= 0 || display->height <= 0)
        return;
    const double want = (double)display->width / display->height;
    int best = -1;
    double best_err = 0.0;
    for (size_t i = 0; i < display->modes.size(); i++) {
        double a = (double)display->modes[i].first / display->modes[i].second;
        double err = fabs(a - want) / want;
        if (best < 0 || err < best_err) { best = (int)i; best_err = err; }
    }
    if (best < 0 || best == display->active_mode)
        return;
    /* Hysteresis: a mode switch re-lays out every app, so it is only worth it
     * when the current shape is actually wrong. */
    double cur = (double)display->phys_width / display->phys_height;
    if (fabs(cur - want) / want < 0.02)
        return;
    request_display_mode(display, display->modes[best].first, display->modes[best].second);
}

static void
fit_guest_display_to_window(struct display *display)
{
    if (display->width <= 0 || display->height <= 0 ||
        display->phys_width <= 0 || display->phys_height <= 0)
        return;
    /* Fit, do not fill: the tighter axis decides, so the whole guest display
     * is always visible. Filling would crop it, and stretching is impossible
     * anyway — one scale factor cannot express a non-uniform mapping. */
    double scale = std::max((double)display->phys_width / display->width,
                            (double)display->phys_height / display->height);
    if (scale <= 0.0)
        return;
    display->scale = scale;
    display->present_x = (int)lround((display->width - display->phys_width / scale) / 2.0);
    display->present_y = (int)lround((display->height - display->phys_height / scale) / 2.0);
    if (display->present_x < 0) display->present_x = 0;
    if (display->present_y < 0) display->present_y = 0;
}

static void
xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *,
                              int32_t width, int32_t height,
                              struct wl_array *states)
{
    struct window *window = (struct window *)data;
    struct display *display = window->display;

    if (width <= 1 || height <= 1) {
		/* Compositor is deferring to us */
        /* or invalid during calibration */
		return;
	}

    display->req_width = width;
    display->req_height = height;

    if (display->height && display->width) {
        int old_width = display->width;
        int old_height = display->height;
        choose_width_height(display, width, height);
        if (display->wantDecorations && display->phys_width > 0)
            fit_guest_display_to_window(display);
        if (display->wm_base)
            xdg_surface_set_window_geometry(window->xdg_surface, 0, 0, display->width, display->height);
        /* The background surface is a 1x1 buffer stretched by its viewport, so
         * the viewport destination IS the surface's logical size. Left at the
         * size it had when the window was created, the surface stays as large
         * as the original display while the window geometry shrinks — the
         * compositor keeps the whole surface interactive, so resize handles and
         * hit regions extend far past the visible window and swallow clicks
         * meant for whatever is behind it. Resize it with the window. */
        if (window->dedicated_background_surface && window->viewport) {
            wp_viewport_set_destination(window->viewport, display->width, display->height);
            struct wl_region *opaque = wl_compositor_create_region(display->compositor);
            wl_region_add(opaque, 0, 0, display->width, display->height);
            wl_surface_set_opaque_region(window->surface, opaque);
            wl_region_destroy(opaque);
            wl_surface_commit(window->surface);
        }
        if (display->width != old_width || display->height != old_height)
            do_hotplug(display);
        /* By the first configure the framework is up, whereas window::create
         * can run early enough that `cmd window` finds no WindowManager. */
        if (display->wantDecorations) {
            /* Reshape the guest to match the window, then let the scale path
             * absorb whatever is left. Only on a settled drag — a mode switch
             * re-lays out every running app. */
            if (!toplevel_has_state(states, XDG_TOPLEVEL_STATE_RESIZING))
                match_display_mode_to_window(display);
            /* The window is a scaled view of the guest display,
             * applied in fit_guest_display_to_window() above. Do NOT resize the
             * guest to match the window — SurfaceFlinger keeps projecting onto
             * the unchanged physical display and letterboxes into it, so the
             * window would show a magnified corner of the result (measured:
             * scale 1412/972, offset (2560-1600*1.4527)/2). Use `wm size` to
             * change the guest resolution; it composes correctly through this
             * same scaled path. */
        } else if (!display->clearedForcedSize) {
            /* A previous windowed session persists its forced size in
             * Settings.Global; the fullscreen path must run at native size.
             * Once per session only, so a later manual `wm size` survives. */
            display->clearedForcedSize = true;
            resize_guest_display(display, 0, 0);
        }
    }
}

static void
send_key_event(display *data, uint32_t key, wl_keyboard_key_state state);

static void
xdg_toplevel_handle_close(void *data, struct xdg_toplevel *)
{
    struct window *window = (struct window *)data;
    struct display *display = window->display;

    // simulate user input to restart idle timeout (TODO: find a better way)
    send_key_event(window->display, 0, WL_KEYBOARD_KEY_STATE_PRESSED);
    send_key_event(window->display, 0, WL_KEYBOARD_KEY_STATE_RELEASED);

    if (display->task != nullptr) {
        if (window->taskID != "none") {
            if (window->taskID == "0") {
                property_set("waydroid.active_apps", "none");
                display->task->removeAllVisibleRecentTasks();
            } else {
                display->task->removeTask(stoi(window->taskID));
            }
        }
    }

    std::scoped_lock lock(window->display->windowsMutex);
    std::string key;
    if (window->taskID != "0" && window->taskID != "none") {
        key = window->taskID;
    } else {
        key = window->appID;
    }
    display->ignored_apps.insert(key);
    display->windows.erase(key);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    xdg_toplevel_handle_configure,
    xdg_toplevel_handle_close,
};

static void
toplevel_decoration_handle_configure(void *, struct zxdg_toplevel_decoration_v1 *,
                                     uint32_t mode)
{
    /* We never draw decorations ourselves, so a compositor that insists on
     * client-side mode leaves the window undecorated — same as the default. */
    if (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE)
        ALOGW("Compositor refused server-side decorations (mode %u); "
              "persist.waydroid.window_decorations has no effect", mode);
}

static const struct zxdg_toplevel_decoration_v1_listener toplevel_decoration_listener = {
    toplevel_decoration_handle_configure,
};

void
shell_surface_ping(void *, struct wl_shell_surface *shell_surface, uint32_t serial)
{
    wl_shell_surface_pong(shell_surface, serial);
}

void
shell_surface_configure(void *data, struct wl_shell_surface *, uint32_t, int32_t width, int32_t height)
{
    struct window *window = (struct window *)data;
    struct display *display = window->display;

    if (width > 1 && height > 1) {
        display->req_width = width;
        display->req_height = height;
	}

    window->configured = true;

    if (display->height && display->width && width > 1 && height > 1) {
        int old_width = display->width;
        int old_height = display->height;
        choose_width_height(display, width, height);
        if (display->width != old_width || display->height != old_height)
            do_hotplug(display);
    }
}

void
shell_surface_popup_done(void *, struct wl_shell_surface *)
{
}

struct wl_shell_surface_listener shell_surface_listener = {
	&shell_surface_ping,
	&shell_surface_configure,
	&shell_surface_popup_done
};


BufferTransform hwc_transform_to_buffer_transform(uint32_t hwc_transform) {
    switch (hwc_transform) {
        case 0:
            return BufferTransform::Normal;
        case HWC_TRANSFORM_FLIP_H:
            return BufferTransform::Flip_Rot_180;
        case HWC_TRANSFORM_FLIP_V:
            return BufferTransform::Flip;
        case HWC_TRANSFORM_ROT_90:
            return BufferTransform::Rot_90;
        case HWC_TRANSFORM_ROT_180:
            return BufferTransform::Rot_180;
        case HWC_TRANSFORM_ROT_270:
            return BufferTransform::Rot_270;
        case HWC_TRANSFORM_FLIP_H_ROT_90:
            return BufferTransform::Flip_Rot_270;
        case HWC_TRANSFORM_FLIP_V_ROT_90:
            return BufferTransform::Flip_Rot_90;
        default:
            ALOGE("Invalid hwc_transform: %" PRIu32, hwc_transform);
            abort();
    }
}

void surface_context::attach_buffer(buffer& buf) {
    wl_surface_attach(surface, buf.wl_buffer, 0, 0);
}

void surface_context::damage_surface(int32_t x, int32_t y, int32_t width, int32_t height) {
    wl_surface_damage(surface, x, y, width, height);
}

void surface_context::set_buffer_transform(BufferTransform transform) {
    wl_surface_set_buffer_transform(surface, static_cast<int32_t>(transform));
}

void surface_context::set_buffer_scale(double d_scale) {
    assert(!viewport);

    // Usually with no viewporter the scale is guaranteed to be integer
    // When supports_cursor_viewport == false, this might not be the case
    // thus use ceil anyway
    int32_t scale = static_cast<int32_t>(ceil(d_scale));
    wl_surface_set_buffer_scale(surface, scale);
}

void surface_context::set_crop(hwc_frect_t crop) {
    assert(viewport);

    float x = fmax(0, crop.left);
    float y = fmax(0, crop.top);
    float width = fmax(1, crop.right - crop.left);
    float height = fmax(1, crop.bottom - crop.top);

    wp_viewport_set_source(viewport,
                           wl_fixed_from_double(x),
                           wl_fixed_from_double(y),
                           wl_fixed_from_double(width),
                           wl_fixed_from_double(height));
}

void surface_context::set_display_frame(hwc_rect_t rect, double scale) {
    assert(viewport);

    int width = static_cast<int>(ceil((rect.right - rect.left) / scale));
    int height = static_cast<int>(ceil((rect.bottom - rect.top) / scale));

    wp_viewport_set_destination(viewport,
                                std::max(1, width),
                                std::max(1, height));
}

void window::reset_per_set_state() {
    lastLayer = 0;
    last_layer_buffer = nullptr;
    if (input_region) {
        wl_region_subtract(input_region, 0, 0, INT_MAX, INT_MAX);
    }
}

void window::set_maximize(bool enabled) {
    if (xdg_toplevel) {
        if (enabled) {
            xdg_toplevel_set_maximized(xdg_toplevel);
        } else {
            xdg_toplevel_unset_maximized(xdg_toplevel);
        }
    } else {
        assert(shell_surface);
        if (enabled) {
            wl_shell_surface_set_maximized(shell_surface, display->output);
        } else {
            ALOGW("wl_shell_surface: does not support un-maximizing");
        }
    }
}

void window::set_title(const char* title) {
    if (xdg_toplevel) {
        xdg_toplevel_set_title(xdg_toplevel, title);
    } else {
        assert(shell_surface);
        wl_shell_surface_set_title(shell_surface, title);
    }
}

void window::set_app_id(std::string appID) {
    this->appID = std::move(appID);
    if (xdg_toplevel) {
        const std::string &wayland_app_id = [&](){
            if (this->appID != "Waydroid") {
                return "waydroid." + this->appID;
            } else {
                return this->appID;
            }
        }();
        xdg_toplevel_set_app_id(xdg_toplevel, wayland_app_id.c_str());
    }
}

void window::minimize() {
    if (xdg_toplevel) {
        xdg_toplevel_set_minimized(xdg_toplevel);
        wl_surface_commit(surface); // unclear if this is required
        wl_display_flush(display->display);
    }
}

window::~window() {
    if (decoration)
        zxdg_toplevel_decoration_v1_destroy(decoration);
    if (xdg_toplevel)
        xdg_toplevel_destroy(xdg_toplevel);
    if (xdg_surface)
        xdg_surface_destroy(xdg_surface);
    if (shell_surface)
        wl_shell_surface_destroy(shell_surface);
    if (bg_buffer)
        wl_buffer_destroy(bg_buffer);
    if (input_region)
        wl_region_destroy(input_region);
    if (idle_inhibitor)
        zwp_idle_inhibitor_v1_destroy(idle_inhibitor);

    layers.clear();
    if (dedicated_background_surface) {
        if (viewport)
            wp_viewport_destroy(viewport);
        if (surface) {
            wl_surface_set_user_data(surface, nullptr);
            wl_surface_destroy(surface);
        }
        if (locked_pointer)
            zwp_locked_pointer_v1_destroy(locked_pointer);
    }

    wl_display_flush(display->display);
}

static void fractional_scale_handle_preferred_scale(void *data, struct wp_fractional_scale_v1 *,
            uint32_t scale_times_120) {
    struct display *display = (struct display *)data;
    if (!display->viewporter) {
        // We should always have the viewporter if we have the fractional scale manager
        // but for debugging purpuses we may decide to disable one
        return;
    }
    display->scale = scale_times_120 / 120.0;
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred_scale
};

std::unique_ptr<window>
window::create(struct display *display, bool use_subsurfaces, std::string appID, std::string taskID, hwc_color_t color)
{
    std::unique_ptr<window> window { new struct window() };
    if (!window)
        return nullptr;

    window->display = display;
    window->surface = wl_compositor_create_surface(display->compositor);
    wl_surface_set_user_data(window->surface, window.get());
    if (display->viewporter)
        window->viewport = wp_viewporter_get_viewport(display->viewporter, window->surface);
    window->taskID = std::move(taskID);
    window->dedicated_background_surface = true;
    window->bg_buffer = nullptr;

    int fd = syscall(SYS_memfd_create, "buffer", 0);
    ftruncate(fd, 4);
    void *shm_data = mmap(NULL, 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm_data == MAP_FAILED) {
        ALOGE("mmap failed");
        close(fd);
        exit(1);
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(display->shm, fd, 4);
    close(fd);

    // Is this the first window created?
    bool calibrating = !display->height || !display->width;

    /* Everything below can block on the compositor; keep the Wayland thread out
     * of SurfaceFlinger until it is done. */
    display->creatingWindow.store(true, std::memory_order_release);
    struct creating_guard {
        struct display *d;
        ~creating_guard() { d->creatingWindow.store(false, std::memory_order_release); }
    } creating_guard{display};

    if (display->wm_base) {
        window->xdg_surface =
                xdg_wm_base_get_xdg_surface(display->wm_base, window->surface);
        assert(window->xdg_surface);
        xdg_surface_add_listener(window->xdg_surface,
                                     &xdg_surface_listener, window.get());

        window->xdg_toplevel = xdg_surface_get_toplevel(window->xdg_surface);
        assert(window->xdg_toplevel);
        xdg_toplevel_add_listener(window->xdg_toplevel, &xdg_toplevel_listener, window.get());

        if (display->wantDecorations) {
            if (display->decoration_manager) {
                window->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
                        display->decoration_manager, window->xdg_toplevel);
                zxdg_toplevel_decoration_v1_add_listener(window->decoration,
                        &toplevel_decoration_listener, window.get());
                zxdg_toplevel_decoration_v1_set_mode(window->decoration,
                        ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
            } else {
                ALOGW("persist.waydroid.window_decorations is set but the compositor "
                      "has no zxdg_decoration_manager_v1; window stays undecorated");
            }
        }
    } else if (display->shell) {
        window->shell_surface =
            wl_shell_get_shell_surface(display->shell, window->surface);
        assert(window->shell_surface);
        wl_shell_surface_add_listener(window->shell_surface, &shell_surface_listener, window.get());
        wl_shell_surface_set_toplevel(window->shell_surface);
    } else {
        abort();
    }

    if (display->isMaximized || calibrating) {
        window->set_maximize(true);
    }
    // Set title
    if (appID != "Waydroid" && display->task) {
        const hidl_string appID_hidl(appID);
        display->task->getAppName(appID_hidl, [&](const hidl_string &value){
            window->set_title(value.c_str());
        });
    } else {
        window->set_title(appID.c_str());
    }
    // Set appID
    window->set_app_id(std::move(appID));

    wl_surface_commit(window->surface);
    // Wait for first configure event
    do {
        wl_display_roundtrip_queue(display->display, display->create_queue);
    } while (!window->configured);

    if (calibrating) {
        wp_fractional_scale_v1* fs = nullptr;
        if (display->fractional_scale_manager) {
            // We only support one global scale
            fs = wp_fractional_scale_manager_v1_get_fractional_scale(
                    display->fractional_scale_manager, window->surface);
            wp_fractional_scale_v1_add_listener(fs, &fractional_scale_listener, display);
        }

        // Some compositors fail to give us a window size or scale without a buffer attached
        // See: https://github.com/swaywm/sway/issues/2176
        // Some other compositors may refine the window size after a buffer is attached (Hyprland)
        // Try second configure event, with buffer attached
        struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
        wl_surface_attach(window->surface, buf, 0, 0);
        if (window->viewport && display->req_width && display->req_height)
            wp_viewport_set_destination(window->viewport, display->req_width, display->req_height);
        wl_surface_commit(window->surface);
        wl_display_roundtrip_queue(display->display, display->create_queue);

        if (fs) {
            wp_fractional_scale_v1_destroy(fs);
        }

        // If after all of this we still did not receive a proper configure event,
        // fallback to using the full output size.
        // NOTICE: full_width and full_heigth should be in compositor logical size!
        if (!display->req_height)
            display->req_height = display->full_height / display->scale;
        if (!display->req_width)
            display->req_width = display->full_width / display->scale;

        finished_calibrating(display);
        if (!display->isMaximized)
            window->set_maximize(false);
    }

    if (display->wm_base)
        xdg_surface_set_window_geometry(window->xdg_surface, 0, 0, display->width, display->height);

    /* Once per session, and only here — a later manual `wm size` (the way you
     * test an app at another resolution) must survive untouched. */
    struct wl_region *region = wl_compositor_create_region(display->compositor);
    if (color.a == 0) {
        wl_surface_set_input_region(window->surface, region);
        if (display->system_version >= 33)
            window->input_region = region;
        else
            wl_region_destroy(region);
    }
    if (color.a == 255) {
        wl_region_add(region, 0, 0, display->width, display->height);
        wl_surface_set_opaque_region(window->surface, region);
        wl_region_destroy(region);
    }

    // TODO: Fix background when viewport is not supported
    // No subsurface background for us!
    if (!display->subcompositor ||
        !display->viewporter ||
        property_get_bool("persist.waydroid.no_background_subsurface", false)) {
        window->dedicated_background_surface = false;
        window->layers.emplace_back(window->surface, window->viewport);
        wl_shm_pool_destroy(pool);
        wl_surface_commit(window->surface);
        return window;
    } else if (!use_subsurfaces) {
        /* If we do not use subsurfaces for compositing create at least one for the window content
         * This surface should be desync so that we don't need to send commits for the background surface.
         * Since the surface's position will always be (0,0) committing the parent surface is not required then*/
        window->create_new_layer();
        wl_subsurface_set_desync(window->layers[0].subsurface);
    }

    uint32_t *buf = (uint32_t*)shm_data;
    *buf = color.a << 24 | color.r << 16 | color.g << 8 | color.b;
    window->bg_buffer = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    wl_surface_attach(window->surface, window->bg_buffer, 0, 0);
    wl_surface_damage(window->surface, 0, 0, INT32_MAX, INT32_MAX);
    wp_viewport_set_source(window->viewport, wl_fixed_from_int(0), wl_fixed_from_int(0), wl_fixed_from_int(1), wl_fixed_from_int(1));
    wp_viewport_set_destination(window->viewport, display->width, display->height);
    wl_surface_commit(window->surface);

    return window;
}

surface_context::surface_context(wl_surface *surface, wp_viewport *viewport) : surface(surface), viewport(viewport) {}
surface_context::~surface_context() {
    if (viewport)
        wp_viewport_destroy(viewport);
    if (surface)
        wl_surface_destroy(surface);
}
surface_context::surface_context(surface_context&& other) : surface(other.surface), viewport(other.viewport) {
    other.surface = nullptr;
    other.viewport = nullptr;
}
surface_context& surface_context::operator=(surface_context&& rhs) {
    if (viewport)
        wp_viewport_destroy(viewport);
    if (surface)
        wl_surface_destroy(surface);

    surface = rhs.surface;
    viewport = rhs.viewport;

    rhs.surface = nullptr;
    rhs.viewport = nullptr;

    return *this;
}

window::layer::layer(wl_surface *surface, wp_viewport *viewport, wl_subsurface *subsurface) : surface_context(surface, viewport), subsurface(subsurface) {}
window::layer::~layer() {
    if (subsurface)
        wl_subsurface_destroy(subsurface);
    if (locked_pointer)
        zwp_locked_pointer_v1_destroy(locked_pointer);
}
window::layer::layer(window::layer&& other) :
        surface_context(std::move(static_cast<surface_context&>(other))),
        subsurface(other.subsurface),
        locked_pointer(other.locked_pointer) {
    other.subsurface = nullptr;
    other.locked_pointer = nullptr;
}
window::layer& window::layer::operator=(window::layer&& rhs) {
    if (subsurface)
        wl_subsurface_destroy(subsurface);
    if (locked_pointer)
        zwp_locked_pointer_v1_destroy(locked_pointer);

    subsurface = rhs.subsurface;
    locked_pointer = rhs.locked_pointer;

    rhs.subsurface = nullptr;
    rhs.locked_pointer = nullptr;

    surface_context::operator=(std::move(rhs));

    return *this;
}

void window::layer::set_position(int32_t x, int32_t y) {
    assert(subsurface || (x == 0 && y == 0));
    if (subsurface) {
        wl_subsurface_set_position(subsurface, x, y);
    }
}

window::layer& window::get_next_layer() {
    if (lastLayer >= layers.size()) {
        assert(lastLayer == layers.size());
        create_new_layer();
    }
    return layers[lastLayer++];
}

window::layer& window::create_new_layer() {
    wl_surface *surface = wl_compositor_create_surface(display->compositor);
    wl_subsurface *subsurface = wl_subcompositor_get_subsurface(display->subcompositor, surface, this->surface);
    wp_viewport *viewport = nullptr;
    if (display->viewporter)
        viewport = wp_viewporter_get_viewport(display->viewporter, surface);

    return layers.emplace_back(surface, viewport, subsurface);
}

static int
ensure_pipe(struct display* display, int input_type)
{
    if (display->input_fd[input_type] == -1) {
        display->input_fd[input_type] = open(INPUT_PIPE_NAME[input_type], O_WRONLY | O_NONBLOCK);
        if (display->input_fd[input_type] == -1) {
            ALOGE("Failed to open pipe to InputFlinger: %s", strerror(errno));
            return -1;
        }
    }
    return 0;
}

#define ADD_EVENT(type_, code_, value_)            \
    event[n].time.tv_sec = rt.tv_sec;              \
    event[n].time.tv_usec = rt.tv_nsec / 1000;     \
    event[n].type = type_;                         \
    event[n].code = code_;                         \
    event[n].value = value_;                       \
    n++;

static void
send_key_event(display *data, uint32_t key, wl_keyboard_key_state state)
{
    struct display* display = (struct display*)data;
    struct input_event event[1];
    struct timespec rt;
    unsigned int res, n = 0;

    if (key >= display->keysDown.size()) {
        ALOGE("Invalid key: %u", key);
        return;
    }

    if (ensure_pipe(display, INPUT_KEYBOARD))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    ADD_EVENT(EV_KEY, key, state);

    res = write(display->input_fd[INPUT_KEYBOARD], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    display->keysDown[(uint8_t)key] = state;
}

static void
keyboard_handle_keymap(void *, struct wl_keyboard *,
               uint32_t format, int fd, uint32_t size)
{
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        char* keymap_shm = (char*)mmap(NULL, size - 1, PROT_READ, MAP_PRIVATE, fd, 0);
        auto xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES);
        auto xkb_keymap = xkb_keymap_new_from_buffer(xkb_ctx, keymap_shm, size - 1,
                            XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
        const char* namep = xkb_keymap_layout_get_name(xkb_keymap, 0);
        if (namep) {
            // Try to convert XKB name to an android identifier.
            // This is not very good, for example "English (UK)" becomes "english"
            // but android understand only "english_uk" or "english_us"
            std::string layout_name(namep);
            std::string layout_id = layout_name.substr(0, layout_name.find(' '));
            std::transform(layout_id.begin(), layout_id.end(), layout_id.begin(), ::tolower);
            property_set("waydroid.keyboard_layout", layout_id.c_str());
        }

        xkb_keymap_unref(xkb_keymap);
        xkb_context_unref(xkb_ctx);
        munmap(keymap_shm, size - 1);
    }
    close(fd);
}

static void
keyboard_handle_enter(void *data, struct wl_keyboard *,
                      uint32_t serial, struct wl_surface *surface,
                      struct wl_array *)
{
    CLIPLOG("clipboard: keyboard enter, serial %u", serial);
    if (!surface) {
        // The surface this enter event corresponds to was already destroyed
        return;
    }

    struct display *display = (struct display *)data;
    display->keyboard_enter_serial = serial;
    note_input_serial(display, serial);

    // Only the task id is read under windowsMutex; setFocusedTask() is made
    // after it is released. It is a synchronous call that ends in
    // ActivityTaskManager.setFocusedTask(), which waits for the window
    // manager lock - and system_server can be holding that while it waits on
    // SurfaceFlinger, which waits on this HWC in hwc_set() for windowsMutex.
    // Holding the mutex across the call closed that cycle and deadlocked the
    // whole display stack until the watchdog killed system_server.
    int taskID = -1;
    {
        std::scoped_lock lock(display->windowsMutex);
        auto window = reinterpret_cast<struct window *>(wl_surface_get_user_data(surface));
        if (!window)
            return; // Window was destroyed in hwc_set

        if (window->taskID != "none" && window->taskID != "0")
            taskID = stoi(window->taskID);
    }

    if (display->task != nullptr && taskID >= 0)
        display->task->setFocusedTask(taskID);
}

static void
keyboard_handle_leave(void *data, struct wl_keyboard *,
                      uint32_t, struct wl_surface *)
{
    struct display *display = (struct display *)data;
    CLIPLOG("clipboard: keyboard leave");

    for (size_t i = 0; i < display->keysDown.size(); i++) {
        if (display->keysDown[i] == WL_KEYBOARD_KEY_STATE_PRESSED) {
            send_key_event(display, i, WL_KEYBOARD_KEY_STATE_RELEASED);
        }
    }
}

static void
keyboard_handle_key(void *data, struct wl_keyboard *,
                    uint32_t serial, uint32_t, uint32_t key,
                    uint32_t state)
{
    struct display *display = (struct display*)data;
    note_input_serial(display, serial);
    if (key == KEY_POWER)
        return;
    send_key_event(display, key, (enum wl_keyboard_key_state)state);
}

static void
keyboard_handle_modifiers(void *data, struct wl_keyboard *, uint32_t serial,
                          uint32_t, uint32_t, uint32_t, uint32_t)
{
    note_input_serial((struct display*)data, serial);
}

static void
keyboard_handle_repeat_info(void *, struct wl_keyboard *,
                            int32_t, int32_t)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_handle_keymap,
    keyboard_handle_enter,
    keyboard_handle_leave,
    keyboard_handle_key,
    keyboard_handle_modifiers,
    keyboard_handle_repeat_info,
};

static void
pointer_handle_enter(void *data, struct wl_pointer *,
                     uint32_t serial, struct wl_surface *surface,
                     wl_fixed_t, wl_fixed_t)
{
    if (!surface) {
        // The surface this enter event corresponds to was already destroyed
        return;
    }

    struct display *display = (struct display *)data;
    display->pointer_surface = surface;
    display->pointer_enter_serial = serial;
    note_input_serial(display, serial);
    display->cursor_handler->on_cursor_enter(display);
}

static void
pointer_handle_leave(void *data, struct wl_pointer *,
                     uint32_t, struct wl_surface *)
{
    struct display *display = (struct display *)data;
    display->pointer_surface = NULL;
}

static void
pointer_handle_motion(void *data, struct wl_pointer *,
                      uint32_t, wl_fixed_t sx, wl_fixed_t sy)
{
    struct display* display = (struct display*)data;
    struct input_event event[5];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_POINTER))
        return;

    if (!display->pointer_surface)
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    x = wl_fixed_to_int(sx);
    y = wl_fixed_to_int(sy);
    if (display->scale != 1) {
        x = int(x * display->scale);
        y = int(y * display->scale);
    }
    x += display->layers[display->pointer_surface].x;
    y += display->layers[display->pointer_surface].y;

    ADD_EVENT(EV_ABS, ABS_X, x);
    ADD_EVENT(EV_ABS, ABS_Y, y);
    ADD_EVENT(EV_REL, REL_X, x - display->ptrPrvX);
    ADD_EVENT(EV_REL, REL_Y, y - display->ptrPrvY);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    display->ptrPrvX = x;
    display->ptrPrvY = y;

    res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

void
handle_relative_motion(void *data, struct zwp_relative_pointer_v1*,
        uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t)
{
    struct display *display = (struct display *)data;
    struct input_event event[3];
    struct timespec rt;
    unsigned int res, n = 0;

    static double acc_x = 0;
    static double acc_y = 0;

    if (ensure_pipe(display, INPUT_POINTER))
        return;

    acc_x += wl_fixed_to_double(dx);
    acc_y += wl_fixed_to_double(dy);

    if (abs(acc_x) < 1 && abs(acc_y) < 1)
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_REL, REL_X, (int)acc_x);
    ADD_EVENT(EV_REL, REL_Y, (int)acc_y);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    acc_x -= (int)acc_x;
    acc_y -= (int)acc_y;

    res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
pointer_handle_button(void *data, struct wl_pointer *,
                      uint32_t serial, uint32_t, uint32_t button,
                      uint32_t state)
{
    struct display* display = (struct display*)data;
    note_input_serial(display, serial);
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_POINTER))
        return;

    if (!display->pointer_surface)
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    ADD_EVENT(EV_KEY, button, state);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
pointer_handle_axis(void *data, struct wl_pointer *,
                    uint32_t, uint32_t axis, wl_fixed_t value)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, move, n = 0;
    double fVal = wl_fixed_to_double(value) / 10.0f;
    double step = 1.0f;

    if (ensure_pipe(display, INPUT_POINTER))
        return;

    if (!display->pointer_surface)
        return;

    if (!display->reverseScroll) {
        fVal = -fVal;
    }

    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        display->wheelAccumulatorY += fVal;
        if (std::abs(display->wheelAccumulatorY) < step)
            return;
        move = (int)(display->wheelAccumulatorY / step);
        display->wheelAccumulatorY = display->wheelEvtIsDiscrete ? 0 :
                                     std::fmod(display->wheelAccumulatorY, step);
    } else {
        display->wheelAccumulatorX += fVal;
        if (std::abs(display->wheelAccumulatorX) < step)
            return;
        move = (int)(display->wheelAccumulatorX / step);
        display->wheelAccumulatorX = display->wheelEvtIsDiscrete ? 0 :
                                     std::fmod(display->wheelAccumulatorY, step);
    }

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_REL, (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
              ? REL_WHEEL : REL_HWHEEL, move);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
pointer_handle_axis_source(void *data, struct wl_pointer *, uint32_t source)
{
    struct display* display = (struct display*)data;
    display->wheelEvtIsDiscrete = (source == WL_POINTER_AXIS_SOURCE_WHEEL);
}

static void
pointer_handle_axis_stop(void *, struct wl_pointer *, uint32_t, uint32_t)
{
}

static void
pointer_handle_axis_discrete(void *, struct wl_pointer *, uint32_t, int32_t)
{
}

static void
pointer_handle_frame(void *, struct wl_pointer *)
{
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
    pointer_handle_frame,
    pointer_handle_axis_source,
    pointer_handle_axis_stop,
    pointer_handle_axis_discrete,
};


static int
get_touch_id(struct display *display, int id)
{
    for (int i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] == id)
            return i;
    }
    return -1;
}

static int
create_touch_id(struct display *display, int id) {
    int i = get_touch_id(display, id);
    if (i != -1)
        return i;

    for (i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] == -1) {
            display->touch_id[i] = id;
            return i;
        }
    }

    return -1;
}


static int
flush_touch_id(struct display *display, int id)
{
    for (int i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] == id) {
            display->touch_id[i] = -1;
            return i;
        }
    }
    return -1;
}

static void
touch_handle_down(void *data, struct wl_touch *,
          uint32_t serial, uint32_t, struct wl_surface *surface,
          int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    note_input_serial((struct display*)data, serial);
    if (!surface) {
        // The surface this down event corresponds to was already destroyed
        return;
    }

    struct display* display = (struct display*)data;
    struct input_event event[6];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    int touch_id = create_touch_id(display, id);
    display->touch_surfaces[id] = surface;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }
    x = wl_fixed_to_int(x_w);
    y = wl_fixed_to_int(y_w);
    if (display->scale != 1) {
        x = int(x * display->scale);
        y = int(y * display->scale);
    }
    x += display->layers[surface].x;
    y += display->layers[surface].y;

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, touch_id);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, touch_id);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, x);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, y);
    ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
touch_handle_up(void *data, struct wl_touch *,
        uint32_t serial, uint32_t, int32_t id)
{
    struct display* display = (struct display*)data;
    note_input_serial(display, serial);
    struct input_event event[3];
    struct timespec rt;
    unsigned int res, n = 0;

    int touch_id = flush_touch_id(display, id);
    // Might not exist if we discarded the down event due to a NULL surface
    if (touch_id != -1) {
        if (ensure_pipe(display, INPUT_TOUCH))
            return;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                  __FILE__, __LINE__, strerror(errno));
        }
        display->touch_surfaces[id] = NULL;

        ADD_EVENT(EV_ABS, ABS_MT_SLOT, touch_id);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);

        res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    }
}

static void
touch_handle_motion(void *data, struct wl_touch *,
            uint32_t, int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    struct display* display = (struct display*)data;
    struct input_event event[6];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

    int touch_id = get_touch_id(display, id);
    // Might not exist if we discarded the down event due to a NULL surface
    if (touch_id != -1) {
        if (ensure_pipe(display, INPUT_TOUCH))
            return;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                  __FILE__, __LINE__, strerror(errno));
        }
        x = wl_fixed_to_int(x_w);
        y = wl_fixed_to_int(y_w);
        if (display->scale != 1) {
            x = int(x * display->scale);
            y = int(y * display->scale);
        }
        x += display->layers[display->touch_surfaces[id]].x;
        y += display->layers[display->touch_surfaces[id]].y;

        ADD_EVENT(EV_ABS, ABS_MT_SLOT, touch_id);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, touch_id);
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, x);
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, y);
        ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);

        res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    }
}

static void
touch_handle_frame(void *, struct wl_touch *)
{
}

static void
touch_handle_cancel(void *data, struct wl_touch *)
{
    struct display* display = (struct display*)data;
    struct input_event event[6];
    struct timespec rt;
    unsigned int res, n;
    int i, id;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }

    // Cancel all touch points.
    for (i = 0; i < MAX_TOUCHPOINTS; i++) {
        if (display->touch_id[i] != -1) {
            id = display->touch_id[i];
            display->touch_id[i] = -1;
            display->touch_surfaces[id] = NULL;

            n = 0;
            // Turn finger into palm.
            ADD_EVENT(EV_ABS, ABS_MT_SLOT, i);
            ADD_EVENT(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM);
            ADD_EVENT(EV_SYN, SYN_REPORT, 0);
            // Lift off.
            ADD_EVENT(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
            ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
            ADD_EVENT(EV_SYN, SYN_REPORT, 0);

            res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
            if (res < sizeof(event))
                ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
        }
    }
}

static void
touch_handle_shape(void *data, struct wl_touch *, int32_t id, wl_fixed_t major, wl_fixed_t minor)
{
    struct display* display = (struct display*)data;
    struct input_event event[5];
    struct timespec rt;
    unsigned int res, n = 0;

    int touch_id = get_touch_id(display, id);
    // Might not exist if we discarded the down event due to a NULL surface
    if (touch_id != -1) {
        if (ensure_pipe(display, INPUT_TOUCH))
            return;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
           ALOGE("%s:%d error in touch clock_gettime: %s",
                __FILE__, __LINE__, strerror(errno));
        }
        ADD_EVENT(EV_ABS, ABS_MT_SLOT, touch_id);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, touch_id);
        ADD_EVENT(EV_ABS, ABS_MT_TOUCH_MAJOR, wl_fixed_to_int(major));
        ADD_EVENT(EV_ABS, ABS_MT_TOUCH_MINOR, wl_fixed_to_int(minor));
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);

        res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    }
}

static void
touch_handle_orientation(void *, struct wl_touch *, int32_t, wl_fixed_t)
{
}

static const struct wl_touch_listener touch_listener = {
    touch_handle_down,
    touch_handle_up,
    touch_handle_motion,
    touch_handle_frame,
    touch_handle_cancel,
    touch_handle_shape,
    touch_handle_orientation,
};

static void
xdg_wm_base_ping(void *, struct xdg_wm_base *wm_base, uint32_t serial)
{
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    xdg_wm_base_ping,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t wl_caps)
{
    struct display *d = (struct display*)data;
    enum wl_seat_capability caps = (enum wl_seat_capability) wl_caps;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->pointer) {
        d->pointer = wl_seat_get_pointer(seat);
        d->input_fd[INPUT_POINTER] = -1;
        d->ptrPrvX = 0;
        d->ptrPrvY = 0;
        d->reverseScroll = property_get_bool("persist.waydroid.reverse_scrolling", false);
        mkfifo(INPUT_PIPE_NAME[INPUT_POINTER], S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
        chown(INPUT_PIPE_NAME[INPUT_POINTER], 1000, 1000);
        wl_pointer_add_listener(d->pointer, &pointer_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && d->pointer) {
        remove(INPUT_PIPE_NAME[INPUT_POINTER]);
        wl_pointer_destroy(d->pointer);
        d->pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->keyboard) {
        d->keyboard = wl_seat_get_keyboard(seat);
        d->input_fd[INPUT_KEYBOARD] = -1;
        mkfifo(INPUT_PIPE_NAME[INPUT_KEYBOARD], S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
        chown(INPUT_PIPE_NAME[INPUT_KEYBOARD], 1000, 1000);
        wl_keyboard_add_listener(d->keyboard, &keyboard_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->keyboard) {
        remove(INPUT_PIPE_NAME[INPUT_KEYBOARD]);
        wl_keyboard_destroy(d->keyboard);
        d->keyboard = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !d->touch) {
        d->touch = wl_seat_get_touch(seat);
        d->input_fd[INPUT_TOUCH] = -1;
        mkfifo(INPUT_PIPE_NAME[INPUT_TOUCH], S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
        chown(INPUT_PIPE_NAME[INPUT_TOUCH], 1000, 1000);
        for (int i = 0; i < MAX_TOUCHPOINTS; i++)
            d->touch_id[i] = -1;
        wl_touch_set_user_data(d->touch, d);
        wl_touch_add_listener(d->touch, &touch_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && d->touch) {
        remove(INPUT_PIPE_NAME[INPUT_TOUCH]);
        wl_touch_destroy(d->touch);
        d->touch = NULL;
    }
}

static void
seat_handle_name(void *, struct wl_seat *, const char *)
{
}

static const struct wl_seat_listener seat_listener = {
    seat_handle_capabilities,
    seat_handle_name,
};

static void dmabuf_format(void *, struct zwp_linux_dmabuf_v1 *, uint32_t);
static void
dmabuf_modifiers(void *data, struct zwp_linux_dmabuf_v1 * dmabuf,
         uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
    dmabuf_format(data, dmabuf, format);

    struct display *d = (struct display*)data;
    uint64_t modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
    if (modifier == DRM_FORMAT_MOD_INVALID)
        return;

    size_t index;
    {
        std::lock_guard<std::mutex> lock(d->formats_mutex);
        index = d->modifiers[format].size();
        d->modifiers[format].push_back(modifier);
        if (fourcc_mod_get_vendor(modifier) == DRM_FORMAT_MOD_VENDOR_NVIDIA)
            d->nvidia_modifier_formats.insert(format);
    }

    std::stringstream prop_name_stream;
    std::stringstream prop_value_stream;
    prop_name_stream << "waydroid.modifiers." << std::hex << format << "." << std::dec << index;
    prop_value_stream << std::hex << modifier;
    std::string prop_name = prop_name_stream.str();
    std::string prop_value = prop_value_stream.str();
    property_set(prop_name.c_str(), prop_value.c_str());
}

static void
dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *, uint32_t format)
{
    struct display *d = (struct display*)data;
    std::lock_guard<std::mutex> lock(d->formats_mutex);
    d->formats.insert(format);
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    dmabuf_format,
    dmabuf_modifiers
};

static void
output_handle_mode(void *data, struct wl_output *,
                   uint32_t, int32_t width, int32_t height,
                   int32_t refresh)
{
    struct display *d = (struct display *)data;
    d->refresh = std::max(d->refresh, refresh);

    // Fallback size
    // We can't do anything meaningful if there's more than one display, just pick one at random
    // Hopefully these won't need to be used
    // NOTICE: Actually, this is wrong when using scaling. We should probably use xdg_output instead.
    d->full_width = width;
    d->full_height = height;
}

static void
output_handle_geometry(void *, struct wl_output *,
               int32_t, int32_t,
               int32_t, int32_t,
               int32_t,
               const char *, const char *,
               int32_t)
{
}

static void
output_handle_done(void *, struct wl_output *)
{
}

static void
output_handle_scale(void *data, struct wl_output *,
            int32_t scale)
{
    struct display *d = (struct display*)data;
    d->scale = std::max((int)d->scale, scale);
}

static const struct wl_output_listener output_listener = {
    output_handle_geometry,
    output_handle_mode,
    output_handle_done,
    output_handle_scale,
};

static void
presentation_clock_id(void *, struct wp_presentation *,
              uint32_t clk_id)
{
    ALOGE("*** %s: clk_id %d CLOCK_MONOTONIC %d", __func__, clk_id, CLOCK_MONOTONIC);
}

static const struct wp_presentation_listener presentation_listener = {
    presentation_clock_id
};

static void tablet_seat_handle_add_tablet(void *, struct zwp_tablet_seat_v2 *,
                                          struct zwp_tablet_v2 *)
{
}

static void tablet_seat_handle_add_pad(void*, struct zwp_tablet_seat_v2 *,
                                       struct zwp_tablet_pad_v2 *)
{
}

static void
tablet_tool_receive_type(void *data, struct zwp_tablet_tool_v2 *tool,
                         uint32_t type)
{
    struct display* display = (struct display*)data;
    uint16_t evt_code;
    switch(type) {
        case ZWP_TABLET_TOOL_V2_TYPE_PEN:
            evt_code = BTN_TOOL_PEN;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_ERASER:
            evt_code = BTN_TOOL_RUBBER;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_BRUSH:
            evt_code = BTN_TOOL_BRUSH;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_PENCIL:
            evt_code = BTN_TOOL_PENCIL;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_AIRBRUSH:
            evt_code = BTN_TOOL_AIRBRUSH;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_FINGER:
            evt_code = BTN_TOOL_FINGER;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_MOUSE:
            evt_code = BTN_TOOL_MOUSE;
            break;
        case ZWP_TABLET_TOOL_V2_TYPE_LENS:
            evt_code = BTN_TOOL_LENS;
            break;
        default:
            evt_code = BTN_DIGI;
    }
    display->tablet_tools_evt[tool] = evt_code;
}

static void
tablet_tool_receive_hardware_serial(void *, struct zwp_tablet_tool_v2 *,
                                    uint32_t, uint32_t)
{
}

static void
tablet_tool_receive_hardware_id_wacom(void *, struct zwp_tablet_tool_v2 *,
                                      uint32_t, uint32_t)
{
}

static void
tablet_tool_receive_capability(void *, struct zwp_tablet_tool_v2 *, uint32_t)
{
}

static void
tablet_tool_receive_done(void *, struct zwp_tablet_tool_v2 *)
{
}

static void
tablet_tool_receive_removed(void *, struct zwp_tablet_tool_v2 *)
{
}

static void
tablet_tool_proximity_in(void *data, struct zwp_tablet_tool_v2 *tool,
                         uint32_t, struct zwp_tablet_v2 *,
                         struct wl_surface *surface)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    display->tablet_surface = surface;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, display->tablet_tools_evt[tool], 1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_proximity_out(void *data, struct zwp_tablet_tool_v2 *tool)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    display->tablet_surface = NULL;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, display->tablet_tools_evt[tool], 0);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_down(void *data, struct zwp_tablet_tool_v2 *, uint32_t)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, BTN_TOUCH, 1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_up(void *data, struct zwp_tablet_tool_v2 *)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, BTN_TOUCH, 0);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_motion(void *data, struct zwp_tablet_tool_v2 *,
                   wl_fixed_t x_w, wl_fixed_t y_w)
{
    struct display* display = (struct display*)data;
    struct input_event event[3];
    struct timespec rt;
    int x, y;
    unsigned int res, n = 0;

    if (display->tablet_surface) {
        if (ensure_pipe(display, INPUT_TABLET))
            return;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                  __FILE__, __LINE__, strerror(errno));
        }
        x = wl_fixed_to_int(x_w);
        y = wl_fixed_to_int(y_w);
        if (display->scale != 1) {
            x = int(x * display->scale);
            y = int(y * display->scale);
        }
        x += display->layers[display->tablet_surface].x;
        y += display->layers[display->tablet_surface].y;

        ADD_EVENT(EV_ABS, ABS_X, x);
        ADD_EVENT(EV_ABS, ABS_Y, y);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);

        res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    }
}

static void
tablet_tool_pressure(void *data, struct zwp_tablet_tool_v2 *,
                     uint32_t pressure)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    // wayland value is 16 bits. android expects 8 bits max.
    pressure >>= 8;

    ADD_EVENT(EV_ABS, ABS_PRESSURE, pressure);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_distance(void *data, struct zwp_tablet_tool_v2 *,
                     uint32_t distance_raw)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_ABS, ABS_DISTANCE, distance_raw);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_tilt(void *data, struct zwp_tablet_tool_v2 *,
                 wl_fixed_t tilt_x, wl_fixed_t tilt_y)
{
    struct display* display = (struct display*)data;
    struct input_event event[3];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_ABS, ABS_TILT_X, wl_fixed_to_int(tilt_x));
    ADD_EVENT(EV_ABS, ABS_TILT_Y, wl_fixed_to_int(tilt_y));
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_rotation(void *, struct zwp_tablet_tool_v2 *, wl_fixed_t)
{
}

static void
tablet_tool_slider(void *, struct zwp_tablet_tool_v2 *, int32_t)
{
}

static void
tablet_tool_wheel(void *, struct zwp_tablet_tool_v2 *, wl_fixed_t, int32_t)
{
}

static void
tablet_tool_button_state(void *data, struct zwp_tablet_tool_v2 *,
                         uint32_t, uint32_t button, uint32_t state)
{
    struct display* display = (struct display*)data;
    struct input_event event[2];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TABLET))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_KEY, button, state);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    res = write(display->input_fd[INPUT_TABLET], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void
tablet_tool_frame(void *, struct zwp_tablet_tool_v2 *, uint32_t)
{
}

static const struct zwp_tablet_tool_v2_listener tablet_tool_listener = {
    .type = tablet_tool_receive_type,
    .hardware_serial = tablet_tool_receive_hardware_serial,
    .hardware_id_wacom = tablet_tool_receive_hardware_id_wacom,
    .capability = tablet_tool_receive_capability,
    .done = tablet_tool_receive_done,
    .removed = tablet_tool_receive_removed,
    .proximity_in = tablet_tool_proximity_in,
    .proximity_out = tablet_tool_proximity_out,
    .down = tablet_tool_down,
    .up = tablet_tool_up,
    .motion = tablet_tool_motion,
    .pressure = tablet_tool_pressure,
    .distance = tablet_tool_distance,
    .tilt = tablet_tool_tilt,
    .rotation = tablet_tool_rotation,
    .slider = tablet_tool_slider,
    .wheel = tablet_tool_wheel,
    .button = tablet_tool_button_state,
    .frame = tablet_tool_frame
};

static void tablet_seat_handle_add_tool(void *data, struct zwp_tablet_seat_v2 *,
                            struct zwp_tablet_tool_v2 *tool)
{
    if (!tool) {
        return;
    }

    struct display *d = (struct display*)data;
    d->tablet_tools.push_back(tool);
    zwp_tablet_tool_v2_add_listener(tool, &tablet_tool_listener, d);
    ALOGI("Added tablet tool");
}

static const struct zwp_tablet_seat_v2_listener tablet_seat_listener = {
    tablet_seat_handle_add_tablet,
    tablet_seat_handle_add_tool,
    tablet_seat_handle_add_pad
};

static void add_tablet_seat(struct display *d) {
    d->input_fd[INPUT_TABLET] = -1;
    mkfifo(INPUT_PIPE_NAME[INPUT_TABLET], S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    chown(INPUT_PIPE_NAME[INPUT_TABLET], 1000, 1000);

    d->tablet_seat = zwp_tablet_manager_v2_get_tablet_seat(d->tablet_manager, d->seat);
    zwp_tablet_seat_v2_add_listener(d->tablet_seat, &tablet_seat_listener, d);
}

static void
registry_handle_global(void *data, struct wl_registry *registry,
               uint32_t id, const char *interface, uint32_t version)
{
    struct display *d = (struct display*)data;

    if (strcmp(interface, "wl_compositor") == 0) {
        d->compositor =
            (struct wl_compositor*)wl_registry_bind(registry,
                id, &wl_compositor_interface, std::min(version, 5U));
    } else if (strcmp(interface, "wl_subcompositor") == 0) {
        d->subcompositor =
        (struct wl_subcompositor*)wl_registry_bind(registry,
                id, &wl_subcompositor_interface, 1);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        d->wm_base = (struct xdg_wm_base*)wl_registry_bind(registry,
                id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(d->wm_base, &xdg_wm_base_listener, d);
    } else if(strcmp(interface, "wl_shell") == 0) {
        d->shell = (struct wl_shell *)wl_registry_bind(
                registry, id, &wl_shell_interface, 1);
    } else if (strcmp(interface, "wl_seat") == 0) {
        d->seat = (struct wl_seat*)wl_registry_bind(registry, id,
                &wl_seat_interface, std::min(version, (uint32_t)WL_POINTER_AXIS_SOURCE_SINCE_VERSION));
        wl_seat_add_listener(d->seat, &seat_listener, d);
        if (d->tablet_manager && !d->tablet_seat)
            add_tablet_seat(d);
        if (d->data_device_manager && !d->data_device)
            d->data_device = wl_data_device_manager_get_data_device(d->data_device_manager, d->seat);
    } else if (strcmp(interface, "wl_shm") == 0) {
		d->shm = (struct wl_shm *)wl_registry_bind(registry, id,
                &wl_shm_interface, 1);
    } else if (strcmp(interface, "wl_output") == 0) {
        d->output = (struct wl_output*)wl_registry_bind(registry, id,
                &wl_output_interface, std::min(version, 3U));
        wl_output_add_listener(d->output, &output_listener, d);
        wl_display_roundtrip(d->display);
    } else if (strcmp(interface, "wp_presentation") == 0) {
        bool no_presentation = property_get_bool("persist.waydroid.no_presentation", false);
        if (!no_presentation) {
            d->presentation = (struct wp_presentation*)wl_registry_bind(registry, id,
                    &wp_presentation_interface, 1);
            wp_presentation_add_listener(d->presentation,
                    &presentation_listener, d);
        }
    } else if (strcmp(interface, "wp_viewporter") == 0) {
        d->viewporter = (struct wp_viewporter*)wl_registry_bind(registry, id,
                &wp_viewporter_interface, 1);
    } else if ((d->gtype == GrallocType::GRALLOC_ANDROID) &&
               (strcmp(interface, "android_wlegl") == 0)) {
        d->android_wlegl = (struct android_wlegl*)wl_registry_bind(registry, id,
                &android_wlegl_interface, 1);
    } else if ((d->gtype == GrallocType::GRALLOC_GBM || d->gtype == GrallocType::GRALLOC_CROS) &&
               (strcmp(interface, "zwp_linux_dmabuf_v1") == 0)) {
        if (version < 3)
            return;
        d->dmabuf = (struct zwp_linux_dmabuf_v1*)wl_registry_bind(registry, id,
                &zwp_linux_dmabuf_v1_interface, 3);
        zwp_linux_dmabuf_v1_add_listener(d->dmabuf, &dmabuf_listener, d);
    } else if (strcmp(interface, "zwp_tablet_manager_v2") == 0) {
        d->tablet_manager = (struct zwp_tablet_manager_v2 *)wl_registry_bind(registry, id,
                &zwp_tablet_manager_v2_interface, 1);
        if (d->tablet_manager && d->seat)
            add_tablet_seat(d);
    } else if (strcmp(interface, "zwp_pointer_constraints_v1") == 0) {
        d->pointer_constraints = (struct zwp_pointer_constraints_v1 *)wl_registry_bind(
                registry, id, &zwp_pointer_constraints_v1_interface, 1);
    } else if (strcmp(interface, "zwp_relative_pointer_manager_v1") == 0) {
        d->relative_pointer_manager = (struct zwp_relative_pointer_manager_v1 *)wl_registry_bind(
                registry, id, &zwp_relative_pointer_manager_v1_interface, 1);
    } else if (strcmp(interface, "zwp_idle_inhibit_manager_v1") == 0) {
        d->idle_manager = (struct zwp_idle_inhibit_manager_v1 *)wl_registry_bind(
                registry, id, &zwp_idle_inhibit_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        d->fractional_scale_manager = (struct wp_fractional_scale_manager_v1*)wl_registry_bind(registry, id,
                &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        d->data_device_manager = (struct wl_data_device_manager *)wl_registry_bind(registry, id,
                &wl_data_device_manager_interface, std::min(version,  3U));
        if (d->data_device_manager && d->seat)
            d->data_device = wl_data_device_manager_get_data_device(d->data_device_manager, d->seat);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        d->decoration_manager = (struct zxdg_decoration_manager_v1 *)wl_registry_bind(registry, id,
                &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, "gtk_shell1") == 0) {
        if (version < 6)
            d->supports_cursor_viewport = false;
    } else if (strcmp(interface, "mir_shell_v1") == 0) {
        d->supports_cursor_hw_buffer = false;
    }
}

static void
registry_handle_global_remove(void *, struct wl_registry *, uint32_t)
{
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};

static int
str_starts_with(const char *a, const char *b)
{
    return strncmp(a, b, strlen(b));
}

GrallocType
get_gralloc_type(const char *gralloc)
{
    if (strcmp(gralloc, "default") == 0) {
        return GrallocType::GRALLOC_DEFAULT;
    } else if (strcmp(gralloc, "gbm") == 0) {
        return GrallocType::GRALLOC_GBM;
    } else if (str_starts_with(gralloc, "minigbm") == 0) {
        return GrallocType::GRALLOC_CROS;
    } else {
        return GrallocType::GRALLOC_ANDROID;
    }
}

static void
wayland_log_handler (const char *format, va_list args)
{
    LOG_PRI_VA (ANDROID_LOG_ERROR, "wayland-hwc", format, args);
}

window *open_windows::add(waydroid_hwc_composer_device_1 *pdev, const std::string& key, const std::string& aid, const std::string& tid, hwc_color_t color) {
    window *window = nullptr;
    update([&](){
        auto res = windows.emplace(
            key,
            window::create(pdev->display, pdev->should_compose, aid, tid, color)
        );
        assert(res.second);
        window = res.first->second.get();
    });
    return window;
}

void open_windows::add(const std::string& key, std::unique_ptr<window> window) {
    update([&](){
        auto res = windows.emplace(
            key,
            std::move(window)
        );
        assert(res.second); (void)res;
    });
}

void open_windows::clear() {
    for (auto& [key, w] : windows)
        retire(std::move(w));
    windows.clear();
    property_set("waydroid.open_windows", "0");
}

void open_windows::erase(const_iterator pos) {
    update([&](){
        // const_iterator: re-find a mutable one so the window can be moved out
        auto it = windows.find(pos->first);
        if (it != windows.end()) {
            retire(std::move(it->second));
            windows.erase(it);
        }
    });
}

void open_windows::erase(const key_type& key) {
    auto it = windows.find(key);
    if (it == windows.end())
        return;
    retire(std::move(it->second));
    windows.erase(it);
    std::string windows_size_str = std::to_string(windows.size());
    property_set("waydroid.open_windows", windows_size_str.c_str());
}

static void* hwc_wayland_thread(void* data) {
    auto* d = static_cast<struct display*>(data);
    auto* display = d->display;
    int ret = 0;

    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

    while (ret != -1) {
        ret = wl_display_dispatch(display);
        /* Safe point: dispatch has returned, so no event handler is running on
         * this thread and windows retired by the composer thread can be freed. */
        d->windows.reap();
    }

    int err = wl_display_get_error(display);
    if (err == EPROTO) {
        const struct wl_interface *iface = nullptr;
        uint32_t id = 0;
        uint32_t code = wl_display_get_protocol_error(display, &iface, &id);
        ALOGE("*** %s: Wayland protocol error: interface %s id %u code %u",
              __PRETTY_FUNCTION__, iface ? iface->name : "?", id, code);
    } else {
        ALOGE("*** %s: Wayland client was disconnected: %s", __PRETTY_FUNCTION__,
              strerror(err));
    }

    abort();
}

struct display *
create_display(const char *gralloc)
{
    struct display *display = new struct display();
    if (display == NULL) {
        ALOGE("out of memory");
        return NULL;
    }
    wl_log_set_handler_client(wayland_log_handler);
    display->system_version = property_get_int32("ro.system.build.version.sdk", 0);
    display->gtype = get_gralloc_type(gralloc);
    display->refresh = 0;
    /* A decorated toplevel is a normal movable/resizable window, so it must not
     * come up maximized. persist.waydroid.width/height still pick the size;
     * without them we keep the size the compositor hands us at calibration. */
    display->wantDecorations = property_get_bool("persist.waydroid.window_decorations", false);
    display->isMaximized = !display->wantDecorations;
    display->supports_cursor_viewport = true;
    display->supports_cursor_hw_buffer = property_get_bool("persist.waydroid.cursor_force_shm", false);
    display->display = wl_display_connect(NULL);
    ALOGI("WAYLAND_DISPLAY: %s", getenv("WAYLAND_DISPLAY"));
    ALOGI("XDG_RUNTIME_DIR: %s", getenv("XDG_RUNTIME_DIR"));
    if (!display->display) {
        ALOGE("Couldn't open Wayland display.");
        return NULL;
    }
    sem_init(&display->egl_go, 0, 0);
    sem_init(&display->egl_done, 0, 0);

    umask(0);
    mkdir("/dev/input", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    chown("/dev/input", 1000, 1000);

    display->create_queue = wl_display_create_queue(display->display);
    if (!display->create_queue) {
        ALOGE("Couldn't create the window-creation event queue");
        wl_display_disconnect(display->display);
        return NULL;
    }

    display->registry = wl_display_get_registry(display->display);
    wl_registry_add_listener(display->registry,
                 &registry_listener, display);
    wl_display_roundtrip(display->display);

    if (pthread_create(&display->wayland_thread, nullptr, hwc_wayland_thread, display) != 0) {
        ALOGE("Couldn't create wayland thread");
        wl_display_disconnect(display->display);
        sem_destroy(&display->egl_go);
        sem_destroy(&display->egl_done);
        return nullptr;
    }

    display->task = IWaydroidTask::getService();
    return display;
}

void
destroy_display(struct display *display)
{
    pthread_kill(display->wayland_thread, SIGTERM);
    pthread_join(display->wayland_thread, nullptr);
    /* The Wayland thread is gone, so this thread is now the only one that can
     * touch windows: drain anything it never got to reap. */
    display->windows.clear();
    display->windows.reap();

    if (display->decoration_manager)
        zxdg_decoration_manager_v1_destroy(display->decoration_manager);

    if (display->wm_base)
        xdg_wm_base_destroy(display->wm_base);

    if (display->shell)
        wl_shell_destroy(display->shell);

    if (display->compositor)
        wl_compositor_destroy(display->compositor);

    if (display->tablet_manager) {
        for (struct zwp_tablet_tool_v2 *t : display->tablet_tools) {
            zwp_tablet_tool_v2_destroy(t);
        }
        zwp_tablet_seat_v2_destroy(display->tablet_seat);
        zwp_tablet_manager_v2_destroy(display->tablet_manager);
    }

    if (display->relative_pointer_manager)
        zwp_relative_pointer_manager_v1_destroy(display->relative_pointer_manager);

    if (display->pointer_constraints)
        zwp_pointer_constraints_v1_destroy(display->pointer_constraints);

    wl_registry_destroy(display->registry);
    if (display->create_queue)
        wl_event_queue_destroy(display->create_queue);
    wl_display_flush(display->display);
    wl_display_disconnect(display->display);

    delete display;
}
