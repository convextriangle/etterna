#include "Texture.h"
#include <RageUtil/Utils/RageUtil.h>
#include "VkUtils.h"

Texture::Texture() {}

Texture::Texture(RageSurface* surface,
				 uint32_t width,
				 uint32_t height,
				 VmaAllocator allocator,
				 vk::raii::Device& device)
{
	width = power_of_two(width);
	height = power_of_two(height);

	VmaAllocationCreateInfo allocCreateInfo = {};
	allocCreateInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

	VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	imageInfo.extent = { width, height, 1 };
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.usage =
	  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

	VmaAllocationInfo allocInfo = {};
	ThrowIfFail(vmaCreateImage(allocator,
							   &imageInfo,
							   &allocCreateInfo,
							   &image,
							   &allocation,
							   &allocInfo));

	vk::ImageViewCreateInfo viewInfo;
	viewInfo.image = image;
	viewInfo.viewType = vk::ImageViewType::e2D;
	viewInfo.format = vk::Format::eR8G8B8A8Unorm;
	viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	view = device.createImageView(viewInfo);
	this->device = *device;
}

Texture::~Texture()
{
	if (image != nullptr && allocator != nullptr) {
		vmaDestroyImage(allocator, image, allocation);
	}
	if (view != nullptr && device != nullptr) {
		vkDestroyImageView(device, view, nullptr);
	}
}
