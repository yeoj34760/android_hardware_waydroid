#!/usr/bin/env python3
"""Exercise production Ctrl+drag handlers and Type-B touch frames without Android."""
from pathlib import Path
import subprocess
import sys
import tempfile
from test_cursor_drag import block

root = Path(sys.argv[1]) / 'hwcomposer'
src = (root / 'wayland-hwc.cpp').read_text()
header = (root / 'wayland-hwc.h').read_text()
state = header[header.index('    bool ctrl_down'):header.index('    double wheelAccumulatorX')]
macro = src[src.index('#define ADD_EVENT'):src.index('\n\n', src.index('#define ADD_EVENT'))]
helpers = src[src.index('// Ctrl+left press fixes'):src.index('static void\nsend_key_event(display *data,', src.index('// Ctrl+left press fixes'))]
harness = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <linux/input.h>
#include <map>
#include <mutex>
#include <unistd.h>
#include <vector>
struct wl_pointer {};
struct wl_keyboard {};
struct wl_surface {};
using wl_fixed_t = int;
constexpr int INPUT_POINTER = 0, INPUT_TOUCH = 1;
constexpr int WL_POINTER_AXIS_VERTICAL_SCROLL = 0;
constexpr int WL_POINTER_BUTTON_STATE_PRESSED = 1;
struct hwc_rect_t { int left, top, right, bottom; } bounds{0,0,1000,800};
struct cursor_handler { void on_cursor_enter(struct display *) {} } cursor;
struct display {
''' + state + r'''
    int input_fd[2]{0,1};
    int ptrPrvX = 500, ptrPrvY = 400;
    int touch_id[10]{-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
    wl_surface *pointer_surface = nullptr;
    std::recursive_mutex windowsMutex;
    struct layerFrame { hwc_rect_t bounds {}; };
    std::map<wl_surface *, layerFrame> layers;
    void *relative_pointer = nullptr;
    bool pointer_button_down = false;
    bool reverseScroll = false, wheelEvtIsDiscrete = true;
    double wheelAccumulatorX = 0, wheelAccumulatorY = 0;
    uint32_t pointer_enter_serial = 0;
    struct cursor_handler *cursor_handler = &cursor;
};
int64_t now_ms = 1000;
bool fail_write = false, fail_open = false;
struct frame { int device; std::vector<input_event> events; };
std::vector<frame> frames;
int fake_clock(clockid_t, timespec *ts) {
    ts->tv_sec = now_ms / 1000; ts->tv_nsec = now_ms % 1000 * 1000000; return 0;
}
ssize_t fake_write(int fd, const void *data, size_t bytes) {
    if (fail_write) { errno=EAGAIN; return -1; }
    const auto *ev = static_cast<const input_event *>(data);
    frames.push_back({fd, {ev, ev + bytes / sizeof(*ev)}});
    assert(frames.back().events.back().type == EV_SYN);
    return bytes;
}
int ensure_pipe(display *, int) { return fail_open ? -1 : 0; }
hwc_rect_t display_content_frame(display *) { return bounds; }
void note_input_serial(display *, uint32_t) {}
int wl_fixed_to_int(int n) { return n/256; }
double wl_fixed_to_double(int n) { return n/256.0; }
void transform_input_coordinates(display *, wl_surface *, int &x, int &y) { x=x*2+100; y=y*2+50; }
#define ALOGV(...) ((void)0)
#define ALOGE(...) ((void)0)
#define clock_gettime fake_clock
#define write fake_write
''' + macro + '\n' + helpers
for name in ('keyboard_handle_modifiers', 'pointer_handle_enter', 'pointer_handle_leave',
             'pointer_handle_motion', 'pointer_handle_button', 'pointer_handle_axis'):
    harness += '\nvoid\n' + block(src, name + '(') + '\n'
harness += r'''
int value(const frame &f, int code, int slot) {
    int current=-1;
    for (const auto &e : f.events) {
        if (e.type==EV_ABS && e.code==ABS_MT_SLOT) current=e.value;
        if (e.type==EV_ABS && e.code==code && current==slot) return e.value;
    }
    assert(false); return 0;
}
int main() {
    wl_surface surface;
    display d;
    d.pointer_surface=&surface;
    d.layers[&surface] = {};
    d.ctrl_mask=4;
    auto ctrl=[&](bool on) { keyboard_handle_modifiers(&d,nullptr,0,on?4:0,0,0,0); };
    auto button=[&](int state) { pointer_handle_button(&d,nullptr,0,0,BTN_LEFT,state); };
    auto motion=[&](int x,int y) { pointer_handle_motion(&d,nullptr,0,(x-100)*128,(y-50)*128); };
    // Ordinary mouse behavior and Ctrl+wheel do not create touches.
    button(1); button(0); assert(frames.size()==2 && frames.back().device==INPUT_POINTER);
    ctrl(true);
    pointer_handle_axis(&d,nullptr,0,0,-2560);
    assert(!d.pinch.active && frames.back().device==INPUT_POINTER);
    frames.clear();
    button(1);
    assert(d.pinch.button && d.pinch.active && !d.pinch.second);
    assert(d.pointer_button_down && frames.size()==1 && frames[0].device==INPUT_TOUCH);
    assert(value(frames[0],ABS_MT_POSITION_X,0)==500);
    assert(value(frames[0],ABS_MT_POSITION_Y,0)==400);
    assert(d.touch_id[0]==-2 && d.touch_id[1]==-3);
    motion(510,400); assert(frames.size()==1 && !d.pinch.second);
    motion(560,420); assert(frames.size()==2 && d.pinch.second);
    assert(value(frames.back(),ABS_MT_TRACKING_ID,1)==1);
    assert(value(frames.back(),ABS_MT_POSITION_X,1)==560);
    assert(value(frames.back(),ABS_MT_POSITION_Y,1)==420);
    motion(700,600);
    assert(d.pinch.x==500 && d.pinch.y==400);
    assert(value(frames.back(),ABS_MT_POSITION_X,1)==700);
    assert(value(frames.back(),ABS_MT_POSITION_Y,1)==600);
    for(const auto &e:frames.back().events) assert(e.code!=ABS_MT_TRACKING_ID || e.type!=EV_ABS);
    // Moving back and across the anchor never moves the first finger.
    motion(540,400); motion(400,400);
    assert(d.pinch.x==500 && value(frames.back(),ABS_MT_POSITION_X,1)==400);
    button(0);
    assert(!d.pinch.active && !d.pinch.button && !d.pointer_button_down);
    assert(d.touch_id[0]==-1 && d.touch_id[1]==-1);
    assert(frames.back().device==INPUT_POINTER); // restore hover after cancellation
    const auto &cancel=frames[frames.size()-2].events;
    int palms=0, reports=0, slot=-1;
    for(const auto &e:cancel) {
        if(e.type==EV_ABS && e.code==ABS_MT_SLOT) slot=e.value;
        if(e.type==EV_ABS && e.code==ABS_MT_TOOL_TYPE && e.value==MT_TOOL_PALM) {
            assert(slot==1-palms); ++palms;
        }
        if(e.type==EV_ABS && e.code==ABS_MT_TRACKING_ID) assert(reports==2);
        if(e.type==EV_SYN) ++reports;
    }
    assert(palms==2 && reports==3);
    // Ctrl released first: swallow remaining drag and physical button release.
    button(1); motion(600,400); ctrl(false);
    assert(!d.pinch.active && d.pinch.button);
    size_t count=frames.size(); motion(620,400); assert(frames.size()==count);
    button(0); assert(!d.pinch.button && frames.size()==count+1);
    ctrl(true);
    // One-contact click/release cleans up only the contact actually created.
    button(1); assert(!d.pinch.second); button(0);
    const auto &single=frames[frames.size()-2];
    assert(value(single,ABS_MT_TRACKING_ID,0)==-1);
    // App bounds, rather than full-display bounds, constrain the moving finger.
    d.layers[&surface].bounds={200,100,700,500};
    motion(400,300); button(1); motion(900,800);
    assert(value(frames.back(),ABS_MT_POSITION_X,1)==699);
    assert(value(frames.back(),ABS_MT_POSITION_Y,1)==499);
    button(0);
    motion(900,600); count=frames.size(); button(1);
    assert(!d.pinch.active && d.pinch.button && frames.size()==count); button(0);
    d.layers[&surface].bounds={}; motion(500,400);
    // Missing pipes / physical touchscreen activity never fall through to clicks.
    d.touch_id[3]=3; count=frames.size(); button(1);
    assert(!d.pinch.active && frames.size()==count); button(0); d.touch_id[3]=-1;
    fail_open=true; button(1); assert(!d.pinch.active && d.pinch.button);
    fail_open=false; button(0);
    // Pointer capture retains ordinary mouse buttons instead of a touch gesture.
    d.relative_pointer=&surface; count=frames.size(); button(1); button(0);
    assert(!d.pinch.active && frames.size()==count+2); d.relative_pointer=nullptr;
    // A full pipe must retry release even after the mouse has stopped moving.
    button(1); motion(600,400); fail_write=true; button(0);
    assert(d.pinch.active && d.pinch.ending && !d.pinch.button);
    fail_write=false; tick_pointer_pinch(&d); assert(!d.pinch.active);
    // Focus loss cancels both contacts; the later release is still swallowed.
    button(1); motion(650,400); pointer_handle_leave(&d,nullptr,0,&surface);
    assert(!d.pinch.active && d.pinch.button);
    button(0); assert(!d.pinch.button);
    // Pointer-enter captures transformed coordinates before the first motion.
    pointer_handle_enter(&d,nullptr,1,&surface,150*256,100*256);
    button(1); assert(d.pinch.x==400 && d.pinch.y==250); button(0);
}

'''
with tempfile.TemporaryDirectory(prefix='hwc-pinch-') as tmp:
    cpp = Path(tmp) / 'test.cpp'
    cpp.write_text(harness)
    exe = Path(tmp) / 'test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: Ctrl+drag fixed anchor, moving second contact, bounds and release/focus cleanup')
