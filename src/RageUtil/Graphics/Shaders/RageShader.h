/* RageShader - interface for shader handlers */

#ifndef RAGE_SHADER_H
#define RAGE_SHADER_H

#include <string>

// TODO: Lua bindings so it's easy to pass these thingies around
class RageShader
{
  public:
	RageShader(const std::string& path)
	  : m_Path(path) {}
	virtual ~RageShader() {}

  public:
	const std::string m_Path;
};

#endif
