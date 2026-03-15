#ifndef VK_UTILS_H
#define VK_UTILS_H

#include <vulkan/vulkan_raii.hpp>
#include <deque>
#include <functional>
#include <source_location>
#include <span>
#include <glslang/Public/ShaderLang.h>
#include <optional>

void
ThrowIfFail(
  VkResult result,
  const std::source_location location = std::source_location::current());

void
ThrowIfFail(
  vk::Result result,
  const std::source_location location = std::source_location::current());

void
Fail(const std::source_location location = std::source_location::current());

vk::raii::ShaderModule
LoadShaderFromFile(std::string path,
				   vk::raii::Device& device,
				   EShLanguage shaderKind);

std::optional<uint32_t>
GetMemoryType(uint32_t typeBits,
			  vk::MemoryPropertyFlags neededProps,
			  vk::PhysicalDeviceMemoryProperties memoryProps);

#endif
