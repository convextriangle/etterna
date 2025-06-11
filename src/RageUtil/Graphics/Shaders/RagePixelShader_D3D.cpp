#include "RagePixelShader_D3D.h"
#include "Core/Services/Locator.hpp"
#include "RageUtil/Graphics/RageDisplay_D3D_Helpers.h"

// Static libraries
// load Windows D3D9 dynamically
#if defined(_MSC_VER)
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dx9.lib")
#endif

HRESULT
RagePixelShader_D3D::Compile(const std::string& pixelShaderProfile)
{
	LPD3DXBUFFER errorBuffer = NULL;
	auto result = D3DXCompileShaderFromFile(m_Path.c_str(),
											NULL,
											NULL,
	  RageDisplay_D3D_Helpers::ShaderEntryPoint.data(),
											pixelShaderProfile.c_str(),
											0,
											&m_ShaderBuffer,
											&errorBuffer,
											NULL);

	if (result != D3D_OK) {
		Locator::getLogger()->warn("RagePixelShader_D3D D3DXCompileShaderFromFile "
								   "failed for {} - {}",
								   m_Path,
		  RageDisplay_D3D_Helpers::GetErrorString(result));
		errorBuffer->Release();
	}

	return result;
}

IDirect3DPixelShader9*
RagePixelShader_D3D::CreateForDevice(LPDIRECT3DDEVICE9 device) const
{
	IDirect3DPixelShader9* shader = NULL;
	auto result = device->CreatePixelShader(
	  (const DWORD*)m_ShaderBuffer->GetBufferPointer(), &shader);

	if (result != D3D_OK) {
		Locator::getLogger()->warn(
		  "RagePixelShader_D3D "
		  "CreateForDevice failed for {} - {}",
		  m_Path,
		  RageDisplay_D3D_Helpers::GetErrorString(result));
		return NULL;
	}

	return shader;
}

RagePixelShader_D3D::~RagePixelShader_D3D()
{
	if (m_ShaderBuffer != nullptr) {
		m_ShaderBuffer->Release();
		m_ShaderBuffer = nullptr;
	}
}
