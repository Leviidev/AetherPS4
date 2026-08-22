package com.bachatas4.android.runtime.device

import org.junit.Assert.assertEquals
import org.junit.Test

class VulkanCapabilitiesTest {
    @Test
    fun supportsAdreno750WithVulkan13SwapchainAndDeviceLocalHeap() {
        val report = validReport(deviceName = "Adreno (TM) 750")

        assertEquals(VulkanSupport.SUPPORTED, VulkanCapabilities.evaluate(report).support)
    }

    @Test
    fun supportsAdreno830WithVulkan13SwapchainAndDeviceLocalHeap() {
        val report = validReport(deviceName = "QUALCOMM Adreno 830")

        assertEquals(VulkanSupport.SUPPORTED, VulkanCapabilities.evaluate(report).support)
    }

    @Test
    fun rejectsVulkan12() {
        val report = validReport(apiVersion = "1.2.281")

        assertEquals(VulkanSupport.VULKAN_UNSUPPORTED, VulkanCapabilities.evaluate(report).support)
    }

    @Test
    fun rejectsMissingSwapchainExtension() {
        val report = validReport(extensions = setOf("VK_KHR_maintenance1"))

        assertEquals(VulkanSupport.VULKAN_UNSUPPORTED, VulkanCapabilities.evaluate(report).support)
    }

    @Test
    fun rejectsMissingDeviceLocalHeap() {
        val report = validReport(memoryHeaps = listOf(VulkanMemoryHeap(sizeBytes = 0, deviceLocal = true)))

        assertEquals(VulkanSupport.VULKAN_UNSUPPORTED, VulkanCapabilities.evaluate(report).support)
    }

    @Test
    fun blocksExplicitDenylistMatch() {
        val report = validReport(driverVersion = "bad-turnip-build")

        assertEquals(
            VulkanSupport.DRIVER_BLOCKED,
            VulkanCapabilities.evaluate(report, denylistedDriverVersions = setOf("bad-turnip-build")).support,
        )
    }

    private fun validReport(
        apiVersion: String = "1.3.280",
        deviceName: String = "Adreno (TM) 750",
        driverVersion: String = "turnip-26.1.3",
        extensions: Set<String> = setOf("VK_KHR_swapchain"),
        memoryHeaps: List<VulkanMemoryHeap> = listOf(VulkanMemoryHeap(sizeBytes = 512L * 1024L * 1024L, deviceLocal = true)),
    ) = VulkanDeviceReport(
        apiVersion = apiVersion,
        deviceName = deviceName,
        driverVersion = driverVersion,
        extensions = extensions,
        memoryHeaps = memoryHeaps,
    )
}
