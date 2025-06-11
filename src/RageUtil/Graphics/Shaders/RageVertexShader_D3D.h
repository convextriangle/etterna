#ifndef RAGE_VERTEX_SHADER_D3D_H
#define RAGE_VERTEX_SHADER_D3D_H

#include "RageShader.h"
#include <d3d9.h>
#include <d3dx9.h>

class RageVertexShader_D3D : public RageShader
{
  public:
	[[nodiscard]] HRESULT Compile(const std::string& pixelShaderProfile);

	// caller is responsible for cleanup
	[[nodiscard]] IDirect3DVertexShader9* CreateForDevice(
	  LPDIRECT3DDEVICE9 device) const;

	RageVertexShader_D3D(const std::string& path)
	  : RageShader(path)
	{
		m_ShaderBuffer = nullptr;
	}
	~RageVertexShader_D3D();

  private:
	LPD3DXBUFFER m_ShaderBuffer;
};

#endif
