/* RageShader - interface for shader handlers */

#ifndef RAGE_SHADER_H
#define RAGE_SHADER_H

#include <string>

class RageShader
{
  public:
	virtual ~RageShader() {}

  protected:
	const std::string m_Path;
};

#endif