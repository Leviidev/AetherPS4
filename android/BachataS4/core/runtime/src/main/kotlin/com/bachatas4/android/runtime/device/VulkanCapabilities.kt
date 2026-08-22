package com.bachatas4.android.runtime.device

data class VulkanDeviceReport(
    val apiVersion: String,
    val deviceName: String,
    val driverVersion: String,
    val extensions: Set<String>,
    val memoryHeaps: List<VulkanMemoryHeap>,
)

data class VulkanMemoryHeap(
    val sizeBytes: Long,
    val deviceLocal: Boolean,
)

data class VulkanCapabilityResult(
    val support: VulkanSupport,
    val reason: String? = null,
)

enum class VulkanSupport {
    SUPPORTED,
    VULKAN_UNSUPPORTED,
    DRIVER_BLOCKED,
}

object VulkanCapabilities {
    fun evaluate(
        report: VulkanDeviceReport,
        denylistedDriverVersions: Set<String> = emptySet(),
    ): VulkanCapabilityResult {
        if (report.driverVersion in denylistedDriverVersions) {
            return VulkanCapabilityResult(VulkanSupport.DRIVER_BLOCKED, "driver denylisted")
        }
        if (!isAtLeastVulkan13(report.apiVersion)) {
            return VulkanCapabilityResult(VulkanSupport.VULKAN_UNSUPPORTED, "Vulkan 1.3 required")
        }
        if (!isSupportedAdreno(report.deviceName)) {
            return VulkanCapabilityResult(VulkanSupport.VULKAN_UNSUPPORTED, "Adreno 750 or 830 required")
        }
        if ("VK_KHR_swapchain" !in report.extensions) {
            return VulkanCapabilityResult(VulkanSupport.VULKAN_UNSUPPORTED, "VK_KHR_swapchain required")
        }
        if (report.memoryHeaps.none { it.deviceLocal && it.sizeBytes > 0L }) {
            return VulkanCapabilityResult(VulkanSupport.VULKAN_UNSUPPORTED, "device-local heap required")
        }
        return VulkanCapabilityResult(VulkanSupport.SUPPORTED)
    }

    private fun isAtLeastVulkan13(apiVersion: String): Boolean {
        val parts = apiVersion.split('.').mapNotNull(String::toIntOrNull)
        val major = parts.getOrNull(0) ?: return false
        val minor = parts.getOrNull(1) ?: return false
        return major > 1 || (major == 1 && minor >= 3)
    }

    private fun isSupportedAdreno(deviceName: String): Boolean {
        val normalized = deviceName.lowercase()
        return "adreno" in normalized && ("750" in normalized || "830" in normalized)
    }
}
