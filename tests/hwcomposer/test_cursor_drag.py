#!/usr/bin/env python3
"""Compile the real host button/leave handlers and cursor reset without Android."""
from pathlib import Path
import subprocess
import sys
import tempfile


def block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    tree = Path(sys.argv[1]) / 'hwcomposer'
    wayland = (tree / 'wayland-hwc.cpp').read_text()
    composer = (tree / 'hwcomposer.cpp').read_text()
    macro = wayland[wayland.index('#define ADD_EVENT'):wayland.index('\n\n', wayland.index('#define ADD_EVENT'))]
    harness = r'''
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <linux/input.h>
#include <unistd.h>
struct wl_pointer {};
struct wl_surface {};
constexpr int INPUT_POINTER = 0;
constexpr uint32_t WL_POINTER_BUTTON_STATE_PRESSED = 1;
struct display {
    wl_pointer *pointer;
    wl_surface *pointer_surface;
    void *relative_pointer = nullptr;
    std::atomic<bool> pointer_button_down{false};
    int input_fd[1]{-1};
    uint32_t pointer_enter_serial = 10;
};
struct waydroid_hwc_composer_device_1 { struct display *display; };
struct wl_cursor_cursor_handler {
    int reset_cursor(waydroid_hwc_composer_device_1 *pdev);
};
bool hidden = false;
int writes = 0;
void wl_pointer_set_cursor(wl_pointer *, uint32_t, wl_surface *surface, int, int) {
    hidden = surface == nullptr;
}
void note_input_serial(display *, uint32_t) {}
int ensure_pipe(display *, int) { return 0; }
ssize_t send_events(int, const void *data, size_t size) {
    const auto *events = static_cast<const input_event *>(data);
    assert(size == 2 * sizeof(input_event));
    assert(events[0].type == EV_KEY && events[1].type == EV_SYN);
    ++writes;
    return size;
}
#define write send_events
#define ALOGE(...) ((void)0)
''' + macro + '\n' + "void\n" + block(wayland, 'pointer_handle_button(void *data, struct wl_pointer *,') + '\n' + "void\n" + block(wayland, 'pointer_handle_leave(void *data, struct wl_pointer *,') + '\n' + block(composer, 'int wl_cursor_cursor_handler::reset_cursor(') + r'''
int main() {
    wl_pointer pointer;
    wl_surface surface;
    display d{&pointer, &surface};
    waydroid_hwc_composer_device_1 dev{&d};
    wl_cursor_cursor_handler handler;
    auto button = [&](uint32_t which, uint32_t state) {
        pointer_handle_button(&d, &pointer, 11, 0, which, state);
    };
    // fake_touch removes the Android sprite throughout a pressed mouse drag.
    button(BTN_LEFT, 1);
    assert(d.pointer_button_down);
    for (int frame = 0; frame < 20; ++frame) {
        handler.reset_cursor(&dev);
        assert(!hidden);
    }
    // Capture games retain their intentional hidden pointer.
    d.relative_pointer = &surface;
    handler.reset_cursor(&dev);
    assert(hidden);
    d.relative_pointer = nullptr;
    hidden = false;
    button(BTN_LEFT, 0);
    assert(!d.pointer_button_down);
    handler.reset_cursor(&dev);
    assert(hidden);
    // Other buttons do not start or end a primary-button drag.
    button(BTN_RIGHT, 1);
    assert(!d.pointer_button_down);
    button(BTN_LEFT, 1);
    button(BTN_RIGHT, 0);
    assert(d.pointer_button_down);
    hidden = false;
    pointer_handle_leave(&d, &pointer, 12, &surface);
    assert(!d.pointer_surface && !d.pointer_button_down);
    handler.reset_cursor(&dev);
    assert(hidden);
    assert(writes == 5);
    d.pointer = nullptr;
    assert(handler.reset_cursor(&dev) == 0);
}
'''
    with tempfile.TemporaryDirectory(prefix='hwc-cursor-') as tmp:
        cpp, binary = Path(tmp) / 'check.cpp', Path(tmp) / 'check'
        cpp.write_text(harness)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('PASS: fake-touch drags preserve the host cursor; release, leave and capture can hide it')


if __name__ == '__main__':
    main()
