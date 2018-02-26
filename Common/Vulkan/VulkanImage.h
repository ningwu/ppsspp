#pragma once

#include "Common/Vulkan/VulkanContext.h"

class VulkanDeviceAllocator;

// Wrapper around what you need to use a texture.
// Not very optimal - if you have many small textures you should use other strategies.
class VulkanTexture {
public:
	VulkanTexture(VulkanContext *vulkan, VulkanDeviceAllocator *allocator)
		: vulkan_(vulkan), image(VK_NULL_HANDLE), mem(VK_NULL_HANDLE), view(VK_NULL_HANDLE),
		tex_width(0), tex_height(0), numMips_(1), format_(VK_FORMAT_UNDEFINED),
		allocator_(allocator), offset_(0) {
	}
	~VulkanTexture() {
		Destroy();
	}

	// Fast uploads from buffer. Mipmaps supported.
	// Usage must at least include VK_IMAGE_USAGE_TRANSFER_DST_BIT in order to use UploadMip.
	// When using UploadMip, initialLayout should be VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL.
	bool CreateDirect(VkCommandBuffer cmd, int w, int h, int numMips, VkFormat format, VkImageLayout initialLayout, VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, const VkComponentMapping *mapping = nullptr);
	void UploadMip(VkCommandBuffer cmd, int mip, int mipWidth, int mipHeight, VkBuffer buffer, uint32_t offset, size_t rowLength);  // rowLength is in pixels
	void GenerateMip(VkCommandBuffer cmd, int mip);
	void EndCreate(VkCommandBuffer cmd, bool vertexTexture = false);

	void Destroy();

	// Used in image copies, etc.
	VkImage GetImage() const { return image; }

	// Used for sampling, generally.
	VkImageView GetImageView() const { return view; }

	int32_t GetWidth() const { return tex_width; }
	int32_t GetHeight() const { return tex_height; }
	int32_t GetNumMips() const { return numMips_; }
	VkFormat GetFormat() const { return format_; }

private:
	void Wipe();

	VulkanContext *vulkan_;
	VkImage image;
	VkImageView view;
	VkDeviceMemory mem;
	int32_t tex_width, tex_height, numMips_;
	VkFormat format_;
	VulkanDeviceAllocator *allocator_;
	size_t offset_;
};
