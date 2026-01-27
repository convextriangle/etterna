#ifndef VK_UTILS_H
#define VK_UTILS_H

#include <vulkan/vulkan_raii.hpp>
#include <deque>
#include <functional>
#include <source_location>
#include <span>
#include <shaderc/shaderc.hpp>

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

std::vector<uint32_t>
CompileShader(const std::string& sourceName,
			  shaderc_shader_kind shaderKind,
			  const std::string& source);

vk::raii::ShaderModule
LoadShaderFromFile(std::string path,
				   vk::raii::Device& device,
				   shaderc_shader_kind shaderKind);


#endif