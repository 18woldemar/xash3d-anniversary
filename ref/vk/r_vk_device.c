/*
r_vk_device.c - ref_vk: instance, device, swapchain, the frame
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

/*
The Vulkan side of what ref/gl3/r_device.c does for GL. The minimum is Vulkan 1.1 and VK_KHR_swapchain.
Validation is switched on from outside, with VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation; the layer prints
to the terminal by itself.
*/

#include "r_local.h"
#include "r_vk.h"

#define VK_DEFINE( name ) PFN_##name name;
VK_GLOBAL_FUNCTIONS( VK_DEFINE )
VK_INSTANCE_FUNCTIONS( VK_DEFINE )
VK_DEVICE_FUNCTIONS( VK_DEFINE )
#undef VK_DEFINE

#define VK_FRAMES        2
#define VK_MAX_SWAP      16
#define VK_MAX_ITEMS     32 // physical devices, queue families, surface formats, present modes

typedef struct
{
	VkCommandBuffer cmd;
	VkFence         done;
	VkSemaphore     acquired;
} vk_frame_t;

static struct
{
	VkInstance       instance;
	VkSurfaceKHR     surface;
	VkPhysicalDevice physical;
	VkDevice         device;
	VkQueue          queue;
	uint32_t         family;
	VkCommandPool    pool;
	VkRenderPass     pass;
	VkFormat         format;
	VkColorSpaceKHR  colorspace;

	VkSwapchainKHR swapchain;
	uint32_t       num_images;
	VkImage        images[VK_MAX_SWAP];
	VkImageView    views[VK_MAX_SWAP];
	VkFramebuffer  framebuffers[VK_MAX_SWAP];
	VkSemaphore    rendered[VK_MAX_SWAP]; // one per image: a present holds its semaphore until the image comes back
	VkExtent2D     extent;
	int            size_w, size_h; // the window size the swapchain was made for
	int            vsync;
	qboolean       rebuild;

	vk_frame_t frames[VK_FRAMES];
	uint       frame;
	float      clear[3];

	cvar_t *gl_vsync;
} vk;

static qboolean R_VKCheck( VkResult result, const char *what )
{
	if( result == VK_SUCCESS )
		return true;

	gEngfuncs.Con_Printf( S_ERROR "ref_vk: %s failed: %d\n", what, (int)result );
	return false;
}

static qboolean R_VKLoadGlobal( PFN_vkGetInstanceProcAddr get )
{
	vkCreateInstance = (PFN_vkCreateInstance)get( NULL, "vkCreateInstance" );
	vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)get( NULL, "vkEnumerateInstanceVersion" );

	// no vkEnumerateInstanceVersion means a Vulkan 1.0 loader
	return vkCreateInstance != NULL && vkEnumerateInstanceVersion != NULL;
}

static qboolean R_VKCreateInstance( PFN_vkGetInstanceProcAddr get )
{
	const char *extensions[VK_MAX_ITEMS];
	unsigned int num_extensions = VK_MAX_ITEMS;
	uint32_t version = 0;
	const VkApplicationInfo app =
	{
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "Xash3D",
		.pEngineName = "Xash3D FWGS",
		.apiVersion = VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo info =
	{
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
		.ppEnabledExtensionNames = extensions,
	};

	if( !R_VKLoadGlobal( get ))
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_vk: no Vulkan 1.1 loader\n" );
		return false;
	}

	if( vkEnumerateInstanceVersion( &version ) != VK_SUCCESS || version < VK_API_VERSION_1_1 )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_vk: no Vulkan 1.1 loader\n" );
		return false;
	}

	if( !gEngfuncs.VK_GetInstanceExtensions( &num_extensions, extensions ))
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_vk: the window system's Vulkan extensions are not available\n" );
		return false;
	}
	info.enabledExtensionCount = num_extensions;

	if( !R_VKCheck( vkCreateInstance( &info, NULL, &vk.instance ), "vkCreateInstance" ))
		return false;

#define VK_LOAD( name ) \
	if(( name = (PFN_##name)get( vk.instance, #name )) == NULL ) \
	{ \
		gEngfuncs.Con_Printf( S_ERROR "ref_vk: no %s\n", #name ); \
		return false; \
	}
	VK_INSTANCE_FUNCTIONS( VK_LOAD )
#undef VK_LOAD

	return true;
}

// the queue family that draws and presents, or -1
static int R_VKFindFamily( VkPhysicalDevice physical )
{
	VkQueueFamilyProperties families[VK_MAX_ITEMS];
	uint32_t count = VK_MAX_ITEMS;

	vkGetPhysicalDeviceQueueFamilyProperties( physical, &count, families );

	for( uint32_t i = 0; i < count; i++ )
	{
		VkBool32 present = VK_FALSE;

		if( !FBitSet( families[i].queueFlags, VK_QUEUE_GRAPHICS_BIT ))
			continue;

		vkGetPhysicalDeviceSurfaceSupportKHR( physical, i, vk.surface, &present );
		if( present )
			return (int)i;
	}

	return -1;
}

static qboolean R_VKHasSwapchain( VkPhysicalDevice physical )
{
	VkExtensionProperties *list;
	uint32_t count = 0;
	qboolean found = false;

	vkEnumerateDeviceExtensionProperties( physical, NULL, &count, NULL );
	list = (VkExtensionProperties *)Mem_Malloc( r_temppool, count * sizeof( *list ));
	vkEnumerateDeviceExtensionProperties( physical, NULL, &count, list );

	for( uint32_t i = 0; i < count && !found; i++ )
		found = !Q_strcmp( list[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME );

	Mem_Free( list );
	return found;
}

static int R_VKDeviceRank( VkPhysicalDeviceType type )
{
	switch( type )
	{
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
	case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 1;
	default:                                     return 0;
	}
}

static qboolean R_VKPickDevice( void )
{
	VkPhysicalDevice list[VK_MAX_ITEMS];
	VkPhysicalDeviceProperties props;
	uint32_t count = VK_MAX_ITEMS;
	int best = -1;

	// VK_INCOMPLETE is fine: more GPUs than anyone has
	vkEnumeratePhysicalDevices( vk.instance, &count, list );

	for( uint32_t i = 0; i < count; i++ )
	{
		int family, rank;

		vkGetPhysicalDeviceProperties( list[i], &props );
		if( props.apiVersion < VK_API_VERSION_1_1 || !R_VKHasSwapchain( list[i] ))
			continue;

		family = R_VKFindFamily( list[i] );
		rank = R_VKDeviceRank( props.deviceType );
		if( family < 0 || rank <= best )
			continue;

		best = rank;
		vk.physical = list[i];
		vk.family = family;
	}

	if( !vk.physical )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_vk: no device with Vulkan 1.1 that can draw to this window\n" );
		return false;
	}

	vkGetPhysicalDeviceProperties( vk.physical, &props );
	gEngfuncs.Con_Printf( "ref_vk: %s, Vulkan %d.%d.%d\n", props.deviceName, VK_API_VERSION_MAJOR( props.apiVersion ),
		VK_API_VERSION_MINOR( props.apiVersion ), VK_API_VERSION_PATCH( props.apiVersion ));
	return true;
}

static qboolean R_VKCreateLogicalDevice( void )
{
	const float priority = 1.0f;
	const char *extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	const VkDeviceQueueCreateInfo queue =
	{
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = vk.family,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	const VkDeviceCreateInfo info =
	{
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queue,
		.enabledExtensionCount = ARRAYSIZE( extensions ),
		.ppEnabledExtensionNames = extensions,
	};

	if( !R_VKCheck( vkCreateDevice( vk.physical, &info, NULL, &vk.device ), "vkCreateDevice" ))
		return false;

#define VK_LOAD( name ) \
	if(( name = (PFN_##name)vkGetDeviceProcAddr( vk.device, #name )) == NULL ) \
	{ \
		gEngfuncs.Con_Printf( S_ERROR "ref_vk: no %s\n", #name ); \
		return false; \
	}
	VK_DEVICE_FUNCTIONS( VK_LOAD )
#undef VK_LOAD

	vkGetDeviceQueue( vk.device, vk.family, 0, &vk.queue );
	return true;
}

// a non-sRGB format: the renderer does its own gamma, as it does on GL
static qboolean R_VKPickFormat( void )
{
	VkSurfaceFormatKHR formats[VK_MAX_ITEMS];
	uint32_t count = VK_MAX_ITEMS;

	vkGetPhysicalDeviceSurfaceFormatsKHR( vk.physical, vk.surface, &count, formats );

	for( uint32_t i = 0; i < count; i++ )
	{
		if( formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM )
		{
			vk.format = formats[i].format;
			vk.colorspace = formats[i].colorSpace;
			return true;
		}
	}

	gEngfuncs.Con_Printf( S_ERROR "ref_vk: the window offers no 8 bit UNORM format\n" );
	return false;
}

static qboolean R_VKCreateRenderPass( void )
{
	const VkAttachmentDescription color =
	{
		.format = vk.format,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	};
	const VkAttachmentReference reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass =
	{
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &reference,
	};
	// the layout change waits for the acquire semaphore, which is waited on at the colour output stage
	const VkSubpassDependency dependency =
	{
		.srcSubpass = VK_SUBPASS_EXTERNAL,
		.dstSubpass = 0,
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	};
	const VkRenderPassCreateInfo info =
	{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &color,
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = 1,
		.pDependencies = &dependency,
	};

	return R_VKCheck( vkCreateRenderPass( vk.device, &info, NULL, &vk.pass ), "vkCreateRenderPass" );
}

static void R_VKFreeSwapchainImages( void )
{
	for( uint32_t i = 0; i < vk.num_images; i++ )
	{
		vkDestroySemaphore( vk.device, vk.rendered[i], NULL );
		vkDestroyFramebuffer( vk.device, vk.framebuffers[i], NULL );
		vkDestroyImageView( vk.device, vk.views[i], NULL );
		vk.rendered[i] = VK_NULL_HANDLE;
		vk.framebuffers[i] = VK_NULL_HANDLE;
		vk.views[i] = VK_NULL_HANDLE;
	}
	vk.num_images = 0;
}

static qboolean R_VKCreateSwapchainImages( void )
{
	uint32_t count = 0;

	vkGetSwapchainImagesKHR( vk.device, vk.swapchain, &count, NULL );
	if( count > VK_MAX_SWAP )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_vk: %u swapchain images\n", count );
		return false;
	}
	vkGetSwapchainImagesKHR( vk.device, vk.swapchain, &count, vk.images );

	for( uint32_t i = 0; i < count; i++ )
	{
		const VkImageViewCreateInfo view =
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = vk.images[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = vk.format,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};
		const VkSemaphoreCreateInfo semaphore = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		VkFramebufferCreateInfo framebuffer =
		{
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = vk.pass,
			.attachmentCount = 1,
			.width = vk.extent.width,
			.height = vk.extent.height,
			.layers = 1,
		};

		vk.num_images = i + 1; // what the free below has to walk, if this one fails halfway

		if( !R_VKCheck( vkCreateImageView( vk.device, &view, NULL, &vk.views[i] ), "vkCreateImageView" ))
			return false;

		framebuffer.pAttachments = &vk.views[i];
		if( !R_VKCheck( vkCreateFramebuffer( vk.device, &framebuffer, NULL, &vk.framebuffers[i] ), "vkCreateFramebuffer" ))
			return false;

		if( !R_VKCheck( vkCreateSemaphore( vk.device, &semaphore, NULL, &vk.rendered[i] ), "vkCreateSemaphore" ))
			return false;
	}

	return true;
}

static VkPresentModeKHR R_VKPresentMode( qboolean vsync )
{
	VkPresentModeKHR modes[VK_MAX_ITEMS];
	uint32_t count = VK_MAX_ITEMS;
	static const VkPresentModeKHR order[] = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR };

	if( vsync )
		return VK_PRESENT_MODE_FIFO_KHR; // the one mode every device has

	vkGetPhysicalDeviceSurfacePresentModesKHR( vk.physical, vk.surface, &count, modes );

	for( int i = 0; i < ARRAYSIZE( order ); i++ )
		for( uint32_t j = 0; j < count; j++ )
			if( modes[j] == order[i] )
				return order[i];

	return VK_PRESENT_MODE_FIFO_KHR;
}

static VkCompositeAlphaFlagBitsKHR R_VKCompositeAlpha( VkCompositeAlphaFlagsKHR supported )
{
	static const VkCompositeAlphaFlagBitsKHR order[] =
	{
		VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
		VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
	};

	for( int i = 0; i < ARRAYSIZE( order ); i++ )
		if( FBitSet( supported, order[i] ))
			return order[i];

	return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

/*
==================
R_VKCreateSwapchain

False when there is nothing to show on (a minimised window has no size) or on an error; the frame is skipped
and the next one tries again.
==================
*/
static qboolean R_VKCreateSwapchain( void )
{
	VkSurfaceCapabilitiesKHR caps;
	VkSwapchainKHR old = vk.swapchain;
	VkExtent2D extent;
	uint32_t images;
	qboolean vsync = vk.gl_vsync->value != 0.0f;

	vk.rebuild = true; // until the end of this function has been reached
	vkDeviceWaitIdle( vk.device );

	if( !R_VKCheck( vkGetPhysicalDeviceSurfaceCapabilitiesKHR( vk.physical, vk.surface, &caps ), "surface capabilities" ))
		return false;

	extent = caps.currentExtent;
	if( extent.width == 0xFFFFFFFFu )
	{
		// the surface takes its size from the swapchain
		extent.width = bound( caps.minImageExtent.width, (uint32_t)gpGlobals->width, caps.maxImageExtent.width );
		extent.height = bound( caps.minImageExtent.height, (uint32_t)gpGlobals->height, caps.maxImageExtent.height );
	}

	if( extent.width == 0 || extent.height == 0 )
		return false;

	images = caps.minImageCount + 1;
	if( caps.maxImageCount && images > caps.maxImageCount )
		images = caps.maxImageCount;

	R_VKFreeSwapchainImages();

	{
		VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		VkSwapchainCreateInfoKHR info =
		{
			.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			.surface = vk.surface,
			.minImageCount = images,
			.imageFormat = vk.format,
			.imageColorSpace = vk.colorspace,
			.imageExtent = extent,
			.imageArrayLayers = 1,
			.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.preTransform = caps.currentTransform,
			.compositeAlpha = R_VKCompositeAlpha( caps.supportedCompositeAlpha ),
			.presentMode = R_VKPresentMode( vsync ),
			.clipped = VK_TRUE,
			.oldSwapchain = old,
		};

		info.imageUsage = usage;

		if( !R_VKCheck( vkCreateSwapchainKHR( vk.device, &info, NULL, &vk.swapchain ), "vkCreateSwapchainKHR" ))
		{
			vk.swapchain = old; // still ours to destroy
			return false;
		}
	}

	if( old )
		vkDestroySwapchainKHR( vk.device, old, NULL );

	vk.extent = extent;
	vk.size_w = gpGlobals->width;
	vk.size_h = gpGlobals->height;
	vk.vsync = vsync;

	if( !R_VKCreateSwapchainImages( ))
	{
		R_VKFreeSwapchainImages();
		return false;
	}

	vk.rebuild = false;
	return true;
}

static qboolean R_VKCreateFrames( void )
{
	const VkCommandPoolCreateInfo pool =
	{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = vk.family,
	};

	if( !R_VKCheck( vkCreateCommandPool( vk.device, &pool, NULL, &vk.pool ), "vkCreateCommandPool" ))
		return false;

	for( int i = 0; i < VK_FRAMES; i++ )
	{
		const VkCommandBufferAllocateInfo cmd =
		{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = vk.pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		const VkFenceCreateInfo fence = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
		const VkSemaphoreCreateInfo semaphore = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

		if( !R_VKCheck( vkAllocateCommandBuffers( vk.device, &cmd, &vk.frames[i].cmd ), "vkAllocateCommandBuffers" )
			|| !R_VKCheck( vkCreateFence( vk.device, &fence, NULL, &vk.frames[i].done ), "vkCreateFence" )
			|| !R_VKCheck( vkCreateSemaphore( vk.device, &semaphore, NULL, &vk.frames[i].acquired ), "vkCreateSemaphore" ))
			return false;
	}

	return true;
}

/*
==================
R_GL3DestroyDevice

Also takes what a failed R_GL3CreateDevice left behind, so every handle is checked.
==================
*/
void R_GL3DestroyDevice( void )
{
	if( vk.device )
	{
		vkDeviceWaitIdle( vk.device );

		R_VKFreeSwapchainImages();

		if( vk.swapchain )
			vkDestroySwapchainKHR( vk.device, vk.swapchain, NULL );

		for( int i = 0; i < VK_FRAMES; i++ )
		{
			if( vk.frames[i].acquired )
				vkDestroySemaphore( vk.device, vk.frames[i].acquired, NULL );
			if( vk.frames[i].done )
				vkDestroyFence( vk.device, vk.frames[i].done, NULL );
		}

		if( vk.pool )
			vkDestroyCommandPool( vk.device, vk.pool, NULL );
		if( vk.pass )
			vkDestroyRenderPass( vk.device, vk.pass, NULL );

		vkDestroyDevice( vk.device, NULL );
	}

	if( vk.surface )
		vkDestroySurfaceKHR( vk.instance, vk.surface, NULL );
	if( vk.instance )
		vkDestroyInstance( vk.instance, NULL );

	memset( &vk, 0, sizeof( vk ));
}

qboolean R_GL3CreateDevice( int width, int height )
{
	PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)gEngfuncs.VK_GetInstanceProcAddr();

	R_VKInstallGLStubs();

	vk.gl_vsync = gEngfuncs.pfnGetCvarPointer( "gl_vsync" );

	if( !get || !vk.gl_vsync
		|| !R_VKCreateInstance( get )
		|| !gEngfuncs.VK_CreateSurface( vk.instance, &vk.surface )
		|| !R_VKPickDevice()
		|| !R_VKCreateLogicalDevice()
		|| !R_VKPickFormat()
		|| !R_VKCreateRenderPass()
		|| !R_VKCreateFrames())
	{
		R_GL3DestroyDevice();
		return false;
	}

	// a window with no size yet is not an error: the first frame that can show something builds it
	vk.rebuild = true;
	return true;
}

void R_GL3Clear( byte r, byte g, byte b )
{
	vk.clear[0] = r / 255.0f;
	vk.clear[1] = g / 255.0f;
	vk.clear[2] = b / 255.0f;
}

static qboolean R_VKNeedsRebuild( void )
{
	return vk.rebuild || !vk.swapchain || vk.size_w != gpGlobals->width || vk.size_h != gpGlobals->height
		|| vk.vsync != ( vk.gl_vsync->value != 0.0f );
}

void R_GL3Present( void )
{
	vk_frame_t *frame = &vk.frames[vk.frame];
	uint32_t index;
	VkResult result;

	if( R_VKNeedsRebuild( ) && !R_VKCreateSwapchain( ))
		return;

	vkWaitForFences( vk.device, 1, &frame->done, VK_TRUE, UINT64_MAX );

	result = vkAcquireNextImageKHR( vk.device, vk.swapchain, UINT64_MAX, frame->acquired, VK_NULL_HANDLE, &index );
	if( result == VK_ERROR_OUT_OF_DATE_KHR )
	{
		vk.rebuild = true; // nothing was signalled: the semaphore can be used again
		return;
	}
	if( result == VK_ERROR_DEVICE_LOST )
		gEngfuncs.Host_Error( "ref_vk: the device was lost\n" );
	if( result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR )
	{
		R_VKCheck( result, "vkAcquireNextImageKHR" );
		return;
	}

	vkResetFences( vk.device, 1, &frame->done );
	vkResetCommandBuffer( frame->cmd, 0 );

	{
		const VkCommandBufferBeginInfo begin =
		{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		const VkClearValue clear = { .color = { .float32 = { vk.clear[0], vk.clear[1], vk.clear[2], 1.0f }}};
		const VkRenderPassBeginInfo pass =
		{
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = vk.pass,
			.framebuffer = vk.framebuffers[index],
			.renderArea = { { 0, 0 }, vk.extent },
			.clearValueCount = 1,
			.pClearValues = &clear,
		};

		vkBeginCommandBuffer( frame->cmd, &begin );
		vkCmdBeginRenderPass( frame->cmd, &pass, VK_SUBPASS_CONTENTS_INLINE );
		vkCmdEndRenderPass( frame->cmd );
		vkEndCommandBuffer( frame->cmd );
	}

	{
		const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		const VkSubmitInfo submit =
		{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &frame->acquired,
			.pWaitDstStageMask = &wait,
			.commandBufferCount = 1,
			.pCommandBuffers = &frame->cmd,
			.signalSemaphoreCount = 1,
			.pSignalSemaphores = &vk.rendered[index],
		};
		const VkPresentInfoKHR present =
		{
			.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &vk.rendered[index],
			.swapchainCount = 1,
			.pSwapchains = &vk.swapchain,
			.pImageIndices = &index,
		};

		result = vkQueueSubmit( vk.queue, 1, &submit, frame->done );
		if( result == VK_ERROR_DEVICE_LOST )
			gEngfuncs.Host_Error( "ref_vk: the device was lost\n" );

		result = vkQueuePresentKHR( vk.queue, &present );
		if( result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR )
			vk.rebuild = true;
	}

	vk.frame = ( vk.frame + 1 ) % VK_FRAMES;
}

/*
The scene target arrives with the frame's passes at stage 2 onward; until then the core has nothing to draw
into and these keep it walking.
*/
qboolean R_GL3SetMSAA( int samples )
{
	return true;
}

void R_GL3BeginSceneTarget( void )
{
}

void R_GL3EnsureSceneTarget( void )
{
}

void R_GL3EndSceneTarget( void )
{
}

qboolean R_GL3SceneTargetActive( void )
{
	return false;
}

void R_GL3RestoreBackBuffer( void )
{
}

int R_GL3TakeSceneTarget( void )
{
	return 0;
}
