#version 450

layout (location = 0) in vec3 inPos;

layout (location = 1) in vec4 blendweight;
layout (location = 2) in uvec4 blendindices;
layout (location = 3) in vec2 inTexcoord0;
layout (location = 4) in vec2 inTexcoord1;

layout (binding = 0) uniform UBO 
{
	mat4 projectionMatrix;
	mat4 modelMatrix;
	mat4 viewMatrix;
	mat4 invProjectionMatrix;
	mat4 invViewMatrix;
	uint faction;
} ubo;

layout (location = 0) out vec2 outTexcoord0;
layout (location = 1) out vec2 outTexcoord1;


out gl_PerVertex 
{
    vec4 gl_Position;   
};


void main() 
{
	outTexcoord0 = inTexcoord0;
	outTexcoord1 = inTexcoord1;

	gl_Position = ubo.projectionMatrix * ubo.viewMatrix * ubo.modelMatrix * vec4(inPos.xyz, 1.0);
}
