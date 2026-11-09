/*
r_vk.h - ref_vk: the Vulkan entry points and the backend's state
Copyright (C) 2026 18woldemar

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#ifndef R_VK_H
#define R_VK_H

// Nothing links libvulkan: the engine hands over vkGetInstanceProcAddr and everything else is fetched from it.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define VK_GLOBAL_FUNCTIONS( X ) \
	X( vkCreateInstance ) \
	X( vkEnumerateInstanceVersion )

#define VK_INSTANCE_FUNCTIONS( X ) \
	X( vkDestroyInstance ) \
	X( vkEnumeratePhysicalDevices ) \
	X( vkGetPhysicalDeviceProperties ) \
	X( vkGetPhysicalDeviceQueueFamilyProperties ) \
	X( vkEnumerateDeviceExtensionProperties ) \
	X( vkCreateDevice ) \
	X( vkGetDeviceProcAddr ) \
	X( vkDestroySurfaceKHR ) \
	X( vkGetPhysicalDeviceSurfaceSupportKHR ) \
	X( vkGetPhysicalDeviceSurfaceCapabilitiesKHR ) \
	X( vkGetPhysicalDeviceSurfaceFormatsKHR ) \
	X( vkGetPhysicalDeviceSurfacePresentModesKHR )

#define VK_DEVICE_FUNCTIONS( X ) \
	X( vkDestroyDevice ) \
	X( vkGetDeviceQueue ) \
	X( vkDeviceWaitIdle ) \
	X( vkQueueSubmit ) \
	X( vkCreateSwapchainKHR ) \
	X( vkDestroySwapchainKHR ) \
	X( vkGetSwapchainImagesKHR ) \
	X( vkAcquireNextImageKHR ) \
	X( vkQueuePresentKHR ) \
	X( vkCreateCommandPool ) \
	X( vkDestroyCommandPool ) \
	X( vkAllocateCommandBuffers ) \
	X( vkResetCommandBuffer ) \
	X( vkBeginCommandBuffer ) \
	X( vkEndCommandBuffer ) \
	X( vkCreateFence ) \
	X( vkDestroyFence ) \
	X( vkWaitForFences ) \
	X( vkResetFences ) \
	X( vkCreateSemaphore ) \
	X( vkDestroySemaphore ) \
	X( vkCreateImageView ) \
	X( vkDestroyImageView ) \
	X( vkCreateRenderPass ) \
	X( vkDestroyRenderPass ) \
	X( vkCreateFramebuffer ) \
	X( vkDestroyFramebuffer ) \
	X( vkCmdBeginRenderPass ) \
	X( vkCmdEndRenderPass )

#define VK_DECLARE( name ) extern PFN_##name name;
VK_GLOBAL_FUNCTIONS( VK_DECLARE )
VK_INSTANCE_FUNCTIONS( VK_DECLARE )
VK_DEVICE_FUNCTIONS( VK_DECLARE )
#undef VK_DECLARE

// the GL entry points, answering nothing, until each file's drawing has moved to the backend (r_vk_glstub.c)
void R_VKInstallGLStubs( void );

#endif // R_VK_H
