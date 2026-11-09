/*
r_vk_backend.c - ref_vk: memory, textures, programs, pipelines and drawing
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
The renderer's core talks to a state machine: bound textures, render states, numbered registers, a draw. Here that
is resolved at each draw: the states key a hash of pipelines, the registers go to a uniform ring, the textures to a
descriptor set from the frame's pool.
*/

#include "r_local.h"
#include "r_vk.h"
#include "vk_shaders.h"

/*
==============================================================================

MEMORY

Images come out of 64 MB blocks by first fit: a device can allow as few as 4096 allocations, which is also how many
textures there can be. Buffers are few and get their own.

==============================================================================
*/
#define VK_BLOCK_SIZE ( 64u * 1024u * 1024u )
#define VK_MAX_BLOCKS 64
#define VK_MAX_FREE   1024

typedef struct
{
	VkDeviceSize offset, size;
} vk_range_t;

typedef struct
{
	VkDeviceMemory memory;
	uint32_t       type;
	qboolean       linear;   // buffers and images share no block: no bufferImageGranularity to keep
	VkDeviceSize   size;
	int            num_free;
	vk_range_t     free[VK_MAX_FREE]; // sorted by offset
} vk_block_t;

static vk_block_t vk_blocks[VK_MAX_BLOCKS];

static int R_VKMemoryType( uint32_t bits, VkMemoryPropertyFlags wanted )
{
	VkPhysicalDeviceMemoryProperties props;

	vkGetPhysicalDeviceMemoryProperties( vk.physical, &props );

	for( uint32_t i = 0; i < props.memoryTypeCount; i++ )
	{
		if( FBitSet( bits, 1u << i ) && ( props.memoryTypes[i].propertyFlags & wanted ) == wanted )
			return (int)i;
	}

	return -1;
}

static qboolean R_VKBlockTake( vk_block_t *block, const VkMemoryRequirements *req, VkDeviceSize *out )
{
	for( int i = 0; i < block->num_free; i++ )
	{
		vk_range_t *r = &block->free[i];
		const VkDeviceSize start = ( r->offset + req->alignment - 1 ) & ~( req->alignment - 1 );
		const VkDeviceSize gap = start - r->offset;
		VkDeviceSize rest;

		if( gap + req->size > r->size )
			continue;

		rest = r->size - gap - req->size;

		if( gap && rest )
		{
			// one range becomes two
			if( block->num_free >= VK_MAX_FREE )
				return false;

			memmove( r + 1, r, ( block->num_free - i ) * sizeof( *r ));
			block->num_free++;
			r[0].size = gap;
			r[1].offset = start + req->size;
			r[1].size = rest;
		}
		else if( gap )
			r->size = gap;
		else if( rest )
		{
			r->offset += req->size;
			r->size = rest;
		}
		else
		{
			memmove( r, r + 1, ( block->num_free - i - 1 ) * sizeof( *r ));
			block->num_free--;
		}

		*out = start;
		return true;
	}

	return false;
}

static qboolean R_VKAllocMemory( const VkMemoryRequirements *req, qboolean linear, vk_allocation_t *out )
{
	vk_block_t *block;
	const int type = R_VKMemoryType( req->memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
	int slot = -1;

	if( type < 0 )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_vk: no device-local memory\n" );
		return false;
	}

	for( int i = 0; i < VK_MAX_BLOCKS; i++ )
	{
		block = &vk_blocks[i];

		if( !block->memory )
		{
			if( slot < 0 )
				slot = i;
			continue;
		}

		if( block->type == (uint32_t)type && block->linear == linear && R_VKBlockTake( block, req, &out->offset ))
		{
			out->memory = block->memory;
			out->size = req->size;
			out->block = i;
			return true;
		}
	}

	if( slot < 0 )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_vk: out of memory blocks\n" );
		return false;
	}

	{
		block = &vk_blocks[slot];
		block->size = Q_max( (VkDeviceSize)VK_BLOCK_SIZE, req->size );
		block->type = type;
		block->linear = linear;

		const VkMemoryAllocateInfo info =
		{
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = block->size,
			.memoryTypeIndex = (uint32_t)type,
		};

		if( !R_VKCheck( vkAllocateMemory( vk.device, &info, NULL, &block->memory ), "vkAllocateMemory" ))
		{
			block->memory = VK_NULL_HANDLE;
			return false;
		}

		block->num_free = 1;
		block->free[0].offset = 0;
		block->free[0].size = block->size;
		R_VKBlockTake( block, req, &out->offset );
		out->memory = block->memory;
		out->size = req->size;
		out->block = slot;
		return true;
	}
}

qboolean R_VKAllocImageMemory( VkImage image, vk_allocation_t *out )
{
	VkMemoryRequirements req;

	vkGetImageMemoryRequirements( vk.device, image, &req );
	return R_VKAllocMemory( &req, false, out )
		&& R_VKCheck( vkBindImageMemory( vk.device, image, out->memory, out->offset ), "vkBindImageMemory" );
}

void R_VKFreeMemory( vk_allocation_t *a )
{
	vk_block_t *block;
	int i;

	if( !a->memory )
		return;

	block = &vk_blocks[a->block];

	for( i = 0; i < block->num_free && block->free[i].offset < a->offset; i++ );

	// joins the range before it, the one after it, or both
	if( i > 0 && block->free[i - 1].offset + block->free[i - 1].size == a->offset )
	{
		block->free[i - 1].size += a->size;

		if( i < block->num_free && block->free[i - 1].offset + block->free[i - 1].size == block->free[i].offset )
		{
			block->free[i - 1].size += block->free[i].size;
			memmove( &block->free[i], &block->free[i + 1], ( block->num_free - i - 1 ) * sizeof( block->free[0] ));
			block->num_free--;
		}
	}
	else if( i < block->num_free && a->offset + a->size == block->free[i].offset )
	{
		block->free[i].offset = a->offset;
		block->free[i].size += a->size;
	}
	else if( block->num_free < VK_MAX_FREE )
	{
		memmove( &block->free[i + 1], &block->free[i], ( block->num_free - i ) * sizeof( block->free[0] ));
		block->free[i].offset = a->offset;
		block->free[i].size = a->size;
		block->num_free++;
	}
	// ponytail: a full free list leaks this range until the map's memory is gone; 1024 holes per block is far off

	memset( a, 0, sizeof( *a ));
}

/*
==============================================================================

RINGS

==============================================================================
*/
#define VK_STAGING_SIZE  ( 32u * 1024u * 1024u )
#define VK_VERTICES_SIZE ( 8u * 1024u * 1024u )
#define VK_UNIFORMS_SIZE ( 8u * 1024u * 1024u )

#define VK_VS_RANGE ( GL3_VS_CONSTANTS * 16 )
#define VK_PS_RANGE ( GL3_PS_CONSTANTS * 16 )

static VkDescriptorPool      vk_uniform_pool;
static VkDescriptorSetLayout vk_uniform_layout, vk_texture_layout;
static VkPipelineLayout      vk_pipeline_layout;

static qboolean R_VKAddChunk( vk_ring_t *ring, VkDeviceSize size )
{
	vk_chunk_t *chunk;
	VkMemoryRequirements req;
	int type;

	if( ring->num_chunks >= VK_MAX_CHUNKS )
	{
		gEngfuncs.Con_Printf( S_ERROR "ref_vk: a ring has outgrown its %d buffers\n", VK_MAX_CHUNKS );
		return false;
	}

	chunk = &ring->chunks[ring->num_chunks];
	chunk->size = size;

	{
		const VkBufferCreateInfo info =
		{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = size,
			.usage = ring->usage,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};

		if( !R_VKCheck( vkCreateBuffer( vk.device, &info, NULL, &chunk->buffer ), "vkCreateBuffer" ))
			return false;
	}

	vkGetBufferMemoryRequirements( vk.device, chunk->buffer, &req );
	type = -1;

	// what the CPU reads back wants the cached kind: reading write-combined memory is about twenty times slower
	if( FBitSet( ring->usage, VK_BUFFER_USAGE_TRANSFER_DST_BIT ))
		type = R_VKMemoryType( req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT );

	if( type < 0 )
		type = R_VKMemoryType( req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );

	{
		const VkMemoryAllocateInfo info =
		{
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = req.size,
			.memoryTypeIndex = (uint32_t)type,
		};

		if( type < 0
			|| !R_VKCheck( vkAllocateMemory( vk.device, &info, NULL, &chunk->memory ), "vkAllocateMemory" )
			|| !R_VKCheck( vkBindBufferMemory( vk.device, chunk->buffer, chunk->memory, 0 ), "vkBindBufferMemory" )
			|| !R_VKCheck( vkMapMemory( vk.device, chunk->memory, 0, VK_WHOLE_SIZE, 0, (void **)&chunk->mapped ), "vkMapMemory" ))
			return false;
	}

	if( FBitSet( ring->usage, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT ))
	{
		const VkDescriptorSetAllocateInfo info =
		{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = vk_uniform_pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &vk_uniform_layout,
		};
		const VkDescriptorBufferInfo buffers[2] =
		{
			{ chunk->buffer, 0, VK_VS_RANGE },
			{ chunk->buffer, 0, VK_PS_RANGE },
		};
		VkWriteDescriptorSet writes[2];

		if( !R_VKCheck( vkAllocateDescriptorSets( vk.device, &info, &chunk->uniforms ), "vkAllocateDescriptorSets" ))
			return false;

		memset( writes, 0, sizeof( writes ));
		for( int i = 0; i < 2; i++ )
		{
			writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[i].dstSet = chunk->uniforms;
			writes[i].dstBinding = i;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
			writes[i].pBufferInfo = &buffers[i];
		}
		vkUpdateDescriptorSets( vk.device, 2, writes, 0, NULL );
	}

	ring->num_chunks++;
	return true;
}

// room for size bytes in the current buffer, moving on to the next if this one has not got it
static qboolean R_VKRingReserve( vk_ring_t *ring, VkDeviceSize size )
{
	if( ring->num_chunks && ring->offset + size <= ring->chunks[ring->current].size )
		return true;

	if( ring->num_chunks )
	{
		ring->current++;
		ring->offset = 0;

		// a later buffer may be too small for this: only the staging ring gets requests that large
		while( ring->current < ring->num_chunks && ring->chunks[ring->current].size < size )
			ring->current++;

		if( ring->current < ring->num_chunks )
			return true;
	}

	if( !R_VKAddChunk( ring, Q_max( ring->chunk_size, size )))
		return false;

	ring->current = ring->num_chunks - 1;
	ring->offset = 0;
	return true;
}

static byte *R_VKRingAlloc( vk_ring_t *ring, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize *offset, vk_chunk_t **chunk )
{
	byte *p;

	ring->offset = ( ring->offset + alignment - 1 ) & ~( alignment - 1 );

	if( !R_VKRingReserve( ring, size ))
		gEngfuncs.Host_Error( "ref_vk: out of stream memory\n" );

	*chunk = &ring->chunks[ring->current];
	*offset = ring->offset;
	p = ( *chunk )->mapped + ring->offset;
	ring->offset += size;
	return p;
}

static void R_VKFreeRing( vk_ring_t *ring )
{
	for( int i = 0; i < ring->num_chunks; i++ )
	{
		vkDestroyBuffer( vk.device, ring->chunks[i].buffer, NULL );
		vkFreeMemory( vk.device, ring->chunks[i].memory, NULL );
	}

	memset( ring, 0, sizeof( *ring ));
}

/*
==============================================================================

TEXTURES

==============================================================================
*/
#define VK_MAX_IMAGES ( GL3_MAX_TEXTURES + 64 )

typedef struct
{
	VkImage         image;
	VkImageView     view;
	vk_allocation_t memory;
	int             levels;
	uint            ready;     // bit per level: holds data, and is in the layout shaders read
	uint            used;      // the frame that last read it, plus one
	VkSampler       sampler;
} vk_image_t;

static vk_image_t  vk_images[VK_MAX_IMAGES]; // 0 is no texture
static int         vk_num_images = 1;
static gl3_handle_t vk_white;
static VkSampler   vk_samplers[2][2][16][17];

static struct
{
	vk_image_t  *image;
	int          level, width, height;
	vk_chunk_t  *chunk;
	VkDeviceSize offset;
} vk_lock;

static gl3_handle_t vk_bound[GL3_UNIT_COUNT];
static qboolean     vk_textures_dirty;

static VkSampler R_VKSampler( qboolean filtered, qboolean clamp, int mipcap, int anisotropy )
{
	VkSampler *slot;

	mipcap = bound( 0, mipcap, 15 );
	anisotropy = bound( 1, anisotropy, 16 );
	slot = &vk_samplers[filtered ? 1 : 0][clamp ? 1 : 0][mipcap][anisotropy];

	if( !*slot )
	{
		const VkSamplerAddressMode mode = clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
		const VkFilter filter = filtered ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		const VkSamplerCreateInfo info =
		{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = filter,
			.minFilter = filter,
			.mipmapMode = filtered ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = mode,
			.addressModeV = mode,
			.addressModeW = mode,
			.anisotropyEnable = vk.anisotropy && anisotropy > 1,
			.maxAnisotropy = (float)anisotropy,
			.maxLod = (float)mipcap,
		};

		R_VKCheck( vkCreateSampler( vk.device, &info, NULL, slot ), "vkCreateSampler" );
	}

	return *slot;
}

static gl3_handle_t R_VKNewSlot( void )
{
	for( int i = 1; i < vk_num_images; i++ )
	{
		if( !vk_images[i].image )
			return i;
	}

	if( vk_num_images >= VK_MAX_IMAGES )
		gEngfuncs.Host_Error( "ref_vk: out of texture slots\n" );

	return vk_num_images++;
}

static void R_VKMakeImage( vk_image_t *image, VkFormat format, int width, int height, int levels, VkImageUsageFlags usage )
{
	memset( image, 0, sizeof( *image ));
	image->levels = levels;
	image->sampler = R_VKSampler( true, false, 15, 1 );

	{
		const VkImageCreateInfo info =
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = format,
			.extent = { (uint32_t)width, (uint32_t)height, 1 },
			.mipLevels = (uint32_t)levels,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = usage,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};

		if( !R_VKCheck( vkCreateImage( vk.device, &info, NULL, &image->image ), "vkCreateImage" )
			|| !R_VKAllocImageMemory( image->image, &image->memory ))
			gEngfuncs.Host_Error( "ref_vk: can't make a %dx%d texture\n", width, height );
	}

	{
		const VkImageViewCreateInfo info =
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = image->image,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = format,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)levels, 0, 1 },
		};

		if( !R_VKCheck( vkCreateImageView( vk.device, &info, NULL, &image->view ), "vkCreateImageView" ))
			gEngfuncs.Host_Error( "ref_vk: can't make a texture view\n" );
	}
}

gl3_handle_t R_GL3BackendCreateTexture( int width, int height, int levels )
{
	const gl3_handle_t handle = R_VKNewSlot();

	R_VKMakeImage( &vk_images[handle], VK_FORMAT_B8G8R8A8_UNORM, width, height, levels, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT );
	return handle;
}

// a picture a pass draws and the 2D part reads: created ready, it never goes through an upload
gl3_handle_t R_VKCreateRenderTexture( VkFormat format, int width, int height, VkImageView *view )
{
	const gl3_handle_t handle = R_VKNewSlot();
	vk_image_t *image = &vk_images[handle];

	R_VKMakeImage( image, format, width, height, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT );
	image->ready = 1;
	*view = image->view;
	return handle;
}

gl3_handle_t R_GL3BackendCreateLightmap( int size )
{
	const gl3_handle_t handle = R_VKNewSlot();
	vk_image_t *image = &vk_images[handle];

	R_VKMakeImage( image, VK_FORMAT_B8G8R8A8_UNORM, size, size, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT );
	image->sampler = R_VKSampler( true, true, 0, 1 );
	return handle;
}

static vk_image_t *R_VKImage( gl3_handle_t handle )
{
	if( handle > 0 && handle < (gl3_handle_t)vk_num_images && vk_images[handle].image )
		return &vk_images[handle];

	return NULL;
}

void R_GL3UnbindTexture( gl3_handle_t texture )
{
	for( int i = 0; i < GL3_UNIT_COUNT; i++ )
	{
		if( vk_bound[i] == texture )
		{
			vk_bound[i] = 0;
			vk_textures_dirty = true;
		}
	}
}

void R_GL3ForgetBindings( void )
{
}

// the image goes when the frame that may still read it has finished
void R_GL3BackendDeleteTexture( gl3_handle_t texture )
{
	vk_image_t *image = R_VKImage( texture );
	vk_frame_t *frame = &vk.frames[vk.frame];

	if( !image )
		return;

	R_GL3UnbindTexture( texture );

	if( frame->num_dead >= VK_MAX_DESTROY )
	{
		vkDeviceWaitIdle( vk.device );
		R_VKResetFrame( frame );
	}

	frame->dead[frame->num_dead].image = image->image;
	frame->dead[frame->num_dead].view = image->view;
	frame->dead[frame->num_dead].memory = image->memory;
	frame->num_dead++;
	memset( image, 0, sizeof( *image ));
}

// A level load uploads far more than one frame's worth with no frame in between: what is there goes to the GPU and
// the ring starts over. Inside a frame the commands being recorded may still read it, so it grows instead.
static byte *R_VKStaging( VkDeviceSize bytes, VkDeviceSize *offset, vk_chunk_t **chunk )
{
	vk_ring_t *ring = &vk.frames[vk.frame].staging;

	if( !vk.cmd_open && ring->num_chunks && ring->offset + bytes > ring->chunks[ring->current].size )
	{
		R_VKFlushUploads();
		ring->current = 0;
		ring->offset = 0;
	}

	return R_VKRingAlloc( ring, bytes, 16, offset, chunk );
}

uint32_t *R_GL3BackendLockLevel( gl3_handle_t texture, int level, int width, int height )
{
	vk_lock.image = R_VKImage( texture );
	vk_lock.level = level;
	vk_lock.width = width;
	vk_lock.height = height;
	return (uint32_t *)R_VKStaging( (VkDeviceSize)width * height * 4, &vk_lock.offset, &vk_lock.chunk );
}

static void R_VKLevelBarrier( VkCommandBuffer cmd, vk_image_t *image, int level, VkImageLayout from, VkImageLayout to,
	VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage )
{
	const VkImageMemoryBarrier barrier =
	{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = src,
		.dstAccessMask = dst,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image->image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 1, 0, 1 },
	};

	vkCmdPipelineBarrier( cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier );
}

#define VK_SHADER_READERS ( VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT )

// A texture this frame has already read has to keep the old contents for those draws: the copy happens here, between
// them, breaking the pass. Any other goes ahead of the whole frame.
static VkCommandBuffer R_VKBeginImageCopy( vk_image_t *image, int level )
{
	const qboolean ready = FBitSet( image->ready, 1u << level ) ? true : false;
	VkCommandBuffer cmd;

	if( image->used == vk.serial + 1 && vk.cmd_open )
	{
		R_VKEndPass();
		cmd = R_VKCmd();
	}
	else cmd = R_VKUploadCmd();

	R_VKLevelBarrier( cmd, image, level,
		ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		ready ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
		ready ? VK_SHADER_READERS : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );
	return cmd;
}

static void R_VKEndImageCopy( VkCommandBuffer cmd, vk_image_t *image, int level )
{
	R_VKLevelBarrier( cmd, image, level,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_SHADER_READERS );
	SetBits( image->ready, 1u << level );
}

void R_GL3BackendUnlockLevel( void )
{
	vk_image_t *image = vk_lock.image;
	VkCommandBuffer cmd;

	if( !image )
		return;

	cmd = R_VKBeginImageCopy( image, vk_lock.level );

	{
		const VkBufferImageCopy region =
		{
			.bufferOffset = vk_lock.offset,
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)vk_lock.level, 0, 1 },
			.imageExtent = { (uint32_t)vk_lock.width, (uint32_t)vk_lock.height, 1 },
		};

		vkCmdCopyBufferToImage( cmd, vk_lock.chunk->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
	}

	R_VKEndImageCopy( cmd, image, vk_lock.level );
}

void R_GL3BackendWriteRows( gl3_handle_t texture, const uint32_t *luxels, int size, const gl3_rect_t *rects, int numrects, qboolean whole )
{
	vk_image_t *image = R_VKImage( texture );
	const gl3_rect_t all = { 0, 0, (short)size, (short)size };
	VkDeviceSize bytes = 0, offset;
	vk_chunk_t *chunk;
	uint32_t *out;
	VkCommandBuffer cmd;

	if( !image )
		return;

	if( whole )
	{
		rects = &all;
		numrects = 1;
	}

	if( numrects <= 0 )
		return;

	// one allocation for all of them: making room can submit the commands, which must not happen halfway
	for( int i = 0; i < numrects; i++ )
		bytes += (VkDeviceSize)rects[i].w * rects[i].h * 4;

	out = (uint32_t *)R_VKStaging( bytes, &offset, &chunk );
	cmd = R_VKBeginImageCopy( image, 0 );

	for( int i = 0; i < numrects; i++ )
	{
		const gl3_rect_t *r = &rects[i];
		const VkBufferImageCopy region =
		{
			.bufferOffset = offset,
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageOffset = { r->x, r->y, 0 },
			.imageExtent = { (uint32_t)r->w, (uint32_t)r->h, 1 },
		};

		for( int y = 0; y < r->h; y++, out += r->w )
			memcpy( out, luxels + ( r->y + y ) * size + r->x, r->w * sizeof( *out ));

		offset += (VkDeviceSize)r->w * r->h * 4;
		vkCmdCopyBufferToImage( cmd, chunk->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
	}

	R_VKEndImageCopy( cmd, image, 0 );
}

void R_GL3BackendBindTexture( int unit, gl3_handle_t texture )
{
	if( vk_bound[unit] != texture )
	{
		vk_bound[unit] = texture;
		vk_textures_dirty = true;
	}
}

void R_GL3BackendSetSampler( int unit, gl3_handle_t texture, int levels, qboolean filtered, qboolean clamp, int mipcap, int anisotropy )
{
	vk_image_t *image = R_VKImage( texture );

	if( !image )
		return;

	image->sampler = R_VKSampler( filtered, clamp, mipcap, anisotropy );
	vk_textures_dirty = true; // the descriptor carries the sampler
}

float R_GL3BackendMaxAnisotropy( void )
{
	return vk.anisotropy ? vk.limits.maxSamplerAnisotropy : 0.0f;
}

/*
==============================================================================

PROGRAMS AND PIPELINES

==============================================================================
*/
enum
{
	VK_LAYOUT_DRAW = 0,
	VK_LAYOUT_WORLD,
	VK_LAYOUT_STUDIO,
	VK_LAYOUT_COUNT
};

struct gl3_program_s
{
	const vk_shader_source_t *source; // NULL: a file not moved to the backend yet, which draws nothing
	VkShaderModule vs, fs;
	uint         vs_serial, ps_serial, vs_used, ps_used; // the registers it last took, and the frame, plus one
	VkDeviceSize vs_offset, ps_offset;
	vk_chunk_t  *vs_chunk, *ps_chunk;
};

#define VK_MAX_PROGRAMS  32
#define VK_MAX_PIPELINES 2048

typedef struct
{
	uint32_t program, layout, primitive, target, scene_mode;
	uint32_t blend, src, dst, zwrite, ztest, zfunc, cull, bias;
} vk_pipeline_key_t;

typedef struct
{
	vk_pipeline_key_t key;
	VkPipeline        pipeline;
} vk_pipeline_slot_t;

static gl3_program_t      vk_programs[VK_MAX_PROGRAMS];
static int                vk_num_programs;
static gl3_program_t     *vk_program;
static gl3_program_t      vk_no_program;
static VkPipelineCache    vk_pipeline_cache;
static vk_pipeline_slot_t vk_pipelines[VK_MAX_PIPELINES];

static gl3_states_t vk_states;
static int          vk_layout;
static struct
{
	int   x, y, width, height;
	float minz, maxz;
} vk_viewport;

static VkPipeline      vk_bound_pipeline;
static VkDescriptorSet vk_texture_set;
static qboolean        vk_viewport_dirty;
static float           vk_bias_applied[2];
static VkBuffer        vk_bound_buffer, vk_bound_index;
static VkDeviceSize    vk_bound_offset, vk_bound_index_offset;

void R_VKMarkViewportDirty( void )
{
	vk_viewport_dirty = true;
}

void R_VKMarkStateDirty( void )
{
	vk_bound_pipeline = VK_NULL_HANDLE;
	vk_texture_set = VK_NULL_HANDLE;
	vk_viewport_dirty = true;
	vk_bias_applied[0] = vk_bias_applied[1] = -1e9f;
	vk_textures_dirty = true;
	vk_bound_buffer = VK_NULL_HANDLE;
	vk_bound_index = VK_NULL_HANDLE;
}

static VkShaderModule R_VKModule( const uint32_t *code, size_t size )
{
	VkShaderModule module = VK_NULL_HANDLE;
	const VkShaderModuleCreateInfo info =
	{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size,
		.pCode = code,
	};

	R_VKCheck( vkCreateShaderModule( vk.device, &info, NULL, &module ), "vkCreateShaderModule" );
	return module;
}

static gl3_program_t *R_VKFindProgram( const char *name, const char *defines, qboolean spot )
{
	if( !defines )
		defines = "";

	for( size_t i = 0; i < ARRAYSIZE( vk_shader_sources ); i++ )
	{
		const vk_shader_source_t *s = &vk_shader_sources[i];
		gl3_program_t *p;

		if( Q_strcmp( s->name, name ) || Q_strcmp( s->defines, defines ) || s->spot != spot )
			continue;

		if( vk_num_programs >= VK_MAX_PROGRAMS )
			return NULL;

		p = &vk_programs[vk_num_programs++];
		memset( p, 0, sizeof( *p ));
		p->source = s;
		p->vs = R_VKModule( s->vs, s->vs_size );
		p->fs = R_VKModule( s->fs, s->fs_size );
		return p->vs && p->fs ? p : NULL;
	}

	return &vk_no_program;
}

gl3_program_t *R_GL3CreateProgram( const char *name, const char *source, const char *defines )
{
	return R_VKFindProgram( name, defines, false );
}

gl3_program_t *R_GL3CreateSpotProgram( const char *name, const char *source, const char *defines )
{
	return R_VKFindProgram( name, defines, true );
}

void R_GL3UseProgram( gl3_program_t *p )
{
	vk_program = p;
}

// the backend takes the registers at its own draws
void R_GL3CommitConstants( void )
{
}

void R_GL3ShutdownPrograms( void )
{
}

static VkBlendFactor R_VKBlendFactor( uint f )
{
	switch( f )
	{
	case GL3_BLEND_ZERO:                return VK_BLEND_FACTOR_ZERO;
	case GL3_BLEND_SRC_COLOR:           return VK_BLEND_FACTOR_SRC_COLOR;
	case GL3_BLEND_SRC_ALPHA:           return VK_BLEND_FACTOR_SRC_ALPHA;
	case GL3_BLEND_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	default:                            return VK_BLEND_FACTOR_ONE;
	}
}

static VkCompareOp R_VKCompare( uint f )
{
	switch( f )
	{
	case GL3_DEPTH_LESS:  return VK_COMPARE_OP_LESS;
	case GL3_DEPTH_EQUAL: return VK_COMPARE_OP_EQUAL;
	default:              return VK_COMPARE_OP_LESS_OR_EQUAL;
	}
}

static VkPipeline R_VKCreatePipeline( const vk_pipeline_key_t *key, gl3_program_t *program )
{
	static const VkVertexInputBindingDescription draw_binding = { 0, sizeof( gl3_vertex_t ), VK_VERTEX_INPUT_RATE_VERTEX };
	static const VkVertexInputAttributeDescription draw_attributes[] =
	{
		{ GL3_ATTR_POSITION, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof( gl3_vertex_t, position ) },
		{ GL3_ATTR_COLOR, 0, VK_FORMAT_B8G8R8A8_UNORM, offsetof( gl3_vertex_t, color ) },
		{ GL3_ATTR_TEXCOORD, 0, VK_FORMAT_R32G32_SFLOAT, offsetof( gl3_vertex_t, texcoord ) },
	};
	static const VkVertexInputBindingDescription world_binding = { 0, sizeof( float ) * VERTEXSIZE, VK_VERTEX_INPUT_RATE_VERTEX };
	static const VkVertexInputAttributeDescription world_attributes[] =
	{
		{ GL3_ATTR_POSITION, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
		{ GL3_ATTR_TEXCOORD, 0, VK_FORMAT_R32G32_SFLOAT, 3 * sizeof( float ) },
		{ GL3_ATTR_LIGHTCOORD, 0, VK_FORMAT_R32G32_SFLOAT, 5 * sizeof( float ) },
	};
	static const VkVertexInputBindingDescription studio_binding = { 0, sizeof( gl3_studiovertex_t ), VK_VERTEX_INPUT_RATE_VERTEX };
	static const VkVertexInputAttributeDescription studio_attributes[] =
	{
		{ GL3_ATTR_POSITION, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof( gl3_studiovertex_t, position ) },
		{ GL3_ATTR_NORMAL, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof( gl3_studiovertex_t, normal ) },
		{ GL3_ATTR_TEXCOORD, 0, VK_FORMAT_R32G32_SFLOAT, offsetof( gl3_studiovertex_t, st ) },
		{ GL3_ATTR_EXTRA, 0, VK_FORMAT_R32_SFLOAT, offsetof( gl3_studiovertex_t, bone ) },
	};
	static const struct
	{
		const VkVertexInputBindingDescription *binding;
		const VkVertexInputAttributeDescription *attributes;
		uint32_t count;
	} layouts[VK_LAYOUT_COUNT] =
	{
		[VK_LAYOUT_DRAW] = { &draw_binding, draw_attributes, ARRAYSIZE( draw_attributes ) },
		[VK_LAYOUT_WORLD] = { &world_binding, world_attributes, ARRAYSIZE( world_attributes ) },
		[VK_LAYOUT_STUDIO] = { &studio_binding, studio_attributes, ARRAYSIZE( studio_attributes ) },
	};
	const qboolean scene = key->target == VK_TARGET_SCENE;
	const qboolean swap = key->target == VK_TARGET_SWAP;
	static const VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS };
	const VkPipelineShaderStageCreateInfo stages[2] =
	{
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = program->vs, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = program->fs, .pName = "main" },
	};
	const VkPipelineVertexInputStateCreateInfo input =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = layouts[key->layout].binding,
		.vertexAttributeDescriptionCount = layouts[key->layout].count,
		.pVertexAttributeDescriptions = layouts[key->layout].attributes,
	};
	const VkPipelineInputAssemblyStateCreateInfo assembly =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = key->primitive == GL3_PRIM_LINES ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	const VkPipelineViewportStateCreateInfo viewport =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	const VkPipelineRasterizationStateCreateInfo raster =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = key->cull == GL3_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT : key->cull == GL3_CULL_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
		// Vulkan's sign of a triangle's area is the opposite of GL's: the swapchain's flipped viewport makes up for it
		.frontFace = swap ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
		.depthBiasEnable = key->bias ? VK_TRUE : VK_FALSE,
		.lineWidth = 1.0f,
	};
	const VkPipelineMultisampleStateCreateInfo multisample =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = scene ? (VkSampleCountFlagBits)vk.scene_samples : VK_SAMPLE_COUNT_1_BIT,
	};
	const VkPipelineDepthStencilStateCreateInfo depth =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = key->ztest ? VK_TRUE : VK_FALSE,
		.depthWriteEnable = key->zwrite ? VK_TRUE : VK_FALSE,
		.depthCompareOp = R_VKCompare( key->zfunc ),
	};
	const VkPipelineColorBlendAttachmentState attachment =
	{
		.blendEnable = key->blend ? VK_TRUE : VK_FALSE,
		.srcColorBlendFactor = R_VKBlendFactor( key->src ),
		.dstColorBlendFactor = R_VKBlendFactor( key->dst ),
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = R_VKBlendFactor( key->src ),
		.dstAlphaBlendFactor = R_VKBlendFactor( key->dst ),
		.alphaBlendOp = VK_BLEND_OP_ADD,
		// the scene is read back through the alpha test, and has no alpha on GL: it stays the clear's 1
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | ( scene ? 0 : VK_COLOR_COMPONENT_A_BIT ),
	};
	const VkPipelineColorBlendStateCreateInfo blend =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &attachment,
	};
	const VkPipelineDynamicStateCreateInfo dynamic =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = ARRAYSIZE( dynamics ),
		.pDynamicStates = dynamics,
	};
	const VkGraphicsPipelineCreateInfo info =
	{
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = ARRAYSIZE( stages ),
		.pStages = stages,
		.pVertexInputState = &input,
		.pInputAssemblyState = &assembly,
		.pViewportState = &viewport,
		.pRasterizationState = &raster,
		.pMultisampleState = &multisample,
		.pDepthStencilState = &depth,
		.pColorBlendState = &blend,
		.pDynamicState = &dynamic,
		.layout = vk_pipeline_layout,
		.renderPass = vk.pass_clear[key->target],
	};
	VkPipeline pipeline = VK_NULL_HANDLE;

	R_VKCheck( vkCreateGraphicsPipelines( vk.device, vk_pipeline_cache, 1, &info, NULL, &pipeline ), "vkCreateGraphicsPipelines" );
	return pipeline;
}

static VkPipeline R_VKFindPipeline( int primitive )
{
	const gl3_states_t *s = &vk_states;
	vk_pipeline_key_t key;
	uint32_t hash = 2166136261u;
	const byte *bytes = (const byte *)&key;

	key.program = vk_program - vk_programs;
	key.layout = vk_layout;
	key.primitive = primitive;
	key.target = vk.target;
	key.scene_mode = vk.target == VK_TARGET_SCENE ? vk.scene_samples * 2 + ( vk.scene_format != VK_FORMAT_B8G8R8A8_UNORM ) : 0;
	key.blend = s->blend;
	key.src = s->blend ? s->src : 0;
	key.dst = s->blend ? s->dst : 0;
	key.ztest = s->ztest;
	key.zwrite = s->zwrite && s->ztest; // GL writes no depth with the test off
	key.zfunc = s->ztest ? s->zfunc : 0;
	key.cull = s->cull;
	key.bias = s->depthbias != 0.0f || s->slopebias != 0.0f;

	for( size_t i = 0; i < sizeof( key ); i++ )
		hash = ( hash ^ bytes[i] ) * 16777619u;

	for( uint32_t i = hash % VK_MAX_PIPELINES, n = 0; n < VK_MAX_PIPELINES; i = ( i + 1 ) % VK_MAX_PIPELINES, n++ )
	{
		vk_pipeline_slot_t *slot = &vk_pipelines[i];

		if( slot->pipeline && !memcmp( &slot->key, &key, sizeof( key )))
			return slot->pipeline;

		if( !slot->pipeline )
		{
			slot->key = key;
			slot->pipeline = R_VKCreatePipeline( &key, vk_program );
			return slot->pipeline;
		}
	}

	gEngfuncs.Host_Error( "ref_vk: out of pipeline slots\n" );
	return VK_NULL_HANDLE;
}

/*
==============================================================================

DRAWING

==============================================================================
*/
void R_GL3BackendApplyStates( const gl3_states_t *wanted, const gl3_states_t *applied )
{
	vk_states = *wanted;
}

void R_GL3BackendViewport( int x, int y, int width, int height )
{
	vk_viewport.x = x;
	vk_viewport.y = y;
	vk_viewport.width = width;
	vk_viewport.height = height;
	vk_viewport_dirty = true;
}

void R_GL3BackendDepthRange( float minz, float maxz )
{
	vk_viewport.minz = minz;
	vk_viewport.maxz = maxz;
	vk_viewport_dirty = true;
}

void R_GL3BackendInitDraw( void )
{
}

void R_GL3BackendShutdownDraw( void )
{
}

void R_GL3BackendUseDrawLayout( void )
{
	vk_layout = VK_LAYOUT_DRAW;
}

static void R_VKApplyViewport( VkCommandBuffer cmd )
{
	const qboolean flip = vk.target == VK_TARGET_SWAP;
	const VkExtent2D extent = R_VKTargetExtent();
	const float x = (float)vk_viewport.x, w = (float)vk_viewport.width, h = (float)vk_viewport.height;
	// the swapchain flips, as its first row is the top of the screen; a target keeps GL's first row at the bottom
	const float y = flip ? (float)extent.height - vk_viewport.y : (float)vk_viewport.y;
	const VkViewport viewport = { x, y, w, flip ? -h : h, vk_viewport.minz, vk_viewport.maxz };
	const VkRect2D scissor = { { 0, 0 }, extent };

	vkCmdSetViewport( cmd, 0, 1, &viewport );
	vkCmdSetScissor( cmd, 0, 1, &scissor );
	vk_viewport_dirty = false;
}

// the registers the program reads: copied once per change, and the frame's first use
static void R_VKApplyRegisters( VkCommandBuffer cmd )
{
	vk_frame_t *frame = &vk.frames[vk.frame];
	vk_ring_t *ring = &frame->uniforms;
	gl3_program_t *p = vk_program;
	const VkDeviceSize alignment = Q_max( vk.limits.minUniformBufferOffsetAlignment, 16 );
	vk_chunk_t *chunk;
	uint32_t offsets[2];

	// both blocks must come from the one buffer the descriptor set describes, and each binding's whole range must fit
	ring->offset = ( ring->offset + alignment - 1 ) & ~( alignment - 1 );
	if( !R_VKRingReserve( ring, VK_VS_RANGE + VK_PS_RANGE + alignment * 2 ))
		gEngfuncs.Host_Error( "ref_vk: out of uniform memory\n" );
	chunk = &ring->chunks[ring->current];

	if( p->vs_serial != gl3_vsc_serial || p->vs_used != vk.serial + 1 || p->vs_chunk != chunk )
	{
		vk_chunk_t *got;
		byte *out = R_VKRingAlloc( ring, p->source->vs_registers * 16, alignment, &p->vs_offset, &got );

		memcpy( out, gl3_vsc, p->source->vs_registers * 16 );
		p->vs_serial = gl3_vsc_serial;
		p->vs_used = vk.serial + 1;
		p->vs_chunk = chunk;
	}

	if( p->ps_serial != gl3_psc_serial || p->ps_used != vk.serial + 1 || p->ps_chunk != chunk )
	{
		vk_chunk_t *got;
		byte *out = R_VKRingAlloc( ring, p->source->ps_registers * 16, alignment, &p->ps_offset, &got );

		memcpy( out, gl3_psc, p->source->ps_registers * 16 );
		p->ps_serial = gl3_psc_serial;
		p->ps_used = vk.serial + 1;
		p->ps_chunk = chunk;
	}

	offsets[0] = (uint32_t)p->vs_offset;
	offsets[1] = (uint32_t)p->ps_offset;
	vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_layout, 0, 1, &chunk->uniforms, 2, offsets );
}

/*
==================
R_GL3BackendReadScreen

The swapchain image, from what is drawn so far. Its first row is the top of the screen, and the bytes are in its own
order.
==================
*/
qboolean R_GL3BackendReadScreen( byte *rgba, int width, int height )
{
	const VkDeviceSize bytes = (VkDeviceSize)width * height * 4;
	const qboolean bgra = vk.format == VK_FORMAT_B8G8R8A8_UNORM;
	vk_ring_t ring = { .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkCommandBuffer cmd;

	memset( rgba, 0, bytes );

	if( !vk.swap_readable )
		gEngfuncs.Con_Printf( S_ERROR "ref_vk: this window can't be read back, the picture is black\n" );

	if( !vk.swap_readable || !R_VKEnsurePass( ) || !vk.acquired || !R_VKAddChunk( &ring, bytes ))
		return false;

	R_VKEndPass();
	cmd = R_VKCmd();

	{
		VkImageMemoryBarrier barrier =
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = vk.images[vk.image_index],
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};
		const VkBufferImageCopy region =
		{
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { Q_min( (uint32_t)width, vk.extent.width ), Q_min( (uint32_t)height, vk.extent.height ), 1 },
		};
		const VkBufferMemoryBarrier to_host =
		{
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = ring.chunks[0].buffer,
			.size = VK_WHOLE_SIZE,
		};

		vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier );
		vkCmdCopyImageToBuffer( cmd, barrier.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ring.chunks[0].buffer, 1, &region );
		vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &to_host, 0, NULL );

		// back to what the frame's next pass loads
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &barrier );
	}

	R_VKSubmitAndWait();

	for( VkDeviceSize i = 0; i < bytes; i += 4 )
	{
		const byte *in = ring.chunks[0].mapped + i;

		rgba[i + 0] = in[bgra ? 2 : 0];
		rgba[i + 1] = in[1];
		rgba[i + 2] = in[bgra ? 0 : 2];
		rgba[i + 3] = 255;
	}

	R_VKFreeRing( &ring );
	return false;
}

static VkDescriptorPool R_VKNewPool( vk_frame_t *frame )
{
	static const VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 * GL3_UNIT_COUNT } };
	const VkDescriptorPoolCreateInfo info =
	{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1024,
		.poolSizeCount = ARRAYSIZE( sizes ),
		.pPoolSizes = sizes,
	};

	if( frame->num_pools >= VK_MAX_POOLS )
		gEngfuncs.Host_Error( "ref_vk: out of descriptor pools\n" );

	if( !R_VKCheck( vkCreateDescriptorPool( vk.device, &info, NULL, &frame->pools[frame->num_pools] ), "vkCreateDescriptorPool" ))
		gEngfuncs.Host_Error( "ref_vk: can't make a descriptor pool\n" );

	return frame->pools[frame->num_pools++];
}

static void R_VKApplyTextures( VkCommandBuffer cmd )
{
	vk_frame_t *frame = &vk.frames[vk.frame];
	VkDescriptorImageInfo infos[GL3_UNIT_COUNT];
	VkWriteDescriptorSet writes[GL3_UNIT_COUNT];
	VkDescriptorSet set = VK_NULL_HANDLE;
	const VkDescriptorSetAllocateInfo allocate_template =
	{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorSetCount = 1,
		.pSetLayouts = &vk_texture_layout,
	};

	if( !vk_textures_dirty && vk_texture_set )
		return;

	for( ;; )
	{
		VkDescriptorSetAllocateInfo allocate = allocate_template;
		VkResult result;

		if( frame->current_pool >= frame->num_pools )
			R_VKNewPool( frame );

		allocate.descriptorPool = frame->pools[frame->current_pool];
		result = vkAllocateDescriptorSets( vk.device, &allocate, &set );

		if( result == VK_SUCCESS )
			break;

		if( result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL )
			gEngfuncs.Host_Error( "ref_vk: vkAllocateDescriptorSets failed: %d\n", (int)result );

		frame->current_pool++;
	}

	memset( writes, 0, sizeof( writes ));

	for( int i = 0; i < GL3_UNIT_COUNT; i++ )
	{
		vk_image_t *image = R_VKImage( vk_bound[i] );

		// no texture (a unit the program ignores), or one still being filled: *white
		if( !image || image->ready != ( 1u << image->levels ) - 1u )
			image = R_VKImage( vk_white );

		image->used = vk.serial + 1;
		infos[i].sampler = image->sampler;
		infos[i].imageView = image->view;
		infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &infos[i];
	}

	vkUpdateDescriptorSets( vk.device, GL3_UNIT_COUNT, writes, 0, NULL );
	vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_layout, 1, 1, &set, 0, NULL );
	vk_texture_set = set;
	vk_textures_dirty = false;
}

static qboolean R_VKPrepareDraw( int primitive, VkCommandBuffer *out )
{
	VkCommandBuffer cmd;
	VkPipeline pipeline;

	if( !vk_program || !vk_program->source || !R_VKEnsurePass( ))
		return false;

	cmd = R_VKCmd();
	pipeline = R_VKFindPipeline( primitive );

	if( !pipeline )
		return false;

	if( pipeline != vk_bound_pipeline )
	{
		vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
		vk_bound_pipeline = pipeline;
	}

	if( vk_viewport_dirty )
		R_VKApplyViewport( cmd );

	if( vk_states.depthbias != vk_bias_applied[0] || vk_states.slopebias != vk_bias_applied[1] )
	{
		vkCmdSetDepthBias( cmd, vk_states.depthbias * 16777216.0f, 0.0f, vk_states.slopebias );
		vk_bias_applied[0] = vk_states.depthbias;
		vk_bias_applied[1] = vk_states.slopebias;
	}

	R_VKApplyRegisters( cmd );
	R_VKApplyTextures( cmd );
	*out = cmd;
	return true;
}

static void R_VKBindVertices( VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset )
{
	if( buffer == vk_bound_buffer && offset == vk_bound_offset )
		return;

	vkCmdBindVertexBuffers( cmd, 0, 1, &buffer, &offset );
	vk_bound_buffer = buffer;
	vk_bound_offset = offset;
}

static void R_VKBindIndices( VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkIndexType type )
{
	if( buffer == vk_bound_index && offset == vk_bound_index_offset )
		return;

	vkCmdBindIndexBuffer( cmd, buffer, offset, type );
	vk_bound_index = buffer;
	vk_bound_index_offset = offset;
}

void R_GL3BackendDrawStream( int primitive, const gl3_vertex_t *vertices, int count )
{
	vk_frame_t *frame = &vk.frames[vk.frame];
	const VkDeviceSize bytes = count * sizeof( *vertices );
	VkCommandBuffer cmd;
	VkDeviceSize offset;
	vk_chunk_t *chunk;
	byte *out;

	if( !R_VKPrepareDraw( primitive, &cmd ))
		return;

	out = R_VKRingAlloc( &frame->vertices, bytes, 16, &offset, &chunk );
	memcpy( out, vertices, bytes );
	R_VKBindVertices( cmd, chunk->buffer, offset );
	vkCmdDraw( cmd, count, 1, 0, 0 );
}

/*
==============================================================================

THE WORLD

==============================================================================
*/
// device-local, filled once through the staging ring; freed when the frame that may read it has finished
static void R_VKMakeStaticBuffer( VkDeviceSize bytes, VkBufferUsageFlags usage, VkBuffer *buffer, vk_allocation_t *memory )
{
	VkMemoryRequirements req;
	const VkBufferCreateInfo info =
	{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = bytes,
		.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};

	if( !R_VKCheck( vkCreateBuffer( vk.device, &info, NULL, buffer ), "vkCreateBuffer" ))
		gEngfuncs.Host_Error( "ref_vk: can't make a %u byte buffer\n", (uint)bytes );

	vkGetBufferMemoryRequirements( vk.device, *buffer, &req );

	if( !R_VKAllocMemory( &req, true, memory )
		|| !R_VKCheck( vkBindBufferMemory( vk.device, *buffer, memory->memory, memory->offset ), "vkBindBufferMemory" ))
		gEngfuncs.Host_Error( "ref_vk: no memory for a %u byte buffer\n", (uint)bytes );
}

static void R_VKCopyToStaticBuffer( vk_chunk_t *chunk, VkDeviceSize offset, VkBuffer buffer, VkDeviceSize bytes, VkAccessFlags readers )
{
	VkCommandBuffer cmd = R_VKUploadCmd();
	const VkBufferCopy copy = { offset, 0, bytes };
	const VkBufferMemoryBarrier barrier =
	{
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = readers,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = buffer,
		.size = VK_WHOLE_SIZE,
	};

	vkCmdCopyBuffer( cmd, chunk->buffer, buffer, 1, &copy );
	vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1, &barrier, 0, NULL );
}

static void R_VKRetireBuffer( VkBuffer *buffer, vk_allocation_t *memory )
{
	vk_frame_t *frame = &vk.frames[vk.frame];

	if( !*buffer )
		return;

	if( frame->num_dead_buffers >= (int)ARRAYSIZE( frame->dead_buffers ))
	{
		vkDeviceWaitIdle( vk.device );
		R_VKResetFrame( frame );
	}

	frame->dead_buffers[frame->num_dead_buffers].buffer = *buffer;
	frame->dead_buffers[frame->num_dead_buffers].memory = *memory;
	frame->num_dead_buffers++;
	*buffer = VK_NULL_HANDLE;
	memset( memory, 0, sizeof( *memory ));
}

static VkBuffer        vk_world_vbo;
static vk_allocation_t vk_world_vbo_memory;
static VkDeviceSize   vk_world_staging_offset;
static vk_chunk_t    *vk_world_staging_chunk;
static byte          *vk_world_staging;
static VkDeviceSize   vk_world_bytes;

static struct
{
	VkBuffer     index, vertex;
	VkDeviceSize index_offset, vertex_offset;
} vk_world_stream;

static qboolean vk_world_dynamic;

void R_GL3BackendInitWorld( void )
{
}

void R_GL3BackendShutdownWorld( void )
{
	R_VKRetireBuffer( &vk_world_vbo, &vk_world_vbo_memory );
}

void R_GL3BackendWorldVerticesBegin( size_t bytes )
{
	R_VKRetireBuffer( &vk_world_vbo, &vk_world_vbo_memory );
	R_VKMakeStaticBuffer( bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vk_world_vbo, &vk_world_vbo_memory );
	vk_world_bytes = bytes;
	vk_world_staging = R_VKStaging( bytes, &vk_world_staging_offset, &vk_world_staging_chunk );
}

void R_GL3BackendWorldVerticesWrite( size_t offset, const void *data, size_t bytes )
{
	memcpy( vk_world_staging + offset, data, bytes );
}

void R_GL3BackendWorldVerticesEnd( void )
{
	R_VKCopyToStaticBuffer( vk_world_staging_chunk, vk_world_staging_offset, vk_world_vbo, vk_world_bytes, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT );
}

void R_GL3BackendWorldStream( const uint32_t *indices, int numindices, const void *dynverts, size_t dynbytes )
{
	vk_frame_t *frame = &vk.frames[vk.frame];
	vk_chunk_t *chunk;
	byte *out;

	memset( &vk_world_stream, 0, sizeof( vk_world_stream ));

	if( numindices )
	{
		out = R_VKRingAlloc( &frame->vertices, numindices * sizeof( *indices ), 16, &vk_world_stream.index_offset, &chunk );
		memcpy( out, indices, numindices * sizeof( *indices ));
		vk_world_stream.index = chunk->buffer;
	}

	if( dynbytes )
	{
		out = R_VKRingAlloc( &frame->vertices, dynbytes, 16, &vk_world_stream.vertex_offset, &chunk );
		memcpy( out, dynverts, dynbytes );
		vk_world_stream.vertex = chunk->buffer;
	}
}

void R_GL3BackendUseWorldLayout( qboolean dynamic )
{
	vk_layout = VK_LAYOUT_WORLD;
	vk_world_dynamic = dynamic;
}

void R_GL3BackendDrawWorldIndexed( int first, int count )
{
	VkCommandBuffer cmd;

	if( !vk_world_vbo || !vk_world_stream.index || !R_VKPrepareDraw( GL3_PRIM_TRIANGLES, &cmd ))
		return;

	R_VKBindVertices( cmd, vk_world_vbo, 0 );

	R_VKBindIndices( cmd, vk_world_stream.index, vk_world_stream.index_offset, VK_INDEX_TYPE_UINT32 );
	vkCmdDrawIndexed( cmd, count, 1, first, 0, 0 );
}

void R_GL3BackendDrawWorldArrays( int first, int count )
{
	VkCommandBuffer cmd;

	if( !vk_world_stream.vertex || !R_VKPrepareDraw( GL3_PRIM_TRIANGLES, &cmd ))
		return;

	R_VKBindVertices( cmd, vk_world_stream.vertex, vk_world_stream.vertex_offset );
	vkCmdDraw( cmd, count, 1, first, 0 );
}

/*
==============================================================================

STUDIO MESHES

==============================================================================
*/
struct gl3_mesh_s
{
	VkBuffer        buffer; // the vertices, then the indices
	vk_allocation_t memory;
	VkDeviceSize    indices;
};

static gl3_mesh_t *vk_mesh;

gl3_mesh_t *R_GL3BackendCreateMesh( poolhandle_t pool, const gl3_studiovertex_t *vertices, int numvertices, const word *indices, int numindices )
{
	gl3_mesh_t *mesh = (gl3_mesh_t *)Mem_Calloc( pool, sizeof( *mesh ));
	const VkDeviceSize vertex_bytes = numvertices * sizeof( *vertices ), index_bytes = numindices * sizeof( *indices );
	VkDeviceSize offset;
	vk_chunk_t *chunk;
	byte *out;

	R_VKMakeStaticBuffer( vertex_bytes + index_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &mesh->buffer, &mesh->memory );
	mesh->indices = vertex_bytes;

	out = R_VKStaging( vertex_bytes + index_bytes, &offset, &chunk );
	memcpy( out, vertices, vertex_bytes );
	memcpy( out + vertex_bytes, indices, index_bytes );
	R_VKCopyToStaticBuffer( chunk, offset, mesh->buffer, vertex_bytes + index_bytes, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT );
	return mesh;
}

void R_GL3BackendDeleteMesh( gl3_mesh_t *mesh )
{
	R_VKRetireBuffer( &mesh->buffer, &mesh->memory );
	Mem_Free( mesh );
}

void R_GL3BackendUseMesh( gl3_mesh_t *mesh )
{
	vk_layout = VK_LAYOUT_STUDIO;
	vk_mesh = mesh;
}

void R_GL3BackendDrawMesh( int first, int count )
{
	VkCommandBuffer cmd;

	if( !vk_mesh || !R_VKPrepareDraw( GL3_PRIM_TRIANGLES, &cmd ))
		return;

	R_VKBindVertices( cmd, vk_mesh->buffer, 0 );
	R_VKBindIndices( cmd, vk_mesh->buffer, vk_mesh->indices, VK_INDEX_TYPE_UINT16 );
	vkCmdDrawIndexed( cmd, count, 1, first, 0, 0 );
}

gl3_handle_t R_GL3BackendCreateGamma( int width, int height )
{
	const gl3_handle_t handle = R_VKNewSlot();
	vk_image_t *image = &vk_images[handle];

	R_VKMakeImage( image, VK_FORMAT_R16_UINT, width, height, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT );
	image->sampler = R_VKSampler( false, true, 0, 1 );
	return handle;
}

void R_GL3BackendWriteGamma( gl3_handle_t texture, const uint16_t *rows, int width, int height )
{
	vk_image_t *image = R_VKImage( texture );
	const VkDeviceSize bytes = (VkDeviceSize)width * height * sizeof( *rows );
	VkBufferImageCopy copy =
	{
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { (uint32_t)width, (uint32_t)height, 1 },
	};
	VkCommandBuffer cmd;
	vk_chunk_t *chunk;
	byte *out;

	if( !image )
		return;

	out = R_VKStaging( bytes, &copy.bufferOffset, &chunk );
	memcpy( out, rows, bytes );
	cmd = R_VKBeginImageCopy( image, 0 );
	vkCmdCopyBufferToImage( cmd, chunk->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy );
	R_VKEndImageCopy( cmd, image, 0 );
}

/*
==============================================================================

SETUP

==============================================================================
*/
void R_VKResetFrame( vk_frame_t *frame )
{
	frame->staging.current = frame->vertices.current = frame->uniforms.current = 0;
	frame->staging.offset = frame->vertices.offset = frame->uniforms.offset = 0;

	for( int i = 0; i < frame->num_pools; i++ )
		vkResetDescriptorPool( vk.device, frame->pools[i], 0 );
	frame->current_pool = 0;

	for( int i = 0; i < frame->num_dead; i++ )
	{
		vkDestroyImageView( vk.device, frame->dead[i].view, NULL );
		vkDestroyImage( vk.device, frame->dead[i].image, NULL );
		R_VKFreeMemory( &frame->dead[i].memory );
	}
	frame->num_dead = 0;

	for( int i = 0; i < frame->num_dead_buffers; i++ )
	{
		vkDestroyBuffer( vk.device, frame->dead_buffers[i].buffer, NULL );
		R_VKFreeMemory( &frame->dead_buffers[i].memory );
	}
	frame->num_dead_buffers = 0;
}

static qboolean R_VKCreateLayouts( void )
{
	VkDescriptorSetLayoutBinding uniforms[2], textures[GL3_UNIT_COUNT];
	VkDescriptorSetLayout sets[2];

	memset( uniforms, 0, sizeof( uniforms ));
	memset( textures, 0, sizeof( textures ));

	uniforms[0].binding = 0;
	uniforms[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	uniforms[0].descriptorCount = 1;
	uniforms[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	uniforms[1] = uniforms[0];
	uniforms[1].binding = 1;
	uniforms[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	for( int i = 0; i < GL3_UNIT_COUNT; i++ )
	{
		textures[i].binding = i;
		textures[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		textures[i].descriptorCount = 1;
		textures[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	}

	{
		VkDescriptorSetLayoutCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };

		info.bindingCount = ARRAYSIZE( uniforms );
		info.pBindings = uniforms;
		if( !R_VKCheck( vkCreateDescriptorSetLayout( vk.device, &info, NULL, &vk_uniform_layout ), "vkCreateDescriptorSetLayout" ))
			return false;

		info.bindingCount = ARRAYSIZE( textures );
		info.pBindings = textures;
		if( !R_VKCheck( vkCreateDescriptorSetLayout( vk.device, &info, NULL, &vk_texture_layout ), "vkCreateDescriptorSetLayout" ))
			return false;
	}

	sets[0] = vk_uniform_layout;
	sets[1] = vk_texture_layout;

	{
		const VkPipelineLayoutCreateInfo info =
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = ARRAYSIZE( sets ),
			.pSetLayouts = sets,
		};
		const VkPipelineCacheCreateInfo cache = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };

		return R_VKCheck( vkCreatePipelineLayout( vk.device, &info, NULL, &vk_pipeline_layout ), "vkCreatePipelineLayout" )
			&& R_VKCheck( vkCreatePipelineCache( vk.device, &cache, NULL, &vk_pipeline_cache ), "vkCreatePipelineCache" );
	}
}

qboolean R_VKInitBackend( void )
{
	static const VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * VK_FRAMES * VK_MAX_CHUNKS } };
	const VkDescriptorPoolCreateInfo pool =
	{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = VK_FRAMES * VK_MAX_CHUNKS,
		.poolSizeCount = ARRAYSIZE( sizes ),
		.pPoolSizes = sizes,
	};

	vk_viewport.minz = 0.0f;
	vk_viewport.maxz = 1.0f;
	vk_viewport.width = gpGlobals->width;
	vk_viewport.height = gpGlobals->height;

	if( !R_VKCheck( vkCreateDescriptorPool( vk.device, &pool, NULL, &vk_uniform_pool ), "vkCreateDescriptorPool" )
		|| !R_VKCreateLayouts( ))
		return false;

	for( int i = 0; i < VK_FRAMES; i++ )
	{
		vk.frames[i].staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		vk.frames[i].staging.chunk_size = VK_STAGING_SIZE;
		vk.frames[i].vertices.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		vk.frames[i].vertices.chunk_size = VK_VERTICES_SIZE;
		vk.frames[i].uniforms.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		vk.frames[i].uniforms.chunk_size = VK_UNIFORMS_SIZE;
	}

	// what an empty unit reads
	vk_white = R_GL3BackendCreateTexture( 1, 1, 1 );
	*R_GL3BackendLockLevel( vk_white, 0, 1, 1 ) = 0xffffffffu;
	R_GL3BackendUnlockLevel();
	return true;
}

void R_VKShutdownBackend( void )
{
	for( int i = 0; i < VK_FRAMES; i++ )
	{
		vk_frame_t *frame = &vk.frames[i];

		R_VKResetFrame( frame );
		R_VKFreeRing( &frame->staging );
		R_VKFreeRing( &frame->vertices );
		R_VKFreeRing( &frame->uniforms );

		for( int j = 0; j < frame->num_pools; j++ )
			vkDestroyDescriptorPool( vk.device, frame->pools[j], NULL );
		frame->num_pools = 0;
	}

	for( int i = 1; i < vk_num_images; i++ )
	{
		if( vk_images[i].image )
		{
			vkDestroyImageView( vk.device, vk_images[i].view, NULL );
			vkDestroyImage( vk.device, vk_images[i].image, NULL );
		}
	}
	memset( vk_images, 0, sizeof( vk_images ));
	vk_num_images = 1;
	vk_white = 0;

	for( int i = 0; i < VK_MAX_PIPELINES; i++ )
	{
		if( vk_pipelines[i].pipeline )
			vkDestroyPipeline( vk.device, vk_pipelines[i].pipeline, NULL );
	}
	memset( vk_pipelines, 0, sizeof( vk_pipelines ));

	for( int i = 0; i < vk_num_programs; i++ )
	{
		vkDestroyShaderModule( vk.device, vk_programs[i].vs, NULL );
		vkDestroyShaderModule( vk.device, vk_programs[i].fs, NULL );
	}
	vk_num_programs = 0;
	vk_program = NULL;

	for( int a = 0; a < 2; a++ )
		for( int b = 0; b < 2; b++ )
			for( int c = 0; c < 16; c++ )
				for( int d = 0; d < 17; d++ )
					if( vk_samplers[a][b][c][d] )
						vkDestroySampler( vk.device, vk_samplers[a][b][c][d], NULL );
	memset( vk_samplers, 0, sizeof( vk_samplers ));

	if( vk_pipeline_cache )
		vkDestroyPipelineCache( vk.device, vk_pipeline_cache, NULL );
	if( vk_pipeline_layout )
		vkDestroyPipelineLayout( vk.device, vk_pipeline_layout, NULL );
	if( vk_texture_layout )
		vkDestroyDescriptorSetLayout( vk.device, vk_texture_layout, NULL );
	if( vk_uniform_layout )
		vkDestroyDescriptorSetLayout( vk.device, vk_uniform_layout, NULL );
	if( vk_uniform_pool )
		vkDestroyDescriptorPool( vk.device, vk_uniform_pool, NULL );
	vk_pipeline_cache = VK_NULL_HANDLE;
	vk_pipeline_layout = VK_NULL_HANDLE;
	vk_texture_layout = vk_uniform_layout = VK_NULL_HANDLE;
	vk_uniform_pool = VK_NULL_HANDLE;

	for( int i = 0; i < VK_MAX_BLOCKS; i++ )
	{
		if( vk_blocks[i].memory )
			vkFreeMemory( vk.device, vk_blocks[i].memory, NULL );
		memset( &vk_blocks[i], 0, sizeof( vk_blocks[i] ));
	}
}
