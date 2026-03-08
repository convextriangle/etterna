#version 460
#extension GL_EXT_nonuniform_qualifier : enable

struct Vertex {
    float pos[3];
    float normal[3];
    uint color;
    float uv[2];
};

layout(std430, set = 0, binding = 0) readonly buffer VertexBuffer {
    Vertex vertices[];
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

struct DrawSettings
{
    uint FirstVertexIndex;
	uint MatrixIndex;
	uint TextureIndex;
	uint SamplerIndex;
};

layout(std430, set = 0, binding = 2) readonly buffer DrawSettingsBuffer {
    uint SettingsCount;
    DrawSettings settingsBuffer[];
};

layout(location = 0) out vec4 vertexColor;
layout(location = 1) out uint textureIndex;
layout(location = 2) out uint samplerIndex;
layout(location = 3) out vec2 vertexUV;

vec2 unpackVec2(float array[2]){
    return vec2(array[0], array[1]);
}

vec3 unpackVec3(float array[3]){
    return vec3(array[0], array[1], array[2]);
}

vec4 unpackColor(uint c)
{
    float b = float(c & 0xFFu);
    float g = float((c >> 8) & 0xFFu);
    float r = float((c >> 16) & 0xFFu);
    float a = float((c >> 24) & 0xFFu);

    return vec4(r, g, b, a) / 255.0;
}

void main() {
    Vertex currentVertex = vertices[gl_VertexIndex];
    
    vertexColor = unpackColor(currentVertex.color);
    uint settingsId = 0;
    if (SettingsCount > 0) {
        uint low = 0;
        uint high = SettingsCount - 1;
        while (low < high) {
            uint mid = (low + high + 1) >> 1;
            if (settingsBuffer[mid].FirstVertexIndex <= gl_VertexIndex) {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        settingsId = low;
    }

    DrawSettings settings = settingsBuffer[settingsId];

    textureIndex = settings.TextureIndex;
    samplerIndex = settings.SamplerIndex;

    uint matrixIndex = settings.MatrixIndex;
    mat4 world = matrices[matrixIndex].world;
    mat4 view = matrices[matrixIndex].view;
    mat4 proj = matrices[matrixIndex].projection;
    mat4 tex = matrices[matrixIndex].texture;
    
    vec4 worldPos = world * vec4(unpackVec3(currentVertex.pos), 1.0);
    vec4 viewPos = view * worldPos;
    vec4 projPos = proj * viewPos;
    projPos.z = 0.0; // whart?
    
    gl_Position = projPos;

    vertexUV = unpackVec2(currentVertex.uv);
    vertexUV.x += tex[3][0];
    vertexUV.y += tex[3][1];
}