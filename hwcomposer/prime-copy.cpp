/*
 * Cross-GPU ("PRIME") presentation of the client target; see prime-copy.h.
 *
 * Recipe (NVIDIA's own Wayland WSI, cross-GPU case): per presented image, after rendering, a
 * copy-engine copy from the block-linear image into a system-memory LINEAR buffer (pitch aligned
 * to 256 bytes, offset 0, size aligned to 4 KiB), presented with DRM_FORMAT_MOD_LINEAR. Without
 * explicit sync the present waits for the copy on the CPU before the commit, and a buffer is
 * reused only after the compositor's wl_buffer.release. Here the copy is a Vulkan
 * vkCmdCopyImageToBuffer on the guest's NVIDIA Vulkan driver (NVK: copy-engine class on the
 * queue's channel), into VkDeviceMemory from a host-visible, non-device-local memory type
 * (system memory) exported as a dma-buf. Video memory is never offered to the other GPU: an
 * nvidia-drm export of video memory has no pages and the foreign importer fails.
 *
 * Copyright 2026 waydroid-nvidia-prolapse. Apache-2.0.
 */
#define VK_NO_PROTOTYPES 1
#include "prime-copy.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <log/log.h>
#include <vulkan/vulkan.h>
#include <xf86drm.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "wayland-hwc.h"

bool prime_find_nvidia_devices(dev_t *render, dev_t *primary)
{
    *render = *primary = 0;
    DIR *dir = opendir("/dev/dri");
    if (!dir) {
        ALOGE("prime: cannot list /dev/dri: %s", strerror(errno));
        return false;
    }
    while (struct dirent *e = readdir(dir)) {
        if (strncmp(e->d_name, "renderD", 7) != 0)
            continue;
        std::string path = std::string("/dev/dri/") + e->d_name;
        int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        drmVersionPtr v = drmGetVersion(fd);
        bool nvidia = v && v->name && strcmp(v->name, "nvidia-drm") == 0;
        drmFreeVersion(v);
        struct stat st;
        if (nvidia && fstat(fd, &st) == 0)
            *render = st.st_rdev;
        close(fd);
        if (*render)
            break;
    }
    closedir(dir);
    if (!*render)
        return false;
    /* The primary node is not bound into the guest; sysfs names it (…/device/drm/cardN/dev). */
    char path[128];
    snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/drm", major(*render), minor(*render));
    if ((dir = opendir(path))) {
        while (struct dirent *e = readdir(dir)) {
            if (strncmp(e->d_name, "card", 4) != 0)
                continue;
            std::string dev = std::string(path) + "/" + e->d_name + "/dev";
            FILE *f = fopen(dev.c_str(), "re");
            unsigned maj, min;
            if (f && fscanf(f, "%u:%u", &maj, &min) == 2)
                *primary = makedev(maj, min);
            if (f)
                fclose(f);
            break;
        }
        closedir(dir);
    }
    if (!*primary)
        ALOGW("prime: NVIDIA primary node not found under %s; comparing feedback devices with the "
              "render node %u:%u only", path, major(*render), minor(*render));
    return true;
}

namespace {

#define VK_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define VK_DEV_FUNCS(X) \
    X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkCreateFence) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkGetMemoryFdKHR) \
    X(vkGetMemoryFdPropertiesKHR) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyImageToBuffer) X(vkQueueSubmit) \
    X(vkWaitForFences) X(vkResetFences)

struct copier;
struct target {
    copier *owner = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    struct wl_buffer *wl_buffer = nullptr;
    bool busy = false;          /* attached and not yet released by the compositor */
    uint64_t last_used = 0;
};

struct source {
    copier *owner = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0, drm_format = 0, bpp = 0;
};

struct copier {
    void *lib = nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    uint32_t sysmem_type_bits = 0;     /* HOST_VISIBLE and not DEVICE_LOCAL */
    VkPhysicalDeviceMemoryProperties mem_props{};
#define DECL(f) PFN_##f f = nullptr;
    VK_FUNCS(DECL) VK_DEV_FUNCS(DECL)
#undef DECL

    /* copy targets for the current geometry/format; release events come from the Wayland thread */
    std::mutex mutex;
    std::condition_variable released;
    std::deque<target> targets;
    uint32_t t_width = 0, t_height = 0, t_format = 0, t_pitch = 0;
    uint64_t frame = 0;
    bool logged_first = false;
};

const char *vk_err(VkResult r)
{
    static char buf[32];
    snprintf(buf, sizeof(buf), "VkResult %d", (int)r);
    return buf;
}

bool init_copier(copier *c)
{
    c->lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!c->lib) {
        ALOGE("prime: cannot load libvulkan.so: %s", dlerror());
        return false;
    }
    c->gipa = (PFN_vkGetInstanceProcAddr)dlsym(c->lib, "vkGetInstanceProcAddr");
    auto create_instance = (PFN_vkCreateInstance)c->gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "hwcomposer.waydroid cross-GPU copy";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkResult r = create_instance(&ici, nullptr, &c->instance);
    if (r != VK_SUCCESS) {
        ALOGE("prime: vkCreateInstance failed: %s", vk_err(r));
        return false;
    }
#define LOAD(f) c->f = (PFN_##f)c->gipa(c->instance, #f); \
    if (!c->f) { ALOGE("prime: Vulkan entry point " #f " missing"); return false; }
    VK_FUNCS(LOAD)
#undef LOAD

    uint32_t n = 0;
    c->vkEnumeratePhysicalDevices(c->instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pdevs(n);
    c->vkEnumeratePhysicalDevices(c->instance, &n, pdevs.data());
    for (auto p : pdevs) {
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        c->vkGetPhysicalDeviceProperties2(p, &props);
        if (props.properties.vendorID == 0x10de) {
            c->pdev = p;
            ALOGI("prime: copying on %s (Vulkan %u.%u)", props.properties.deviceName,
                  VK_API_VERSION_MAJOR(props.properties.apiVersion),
                  VK_API_VERSION_MINOR(props.properties.apiVersion));
            break;
        }
    }
    if (!c->pdev) {
        ALOGE("prime: no NVIDIA Vulkan device among %u", n);
        return false;
    }

    const char *exts[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    };
    c->vkEnumerateDeviceExtensionProperties(c->pdev, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> have(n);
    c->vkEnumerateDeviceExtensionProperties(c->pdev, nullptr, &n, have.data());
    for (const char *e : exts) {
        if (std::none_of(have.begin(), have.end(), [&](auto &h) { return !strcmp(h.extensionName, e); })) {
            ALOGE("prime: the NVIDIA Vulkan device lacks %s", e);
            return false;
        }
    }

    /* A transfer-capable queue; NVK's copies run on the copy-engine class of that channel. */
    c->vkGetPhysicalDeviceQueueFamilyProperties(c->pdev, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    c->vkGetPhysicalDeviceQueueFamilyProperties(c->pdev, &n, qf.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++) {   /* prefer a transfer-only family (a copy-engine channel) */
        if ((qf[i].queueFlags & VK_QUEUE_TRANSFER_BIT)
            && !(qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) { family = i; break; }
    }
    for (uint32_t i = 0; family == UINT32_MAX && i < n; i++)
        if (qf[i].queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            family = i;
    if (family == UINT32_MAX) {
        ALOGE("prime: no transfer-capable queue family");
        return false;
    }
    c->queue_family = family;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = sizeof(exts) / sizeof(exts[0]);
    dci.ppEnabledExtensionNames = exts;
    r = c->vkCreateDevice(c->pdev, &dci, nullptr, &c->dev);
    if (r != VK_SUCCESS) {
        ALOGE("prime: vkCreateDevice failed: %s", vk_err(r));
        return false;
    }
#define LOAD(f) c->f = (PFN_##f)c->vkGetDeviceProcAddr(c->dev, #f); \
    if (!c->f) { ALOGE("prime: Vulkan device entry point " #f " missing"); return false; }
    VK_DEV_FUNCS(LOAD)
#undef LOAD
    c->vkGetDeviceQueue(c->dev, family, 0, &c->queue);

    c->vkGetPhysicalDeviceMemoryProperties(c->pdev, &c->mem_props);
    for (uint32_t i = 0; i < c->mem_props.memoryTypeCount; i++) {
        auto f = c->mem_props.memoryTypes[i].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            c->sysmem_type_bits |= 1u << i;
    }
    if (!c->sysmem_type_bits) {
        ALOGE("prime: the NVIDIA Vulkan device has no host-visible system-memory type");
        return false;
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if ((r = c->vkCreateCommandPool(c->dev, &pci, nullptr, &c->pool)) != VK_SUCCESS
        || ((cai.commandPool = c->pool), (r = c->vkAllocateCommandBuffers(c->dev, &cai, &c->cmd)) != VK_SUCCESS)
        || (r = c->vkCreateFence(c->dev, &fci, nullptr, &c->fence)) != VK_SUCCESS) {
        ALOGE("prime: command pool/buffer/fence creation failed: %s", vk_err(r));
        return false;
    }
    return true;
}

copier *get_copier(display *d)
{
    static bool failed = false;
    if (d->prime_copier)
        return (copier *)d->prime_copier;
    if (failed)
        return nullptr;
    copier *c = new copier();
    if (!init_copier(c)) {
        failed = true;   /* logged above; not retried every frame. Objects leak on purpose:
                          * this is a one-time failure of a process-lifetime object. */
        ALOGE("prime: cross-GPU copy unavailable; the client target is not shown (a "
              "block-linear buffer would be rejected by the compositor with error 7)");
        return nullptr;
    }
    d->prime_copier = c;
    return c;
}

bool vk_format_for(uint32_t drm_format, VkFormat *fmt, uint32_t *bpp)
{
    switch (drm_format) {
        case DRM_FORMAT_ABGR8888: case DRM_FORMAT_XBGR8888: *fmt = VK_FORMAT_R8G8B8A8_UNORM; *bpp = 4; return true;
        case DRM_FORMAT_ARGB8888: case DRM_FORMAT_XRGB8888: *fmt = VK_FORMAT_B8G8R8A8_UNORM; *bpp = 4; return true;
        case DRM_FORMAT_ABGR2101010: case DRM_FORMAT_XBGR2101010:
            *fmt = VK_FORMAT_A2B10G10R10_UNORM_PACK32; *bpp = 4; return true;
        case DRM_FORMAT_RGB565: *fmt = VK_FORMAT_R5G6B5_UNORM_PACK16; *bpp = 2; return true;
        case DRM_FORMAT_ABGR16161616F: case DRM_FORMAT_XBGR16161616F:
            *fmt = VK_FORMAT_R16G16B16A16_SFLOAT; *bpp = 8; return true;
        default: return false;
    }
}

uint32_t pick_type(const copier *c, uint32_t bits)
{
    for (uint32_t i = 0; i < c->mem_props.memoryTypeCount; i++)
        if (bits & (1u << i))
            return i;
    return UINT32_MAX;
}

void target_release(void *data, struct wl_buffer *)
{
    auto *t = (target *)data;
    std::lock_guard<std::mutex> lock(t->owner->mutex);
    t->busy = false;
    t->owner->released.notify_all();
}
const struct wl_buffer_listener target_listener = { target_release };

void destroy_targets(copier *c)
{
    std::lock_guard<std::mutex> lock(c->mutex);
    for (auto &t : c->targets) {
        /* The compositor keeps its own reference to the dma-buf; destroying the wl_buffer and
         * freeing our memory while it is still on screen is safe. */
        wl_buffer_destroy(t.wl_buffer);
        c->vkDestroyBuffer(c->dev, t.buffer, nullptr);
        c->vkFreeMemory(c->dev, t.memory, nullptr);
    }
    c->targets.clear();
}

/* One sysmem LINEAR copy target: pitch = align(width * bpp, 256), size = align(pitch * height,
 * 4096), offset 0, modifier LINEAR (the stock request). Appended to c->targets under c->mutex. */
bool make_target(display *d, copier *c)
{
    const VkDeviceSize size = ((VkDeviceSize)c->t_pitch * c->t_height + 0xfff) & ~(VkDeviceSize)0xfff;
    VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = &ext;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    target t;
    t.owner = c;
    VkResult r = c->vkCreateBuffer(c->dev, &bci, nullptr, &t.buffer);
    if (r != VK_SUCCESS) {
        ALOGE("prime: vkCreateBuffer(%llu bytes) failed: %s", (unsigned long long)size, vk_err(r));
        return false;
    }
    VkMemoryRequirements req;
    c->vkGetBufferMemoryRequirements(c->dev, t.buffer, &req);
    uint32_t type = pick_type(c, req.memoryTypeBits & c->sysmem_type_bits);
    if (type == UINT32_MAX) {
        ALOGE("prime: no system-memory type for the copy target (buffer types 0x%x, sysmem 0x%x)",
              req.memoryTypeBits, c->sysmem_type_bits);
        c->vkDestroyBuffer(c->dev, t.buffer, nullptr);
        return false;
    }
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.buffer = t.buffer;
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.pNext = &ded;
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &exp;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    int fd = -1;
    VkMemoryGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    if ((r = c->vkAllocateMemory(c->dev, &mai, nullptr, &t.memory)) != VK_SUCCESS
        || (r = c->vkBindBufferMemory(c->dev, t.buffer, t.memory, 0)) != VK_SUCCESS
        || ((gfi.memory = t.memory), (r = c->vkGetMemoryFdKHR(c->dev, &gfi, &fd)) != VK_SUCCESS)) {
        ALOGE("prime: system-memory copy target (type %u, %llu bytes): allocate/bind/export "
              "failed: %s", type, (unsigned long long)req.size, vk_err(r));
        if (t.memory)
            c->vkFreeMemory(c->dev, t.memory, nullptr);
        c->vkDestroyBuffer(c->dev, t.buffer, nullptr);
        return false;
    }
    zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(d->dmabuf);
    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, c->t_pitch, DRM_FORMAT_MOD_LINEAR >> 32,
                                   DRM_FORMAT_MOD_LINEAR & 0xffffffff);
    t.wl_buffer = zwp_linux_buffer_params_v1_create_immed(params, c->t_width, c->t_height, c->t_format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    close(fd);   /* libwayland duplicated it when marshalling the request */
    c->targets.push_back(t);
    target &placed = c->targets.back();   /* deque: stable address for the listener */
    wl_buffer_add_listener(placed.wl_buffer, &target_listener, &placed);
    ALOGI("prime: copy target %zu: %ux%u fourcc 0x%x pitch %u, %llu bytes system memory (type %u "
          "flags 0x%x), modifier LINEAR", c->targets.size(), c->t_width, c->t_height, c->t_format,
          c->t_pitch, (unsigned long long)req.size, type, c->mem_props.memoryTypes[type].propertyFlags);
    return true;
}

}  // namespace

std::unique_ptr<buffer> prime_create_buffer(display *d, const buffer_metadata &metadata, int dma_fd,
                                            uint32_t drm_format, uint32_t stride, uint32_t offset,
                                            uint64_t modifier, buffer_handle_t handle)
{
    copier *c = get_copier(d);
    if (!c)
        return nullptr;
    VkFormat fmt;
    uint32_t bpp;
    if (!vk_format_for(drm_format, &fmt, &bpp)) {
        ALOGE("prime: client target fourcc 0x%x has no copy format; not shown", drm_format);
        return nullptr;
    }
    auto *s = new source();
    s->owner = c;
    s->width = metadata.width;
    s->height = metadata.height;
    s->drm_format = drm_format;
    s->bpp = bpp;

    VkSubresourceLayout plane{};
    plane.offset = offset;
    plane.rowPitch = stride;
    VkImageDrmFormatModifierExplicitCreateInfoEXT mod{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
    mod.drmFormatModifier = modifier;
    mod.drmFormatModifierPlaneCount = 1;
    mod.pPlaneLayouts = &plane;
    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.pNext = &mod;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &ext;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {metadata.width, metadata.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = c->vkCreateImage(c->dev, &ici, nullptr, &s->image);
    if (r != VK_SUCCESS) {
        ALOGE("prime: importing the client target (%ux%u fourcc 0x%x modifier 0x%llx stride %u) "
              "as an image failed: %s", metadata.width, metadata.height, drm_format,
              (unsigned long long)modifier, stride, vk_err(r));
        delete s;
        return nullptr;
    }
    VkMemoryRequirements req;
    c->vkGetImageMemoryRequirements(c->dev, s->image, &req);
    VkMemoryFdPropertiesKHR fdp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    r = c->vkGetMemoryFdPropertiesKHR(c->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dma_fd, &fdp);
    if (r != VK_SUCCESS)
        fdp.memoryTypeBits = 0;   /* reported with the failed import below */
    uint32_t type = pick_type(c, req.memoryTypeBits & fdp.memoryTypeBits);
    int fd = fcntl(dma_fd, F_DUPFD_CLOEXEC, 0);   /* a successful import consumes it */
    VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    imp.fd = fd;
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.pNext = &imp;
    ded.image = s->image;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &ded;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (fd < 0 || type == UINT32_MAX
        || (r = c->vkAllocateMemory(c->dev, &mai, nullptr, &s->memory)) != VK_SUCCESS
        || (r = c->vkBindImageMemory(c->dev, s->image, s->memory, 0)) != VK_SUCCESS) {
        ALOGE("prime: importing the client target's dma-buf failed (fd %d, image types 0x%x, "
              "dma-buf types 0x%x, chosen %u, %s)", fd, req.memoryTypeBits, fdp.memoryTypeBits,
              type, vk_err(r));
        if (s->memory)
            c->vkFreeMemory(c->dev, s->memory, nullptr);
        else if (fd >= 0)
            close(fd);
        c->vkDestroyImage(c->dev, s->image, nullptr);
        delete s;
        return nullptr;
    }

    std::unique_ptr<buffer> buf { new buffer() };
    buf->metadata = metadata;
    buf->handle = handle;
    buf->isShm = false;
    buf->prime = true;
    buf->prime_source = s;
    buf->wl_buffer = nullptr;   /* chosen per frame by prime_copy() */
    ALOGI("prime: client target %ux%u fourcc 0x%x modifier 0x%llx stride %u imported for copying",
          metadata.width, metadata.height, drm_format, (unsigned long long)modifier, stride);
    return buf;
}

void prime_release_source(void *p)
{
    auto *s = (source *)p;
    if (!s)
        return;
    copier *c = s->owner;
    /* prime_copy() waits for its copy before returning, so the image is idle here */
    c->vkDestroyImage(c->dev, s->image, nullptr);
    c->vkFreeMemory(c->dev, s->memory, nullptr);
    delete s;
}

int prime_copy(display *d, buffer *buf)
{
    auto *s = (source *)buf->prime_source;
    copier *c = s->owner;
    /* Geometry or format change (resize, display-mode switch): new pool. */
    uint32_t format = s->drm_format;
    if (buf->metadata.opaque) {
        if (format == DRM_FORMAT_ABGR8888) format = DRM_FORMAT_XBGR8888;
        else if (format == DRM_FORMAT_ARGB8888) format = DRM_FORMAT_XRGB8888;
        else if (format == DRM_FORMAT_ABGR2101010) format = DRM_FORMAT_XBGR2101010;
        else if (format == DRM_FORMAT_ABGR16161616F) format = DRM_FORMAT_XBGR16161616F;
    }
    if (c->t_width != s->width || c->t_height != s->height || c->t_format != format) {
        destroy_targets(c);
        c->t_width = s->width;
        c->t_height = s->height;
        c->t_format = format;
        c->t_pitch = (s->width * s->bpp + 0xff) & ~0xffu;   /* stock: 256-byte pitch alignment */
    }

    /* A target the compositor has released; up to 4 (SurfaceFlinger triple-buffers the client
     * target, plus one in flight), then wait for a release like NVIDIA's WSI does without
     * explicit sync. */
    constexpr size_t kMaxTargets = 4;
    target *t = nullptr;
    {
        std::unique_lock<std::mutex> lock(c->mutex);
        auto find_free = [&]() -> target * {
            target *best = nullptr;
            for (auto &x : c->targets)
                if (!x.busy && (!best || x.last_used < best->last_used))
                    best = &x;
            return best;
        };
        t = find_free();
        if (!t && c->targets.size() < kMaxTargets) {
            lock.unlock();
            bool ok = make_target(d, c);   /* takes no lock while talking Vulkan */
            lock.lock();
            if (!ok)
                return -1;
            t = &c->targets.back();
        }
        if (!t && !c->released.wait_for(lock, std::chrono::milliseconds(100),
                                        [&] { return (t = find_free()) != nullptr; })) {
            /* the compositor holds every target: reuse the oldest (may tear), say so */
            static int count = 0;
            if ((count++ % 100) == 0)
                ALOGE("prime: the compositor released none of %zu copy targets within 100 ms; "
                      "reusing the oldest (%d times)", c->targets.size(), count);
            t = &*std::min_element(c->targets.begin(), c->targets.end(),
                                   [](auto &a, auto &b) { return a.last_used < b.last_used; });
        }
        t->busy = true;
        t->last_used = ++c->frame;
    }

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    c->vkResetCommandBuffer(c->cmd, 0);
    c->vkBeginCommandBuffer(c->cmd, &bi);
    /* acquire the image from its producer (SurfaceFlinger's GPU work, already complete: the
     * caller waited for the acquire fence) */
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = 0;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    ib.dstQueueFamilyIndex = c->queue_family;
    ib.image = s->image;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    c->vkCmdPipelineBarrier(c->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &ib);
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = c->t_pitch / s->bpp;
    region.bufferImageHeight = s->height;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {s->width, s->height, 1};
    c->vkCmdCopyImageToBuffer(c->cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->buffer, 1, &region);
    /* hand the buffer to the other GPU, and the image back to its producer */
    VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = 0;
    bb.srcQueueFamilyIndex = c->queue_family;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    bb.buffer = t->buffer;
    bb.size = VK_WHOLE_SIZE;
    ib.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    ib.dstAccessMask = 0;
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = c->queue_family;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    c->vkCmdPipelineBarrier(c->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            0, 0, nullptr, 1, &bb, 1, &ib);
    VkResult r = c->vkEndCommandBuffer(c->cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c->cmd;
    if (r == VK_SUCCESS)
        r = c->vkQueueSubmit(c->queue, 1, &si, c->fence);
    /* No explicit sync towards the compositor: the commit waits for the copy (stock's fallback
     * without wp_linux_drm_syncobj). */
    if (r == VK_SUCCESS)
        r = c->vkWaitForFences(c->dev, 1, &c->fence, VK_TRUE, 1000000000ull);
    c->vkResetFences(c->dev, 1, &c->fence);
    if (r != VK_SUCCESS) {
        ALOGE("prime: copy of the client target failed: %s; frame not presented", vk_err(r));
        std::lock_guard<std::mutex> lock(c->mutex);
        t->busy = false;
        return -1;
    }
    buf->wl_buffer = t->wl_buffer;
    if (!c->logged_first) {
        c->logged_first = true;
        ALOGI("prime: first cross-GPU frame: %ux%u copied on the NVIDIA GPU into system-memory "
              "LINEAR pitch %u and attached", s->width, s->height, c->t_pitch);
    }
    return 0;
}
