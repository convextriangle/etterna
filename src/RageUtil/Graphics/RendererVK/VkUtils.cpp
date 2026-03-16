#include "VkUtils.h"
#include <format>
#include <fstream>
#include <sstream>
#include "Core/Services/Locator.hpp"
#include <Etterna/Globals/global.h>

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

void
ThrowIfFail(vk::Result result, const std::source_location location)
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
		Locator::getLogger()->error(message);
		sm_crash(message.c_str());
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

std::optional<uint32_t>
GetMemoryType(uint32_t typeBits,
			  vk::MemoryPropertyFlags neededProps,
			  vk::PhysicalDeviceMemoryProperties memoryProps)
{
	for (uint32_t i = 0; i < memoryProps.memoryTypeCount; i++) {
		if ((typeBits & 1) == 1) {
			if ((memoryProps.memoryTypes[i].propertyFlags & neededProps) ==
				neededProps) {
				return i;
			}
		}
		typeBits >>= 1;
	}

	return std::nullopt;
}
