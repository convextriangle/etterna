#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout(set = 0, binding = 0) readonly buffer DrawCommandArgumentBuffer {
    uint matrixStateIndex;
    uint renderStateIndex;
} arguments[];

layout(set = 0, binding = 1) readonly buffer MatrixStateBuffer {
    mat4 projection;
    mat4 view;
    mat4 world;
    mat4 texture;
} matrices[];

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inUV;

layout(location = 0) out vec4 outColor;

void main() {
    uint instance = gl_InstanceIndex;
    
    uint matrixIndex = arguments[instance].matrixStateIndex;
    
    mat4 worldMatrix = matrices[instance].world;
    mat4 viewMatrix = matrices[instance].view;
    mat4 projMatrix = matrices[instance].projection;
    
    vec4 worldPos = worldMatrix * vec4(inPosition, 1.0);
    vec4 viewPos = viewMatrix * worldPos;
    vec4 projPos = projMatrix * viewPos;
    
    gl_Position = projPos;

    outColor = inColor;
}