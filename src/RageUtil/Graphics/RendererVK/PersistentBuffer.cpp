#include "PersistentBuffer.h"
#include "VkUtils.h"

void
PersistentBuffer::Init(VmaAllocator allocator,
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
PersistentBuffer::Get() const
{
	return vk::Buffer(buffer);
}

void*
PersistentBuffer::GetMappedData() const
{
	return allocInfo.pMappedData;
}

PersistentBuffer::~PersistentBuffer()
{
	if (buffer != VK_NULL_HANDLE && allocation != VK_NULL_HANDLE) {
		vmaDestroyBuffer(allocator, buffer, allocation);
	}
}
