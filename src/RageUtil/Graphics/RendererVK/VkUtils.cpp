#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#ifdef __unix__
#define VK_USE_PLATFORM_XLIB_KHR
#endif

#include "VkUtils.h"
#include <fmt/format.h>
#include <fstream>
#include <sstream>
#include "Core/Services/Locator.hpp"
#include <Etterna/Globals/global.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

void
ThrowIfFail(VkResult result, const std::source_location location)
{
	if (result == VK_SUCCESS) {
		return;
	}

	const std::string message =
	  fmt::format("RendererVK failed: VkResult {} at {}:{} in function {}",
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
	  fmt::format("RendererVK failed at {}:{} in function {}",
				  location.file_name(),
				  location.line(),
				  location.function_name());
	Locator::getLogger()->error(message);
	throw std::runtime_error(message.c_str());
}

std::vector<uint32_t>
CompileShader(EShLanguage shaderStage,
			  const std::string& source)
{
	static bool initialized = false;
	if (!initialized) {
		glslang::InitializeProcess();
		initialized = true;
	}

	const char* strings[] = { source.c_str() };

	glslang::TShader shader(shaderStage);
	shader.setStrings(strings, 1);

	shader.setEnvInput(
	  glslang::EShSourceGlsl, shaderStage, glslang::EShClientVulkan, 100);

	shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);

	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

	EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);

	if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
		auto message = fmt::format("Vulkan GLSL shader compilation failed:\n{}",
								   shader.getInfoLog());

		Locator::getLogger()->error(message);
		sm_crash(message.c_str());
	}

	glslang::TProgram program;
	program.addShader(&shader);

	if (!program.link(messages)) {
		auto message = fmt::format("Vulkan GLSL shader linking failed:\n{}",
								   program.getInfoLog());

		Locator::getLogger()->error(message);
		sm_crash(message.c_str());
	}

	std::vector<uint32_t> spirv;

	glslang::GlslangToSpv(*program.getIntermediate(shaderStage), spirv);

	return spirv;
}

vk::raii::ShaderModule
LoadShaderFromFile(std::string path,
				   vk::raii::Device& device,
				   ShaderType shaderType)
{
#ifdef _WIN32
	if (path[0] == '/') {
		path = path.substr(1);
	}
#endif

	EShLanguage shaderKind = {};
	switch (shaderType) {
		case ShaderType_Vertex:
			shaderKind = EShLangVertex;
			break;
		case ShaderType_Fragment:
			shaderKind = EShLangFragment;
			break;
		default:
			assert(false && "Invalid shader type specified!");
	}

	std::ifstream inputFile(path);
	std::stringstream contents;
	contents << inputFile.rdbuf();
	auto shaderBlob = CompileShader(shaderKind, contents.str());

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
