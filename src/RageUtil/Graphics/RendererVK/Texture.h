#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
#include <RageUtil/Graphics/RageSurface.h>

struct Texture
{
	enum
	{
		Wrapping = 0b01,
		Filtering = 0b10,
		PossibleSamplerCount = 4,
		MaxSlots = 64
	};
	RageSurface* surface = nullptr;
	VmaAllocation allocation = nullptr;
	VmaAllocator allocator = nullptr;
	VkImage image = nullptr;
	vk::ImageView view = nullptr;
	VkDevice device = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
	Texture();
	Texture(RageSurface* surface,
			uint32_t width,
			uint32_t height,
			VmaAllocator allocator,
			vk::raii::Device& device);
	~Texture();
};
