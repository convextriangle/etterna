#include "VkUtils.h"
#include <format>
#include <fstream>
#include <sstream>
#include "Core/Services/Locator.hpp"

void
DeletionQueue::PushDeletionCallback(std::function<void()>&& callback)
{
	Callbacks.push_back(callback);
}

void
DeletionQueue::FlushCallbacks()
{
	for (auto it = Callbacks.rbegin(); it != Callbacks.rend(); it++) {
		(*it)();
	}

	Callbacks.clear();
}

void
ThrowIfFail(VkResult result, const std::source_location location)
{
	if (result == VK_SUCCESS) {
		return;
	}

	const std::string message =
	  std::format("RendererVK failed: VkResult {} at {}:{} in function {}",
				  static_cast<int>(result),
				  location.file_name(),
				  location.line(),
				  location.function_name());
	Locator::getLogger()->error(message);
	throw std::runtime_error(message.c_str());
}

void ThrowIfFail(vk::Result result, const std::source_location location)
{
	ThrowIfFail(static_cast<VkResult>(result), location);
}

void
Fail(const std::source_location location)
{
	const std::string message =
	  std::format("RendererVK failed at {}:{} in function {}",
				  location.file_name(),
				  location.line(),
				  location.function_name());
	Locator::getLogger()->error(message);
	throw std::runtime_error(message.c_str());
}

std::vector<uint32_t>
CompileShader(const std::string& sourceName,
			  shaderc_shader_kind shaderKind,
			  const std::string& source)
{
	shaderc::Compiler compiler;
	shaderc::CompileOptions options;

	auto result = compiler.CompileGlslToSpv(
	  source, shaderKind, sourceName.c_str(), options);

	if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
		auto message = std::format("Vulkan GLSL shader compilation failed: {}",
								   result.GetErrorMessage());
		throw std::runtime_error(message);
	}

	return { result.begin(), result.end() };
}

vk::raii::ShaderModule
LoadShaderFromFile(std::string path,
				   vk::raii::Device& device,
				   shaderc_shader_kind shaderKind)
{
#ifdef _WIN32
	if (path[0] == '/') {
		path = path.substr(1);
	}
#endif

	std::ifstream inputFile(path);
	std::stringstream contents;
	contents << inputFile.rdbuf();
	auto shaderBlob = CompileShader("meow", shaderKind, contents.str());

	VkShaderModuleCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.pNext = nullptr;
	createInfo.codeSize = shaderBlob.size() * sizeof(uint32_t);
	createInfo.pCode = shaderBlob.data();

	return vk::raii::ShaderModule(device, createInfo);
}

uint32_t
FindMemoryType(VkPhysicalDevice physicalDevice,
			   uint32_t typeFilter,
			   VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties memoryProps = {};
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProps);

	for (uint32_t i = 0; i < memoryProps.memoryTypeCount; i++) {
		if ((typeFilter & (1 << i)) &&
			(memoryProps.memoryTypes[i].propertyFlags & properties) ==
			  properties) {
			return i;
		}
	}

	Fail();
}

void
CreateBuffer(VkDevice device,
			 VkPhysicalDevice gpu,
			 VkDeviceSize size,
			 VkBufferUsageFlags usageFlags,
			 VkMemoryPropertyFlags properties,
			 VkBuffer& buffer,
			 VkDeviceMemory& bufferMemory)
{
	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usageFlags;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	ThrowIfFail(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer));

	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(device, buffer, &requirements);

	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex =
	  FindMemoryType(gpu, requirements.memoryTypeBits, properties);

	ThrowIfFail(vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory));

	ThrowIfFail(vkBindBufferMemory(device, buffer, bufferMemory, 0));
}

void
CreateDynamicBuffer(VkDevice device,
					VkPhysicalDevice gpu,
					VkBuffer& buffer,
					VkDeviceMemory& bufferMemory,
					size_t neededSize,
					VkBufferUsageFlags usageFlags)
{
	CreateBuffer(device,
				 gpu,
				 neededSize,
				 usageFlags,
				 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				 buffer,
				 bufferMemory);
}

void
UpdateDynamicBuffer(VkDevice device,
					VkPhysicalDevice gpu,
					VkBuffer& buffer,
					VkDeviceMemory& bufferMemory,
					const void* data,
					size_t dataSize)
{
	if (!dataSize) {
		return;
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(gpu, &props);
	VkDeviceSize nonCoherentAtomSize = props.limits.nonCoherentAtomSize;

	VkDeviceSize alignedSize =
	  (dataSize + nonCoherentAtomSize - 1) & ~(nonCoherentAtomSize - 1);

	VkMemoryRequirements memReqs;
	vkGetBufferMemoryRequirements(device, buffer, &memReqs);
	alignedSize = std::min(alignedSize, memReqs.size);

	void* mappedData = nullptr;
	ThrowIfFail(
	  vkMapMemory(device, bufferMemory, 0, alignedSize, 0, &mappedData));

	std::memcpy(mappedData, data, dataSize);

	VkMappedMemoryRange memoryRange = { .sType =
										  VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
										.memory = bufferMemory,
										.offset = 0,
										.size = alignedSize };
	vkFlushMappedMemoryRanges(device, 1, &memoryRange);

	vkUnmapMemory(device, bufferMemory);
}
