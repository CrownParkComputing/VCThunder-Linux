#!/usr/bin/env python3
"""Compare Vulkan with CPU output, including batches beyond the 4 KiB boundary."""
from pathlib import Path
import importlib.util
import os
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/native/gpu-check'
OUT.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location('conformance', ROOT / 'tools/make-conformance-capture.py')
scene = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scene)


def png_rgb(path):
    """Read the project's filter-zero RGB8 PNG output using only the stdlib."""
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, compressed = 8, bytearray()
    while pos < len(data):
        size, = struct.unpack_from('>I', data, pos)
        tag, payload = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + size]
        if tag == b'IHDR':
            width, height, bits, kind, *_ = struct.unpack('>IIBBBBB', payload)
            assert (bits, kind) == (8, 2), (path, bits, kind)
        if tag == b'IDAT': compressed.extend(payload)
        pos += size + 12
    rows = zlib.decompress(compressed)
    stride = width * 3 + 1
    assert len(rows) == stride * height, path
    assert all(rows[y * stride] == 0 for y in range(height)), path
    return width, height, b''.join(rows[y * stride + 1:(y + 1) * stride] for y in range(height))


def batch_scene():
    cap = scene.Capture('gpu-batch-regression')
    cap.call('grGlideInit')
    cap.call('grSstSelect', 0)
    cap.call('grSstWinOpen', 0, scene.RES_640x400, 0, 1, 0, 2, 1)
    cap.call('grClipWindow', 0, 0, 640, 400)
    cap.call('grColorCombine', 1, 0, 0, 0, 0)
    cap.call('grAlphaCombine', 1, 0, 0, 0, 0)
    cap.call('grAlphaBlendFunction', 4, 0, 4, 0)
    cap.call('grDepthBufferMode', 2)
    cap.call('grDepthBufferFunction', 3)
    cap.call('grDepthMask', 1)
    cap.frame()
    cap.call('grBufferClear', 0x00102030, 255, 65535)
    for i in range(64):
        x, y = 10 + i % 8 * 76, 10 + i // 8 * 46
        color = (32 + i * 3, 220 - i * 2, 80 + i)
        v = lambda px, py: scene.vertex(px, py, *color, 255, 0.005 + i * 0.0001)
        scene.tri(cap, v(x, y), v(x + 40, y), v(x, y + 30))
    cap.call('grBufferSwap', 0)
    cap.frame()
    cap.call('grBufferClear', 0, 255, 65535)
    for i in range(64):
        color = (32 + i * 3, 220 - i * 2, 80 + i)
        v = lambda x, y: scene.vertex(x, y, *color, 255, 0.005 + i * 0.0001)
        scene.tri(cap, v(40, 50), v(600, 50), v(40, 350))
    cap.call('grBufferSwap', 0)
    cap.call('grSstWinClose')
    cap.call('grGlideShutdown')
    path = OUT / 'batch.vglc'
    cap.write(path)
    return path


def compare(capture, exact):
    name = capture.stem
    for backend in ('cpu', 'gpu'):
        directory = OUT / f'{name}-{backend}'
        directory.mkdir(exist_ok=True)
        env = dict(os.environ, VCGLIDE_RENDERER=backend, VCGLIDE_THREADS='1',
                   VCGLIDE_LOG=str(directory / 'renderer.log'), VCGLIDE_RENDER_SCALE='1')
        with (directory / 'replay.log').open('w') as log:
            subprocess.run([str(ROOT / 'build/linux/glide-replay'), str(capture),
                            '--provider', str(ROOT / 'build/libvcglide.so'), '--out', str(directory)],
                           env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        if backend == 'gpu':
            assert 'SDL GPU renderer' in (directory / 'renderer.log').read_text(), 'Vulkan silently fell back to CPU'
    count = 0
    for gpu in sorted((OUT / f'{name}-gpu').glob('frame*.png')):
        gw, gh, actual = png_rgb(gpu)
        cw, ch, reference = png_rgb(OUT / f'{name}-cpu' / gpu.name)
        assert (gw, gh) == (cw, ch), gpu
        differences = [abs(a - b) for a, b in zip(actual, reference)]
        mean = sum(differences) / len(differences)
        bad = sum(max(differences[p:p + 3]) > 8 for p in range(0, len(differences), 3)) / (gw * gh)
        assert actual == reference if exact else mean < 1.0 and bad < .02, (gpu, mean, bad)
        print(f'{name}/{gpu.name}: mean error {mean:.3f}, pixels >8 error {bad:.2%}')
        count += 1
    assert count, 'No frames produced'
    return count


count = compare(ROOT / 'build/conformance.vglc', False)
count += compare(batch_scene(), True)
print(f'Vulkan: {count} frames verified against CPU, including long batches and depth overlap.')
