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

layout(set = 0, binding = 2) readonly buffer RenderStateBuffer {
    uint cullMode;
    uint zTestMode;
    uint blendMode;
    float zBias;
    int zWrite;
    int alphaTest;
    int textureWrapping[8];
    int textureFiltering[8];
    uint textureMode[8];
    //uint64_t textures[8];
} renderStates[];

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inUV;

layout(location = 0) out vec4 outColor;

void main() {
    uint instanceID = gl_InstanceIndex;
    
    uint matrixIndex = arguments[instanceID].matrixStateIndex;
    uint renderIndex = arguments[instanceID].renderStateIndex;
    
    mat4 worldMatrix = matrices[matrixIndex].world;
    mat4 viewMatrix = matrices[matrixIndex].view;
    mat4 projMatrix = matrices[matrixIndex].projection;
    
    vec4 worldPos = worldMatrix * vec4(inPosition, 1.0);
    vec4 viewPos = viewMatrix * worldPos;
    vec4 projPos = projMatrix * viewPos;
    
    gl_Position = projPos;
    gl_Position.y *= -1.0;
    
    outColor = inColor;
}