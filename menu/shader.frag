#version 450

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;

layout(binding = 0) uniform sampler2D uTexture;

layout(location = 0) out vec4 fragColor;

void main() {
    /* The font atlas is R8; sample its alpha and multiply the vertex colour. */
    float a = texture(uTexture, vUV).r;
    fragColor = vec4(vColor.rgb, vColor.a * a);
}
