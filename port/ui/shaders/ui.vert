#version 450
// PS5CEMU-HAR's UI kit (port/ui/gfx.cpp): one quad per instance, in the 1920 x 1080 layout space; the
// viewport scales it to the output. Its corners come from the vertex index (two triangles), so the
// kit binds no vertex buffer, only the instances.

layout(location = 0) in vec4 aRect;   // the quad drawn: x, y, width, height
layout(location = 1) in vec4 aBox;    // the shape: x, y, width, height
layout(location = 2) in vec4 aShape;  // corner radius, ring width, softness, kind
layout(location = 3) in vec4 aColour0;
layout(location = 4) in vec4 aColour1;
layout(location = 5) in vec4 aGradient; // a line (x0, y0, x1, y1) or a centre and a radius (x, y, r, -)
layout(location = 6) in vec4 aUv;     // the texture's rectangle for the box
layout(location = 7) in vec4 aClip;   // x, y, width, height; width 0: none
layout(location = 8) in vec4 aExtra;  // clip's corner radius, gradient kind, glyph weight, gradient start

layout(push_constant) uniform Constants
{
	vec2 layoutSize; // 1920, 1080
	float scale;     // output pixels per layout unit
	float time;      // seconds, for the grain
} pc;

layout(location = 0) out vec2 vPos;
layout(location = 1) flat out vec4 vBox;
layout(location = 2) flat out vec4 vShape;
layout(location = 3) flat out vec4 vColour0;
layout(location = 4) flat out vec4 vColour1;
layout(location = 5) flat out vec4 vGradient;
layout(location = 6) flat out vec4 vUv;
layout(location = 7) flat out vec4 vClip;
layout(location = 8) flat out vec4 vExtra;

const vec2 kCorners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main()
{
	vec2 p = aRect.xy + kCorners[gl_VertexIndex] * aRect.zw;
	vPos = p;
	vBox = aBox;
	vShape = aShape;
	vColour0 = aColour0;
	vColour1 = aColour1;
	vGradient = aGradient;
	vUv = aUv;
	vClip = aClip;
	vExtra = aExtra;
	gl_Position = vec4(p / pc.layoutSize * 2.0 - 1.0, 0.0, 1.0);
}
