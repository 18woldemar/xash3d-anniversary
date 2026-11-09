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

#define VK_MAX_ITEMS 32 // physical devices, queue families, surface formats, present modes

vk_t vk;

qboolean R_VKCheck( VkResult result, const char *what )
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
	VkPhysicalDeviceFeatures available, features;
	VkPhysicalDeviceProperties props;
	VkDeviceCreateInfo info =
	{
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queue,
		.enabledExtensionCount = ARRAYSIZE( extensions ),
		.ppEnabledExtensionNames = extensions,
		.pEnabledFeatures = &features,
	};

	vkGetPhysicalDeviceProperties( vk.physical, &props );
	vk.limits = props.limits;
	vkGetPhysicalDeviceFeatures( vk.physical, &available );
	memset( &features, 0, sizeof( features ));
	features.samplerAnisotropy = available.samplerAnisotropy;
	vk.anisotropy = available.samplerAnisotropy;

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
			break;
		}
	}

	if( vk.format != VK_FORMAT_UNDEFINED )
	{
		// the specification guarantees one of the two
		static const VkFormat depths[] = { VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT };

		for( int i = 0; i < ARRAYSIZE( depths ); i++ )
		{
			VkFormatProperties props;

			vkGetPhysicalDeviceFormatProperties( vk.physical, depths[i], &props );
			if( FBitSet( props.optimalTilingFeatures, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ))
			{
				vk.depth_format = depths[i];
				gEngfuncs.Con_Reportf( "ref_vk: depth buffer %s\n", depths[i] == VK_FORMAT_D32_SFLOAT ? "D32" : "D24" );
				return true;
			}
		}
	}

	gEngfuncs.Con_Printf( S_ERROR "ref_vk: the window offers no 8 bit UNORM format\n" );
	return false;
}

static VkRenderPass R_VKMakePass( int target, qboolean clear )
{
	VkRenderPass pass = VK_NULL_HANDLE;
	const qboolean scene = target == VK_TARGET_SCENE;
	const qboolean multisampled = scene && vk.scene_samples > 1;
	const VkSampleCountFlagBits samples = scene ? (VkSampleCountFlagBits)vk.scene_samples : VK_SAMPLE_COUNT_1_BIT;
	const VkFormat format = scene ? vk.scene_format : target == VK_TARGET_SHADOW ? VK_SHADOW_FORMAT : vk.format;
	// a picture the 2D part or a shader reads next ends up readable; the swapchain goes on to be presented, and
	// the samples of a multisampled target stay for a pass after a break
	const VkImageLayout rest = target == VK_TARGET_SWAP || multisampled ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	const VkAttachmentDescription attachments[3] =
	{
		{
			.format = format,
			.samples = samples,
			.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : rest,
			.finalLayout = rest,
		},
		{
			.format = vk.depth_format,
			.samples = samples,
			.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		},
		{
			.format = format, // the resolve, into the scene texture
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		},
	};
	const VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	const VkAttachmentReference resolve = { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	const VkSubpassDescription subpass =
	{
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
		.pResolveAttachments = multisampled ? &resolve : NULL,
		.pDepthStencilAttachment = &depth,
	};
	// Coming in: the layout change waits for the acquire semaphore, and a pass after a break for the one before it,
	// and for the copy that made the break. Going out: the copy or barrier that follows.
	const VkPipelineStageFlags outputs = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	const VkAccessFlags writes = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	const VkSubpassDependency dependencies[2] =
	{
		{
			.srcSubpass = VK_SUBPASS_EXTERNAL,
			.dstSubpass = 0,
			.srcStageMask = outputs | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			.dstStageMask = outputs | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			.srcAccessMask = writes,
			.dstAccessMask = writes | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
		},
		{
			.srcSubpass = 0,
			.dstSubpass = VK_SUBPASS_EXTERNAL,
			.srcStageMask = outputs,
			.dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			.srcAccessMask = writes,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
		},
	};
	const VkRenderPassCreateInfo info =
	{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = multisampled ? 3 : 2,
		.pAttachments = attachments,
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = ARRAYSIZE( dependencies ),
		.pDependencies = dependencies,
	};

	R_VKCheck( vkCreateRenderPass( vk.device, &info, NULL, &pass ), "vkCreateRenderPass" );
	return pass;
}

static qboolean R_VKCreateRenderPasses( void )
{
	for( int i = 0; i < VK_TARGET_COUNT; i++ )
	{
		vk.pass_clear[i] = R_VKMakePass( i, true );
		vk.pass_load[i] = R_VKMakePass( i, false );

		if( !vk.pass_clear[i] || !vk.pass_load[i] )
			return false;
	}

	return true;
}

static void R_VKFreeSceneTarget( void );

static qboolean R_VKMakeAttachment( vk_attachment_t *out, VkFormat format, VkExtent2D extent, int samples, VkImageUsageFlags usage, VkImageAspectFlags aspect )
{
	const VkImageCreateInfo image =
	{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = { extent.width, extent.height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = (VkSampleCountFlagBits)samples,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};

	if( !R_VKCheck( vkCreateImage( vk.device, &image, NULL, &out->image ), "vkCreateImage" )
		|| !R_VKAllocImageMemory( out->image, &out->memory ))
		return false;

	{
		const VkImageViewCreateInfo view =
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = out->image,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = format,
			.subresourceRange = { aspect, 0, 1, 0, 1 },
		};

		return R_VKCheck( vkCreateImageView( vk.device, &view, NULL, &out->view ), "vkCreateImageView" );
	}
}

static void R_VKFreeAttachment( vk_attachment_t *a )
{
	if( a->view )
		vkDestroyImageView( vk.device, a->view, NULL );
	if( a->image )
		vkDestroyImage( vk.device, a->image, NULL );
	R_VKFreeMemory( &a->memory );
	a->view = VK_NULL_HANDLE;
	a->image = VK_NULL_HANDLE;
}

// the swapchain's depth: a 3D scene the menu draws into the 2D part (the player model's preview) needs one
static qboolean R_VKCreateDepth( void )
{
	return R_VKMakeAttachment( &vk.depth, vk.depth_format, vk.extent, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT );
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
	R_VKFreeAttachment( &vk.depth );
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

	if( !R_VKCreateDepth( ))
		return false;

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
		VkImageView attachments[2] = { VK_NULL_HANDLE, vk.depth.view };
		VkFramebufferCreateInfo framebuffer =
		{
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = vk.pass_clear[VK_TARGET_SWAP],
			.attachmentCount = ARRAYSIZE( attachments ),
			.pAttachments = attachments,
			.width = vk.extent.width,
			.height = vk.extent.height,
			.layers = 1,
		};

		vk.num_images = i + 1; // what the free below has to walk, if this one fails halfway

		if( !R_VKCheck( vkCreateImageView( vk.device, &view, NULL, &vk.views[i] ), "vkCreateImageView" ))
			return false;

		attachments[0] = vk.views[i];
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
		// screenshots and save pictures copy from it, where the surface allows that
		const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | ( caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT );
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
		vk.swap_readable = FBitSet( usage, VK_IMAGE_USAGE_TRANSFER_SRC_BIT ) ? true : false;

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

	gEngfuncs.Con_Reportf( "ref_vk: swapchain %ux%u, %u images, vsync %d\n", extent.width, extent.height, vk.num_images, vk.vsync );
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
			.commandBufferCount = 2,
		};
		VkCommandBuffer buffers[2];
		const VkFenceCreateInfo fence = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
		const VkSemaphoreCreateInfo semaphore = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

		if( !R_VKCheck( vkAllocateCommandBuffers( vk.device, &cmd, buffers ), "vkAllocateCommandBuffers" ))
			return false;

		vk.frames[i].cmd = buffers[0];
		vk.frames[i].upload = buffers[1];

		if( !R_VKCheck( vkCreateFence( vk.device, &fence, NULL, &vk.frames[i].done ), "vkCreateFence" )
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
		R_VKFreeSceneTarget();
		R_VKShutdownBackend();

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
		for( int i = 0; i < VK_TARGET_COUNT; i++ )
		{
			if( vk.pass_clear[i] )
				vkDestroyRenderPass( vk.device, vk.pass_clear[i], NULL );
			if( vk.pass_load[i] )
				vkDestroyRenderPass( vk.device, vk.pass_load[i], NULL );
		}

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

	vk.gl_vsync = gEngfuncs.pfnGetCvarPointer( "gl_vsync" );

	vk.scene_format = VK_FORMAT_B8G8R8A8_UNORM;
	vk.scene_samples = 1;

	if( !get || !vk.gl_vsync
		|| !R_VKCreateInstance( get )
		|| !gEngfuncs.VK_CreateSurface( vk.instance, &vk.surface )
		|| !R_VKPickDevice()
		|| !R_VKCreateLogicalDevice()
		|| !R_VKPickFormat()
		|| !R_VKCreateRenderPasses()
		|| !R_VKCreateFrames()
		|| !R_VKInitBackend())
	{
		R_GL3DestroyDevice();
		return false;
	}

	// a window with no size yet is not an error: the first frame that can show something builds it
	vk.rebuild = true;
	return true;
}

static void R_VKBeginCommands( VkCommandBuffer cmd )
{
	const VkCommandBufferBeginInfo begin =
	{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	vkBeginCommandBuffer( cmd, &begin );
}

VkCommandBuffer R_VKCmd( void )
{
	vk_frame_t *frame = &vk.frames[vk.frame];

	if( !vk.cmd_open )
	{
		R_VKBeginCommands( frame->cmd );
		vk.cmd_open = true;
		R_VKMarkStateDirty();
	}

	return frame->cmd;
}

VkCommandBuffer R_VKUploadCmd( void )
{
	vk_frame_t *frame = &vk.frames[vk.frame];

	if( !vk.upload_open )
	{
		R_VKBeginCommands( frame->upload );
		vk.upload_open = true;
	}

	return frame->upload;
}

// the uploads so far, now: for between frames, where nothing else will submit them
void R_VKFlushUploads( void )
{
	vk_frame_t *frame = &vk.frames[vk.frame];
	const VkSubmitInfo submit =
	{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &frame->upload,
	};

	if( !vk.upload_open )
		return;

	vkEndCommandBuffer( frame->upload );
	vk.upload_open = false;
	vkQueueSubmit( vk.queue, 1, &submit, VK_NULL_HANDLE );
	vkQueueWaitIdle( vk.queue );
	vkResetCommandBuffer( frame->upload, 0 );
}

// The screenshot's copy has to have happened when the call returns, so the frame is cut here and goes on in a new
// command buffer. The image keeps its layout between the two, and only the first submit may wait for it to be acquired.
void R_VKSubmitAndWait( void )
{
	vk_frame_t *frame = &vk.frames[vk.frame];
	const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkCommandBuffer buffers[2];
	VkSubmitInfo submit =
	{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pCommandBuffers = buffers,
		.pWaitDstStageMask = &wait,
	};

	if( vk.upload_open )
	{
		vkEndCommandBuffer( frame->upload );
		buffers[submit.commandBufferCount++] = frame->upload;
	}
	if( vk.cmd_open )
	{
		vkEndCommandBuffer( frame->cmd );
		buffers[submit.commandBufferCount++] = frame->cmd;
	}

	if( vk.acquired && !vk.acquire_waited )
	{
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &frame->acquired;
		vk.acquire_waited = true;
	}

	vkQueueSubmit( vk.queue, 1, &submit, VK_NULL_HANDLE );
	vkQueueWaitIdle( vk.queue );
	vkResetCommandBuffer( frame->upload, 0 );
	vkResetCommandBuffer( frame->cmd, 0 );
	vk.upload_open = vk.cmd_open = false;
}

static qboolean R_VKNeedsRebuild( void )
{
	return vk.rebuild || !vk.swapchain || vk.size_w != gpGlobals->width || vk.size_h != gpGlobals->height
		|| vk.vsync != ( vk.gl_vsync->value != 0.0f );
}

static qboolean R_VKAcquire( void )
{
	vk_frame_t *frame = &vk.frames[vk.frame];
	VkResult result;

	if( R_VKNeedsRebuild( ) && !R_VKCreateSwapchain( ))
		return false;

	result = vkAcquireNextImageKHR( vk.device, vk.swapchain, UINT64_MAX, frame->acquired, VK_NULL_HANDLE, &vk.image_index );
	if( result == VK_ERROR_OUT_OF_DATE_KHR )
	{
		vk.rebuild = true; // nothing was signalled: the semaphore can be used again
		return false;
	}
	if( result == VK_ERROR_DEVICE_LOST )
		gEngfuncs.Host_Error( "ref_vk: the device was lost\n" );
	if( result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR )
	{
		R_VKCheck( result, "vkAcquireNextImageKHR" );
		return false;
	}

	vk.acquired = true;
	return true;
}

VkExtent2D R_VKTargetExtent( void )
{
	return vk.target == VK_TARGET_SWAP ? vk.extent : vk.target == VK_TARGET_SCENE ? vk.scene_extent : vk.shadow_extent;
}

qboolean R_VKEnsurePass( void )
{
	const int target = vk.target;

	if( vk.pass_open )
		return true;

	if( target == VK_TARGET_SWAP )
	{
		if( vk.skip )
			return false;

		if( !vk.acquired && !R_VKAcquire( ))
		{
			vk.skip = true;
			return false;
		}
	}

	{
		const float c = target == VK_TARGET_SHADOW ? 1.0f : 0.0f; // a target's own clear: the far end of the map, or black
		const qboolean swap = target == VK_TARGET_SWAP;
		const VkClearValue clear[2] =
		{
			{ .color = { .float32 = { swap ? vk.clear[0] : c, swap ? vk.clear[1] : c, swap ? vk.clear[2] : c, 1.0f }}},
			{ .depthStencil = { 1.0f, 0 }},
		};
		const VkRenderPassBeginInfo pass =
		{
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = vk.started[target] ? vk.pass_load[target] : vk.pass_clear[target],
			.framebuffer = swap ? vk.framebuffers[vk.image_index] : target == VK_TARGET_SCENE ? vk.scene_framebuffer : vk.shadow_framebuffer,
			.renderArea = { { 0, 0 }, R_VKTargetExtent( ) },
			.clearValueCount = ARRAYSIZE( clear ),
			.pClearValues = clear,
		};

		vkCmdBeginRenderPass( R_VKCmd( ), &pass, VK_SUBPASS_CONTENTS_INLINE );
		vk.pass_open = true;
		vk.started[target] = true;
		R_VKMarkViewportDirty();
	}

	return true;
}

void R_VKEndPass( void )
{
	if( !vk.pass_open )
		return;

	vkCmdEndRenderPass( vk.frames[vk.frame].cmd );
	vk.pass_open = false;
}

void R_GL3Clear( byte r, byte g, byte b )
{
	vk.clear[0] = r / 255.0f;
	vk.clear[1] = g / 255.0f;
	vk.clear[2] = b / 255.0f;
	R_GL3ClearBuffers( GL3_CLEAR_COLOR | GL3_CLEAR_DEPTH | GL3_CLEAR_STENCIL );
}

// the frame's first pass clears by itself: anything later has to do it inside a pass
void R_GL3BackendClearBuffers( uint mask )
{
	VkClearAttachment attachments[2];
	uint32_t count = 0;

	if( !vk.started[vk.target] || !R_VKEnsurePass( ))
		return;

	if( FBitSet( mask, GL3_CLEAR_COLOR ))
	{
		attachments[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		attachments[count].colorAttachment = 0;
		attachments[count].clearValue.color.float32[0] = vk.target == VK_TARGET_SHADOW ? 1.0f : vk.clear[0];
		attachments[count].clearValue.color.float32[1] = vk.target == VK_TARGET_SHADOW ? 1.0f : vk.clear[1];
		attachments[count].clearValue.color.float32[2] = vk.target == VK_TARGET_SHADOW ? 1.0f : vk.clear[2];
		attachments[count].clearValue.color.float32[3] = 1.0f;
		count++;
	}

	if( FBitSet( mask, GL3_CLEAR_DEPTH ))
	{
		attachments[count].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		attachments[count].colorAttachment = 0;
		attachments[count].clearValue.depthStencil.depth = 1.0f;
		attachments[count].clearValue.depthStencil.stencil = 0;
		count++;
	}

	if( count )
	{
		const VkClearRect rect = { { { 0, 0 }, R_VKTargetExtent( ) }, 0, 1 };

		vkCmdClearAttachments( R_VKCmd( ), count, attachments, 1, &rect );
	}
}

void R_GL3Present( void )
{
	vk_frame_t *frame = &vk.frames[vk.frame];
	VkResult result;

	// a frame that drew nothing still shows its clear
	R_VKEnsurePass();
	R_VKEndPass();

	if( vk.acquired )
	{
		const VkImageMemoryBarrier barrier =
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = vk.images[vk.image_index],
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};

		vkCmdPipelineBarrier( R_VKCmd( ), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
			0, NULL, 0, NULL, 1, &barrier );
	}

	if( !vk.cmd_open && !vk.upload_open )
	{
		vk.skip = false; // no window to draw on, and nothing waiting to reach the GPU: the next frame tries again
		return;
	}

	{
		const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkCommandBuffer buffers[2];
		VkSubmitInfo submit =
		{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.pCommandBuffers = buffers,
			.pWaitDstStageMask = &wait,
		};

		// the uploads run first: they are what the frame's draws read
		if( vk.upload_open )
		{
			vkEndCommandBuffer( frame->upload );
			buffers[submit.commandBufferCount++] = frame->upload;
		}
		if( vk.cmd_open )
		{
			vkEndCommandBuffer( frame->cmd );
			buffers[submit.commandBufferCount++] = frame->cmd;
		}

		if( vk.acquired )
		{
			if( !vk.acquire_waited )
			{
				submit.waitSemaphoreCount = 1;
				submit.pWaitSemaphores = &frame->acquired;
			}
			submit.signalSemaphoreCount = 1;
			submit.pSignalSemaphores = &vk.rendered[vk.image_index];
		}

		vkResetFences( vk.device, 1, &frame->done );
		result = vkQueueSubmit( vk.queue, 1, &submit, frame->done );
		if( result == VK_ERROR_DEVICE_LOST )
			gEngfuncs.Host_Error( "ref_vk: the device was lost\n" );
	}

	if( vk.acquired )
	{
		const VkPresentInfoKHR present =
		{
			.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &vk.rendered[vk.image_index],
			.swapchainCount = 1,
			.pSwapchains = &vk.swapchain,
			.pImageIndices = &vk.image_index,
		};

		result = vkQueuePresentKHR( vk.queue, &present );
		if( result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR )
			vk.rebuild = true;
	}

	vk.cmd_open = vk.upload_open = false;
	vk.acquired = vk.acquire_waited = vk.skip = false;
	memset( vk.started, 0, sizeof( vk.started ));
	vk.serial++;

	// on to the next frame, and everything of it is idle from here
	vk.frame = ( vk.frame + 1 ) % VK_FRAMES;
	frame = &vk.frames[vk.frame];
	vkWaitForFences( vk.device, 1, &frame->done, VK_TRUE, UINT64_MAX );
	vkResetCommandBuffer( frame->cmd, 0 );
	vkResetCommandBuffer( frame->upload, 0 );
	R_VKResetFrame( frame );
}

/*
==============================================================================

THE SCENE TARGET

Unlike ref_gl3, the 3D part always goes through it: an image that is not flipped has the memory of a GL framebuffer,
so the derivatives in the shaders and the upside-down scene quad of the 2D part come out as they do on GL.

==============================================================================
*/
static void R_VKFreeSceneTarget( void )
{
	if( vk.scene_framebuffer )
		vkDestroyFramebuffer( vk.device, vk.scene_framebuffer, NULL );
	vk.scene_framebuffer = VK_NULL_HANDLE;
	R_VKFreeAttachment( &vk.scene_color );
	R_VKFreeAttachment( &vk.scene_depth );
}

// the most samples the device can draw at, of what was asked for
static int R_VKSupportedSamples( int samples )
{
	const VkSampleCountFlags supported = vk.limits.framebufferColorSampleCounts & vk.limits.framebufferDepthSampleCounts;

	while( samples > 1 && !FBitSet( supported, samples ))
		samples /= 2;

	return samples;
}

// A pass is made for a format and a sample count, so the scene's two are remade with the target. Pipelines made for
// them stay in the cache, keyed by the scene's mode, and are good again when the mode comes back.
static qboolean R_VKRemakeScenePasses( void )
{
	for( int i = 0; i < 2; i++ )
	{
		VkRenderPass *pass = i ? &vk.pass_load[VK_TARGET_SCENE] : &vk.pass_clear[VK_TARGET_SCENE];

		if( *pass )
			vkDestroyRenderPass( vk.device, *pass, NULL );

		*pass = R_VKMakePass( VK_TARGET_SCENE, i == 0 );
		if( !*pass )
			return false;
	}

	return true;
}

/*
==================
R_GL3SetMSAA

Every view goes through the target here, one sample or several: see above. The software style's target is 16 bit and
has one. A count the renderer has no use for answers false and leaves the one-sample target.
==================
*/
qboolean R_GL3SetMSAA( int samples )
{
	const VkExtent2D extent = { (uint32_t)gpGlobals->width, (uint32_t)gpGlobals->height };
	const qboolean soft = R_GL3Style( ) == GL3_STYLE_SOFTWARE;
	const qboolean known = samples == 0 || samples == 2 || samples == 4 || samples == 8;
	VkImageView resolve_view;
	gl3_handle_t color;

	vkDeviceWaitIdle( vk.device );

	if( vk.scene_texture )
		R_GL3FreeTexture( vk.scene_texture );
	vk.scene_texture = 0;
	vk.scene_pending = vk.scene_active = vk.scene_resolved = false;
	R_VKFreeSceneTarget();

	// the span renderer had no multisampling, and its output was 16 bit
	vk.scene_format = soft ? VK_FORMAT_R5G6B5_UNORM_PACK16 : VK_FORMAT_B8G8R8A8_UNORM;
	vk.scene_samples = soft || samples < 2 || !known ? 1 : R_VKSupportedSamples( samples );

	if( !R_VKRemakeScenePasses( ))
		return false;

	color = R_VKCreateRenderTexture( vk.scene_format, extent.width, extent.height, &resolve_view );

	if( !R_VKMakeAttachment( &vk.scene_depth, vk.depth_format, extent, vk.scene_samples, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT ))
		return false;

	if( vk.scene_samples > 1 && !R_VKMakeAttachment( &vk.scene_color, vk.scene_format, extent, vk.scene_samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT ))
		return false;

	{
		const qboolean multisampled = vk.scene_samples > 1;
		const VkImageView attachments[3] = { multisampled ? vk.scene_color.view : resolve_view, vk.scene_depth.view, resolve_view };
		const VkFramebufferCreateInfo framebuffer =
		{
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = vk.pass_clear[VK_TARGET_SCENE],
			.attachmentCount = multisampled ? 3 : 2,
			.pAttachments = attachments,
			.width = extent.width,
			.height = extent.height,
			.layers = 1,
		};

		if( !R_VKCheck( vkCreateFramebuffer( vk.device, &framebuffer, NULL, &vk.scene_framebuffer ), "vkCreateFramebuffer" ))
			return false;
	}

	vk.scene_extent = extent;
	vk.scene_texture = R_GL3WrapTexture( "*scene", color, extent.width, extent.height, TF_NEAREST | TF_CLAMP | TF_NOMIPMAP );

	if( soft )
		gEngfuncs.Con_Printf( "ref_vk: the software style's 16 bit scene target, %ux%u\n", extent.width, extent.height );
	else gEngfuncs.Con_Printf( "ref_vk: %dx multisampled scene target, %ux%u\n", vk.scene_samples, extent.width, extent.height );
	return known;
}

void R_GL3BeginSceneTarget( void )
{
	if( vk.scene_framebuffer && !vk.scene_active )
		vk.scene_pending = true;
}

void R_GL3EnsureSceneTarget( void )
{
	if( !vk.scene_pending || vk.scene_active )
		return;

	vk.scene_pending = false;
	R_GL3DrawReset();
	R_VKEndPass();
	vk.target = VK_TARGET_SCENE;
	vk.scene_active = true;
	R_GL3BackendViewport( 0, 0, vk.scene_extent.width, vk.scene_extent.height );
}

void R_GL3EndSceneTarget( void )
{
	vk.scene_pending = false;

	// an error in the middle of the shadow map's pass
	if( vk.target == VK_TARGET_SHADOW )
	{
		R_VKEndPass();
		vk.target = VK_TARGET_SWAP;
	}

	if( !vk.scene_active )
		return;

	R_GL3DrawReset();
	R_VKEnsurePass(); // a view that drew nothing still leaves a cleared picture for the 2D part to read
	R_VKEndPass();
	vk.scene_active = false;
	vk.target = VK_TARGET_SWAP;
	vk.scene_resolved = true;
}

qboolean R_GL3SceneTargetActive( void )
{
	return vk.scene_active;
}

/*
==============================================================================

THE FLASHLIGHT'S MAP

==============================================================================
*/
void R_GL3BackendFreeShadowMap( void )
{
	vkDeviceWaitIdle( vk.device );

	if( vk.shadow_framebuffer )
		vkDestroyFramebuffer( vk.device, vk.shadow_framebuffer, NULL );
	vk.shadow_framebuffer = VK_NULL_HANDLE;
	R_VKFreeAttachment( &vk.shadow_depth );
}

gl3_handle_t R_GL3BackendCreateShadowMap( int size )
{
	const VkExtent2D extent = { (uint32_t)size, (uint32_t)size };
	VkImageView view;
	const gl3_handle_t color = R_VKCreateRenderTexture( VK_SHADOW_FORMAT, size, size, &view );

	if( !R_VKMakeAttachment( &vk.shadow_depth, vk.depth_format, extent, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT ))
		return 0;

	{
		const VkImageView attachments[2] = { view, vk.shadow_depth.view };
		const VkFramebufferCreateInfo framebuffer =
		{
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = vk.pass_clear[VK_TARGET_SHADOW],
			.attachmentCount = ARRAYSIZE( attachments ),
			.pAttachments = attachments,
			.width = extent.width,
			.height = extent.height,
			.layers = 1,
		};

		if( !R_VKCheck( vkCreateFramebuffer( vk.device, &framebuffer, NULL, &vk.shadow_framebuffer ), "vkCreateFramebuffer" ))
			return 0;
	}

	vk.shadow_extent = extent;
	return color;
}

void R_GL3BackendBeginShadowPass( int size )
{
	R_VKEndPass();
	vk.target = VK_TARGET_SHADOW;
	R_GL3BackendViewport( 0, 0, size, size );
	R_GL3BackendDepthRange( 0.0f, 1.0f );
}

void R_GL3BackendEndShadowPass( void )
{
	R_VKEnsurePass();
	R_VKEndPass();
	vk.target = vk.scene_active ? VK_TARGET_SCENE : VK_TARGET_SWAP;
}

int R_GL3TakeSceneTarget( void )
{
	const qboolean resolved = vk.scene_resolved;

	vk.scene_resolved = false;
	return resolved ? vk.scene_texture : 0;
}
