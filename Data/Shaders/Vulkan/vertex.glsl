#version 460
#extension GL_EXT_nonuniform_qualifier : enable

struct DrawCommandArgument {
    uint renderStateIndex;
};

layout(std430, set = 0, binding = 0) readonly buffer DrawCommandArgumentBuffer {
    DrawCommandArgument arguments[];
};

struct MatrixState {
    mat4 projection;
    mat4 view;
    mat4 world;
    mat4 texture;
};

layout(std430, set = 0, binding = 1) readonly buffer MatrixStateBuffer {
    MatrixState matrices[];
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inUV;

layout(location = 0) out vec4 outColor;

void main() {
    uint matrixIndex = gl_BaseInstance;
    
    mat4 worldMatrix = matrices[matrixIndex].world;
    mat4 viewMatrix = matrices[matrixIndex].view;
    mat4 projMatrix = matrices[matrixIndex].projection;
    
    vec4 worldPos = worldMatrix * vec4(inPosition, 1.0);
    vec4 viewPos = viewMatrix * worldPos;
    vec4 projPos = projMatrix * viewPos;
    
    gl_Position = projPos;
    outColor = inColor;
}