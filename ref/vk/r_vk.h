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
	X( vkGetPhysicalDeviceFeatures ) \
	X( vkGetPhysicalDeviceMemoryProperties ) \
	X( vkGetPhysicalDeviceFormatProperties ) \
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
	X( vkCmdEndRenderPass ) \
	X( vkCreateBuffer ) \
	X( vkDestroyBuffer ) \
	X( vkGetBufferMemoryRequirements ) \
	X( vkBindBufferMemory ) \
	X( vkAllocateMemory ) \
	X( vkFreeMemory ) \
	X( vkMapMemory ) \
	X( vkCreateImage ) \
	X( vkDestroyImage ) \
	X( vkGetImageMemoryRequirements ) \
	X( vkBindImageMemory ) \
	X( vkCreateSampler ) \
	X( vkDestroySampler ) \
	X( vkCreateShaderModule ) \
	X( vkDestroyShaderModule ) \
	X( vkCreatePipelineCache ) \
	X( vkDestroyPipelineCache ) \
	X( vkCreateGraphicsPipelines ) \
	X( vkDestroyPipeline ) \
	X( vkCreatePipelineLayout ) \
	X( vkDestroyPipelineLayout ) \
	X( vkCreateDescriptorSetLayout ) \
	X( vkDestroyDescriptorSetLayout ) \
	X( vkCreateDescriptorPool ) \
	X( vkDestroyDescriptorPool ) \
	X( vkResetDescriptorPool ) \
	X( vkAllocateDescriptorSets ) \
	X( vkUpdateDescriptorSets ) \
	X( vkQueueWaitIdle ) \
	X( vkCmdPipelineBarrier ) \
	X( vkCmdCopyBufferToImage ) \
	X( vkCmdBindPipeline ) \
	X( vkCmdBindDescriptorSets ) \
	X( vkCmdBindVertexBuffers ) \
	X( vkCmdSetViewport ) \
	X( vkCmdSetScissor ) \
	X( vkCmdSetDepthBias ) \
	X( vkCmdClearAttachments ) \
	X( vkCmdDraw ) \
	X( vkCmdDrawIndexed ) \
	X( vkCmdBindIndexBuffer ) \
	X( vkCmdCopyBuffer )

#define VK_DECLARE( name ) extern PFN_##name name;
VK_GLOBAL_FUNCTIONS( VK_DECLARE )
VK_INSTANCE_FUNCTIONS( VK_DECLARE )
VK_DEVICE_FUNCTIONS( VK_DECLARE )
#undef VK_DECLARE

#define VK_FRAMES     2
#define VK_MAX_SWAP   16
#define VK_MAX_CHUNKS 16
#define VK_MAX_POOLS  32
#define VK_MAX_DESTROY 8192

// memory carved out of a block (r_vk_backend.c)
typedef struct
{
	VkDeviceMemory memory;
	VkDeviceSize   offset, size;
	int            block;
} vk_allocation_t;

// a growing list of host-visible buffers, emptied when its frame has finished
typedef struct
{
	VkBuffer        buffer;
	VkDeviceMemory  memory;
	byte           *mapped;
	VkDeviceSize    size;
	VkDescriptorSet uniforms; // the uniform ring's: both register blocks of this buffer
} vk_chunk_t;

typedef struct
{
	vk_chunk_t         chunks[VK_MAX_CHUNKS];
	int                num_chunks, current;
	VkDeviceSize       offset, chunk_size;
	VkBufferUsageFlags usage;
} vk_ring_t;

typedef struct
{
	VkImage         image;
	VkImageView     view;
	vk_allocation_t memory;
} vk_dead_image_t;

typedef struct
{
	VkBuffer       buffer;
	VkDeviceMemory memory;
} vk_dead_buffer_t;

// what the 3D part or the 2D part draws into
enum
{
	VK_TARGET_SWAP = 0,
	VK_TARGET_SCENE,
	VK_TARGET_COUNT
};

// A frame's resources are idle whenever it is the current one: R_GL3Present waits for it right after moving on to it.
typedef struct
{
	VkCommandBuffer cmd, upload;
	VkFence         done;
	VkSemaphore     acquired;

	vk_ring_t staging, vertices, uniforms;

	VkDescriptorPool pools[VK_MAX_POOLS];
	int              num_pools, current_pool;

	vk_dead_image_t  dead[VK_MAX_DESTROY];
	int              num_dead;
	vk_dead_buffer_t dead_buffers[16];
	int              num_dead_buffers;
} vk_frame_t;

typedef struct
{
	VkInstance       instance;
	VkSurfaceKHR     surface;
	VkPhysicalDevice physical;
	VkDevice         device;
	VkQueue          queue;
	uint32_t         family;
	VkCommandPool    pool;
	VkPhysicalDeviceLimits limits;
	qboolean         anisotropy;

	VkFormat        format, depth_format;
	VkColorSpaceKHR colorspace;
	VkRenderPass    pass_clear[VK_TARGET_COUNT], pass_load[VK_TARGET_COUNT]; // a target's first pass of a frame, and any after a break

	// the 3D part's picture: its memory is row for row a GL framebuffer's, which is why nothing flips here
	VkFramebuffer   scene_framebuffer;
	VkImage         scene_depth;
	VkImageView     scene_depth_view;
	vk_allocation_t scene_depth_memory;
	VkExtent2D      scene_extent;
	int             scene_texture; // its slot in the core's texture table
	qboolean        scene_pending, scene_active, scene_resolved;

	VkSwapchainKHR  swapchain;
	uint32_t        num_images;
	VkImage         images[VK_MAX_SWAP];
	VkImageView     views[VK_MAX_SWAP];
	VkFramebuffer   framebuffers[VK_MAX_SWAP];
	VkSemaphore     rendered[VK_MAX_SWAP]; // one per image: a present holds its semaphore until the image comes back
	VkImage         depth;
	VkImageView     depth_view;
	vk_allocation_t depth_memory;
	VkExtent2D      extent;
	int             size_w, size_h; // the window size the swapchain was made for
	int             vsync;
	qboolean        rebuild;

	vk_frame_t frames[VK_FRAMES];
	uint       frame;
	uint       serial;       // counts frames: what "used this frame" compares against
	qboolean   cmd_open, upload_open, pass_open;
	qboolean   skip;         // no image to draw on this frame
	qboolean   acquired;
	qboolean   started[VK_TARGET_COUNT]; // this frame's picture has had its first pass
	int        target;
	uint32_t   image_index;
	float      clear[3];

	cvar_t *gl_vsync;
} vk_t;

extern vk_t vk;

// r_vk_device.c
qboolean R_VKCheck( VkResult result, const char *what );
VkCommandBuffer R_VKCmd( void );
VkCommandBuffer R_VKUploadCmd( void );
qboolean R_VKEnsurePass( void ); // false: nothing to draw on this frame
void R_VKEndPass( void );
void R_VKFlushUploads( void );

// r_vk_backend.c
qboolean R_VKInitBackend( void );
void R_VKShutdownBackend( void );
void R_VKResetFrame( vk_frame_t *frame ); // its work has finished
qboolean R_VKAllocImageMemory( VkImage image, vk_allocation_t *out );
void R_VKFreeMemory( vk_allocation_t *allocation );
void R_VKMarkStateDirty( void ); // a new command buffer has no dynamic state yet
void R_VKMarkViewportDirty( void ); // another target
gl3_handle_t R_VKCreateRenderTexture( VkFormat format, int width, int height, VkImageView *view );

// the GL entry points, answering nothing, until each file's drawing has moved to the backend (r_vk_glstub.c)
void R_VKInstallGLStubs( void );

#endif // R_VK_H
