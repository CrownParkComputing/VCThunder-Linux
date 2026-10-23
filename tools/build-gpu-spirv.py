#!/usr/bin/env python3
"""Compile the shared renderer HLSL to embedded SDL GPU SPIR-V shaders."""
from pathlib import Path
import argparse
import struct
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', default='glslc')
parser.add_argument('--output', default='build/native/generated/gpu_spirv.inc')
args = parser.parse_args()
output = Path(args.output)
output.parent.mkdir(parents=True, exist_ok=True)
entries = ['main_vs', 'main_ps', 'copy_vs', 'overlay_ps', 'clear_ps',
           'depth_resolve_ps', 'over_ps', 'upscale_bilinear_ps', 'clear_layer_ps',
           'clear_layer_color_ps', 'layer_depth_ps', 'compose_ps',
           'upscale_catmull_ps', 'fsr_easu_ps', 'fsr_rcas_ps', 'bezel_ps']
lines = ['/* Generated SPIR-V; shader source: renderer/posix/gpu_shader.hlsl. */']
for entry in entries:
    binary = output.parent / (entry + '.spv')
    subprocess.run([args.compiler, '-x', 'hlsl', '-fshader-stage=' +
                    ('vertex' if entry.endswith('_vs') else 'fragment'),
                    '-fentry-point=' + entry, '-fauto-combined-image-sampler',
                    '-DGPU_BATCH_TRIANGLES=16', '-O', 'renderer/posix/gpu_shader.hlsl', '-o', str(binary)], check=True)
    data = binary.read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    lines.append(f'static const uint32_t spv_{entry}[] = {{')
    for pos in range(0, len(words), 8):
        lines.append(','.join(f'0x{x:08x}u' for x in words[pos:pos + 8]) + ',')
    lines.append('};')
lines += ['static const shader_blob_t vgl_spirv[] = {']
for entry in entries:
    lines.append(f'{{spv_{entry}, sizeof spv_{entry}, "{entry}"}},')
lines += ['{NULL, 0, NULL}', '};']
output.write_text('\n'.join(lines) + '\n')
