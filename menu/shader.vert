#version 450

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main() {
    /* Positions arrive pre-transformed to clip space by the backend. */
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV;
    vColor = aColor;
}
