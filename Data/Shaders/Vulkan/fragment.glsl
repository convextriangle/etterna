#version 460

const uint TextureCount = 64U;
layout(set = 0, binding = 2) uniform sampler2D textures[TextureCount];

layout(location = 0) in vec4 inColor;
layout(location = 1) flat in uint inTexture;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = inColor * (inTexture > 0 ? texture(textures[inTexture - 1], inUV) : vec4(1.0));
    outColor.a = 1.0;
}