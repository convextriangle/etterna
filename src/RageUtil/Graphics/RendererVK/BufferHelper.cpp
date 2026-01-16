#include "BufferHelper.h"
#include "VkUtils.h"

void
BufferHelper::Init(VmaAllocator allocator,
				   const vk::BufferCreateInfo& createInfo,
				   const VmaAllocationCreateInfo& allocInfo)
{
	this->allocator = allocator;
	ThrowIfFail(
	  vmaCreateBuffer(allocator,
					  &static_cast<const VkBufferCreateInfo&>(createInfo),
					  &allocInfo,
					  &this->buffer,
					  &this->allocation,
					  &this->allocInfo));
}

vk::Buffer
BufferHelper::Get() const
{
	return vk::Buffer(buffer);
}

void*
BufferHelper::GetMappedData() const
{
	return allocInfo.pMappedData;
}

BufferHelper::~BufferHelper()
{
	if (buffer != VK_NULL_HANDLE && allocation != VK_NULL_HANDLE) {
		vmaDestroyBuffer(allocator, buffer, allocation);
	}
}
