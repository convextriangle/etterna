#version 460

const uint TextureCount = 64U;
layout(set = 0, binding = 2) uniform sampler2D textures[TextureCount];

layout(location = 0) in vec4 vertexColor;
layout(location = 1) flat in uint textureIndex;
layout(location = 2) in vec2 vertexUV;

layout(location = 0) out vec4 fragmentColor;

void main() {
    if(textureIndex == 0){
        fragmentColor = vertexColor;
        return;
    }

    vec4 textureColor = texture(textures[textureIndex - 1], vertexUV);
    fragmentColor = vertexColor * textureColor;
}