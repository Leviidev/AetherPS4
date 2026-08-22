#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    }
    return false;
}

}  // namespace

int main() {
    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "bachata-vulkan-info";
    app_info.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app_info;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instance_info, nullptr, &instance);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "BACHATA_VULKAN_ERROR create_instance=%d\n", result);
        return 1;
    }

    uint32_t physical_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &physical_count, nullptr);
    if (result != VK_SUCCESS || physical_count == 0) {
        std::fprintf(stderr, "BACHATA_VULKAN_ERROR enumerate_devices=%d count=%u\n", result, physical_count);
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    std::vector<VkPhysicalDevice> devices(physical_count);
    vkEnumeratePhysicalDevices(instance, &physical_count, devices.data());
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(devices[0], &properties);

    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(devices[0], nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(devices[0], nullptr, &extension_count, extensions.data());
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(devices[0], &memory);
    uint64_t device_local_bytes = 0;
    for (uint32_t index = 0; index < memory.memoryHeapCount; ++index) {
        if ((memory.memoryHeaps[index].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            device_local_bytes += memory.memoryHeaps[index].size;
        }
    }

    const bool api_ok = VK_VERSION_MAJOR(properties.apiVersion) > 1 ||
        (VK_VERSION_MAJOR(properties.apiVersion) == 1 && VK_VERSION_MINOR(properties.apiVersion) >= 3);
    const bool swapchain_ok = has_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    std::printf(
        "BACHATA_VULKAN_DEVICE name=\"%s\" api=%u.%u.%u vendor=0x%04x device=0x%04x "
        "driver=%u deviceLocalBytes=%llu swapchain=%s\n",
        properties.deviceName,
        VK_VERSION_MAJOR(properties.apiVersion),
        VK_VERSION_MINOR(properties.apiVersion),
        VK_VERSION_PATCH(properties.apiVersion),
        properties.vendorID,
        properties.deviceID,
        properties.driverVersion,
        static_cast<unsigned long long>(device_local_bytes),
        swapchain_ok ? "true" : "false");
    vkDestroyInstance(instance, nullptr);
    if (!api_ok || !swapchain_ok || device_local_bytes == 0) {
        std::fputs("BACHATA_VULKAN_UNSUPPORTED\n", stderr);
        return 2;
    }
    std::puts("BACHATA_VULKAN_OK frames=0 mode=capability");
    return 0;
}
