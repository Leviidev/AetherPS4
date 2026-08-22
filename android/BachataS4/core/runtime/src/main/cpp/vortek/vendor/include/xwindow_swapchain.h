#ifndef VORTEK_XWINDOW_SWAPCHAIN_H
#define VORTEK_XWINDOW_SWAPCHAIN_H

#include <android/hardware_buffer.h>

#include "vortek.h"

/* Per-image present lifetime (product ownership model). */
typedef enum XWImageLife {
    XW_IMG_AVAILABLE = 0,
    XW_IMG_ACQUIRED = 1,
    XW_IMG_RENDER_SUBMITTED = 2,
    XW_IMG_PRESENT_PENDING = 3,
} XWImageLife;

typedef struct XWindowSwapchain_Image {
    VkImage image;
    VkDeviceMemory memory;
    AHardwareBuffer* ahb;
    int owns_ahb; /* 1 if AHardwareBuffer_allocate (must release) */
    uint64_t ahb_id; /* tracker identity (0x1xxx...) */
    uint64_t allocation_size;
    uint32_t ahb_usage;
    uint32_t stride;
    uint64_t native_handle; /* AHardwareBuffer_getId or pointer fallback */
    uint32_t image_index;
    XWImageLife life;
    uint64_t acquire_generation;
    uint64_t present_id;
    bool present_pending;
} XWindowSwapchain_Image;

typedef struct XWindowSwapchain {
    int windowId;
    XWindowSwapchain_Image* images;
    int imageCount;
    int nextAcquireIndex;
    int currentImageIndex;
    int sharedAhb; /* 1 if residual shared window AHB (fail if require_distinct) */
    int distinct_ahb_confirmed;
    /* Compositor target: window drawable AHB (not guest swapchain image). */
    AHardwareBuffer* windowAhb;
    VkImage windowImage;
    VkDeviceMemory windowMemory;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    VkCommandPool blitCmdPool;
    VkCommandBuffer blitCmd;
    VkFormat imageFormat;
    VkExtent2D imageExtent;
    VkImageUsageFlags imageUsage;
    VkImageCreateFlags imageFlags;
    uint64_t recommended_ahb_usage;
    VkQueue queue;
    JMethods* jmethods;
} XWindowSwapchain;

extern void getWindowExtent(JMethods* jmethods, int windowId, VkExtent2D* extent);
extern int getSurfaceMinImageCount();
extern VkSurfaceFormatKHR* getSurfaceFormats(uint32_t* formatCount);

extern XWindowSwapchain* XWindowSwapchain_create(VkDevice device, uint32_t graphicsQueueIndex,
                                                 VkSwapchainCreateInfoKHR* swapchainInfo,
                                                 JMethods* jmethods, int windowId);
extern void XWindowSwapchain_destroy(VkDevice device, XWindowSwapchain* swapchain);
extern VkResult XWindowSwapchain_acquireNextImage(XWindowSwapchain* swapchain, uint64_t timeout,
                                                  VkSemaphore signalSemaphore, VkFence fence,
                                                  uint32_t* imageIndex);
extern void XWindowSwapchain_presentImage(XWindowSwapchain* swapchain);

#ifdef BACHATA_VORTEK_SERVER
extern VkResult BachataXWindowSwapchain_presentWithWaits(XWindowSwapchain* swapchain,
                                                         uint32_t imageIndex,
                                                         const VkSemaphore* waitSemaphores,
                                                         uint32_t waitSemaphoreCount,
                                                         uint64_t presentId);
#endif

#endif
