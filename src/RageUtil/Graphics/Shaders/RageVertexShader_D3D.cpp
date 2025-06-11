#include "RageVertexShader_D3D.h"
#include "Core/Services/Locator.hpp"
#include "RageUtil/Graphics/RageDisplay_D3D_Helpers.h"

// Static libraries
// load Windows D3D9 dynamically
#if defined(_MSC_VER)
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dx9.lib")
#endif

HRESULT
RageVertexShader_D3D::Compile(const std::string& pixelShaderProfile)
{
	LPD3DXBUFFER errorBuffer = nullptr;
	auto result = D3DXCompileShaderFromFile(m_Path.c_str(),
											nullptr,
											nullptr,
	  RageDisplay_D3D_Helpers::ShaderEntryPoint.data(),
											pixelShaderProfile.c_str(),
											0,
											&m_ShaderBuffer,
											&errorBuffer,
											nullptr);

	if (result != D3D_OK) {
		Locator::getLogger()->warn(
		  "RageVertexShader_D3D D3DXCompileShaderFromFile "
		  "failed for {} - {}",
		  m_Path,
		  RageDisplay_D3D_Helpers::GetErrorString(result));
		errorBuffer->Release();
	}

	return result;
}

IDirect3DVertexShader9*
RageVertexShader_D3D::CreateForDevice(LPDIRECT3DDEVICE9 device) const
{
	IDirect3DVertexShader9* shader = nullptr;
	auto result = device->CreateVertexShader(
	  (const DWORD*)m_ShaderBuffer->GetBufferPointer(), &shader);

	if (result != D3D_OK) {
		Locator::getLogger()->warn(
		  "RageVertexShader_D3D "
		  "CreateForDevice failed for {} - {}",
		  m_Path,
		  RageDisplay_D3D_Helpers::GetErrorString(result));
		return nullptr;
	}

	return shader;
}

RageVertexShader_D3D::~RageVertexShader_D3D()
{
	if (m_ShaderBuffer != nullptr) {
		m_ShaderBuffer->Release();
		m_ShaderBuffer = nullptr;
	}
}
