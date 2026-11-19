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

#pragma once

#include <cutils/native_handle.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <list>
#include <set>
#include <pthread.h>
#include <semaphore.h>
#include <hardware/hwcomposer.h>
#include <cutils/properties.h>
#include <vendor/waydroid/task/1.0/IWaydroidTask.h>
#include <wayland-util.h>
#include <wayland-client.h>

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>
#include <atomic>

using ::android::sp;
using ::vendor::waydroid::task::V1_0::IWaydroidTask;

enum {
    INPUT_TOUCH,
    INPUT_KEYBOARD,
    INPUT_POINTER,
    INPUT_TABLET,
    INPUT_TOTAL
};

static const char *INPUT_PIPE_NAME[INPUT_TOTAL] = {
    "/dev/input/wl_touch_events",
    "/dev/input/wl_keyboard_events",
    "/dev/input/wl_pointer_events",
    "/dev/input/wl_tablet_events"
};

enum class GrallocType {
    GRALLOC_ANDROID,
    GRALLOC_GBM,
    GRALLOC_CROS,
    GRALLOC_DEFAULT
};

#define MAX_TOUCHPOINTS 10

struct layerFrame {
    int x;
    int y;
    double scale_x = 0;
    double scale_y = 0;
};

struct handleExt {
    uint32_t format;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
};

struct waydroid_hwc_composer_device_1;

struct buffer_metadata {
    uint32_t height;
    uint32_t width;
    uint32_t pixel_stride;
    uint32_t format;
    /* Layer is opaque (HWC_BLENDING_NONE): export the buffer with an
     * alpha-ignoring fourcc so the compositor cannot blend garbage alpha
     * (opaque app windows routinely carry undefined alpha, e.g. games
     * leave the sky at a=0 — SF ignores it, the compositor must too). */
    bool opaque{false};
    /* Identity of the memory behind the handle: the inode of its first dma-buf fd (dma-buf
     * inode numbers are unique and never reused while the kernel runs), 0 where the gralloc
     * has no dma-buf. buffer_map is keyed by the buffer_handle_t pointer, which the mapper
     * reuses for a new buffer after freeing an old one; without this a new buffer of the same
     * size and format would be shown through the old buffer's cached wl_buffer. */
    uint64_t ident{0};
};
constexpr bool operator==(const buffer_metadata &lhs, const buffer_metadata &rhs) {
    return lhs.height == rhs.height
           && lhs.width == rhs.width
           && lhs.pixel_stride == rhs.pixel_stride
           && lhs.format == rhs.format
           && lhs.opaque == rhs.opaque
           && lhs.ident == rhs.ident;
}
constexpr bool operator!=(const buffer_metadata &lhs, const buffer_metadata &rhs) {
    return !(lhs == rhs);
}

struct buffer {
    struct wl_buffer *wl_buffer;

    buffer_handle_t handle;
    buffer_metadata metadata;

    bool isShm;
    void *shm_data;
    int size;
    /* hwc_set() calls in a row in which no layer used this buffer; see
     * prune_stale_cached_buffers() in hwcomposer.cpp. */
    int unused_frames = 0;
    /* Client target presented through a cross-GPU copy (prime-copy.cpp): wl_buffer is borrowed
     * from the copy-target pool and re-pointed by prime_copy() every frame; prime_source is the
     * imported source image, released with the buffer. */
    bool prime = false;
    void *prime_source = nullptr;

    ~buffer();
};

enum class BufferTransform : int32_t {
    Normal = WL_OUTPUT_TRANSFORM_NORMAL,
    Rot_90 = WL_OUTPUT_TRANSFORM_90,
    Rot_180 = WL_OUTPUT_TRANSFORM_180,
    Rot_270 = WL_OUTPUT_TRANSFORM_270,
    Flip = WL_OUTPUT_TRANSFORM_FLIPPED,
    Flip_Rot_90 = WL_OUTPUT_TRANSFORM_FLIPPED_90,
    Flip_Rot_180 = WL_OUTPUT_TRANSFORM_FLIPPED_180,
    Flip_Rot_270 = WL_OUTPUT_TRANSFORM_FLIPPED_270,
};

BufferTransform hwc_transform_to_buffer_transform(uint32_t hwc_transform);

struct surface_context {
    struct wl_surface *surface;
    struct wp_viewport *viewport;

    surface_context() = default;
    surface_context(wl_surface *surface, wp_viewport *viewport);
    ~surface_context();

    surface_context(surface_context &&other);
    surface_context &operator=(surface_context &&rhs);

    void attach_buffer(buffer& buf);
    void damage_surface(int32_t x, int32_t y, int32_t width, int32_t height);
    void set_buffer_transform(BufferTransform transform);
    void set_buffer_scale(double scale);
    // Requires the transformed rectangle
    // Clamped to the bounds (bound_w x bound_h, in the same post-transform space; 0 = unknown).
    void set_crop(hwc_frect_t crop, uint32_t bound_w, uint32_t bound_h);
    // TODO: This should not require scale. Scaling handling is broken
    void set_display_frame(hwc_rect_t rect, double scale);
};

struct window {
    struct layer : public surface_context {
        struct wl_subsurface *subsurface;
        struct zwp_locked_pointer_v1 *locked_pointer {};

        layer() = default;
        layer(wl_surface *surface, wp_viewport *viewport, wl_subsurface *subsurface = nullptr);
        ~layer();

        layer(layer &&other);
        layer &operator=(layer &&rhs);

        void set_position(int32_t x, int32_t y);
    };

    struct display *display;

    struct wl_shell_surface *shell_surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;

    /* Used for the background color */
    bool dedicated_background_surface;
    bool background_opaque;
    /* Android-space bounds of this task; the HWC thread writes, configure reads. */
    std::mutex content_mutex;
    hwc_rect_t content_frame {};
    int host_width = 0, host_height = 0;
    std::thread task_resize_thread;
    std::condition_variable task_resize_cond;
    bool task_resize_quit = false, task_resize_pending = false;
    std::chrono::steady_clock::time_point task_resize_deadline;
    bool uses_task_geometry() const;
    void set_content_frame(hwc_rect_t frame);
    void resize_task(int width, int height);
    void configure_task(int width, int height);
    void stop_task_resize();
    hwc_rect_t task_layer_frame(hwc_rect_t frame);
    struct wl_surface *surface;
    struct wp_viewport *viewport;
    struct wl_buffer *bg_buffer;
    struct zwp_locked_pointer_v1 *locked_pointer;

    struct zwp_idle_inhibitor_v1 *idle_inhibitor;

    std::vector<layer> layers;

    std::unique_ptr<buffer> snapshot_buffer;

    std::string appID;
    std::string taskID;

    std::atomic<bool> configured;

    // Reset every hwc_set cycle
    struct wl_region* input_region;
    int lastLayer;
    struct buffer *last_layer_buffer;

    ~window();

    static std::unique_ptr<window> create(struct display *display, bool use_subsurfaces, std::string appID, std::string taskID, hwc_color_t color);

    window::layer &get_next_layer();
    window::layer &create_new_layer();
    void reset_per_set_state();
    void update_background_geometry();

    void minimize();
    void set_maximize(bool enabled);
    void set_title(const char *title);
    void set_app_id(std::string appID);

  private:
    window() = default;
};

class open_windows {
    using Collection = std::map<std::string, std::unique_ptr<window>>;
    Collection windows;

    /* Windows are erased from the composer thread (hwc_set -> mode cleanup)
     * while the Wayland thread may be part-way through dispatching an event
     * for that very toplevel — libwayland reads the listener's user_data and
     * only then invokes the handler, so freeing in between is a use-after-free
     * (observed: xdg_toplevel_handle_close reading window->display on freed
     * memory, taking down SurfaceFlinger with it). So erasing only *retires*
     * a window here; the Wayland thread destroys it in reap(), between
     * dispatches, when no handler can be in flight.
     *
     * graveyard_mutex is a LEAF lock: never take another lock while holding
     * it, and never hold it while running a destructor. */
    std::vector<std::unique_ptr<window>> graveyard;
    std::mutex graveyard_mutex;

    void retire(std::unique_ptr<window> &&dead) {
        if (!dead)
            return;
        std::scoped_lock lock(graveyard_mutex);
        graveyard.push_back(std::move(dead));
    }

  public:
    /* Wayland thread only, between wl_display_dispatch() calls. */
    void reap() {
        std::vector<std::unique_ptr<window>> dead;
        {
            std::scoped_lock lock(graveyard_mutex);
            dead.swap(graveyard);
        }
        // destructors run here, with no lock held
    }

    using key_type = Collection::key_type;
    using mapped_type = Collection::mapped_type;
    using size_type = Collection::size_type;
    using iterator = Collection::iterator;
    using const_iterator = Collection::const_iterator;
    using reverse_iterator = Collection::reverse_iterator;
    using const_reverse_iterator = Collection::const_reverse_iterator;

    iterator begin() {
        return windows.begin();
    }
    iterator end() {
        return windows.end();
    }

    iterator find(const key_type& key) {
        return windows.find(key);
    }

    mapped_type& operator[](const key_type& key) {
        return windows[key];
    }
    mapped_type& operator[](key_type&& key) {
        return windows[std::move(key)];
    }

    size_type size() const {
        return windows.size();
    }

    template<class Func>
    void update(Func func) {
        func();

        std::string windows_size_str = std::to_string(windows.size());
        property_set("waydroid.open_windows", windows_size_str.c_str());
    }

    window *add(waydroid_hwc_composer_device_1 *pdev, const std::string& key, const std::string& aid, const std::string& tid, hwc_color_t color = {0, 0, 0, 255});
    void add(const std::string& key, std::unique_ptr<window> window);
    void clear();
    void erase(const_iterator pos);
    void erase(const key_type& key);
    template<class Pred>
    void erase_if(Pred pred) {
        update([&](){
            for (auto it = windows.begin(), end = windows.end(); it != end;) {
                if (pred(*it)) {
                    retire(std::move(it->second));
                    it = windows.erase(it);
                } else {
                    ++it;
                }
            }
        });
    }
};

struct cursor_handler {
    virtual ~cursor_handler() = default;
    virtual std::unique_ptr<buffer> create_buffer(waydroid_hwc_composer_device_1 *pdev, const buffer_metadata& metadata, hwc_layer_1 *hwc_layer);
    virtual int apply_cursor(waydroid_hwc_composer_device_1 *pdev, hwc_layer_1 *hwc_layer, size_t hwc_layer_index) = 0;
    virtual int reset_cursor(waydroid_hwc_composer_device_1 *pdev) = 0;
    virtual int on_cursor_enter(display *display) = 0;
};

struct display {
    pthread_t wayland_thread; // constant after init

    struct wl_display *display;
    /* window::create() runs on the composer thread and must sync with the
     * compositor, but hwc_wayland_thread is concurrently dispatching the
     * DEFAULT queue. Two threads on one queue is the libwayland deadlock
     * hazard, and it wedged window creation ~50% of the time with decorations
     * on (both threads parked in ppoll, waiting on each other). Sync callbacks
     * for creation go to this private queue instead, which only the creating
     * thread ever dispatches. The new window's role objects (xdg_surface,
     * xdg_toplevel, decoration, wl_shell_surface, fractional scale) are made
     * on it too, so their first configure is dispatched by the creating thread
     * and never by hwc_wayland_thread (see window::create()). */
    struct wl_event_queue *create_queue;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_seat *seat;
    struct wl_shell *shell;
    struct wl_shm *shm;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_touch *touch;
    struct wl_output *output;
    struct wp_presentation *presentation;
    /* wp_presentation.clock_id (UINT32_MAX until the compositor sends it). Presentation
     * timestamps feed last_vsync_ns, which the vsync thread compares with CLOCK_MONOTONIC, so
     * they are used only when the compositor's clock is CLOCK_MONOTONIC. */
    std::atomic<uint32_t> presentation_clk_id{UINT32_MAX};
    struct wp_viewporter *viewporter;
    struct android_wlegl *android_wlegl;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    struct xdg_wm_base *wm_base;
    struct zwp_tablet_manager_v2* tablet_manager;
    struct zwp_tablet_seat_v2 *tablet_seat;
    struct zwp_pointer_constraints_v1 *pointer_constraints;
    struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
    struct zwp_relative_pointer_v1 *relative_pointer;
    struct zwp_idle_inhibit_manager_v1 *idle_manager;
    struct wp_fractional_scale_manager_v1 *fractional_scale_manager;
    struct wl_data_device_manager *data_device_manager;
    struct wl_data_device *data_device;
    struct zxdg_decoration_manager_v1 *decoration_manager;

    int system_version;
    GrallocType gtype;
    double scale;

    int input_fd[INPUT_TOTAL];
    int ptrPrvX;
    int ptrPrvY;
    double wheelAccumulatorX;
    double wheelAccumulatorY;
    bool wheelEvtIsDiscrete;
    bool reverseScroll;
    int touch_id[MAX_TOUCHPOINTS];
    std::map<struct wl_surface *, struct layerFrame> layers;

    open_windows windows;
    std::set<std::string> ignored_apps;
    std::recursive_mutex windowsMutex;

    std::map<int, struct wl_surface *> touch_surfaces;
    struct wl_surface *pointer_surface;
    struct wl_surface *tablet_surface;
    std::list<struct zwp_tablet_tool_v2 *> tablet_tools;
    std::map<struct zwp_tablet_tool_v2 *, uint16_t> tablet_tools_evt;
    uint32_t keyboard_enter_serial;
    /* Serial of the newest serial-carrying input event (focus enter, key,
     * pointer button, touch). wl_data_device.set_selection must quote the
     * serial of the event that triggered it: compositors reject a selection
     * whose serial is older than the one that set the current selection
     * (KWin: SeatInterfacePrivate::updateSelection cancels the source), so
     * the guest->host clipboard cannot use the focus-enter serial alone.
     * Advanced monotonically: keyboard and pointer events do not reach the
     * client in serial order, so last-writer-wins can move it backwards. */
    uint32_t last_input_serial;
    std::string clipboard;
    std::list<std::string> clipboard_offer_mime_types;
    uint32_t pointer_enter_serial;
    struct {float x; float y;} cursor_hotspot;

    EGLDisplay egl_dpy;
    std::list<std::function<void()>> egl_work_queue;
    sem_t egl_go;
    sem_t egl_done;

    int width;
    int height;
    int full_width;
    int full_height;
    int req_width;
    int req_height;
    int refresh;

    /* formats/modifiers are written by dmabuf_format()/dmabuf_modifiers() (zwp_linux_dmabuf_v1
     * listener, on the Wayland dispatch thread) and read by gralloc_handler.cpp's
     * isFormatSupported()/compositor_supports() from SurfaceFlinger's HWC binder thread.
     * Every access holds formats_mutex; readers copy what they need out under it. */
    std::mutex formats_mutex;
    std::unordered_set<uint32_t> formats;
    /* Formats for which the compositor advertised at least one NVIDIA-vendor (0x03) modifier,
     * i.e. it imports them through NVIDIA's EGL. Maintained by dmabuf_modifiers() under
     * formats_mutex; see compositor_supports(). */
    std::unordered_set<uint32_t> nvidia_modifier_formats;

    std::map<uint32_t, std::vector<uint64_t>> modifiers;

    /* zwp_linux_dmabuf_v1 v4 default feedback (Wayland thread only) and the decision made from
     * it (read by the HWC thread). See dmabuf_feedback_done() and prime-copy.h. */
    struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback;
    struct dmabuf_feedback_state {
        void *table = nullptr;
        size_t table_size = 0;
        dev_t main_device = 0;
        dev_t tranche_device = 0;
        std::vector<uint16_t> tranche_indices;
        struct tranche { dev_t target; std::vector<uint16_t> indices; };
        std::vector<tranche> tranches;
    } feedback;
    std::atomic<int> present_path{0};
    void *prime_copier = nullptr;

    std::map<uint32_t, std::string> layer_names;
    std::map<uint32_t, struct handleExt> layer_handles_ext;
    struct handleExt target_layer_handle_ext;
    std::unordered_map<buffer_handle_t, std::unique_ptr<buffer>> buffer_map;
    std::array<uint8_t, 239> keysDown;

    std::unique_ptr<cursor_handler> cursor_handler;
    bool supports_cursor_viewport;
    bool supports_cursor_hw_buffer;

    bool isMaximized;
    /* persist.waydroid.window_decorations: ask the compositor for a normal
     * server-side-decorated toplevel instead of the default undecorated
     * maximized surface. Off by default — a decorated window no longer covers
     * the output, which costs direct scanout. */
    bool wantDecorations;
    /* Guest display size we last forced to follow the window, so a stream of
     * configures with an unchanged size costs nothing. 0 = never set. */
    int forced_width;
    int forced_height;
    /* The Android-side display size: set at calibration, then changed by a
     * display-mode switch or, when the display follows the window, by the resize
     * thread (guarded by modes_mutex). SurfaceFlinger always projects onto THIS,
     * whatever the logical size is set to, so it is the space hwc has to
     * present — scaled into the window, never cropped. */
    int phys_width;
    int phys_height;
    /* Where the scaled guest display sits inside the window, in window-logical
     * coordinates. The window can be any shape; the guest cannot be stretched
     * to match it without distorting, so it is fitted and centred and the rest
     * of the window shows the background (letterbox). Applied to subsurface
     * positions only — input maps through the layer's guest-space origin, so
     * it is unaffected. */
    int present_x;
    int present_y;
    /* Display modes offered to SurfaceFlinger. HWC1 can advertise up to 128
     * configs and the HWC2On1Adapter maps each one's Width/Height straight
     * through (populateConfigs), so a *resolution* switch needs no hotplug —
     * it is the same path a phone's FHD+/QHD+ setting uses:
     *   cmd display set-user-preferred-display-mode W H REFRESH
     * SF reads the list at startup and again on a display reconnect, so every
     * offered mode must be present before the first query. When the display
     * follows the window there is one mode, the window's size. */
    std::vector<std::pair<int,int>> modes;
    int active_mode;
    /* Refresh rate SurfaceFlinger reports for our modes; needed verbatim when
     * asking the framework to switch (`cmd display set-user-preferred-display-
     * mode W H REFRESH` matches on all three). */
    double refresh_hz;
    /* Set when the active mode changed under us: the guest display is a new
     * shape, so the window's scale/letterbox has to be recomputed and the
     * surface geometry re-declared. Consumed by hwc_set on the composer
     * thread, which is where touching wl objects is already safe. */
    std::atomic<bool> modeChanged;
    /* setForcedDisplaySize persists in Settings.Global, so a windowed session
     * would otherwise strand a small guest display the next time the window is
     * fullscreen. Cleared once, at first window creation. */
    bool clearedForcedSize;
    sp<IWaydroidTask> task;

    const hwc_procs_t *procs;
    /* Set while the composer thread is inside window::create(). The configure
     * handlers of the window being created run on that thread then, inside
     * presentDisplay; they skip the procs->invalidate() nudge, which loses
     * nothing: the frame being presented and the next one re-position the
     * layers anyway. It is not what keeps hwc_wayland_thread out of a lock
     * cycle with the composer (a check-then-call cannot): window::create()
     * never waits for that thread, so an invalidate() it makes during a present
     * only waits for the present to finish. */
    std::atomic<bool> creatingWindow;

    /* modes, active_mode, phys_width and phys_height are replaced by the resize
     * thread (display-resize.cpp) while composer threads read them
     * (getDisplayConfigs/getDisplayAttributes/setActiveConfig) and the Wayland
     * thread fits the window to them. A leaf lock: nothing is called while it is
     * held. */
    std::mutex modes_mutex;
    /* The output's fractional scale measured at calibration. The Android display
     * size for a window of W x H is floor(W * output_scale) x floor(H * output_scale),
     * the same size the display was calibrated with for the full output. */
    double output_scale;
    /* Physical reconnect on capable HALs; logical WindowManager resize on stock
     * HALs when the optional Java helper and native decorations are enabled. */
    bool resizeFollowsWindow;
    bool resizeUsesReconnect = false;
    /* Logical fallback size, guarded by modes_mutex. The physical mode stays fixed. */
    int logical_width = 0, logical_height = 0;
    /* persist.waydroid.multi_windows was accepted: every per-app window is a
     * card on a fixed Android display, so a card's size must never become the
     * display's. Off, the one per-app window IS the display (single-window
     * mode). Written once before the first toplevel, read by the Wayland
     * thread on a configure. */
    std::atomic<bool> multiWindows;
    /* The Android display size the last configure asked for (Wayland thread). */
    int follow_width;
    int follow_height;
    /* Resize thread state, see display-resize.cpp. */
    std::mutex resize_mutex;
    std::condition_variable resize_cond;
    std::chrono::steady_clock::time_point resize_deadline;
    int resize_width;
    int resize_height;
    bool resize_armed;
    bool resize_quit;
    std::thread resize_thread;
};

/* persist.waydroid.clipboard_debug — traces the host<->guest clipboard
 * handshake (offers, selections, reads, ownership) to logcat. Off by default;
 * clipboard breakage is compositor-specific and otherwise invisible. */
bool clipboard_debug_enabled();
#define CLIPLOG(...) do { if (clipboard_debug_enabled()) ALOGI(__VA_ARGS__); } while (0)

void
handle_relative_motion(void *data, struct zwp_relative_pointer_v1*,
        uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t);

void
snapshot_inactive_app_window(struct display *display, struct window *window);

/* Re-derive every window's scale/letterbox and surface geometry after the guest
 * display changed shape (a display-mode switch). Composer thread only. */
void
refit_windows_to_display(struct waydroid_hwc_composer_device_1 *pdev);

/* Exit status of the composer when its Wayland connection is lost (EX_TEMPFAIL): see
 * hwc_wayland_thread(). The cause is in logcat and in the property waydroid.hwc.wayland_lost. */
constexpr int HWC_EXIT_WAYLAND_LOST = 75;

/* display-resize.cpp: the Android display follows the host window. start reads the
 * vendor capability and logs which resize path is active; arm (Wayland thread)
 * schedules a settled size; stop joins the resize thread. */
void
display_resize_start(struct display *display);
void
display_resize_arm(struct display *display, int width, int height);
void
display_resize_stop(struct display *display);

struct display *
create_display(const char* gralloc);
void
destroy_display(struct display *display);

/* Physical projection of the forced logical display (Android LogicalDisplay). */
hwc_rect_t display_content_frame(struct display *display);
