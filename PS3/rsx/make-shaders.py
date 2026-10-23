#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.
"""Generate the RSX shader set used by the PS3 hardware renderer and self-test.

PSL1GHT has no runtime shader compiler, so every program the renderer can use is written here as
NV40 assembly, assembled offline with cgcomp -a (which needs no Cg toolkit), and embedded in
RSXShaders.inc. Run again after changing this file:

    python3 PS3/rsx/make-shaders.py $PS3DEV/bin/cgcomp PS3/rsx/RSXShaders.inc

The T&L program is over 255 instructions: PSL1GHT's cgcomp parser has a fixed 256-instruction
buffer (MAX_NV_VERTEX_PROGRAM_INSTRUCTIONS in tools/cgcomp/include/parser.h) and corrupts its
heap beyond that, so it is built with 512 (the RSX's limit). The output is otherwise identical.

NV40 fragment-program rule this generator respects: one instruction may read at most ONE
interpolated input (f[...]) and at most one inline constant. Inputs are copied to temporaries first.

Vertex layout (see RSXCore.h, RSXVertex): attribute 0 = position (x,y,z,w, already clip space),
3 = color0 (RGBA8 normalized), 4 = color1 (RGBA8), 8 = texcoord (s, t, 0, q), 9 = fog (x).
Vertex constants c[0] = texture env color, c[1] = fog color, c[2] = source blend multiplier.
They reach the fragment program as constant varyings TEX2, TEX3, TEX4, which avoids patching
fragment-program constants in RSX memory between draws.
"""
import os, subprocess, sys, tempfile

VP = """!!ARBvp1.0
#var float4 envColor : : c[0] : 1 : 1
#var float4 fogColor : : c[1] : 1 : 1
#var float4 blendMul : : c[2] : 1 : 1
MOV o[HPOS], v[OPOS];
MOV o[COL0], v[COL0];
MOV o[COL1], v[COL1];
MOV o[TEX0], v[TEX0];
MOV o[TEX1], v[TEX1];
MOV o[TEX2], c[0];
MOV o[TEX3], c[1];
MOV o[TEX4], c[2];
END
"""

# Hardware transform and lighting (the PSP GE's vertex stage), one constant per instruction (NV40).
# Inputs: v[OPOS] position (model space), v[NRML] normal, v[COL0] vertex color (or the material
# ambient color when the vertex has none), v[TEX0] uv. Constants (see RSXRenderer::TLConstants):
#  c0-c2  env / fog color / blend multiplier (shared with the main program, passed to TEX2-TEX4)
#  c3-c6  rows of the final matrix (model -> RSX clip space, viewport folded in)
#  c7-c9  rows of the world matrix      c10 view-z row (fog)
#  c11    fog end, fog slope, fog on, lighting on      c12 uv scale u/v, offset u/v
#  c13    material emissive   c14 global ambient   c15 color source flags (ambient, diffuse, specular)
#  c16-18 material ambient, diffuse, specular        c19 specular exponent, set color1, add color1
#  c20    view direction (w: 1 = clamp depth instead of clipping)       c21 (0, 1, 0.5, tiny)
#  c22+6i light i: position/direction (w = directional), attenuation, spot direction (w = cutoff),
#         ambient (w = spot exponent), diffuse, specular (w = spot light). Powered diffuse is not done.
#  c46+i  environment mapping weights of light i: N.L into s, N.H into s, N.L into t, N.H into t
#  c50    x: environment mapping on (uv from the lights), y: texture matrix on
#  c51-53 texture matrix rows for s, t, q (the GE's tgen matrix, translation in w)
#  c54    texture matrix source weights: model position, raw uv, normal, normalized normal
def tl_program(lights=4):
    a = ['!!ARBvp1.0',
         '#var float4 envColor : : c[0] : 1 : 1',
         '#var float4 tlLast : : c[54] : 1 : 1',
         'MOV R0, v[OPOS];',
         'DP4 R4.x, R0, c[3];', 'DP4 R4.y, R0, c[4];', 'DP4 R4.z, R0, c[5];', 'DP4 R4.w, R0, c[6];',
         # Depth clip off on the GE (c[20].w = 1): nothing is clipped at the near or far plane, the depth
         # is clamped instead (Gran Turismo's sky lies entirely in front of the near plane).
         'MAX R16.z, R4.z, -R4.w;', 'MIN R16.z, R16.z, R4.w;', 'ADD R16.z, R16.z, -R4.z;', 'MAD R4.z, R16.z, c[20].w, R4.z;',
         'MOV o[HPOS], R4;',
         'DP4 R1.x, R0, c[7];', 'DP4 R1.y, R0, c[8];', 'DP4 R1.z, R0, c[9];',
         'MOV R3, v[NRML];',
         'DP3 R2.x, R3, c[7];', 'DP3 R2.y, R3, c[8];', 'DP3 R2.z, R3, c[9];',
         'DP3 R3.w, R2, R2;', 'MAX R3.w, R3.w, c[21].w;', 'RSQ R3.w, R3.w;', 'MUL R2.xyz, R2, R3.w;',
         # Fog: clamp((viewZ + end) * slope, 0, 1), or 1 when off.
         'DP4 R4.x, R0, c[10];', 'ADD R4.x, R4.x, c[11].x;', 'MUL R4.x, R4.x, c[11].y;',
         'MAX R4.x, R4.x, c[21].x;', 'MIN R4.x, R4.x, c[21].y;', 'ADD R4.x, R4.x, -c[21].y;', 'MUL R4.x, R4.x, c[11].z;',
         'ADD R4.x, R4.x, c[21].y;', 'MOV o[TEX1], R4;',
         # UV: scale and offset (written after the lights: environment mapping uses them).
         'MOV R5, v[TEX0];', 'MUL R5.xy, R5, c[12];', 'ADD R5.xy, R5, c[12].zwzw;', 'MOV R5.z, c[21].x;', 'MOV R5.w, c[21].y;',
         # Texture matrix (c[50].y = 1): (s, t, 0, q) = the matrix times the source c[54] picks (q
         # divides s and t in the fragment program, TXP). Applied at the end, in R18 until then.
         'MUL R17.xyz, v[OPOS], c[54].x;',
         'MOV R18, v[TEX0];', 'MOV R18.z, c[21].x;', 'MAD R17.xyz, R18, c[54].y, R17;',
         'MAD R17.xyz, v[NRML], c[54].z, R17;',
         'DP3 R18.w, v[NRML], v[NRML];', 'MAX R18.w, R18.w, c[21].w;', 'RSQ R18.w, R18.w;', 'MUL R18.xyz, v[NRML], R18.w;',
         'MAD R17.xyz, R18, c[54].w, R17;', 'MOV R17.w, c[21].y;',
         'DP4 R18.x, R17, c[51];', 'DP4 R18.y, R17, c[52];', 'MOV R18.z, c[21].x;', 'DP4 R18.w, R17, c[53];',
         ] + (['MOV R16, c[21].x;'] if lights == 0 else []) + [
         # Material colors: the vertex color where the GE says so.
         'MOV R6, v[COL0];',
         'ADD R7, R6, -c[16];', 'MUL R7, R7, c[15].x;', 'ADD R7, R7, c[16];',
         'ADD R8, R6, -c[17];', 'MUL R8, R8, c[15].y;', 'ADD R8, R8, c[17];',
         'ADD R9, R6, -c[18];', 'MUL R9, R9, c[15].z;', 'ADD R9, R9, c[18];',
         'MUL R10, R7, c[14];', 'ADD R10, R10, c[13];',
         'MOV R11, c[21].x;']
    for i in range(lights):
        b = 22 + 6 * i
        a += [
            # L: normalized vector to a point light, or the (normalized) direction.
            'ADD R12.xyz, c[%d], -R1;' % b, 'DP3 R12.w, R12, R12;', 'MAX R13.w, R12.w, c[21].w;', 'RSQ R13.w, R13.w;',
            'MUL R12.xyz, R12, R13.w;', 'MUL R13.x, R12.w, R13.w;',
            'ADD R14.xyz, c[%d], -R12;' % b, 'MAD R12.xyz, R14, c[%d].w, R12;' % b,
            # Attenuation 1 / (a0 + a1 d + a2 d^2), clamped to 0..1 (directional: 1, 0, 0).
            'MUL R14.w, R12.w, c[%d].z;' % (b + 1), 'MAD R14.w, R13.x, c[%d].y, R14.w;' % (b + 1),
            'ADD R14.w, R14.w, c[%d].x;' % (b + 1), 'RCP R14.w, R14.w;', 'MAX R14.w, R14.w, c[21].x;', 'MIN R14.w, R14.w, c[21].y;',
            # Spot: (s >= cutoff) * max(s, 0)^exp, or 1 when not a spot light.
            # (LIT computes y^w as 2^(w log2 y): y = 0 with w = 0 gives NaN on the RSX, so y >= tiny.)
            'DP3 R15.y, R12, c[%d];' % (b + 2), 'SGE R3.x, R15.y, c[%d].w;' % (b + 2), 'MAX R15.y, R15.y, c[21].w;', 'MOV R15.x, c[21].y;',
            'MOV R15.w, c[%d].w;' % (b + 3), 'LIT R15, R15;', 'MUL R15.z, R15.z, R3.x;',
            'ADD R15.z, R15.z, -c[21].y;', 'MUL R15.z, R15.z, c[%d].w;' % (b + 5), 'ADD R15.z, R15.z, c[21].y;',
            'MUL R14.w, R14.w, R15.z;',
            # Ambient.
            'MUL R13, R7, c[%d];' % (b + 3), 'MAD R10.xyz, R13, R14.w, R10;',
            # Half vector; LIT gives max(N.L, 0) and the specular power (0 where N.L <= 0).
            'ADD R13.xyz, R12, c[20];', 'DP3 R13.w, R13, R13;', 'MAX R13.w, R13.w, c[21].w;', 'RSQ R13.w, R13.w;',
            'MUL R13.xyz, R13, R13.w;',
            # N.L and N.H for environment mapping (R13 is reused below).
            'DP3 R4.z, R12, R2;', 'DP3 R4.w, R13, R2;',
            # (N.L + tiny: the GE keeps specular at N.L == 0, LIT needs it above 0.)
            'ADD R15.x, R4.z, c[21].w;', 'MAX R15.y, R4.w, c[21].w;',
            'MOV R15.w, c[19].x;', 'LIT R15, R15;',
            # Diffuse and specular.
            'MUL R13, R8, c[%d];' % (b + 4), 'MUL R15.y, R15.y, R14.w;', 'MAD R10.xyz, R13, R15.y, R10;',
            'MUL R13, R9, c[%d];' % (b + 5), 'MUL R15.z, R15.z, R14.w;', 'MAD R11.xyz, R13, R15.z, R11;',
            # Environment mapping: N.L or N.H of this light into s and/or t (c[46+i] picks).
            ('MUL R16.xy, R4.z, c[%d].xzzz;' % (46 + i)) if i == 0 else
            ('MAD R16.xy, R4.z, c[%d].xzzz, R16;' % (46 + i)),
            'MAD R16.xy, R4.w, c[%d].ywww, R16;' % (46 + i),
        ]
    a += [
        # Environment mapping (c[50].x = 1): uv = (dot + 1) / 2 instead of the vertex's.
        'MAD R16.xy, R16, c[21].z, c[21].z;', 'ADD R16.xy, R16, -R5;', 'MAD R5.xy, R16, c[50].x, R5;',
        'ADD R18, R18, -R5;', 'MAD R5, R18, c[50].y, R5;',
        'MOV o[TEX0], R5;',
        'MAD R10.xyz, R11, c[19].z, R10;', 'MUL R11.xyz, R11, c[19].y;',
        # Lighting off: the vertex color, no secondary color.
        'ADD R12, R10, -R6;', 'MAD R10, R12, c[11].w, R6;', 'MUL R11.xyz, R11, c[11].w;', 'MOV R11.w, c[21].x;',
        'MAX R10, R10, c[21].x;', 'MIN o[COL0], R10, c[21].y;',
        'MAX R11, R11, c[21].x;', 'MIN o[COL1], R11, c[21].y;',
        'MOV o[TEX2], c[0];', 'MOV o[TEX3], c[1];', 'MOV o[TEX4], c[2];',
        'END']
    return '\n'.join(a) + '\n'

# Smallest program on the T&L vertex layout (startup self-test stage): position and color through.
TL_MIN = """!!ARBvp1.0
#var float4 envColor : : c[0] : 1 : 1
MOV o[HPOS], v[OPOS];
MOV o[COL0], v[COL0];
MOV o[COL1], c[0];
MOV o[TEX0], c[0];
MOV o[TEX1], c[0];
MOV o[TEX2], c[0];
MOV o[TEX3], c[1];
MOV o[TEX4], c[2];
END
"""

TL_POS = """!!ARBvp1.0
#var float4 envColor : : c[0] : 1 : 1
#var float4 tlLast : : c[6] : 1 : 1
MOV R0, v[OPOS];
DP4 R4.x, R0, c[3];
DP4 R4.y, R0, c[4];
DP4 R4.z, R0, c[5];
DP4 R4.w, R0, c[6];
MOV o[HPOS], R4;
MOV o[COL0], v[COL0];
MOV o[COL1], c[0];
MOV o[TEX0], c[0];
MOV o[TEX1], c[0];
MOV o[TEX2], c[0];
MOV o[TEX3], c[1];
MOV o[TEX4], c[2];
END
"""

TL_POS_DIRECT = TL_POS.replace('DP4 R4.x, R0, c[3];\nDP4 R4.y, R0, c[4];\nDP4 R4.z, R0, c[5];\nDP4 R4.w, R0, c[6];\nMOV o[HPOS], R4;',
    'DP4 o[HPOS].x, R0, c[3];\nDP4 o[HPOS].y, R0, c[4];\nDP4 o[HPOS].z, R0, c[5];\nDP4 o[HPOS].w, R0, c[6];')

# Texture functions, in GE order.
MODULATE, DECAL, BLEND, REPLACE, ADD = range(5)

def fragment(tex, func, rgba, doubling, fog, blendmul, clear=False):
    """Body for one variant. R0 ends up as the output color."""
    a = ['!!ARBfp1.0', 'OPTION NV_fragment_program2;']
    if doubling:
        a.append('#const c[0] = 2 2 2 1')
    a.append('MOVR R1, f[COL0];')
    if clear or not tex:
        a.append('MOVR R0, R1;')
    else:
        a.append('TXP R0, f[TEX0], texture[0], 2D;')
        if func == MODULATE:
            a.append('MULR R0.xyz, R0, R1;')
            a.append('MULR R0.w, R0, R1;' if rgba else 'MOVR R0.w, R1;')
        elif func == DECAL:
            if rgba:
                a.append('LRPR R0.xyz, R0.w, R0, R1;')
            a.append('MOVR R0.w, R1;')
        elif func == BLEND:
            a.append('MOVR R2, f[TEX2];')
            a.append('LRPR R2.xyz, R0, R2, R1;')
            a.append('MULR R2.w, R0, R1;' if rgba else 'MOVR R2.w, R1;')
            a.append('MOVR R0, R2;')
        elif func == REPLACE:
            if not rgba:
                a.append('MOVR R0.w, R1;')
        elif func == ADD:
            a.append('ADDR_SAT R0.xyz, R0, R1;')
            a.append('MULR R0.w, R0, R1;' if rgba else 'MOVR R0.w, R1;')
    if not clear:
        # Secondary (specular) color; zero when the GE doesn't use it.
        a.append('MOVR R3, f[COL1];')
        a.append('ADDR_SAT R0.xyz, R0, R3;')
        if doubling:
            a.append('MULR_SAT R0.xyz, R0, c[0];')
        if fog:
            a.append('MOVR R2, f[TEX1];')
            a.append('MOVR R3, f[TEX3];')
            a.append('LRPR R0.xyz, R2.x, R0, R3;')
        if blendmul:
            a.append('MOVR R2, f[TEX4];')
            a.append('MULR R0.xyz, R0, R2;')
    a.append('MOVR o[COLR], R0;')
    a.append('END')
    return '\n'.join(a) + '\n'

def burn():
    """Stress-test load: a texture fetch and ~200 ALU instructions per pixel (sin, cos, rsq, log,
    exp, dot, mad), so the shader units run flat out. Deterministic for the same inputs."""
    a = ['!!ARBfp1.0', 'OPTION NV_fragment_program2;', 'MOVR R1, f[COL0];',
         'TXP R0, f[TEX0], texture[0], 2D;', 'MOVR R2, f[TEX0];']
    for i in range(24):
        a += ['MADR R0, R0, R1, R2;', 'SINR R3.x, R0.x;', 'COSR R3.y, R0.y;', 'RSQR R3.z, |R1.z|;',
              'LG2R R3.w, |R0.w|;', 'EX2R R4.x, R3.x;', 'DP4R R4.y, R3, R0;', 'MADR R0, R3, R1, R0;', 'FRCR R0, R0;']
    a += ['MOVR o[COLR], R0;', 'END']
    return '\n'.join(a) + '\n'

def depal():
    """Palette lookup from a framebuffer (a CLUT texture reading rendered pixels). Texture 0 is the
    framebuffer, texture 1 the palette as a 1-pixel-high row. Per vertex: color0 selects the channel
    holding the index (one-hot), TEX0.z is the palette start, TEX0.w is 1 / palette width, and the
    fog value (TEX1.x) is mask + 1 (a power of two)."""
    return '\n'.join([
        '!!ARBfp1.0', 'OPTION NV_fragment_program2;',
        '#const c[0] = 255 0.5 0 0',
        'MOVR R1, f[COL0];',
        'MOVR R2, f[TEX0];',
        'TEX R0, R2, texture[0], 2D;',
        'DP4R R3.x, R0, R1;',
        'MADR R3.x, R3.x, c[0].x, c[0].y;',
        'FLRR R3.x, R3.x;',
        'MOVR R4, f[TEX1];',
        'RCPR R5.x, R4.x;',
        'MULR R5.y, R3.x, R5.x;',
        'FLRR R5.y, R5.y;',
        'MADR R3.x, -R5.y, R4.x, R3.x;',
        'ADDR R3.x, R3.x, R2.z;',
        'ADDR R3.x, R3.x, c[0].y;',
        'MULR R3.x, R3.x, R2.w;',
        'MOVR R3.y, c[0].y;',
        'TEX R6, R3, texture[1], 2D;',
        'MOVR o[COLR], R6;',
        'END']) + '\n'

def depal16():
    """Palette lookup from a 16-bit framebuffer read as a CLUT32 texture: each 32-bit texel is two
    pixels, the index a byte of one of them (Silent Hill Origins draws distances into a 565 buffer
    and reads them through a palette for its fog). Texture 0 is the framebuffer (each pixel's
    4/5/6-bit channels widened to 8 bits), texture 1 the palette as a 1-pixel-high row.
    Per vertex: TEX0.x is the output x in RSX pixels, TEX0.y the source v (normalized), TEX0.z the
    palette start, TEX0.w 1 / palette width; the fog value (TEX1.x) is mask + 1 (a power of two).
    Constant varyings: TEX2 per channel the divisor from 8 bits down to the format's bits (565: 8,
    4, 8; 256 for an unused alpha), TEX3 per channel its weight in the 16-bit value (565: 1, 32,
    2048), TEX4 = (scale, 1 or 256 for the pixel's low or high byte, source x offset in RSX pixels
    + 0.5, 1 / source width). The integers are rounded first: varyings may not arrive exact, and a
    weight of 2048 off by a tiny fraction moves the 16-bit value by more than one. Output pixel x
    (scale s) reads source pixel x + s * floor(x / s) + offset: the texel's pixel, at the same
    position inside it."""
    return '\n'.join([
        '!!ARBfp1.0', 'OPTION NV_fragment_program2;',
        '#const c[0] = 255 0.5 0.0009765625 0',
        'MOVR R2, f[TEX0];',
        'MOVR R7, f[TEX4];',
        'ADDR R7.xy, R7, c[0].y;',
        'FLRR R7.xy, R7;',
        # Source x: floor(x) + s * floor(x / s) + offset (half a step added before each floor).
        'FLRR R8.x, R2.x;',
        'RCPR R8.y, R7.x;',
        'MULR R13.x, R8.y, c[0].y;',
        'MADR R8.z, R8.x, R8.y, R13.x;',
        'FLRR R8.z, R8.z;',
        'MADR R8.w, R8.z, R7.x, R8.x;',
        'ADDR R8.w, R8.w, R7.z;',
        'MOVR R9, R2;',
        'MULR R9.x, R8.w, R7.w;',
        'TEX R0, R9, texture[0], 2D;',
        # 8-bit channels, then the format's bits (divided by powers of two: exact).
        'MADR R0, R0, c[0].x, c[0].y;',
        'FLRR R0, R0;',
        'MOVR R10, f[TEX2];',
        'ADDR R10, R10, c[0].y;',
        'FLRR R10, R10;',
        'RCPR R12.x, R10.x;',
        'RCPR R12.y, R10.y;',
        'RCPR R12.z, R10.z;',
        'RCPR R12.w, R10.w;',
        'MULR R0, R0, R12;',
        'ADDR R0, R0, c[0].z;',
        'FLRR R0, R0;',
        # The 16-bit value, then its low or high byte.
        'MOVR R11, f[TEX3];',
        'ADDR R11, R11, c[0].y;',
        'FLRR R11, R11;',
        'DP4R R3.x, R0, R11;',
        'RCPR R13.y, R7.y;',
        'MULR R13.w, R13.y, c[0].y;',
        'MADR R3.x, R3.x, R13.y, R13.w;',
        'FLRR R3.x, R3.x;',
        # & mask (mod mask + 1), + start, palette fetch.
        'MOVR R4, f[TEX1];',
        'ADDR R4.x, R4.x, c[0].y;',
        'FLRR R4.x, R4.x;',
        'RCPR R5.x, R4.x;',
        'MULR R13.z, R5.x, c[0].y;',
        'MADR R5.y, R3.x, R5.x, R13.z;',
        'FLRR R5.y, R5.y;',
        'MADR R3.x, -R5.y, R4.x, R3.x;',
        'ADDR R3.x, R3.x, R2.z;',
        'ADDR R3.x, R3.x, c[0].y;',
        'MULR R3.x, R3.x, R2.w;',
        'MOVR R3.y, c[0].y;',
        'TEX R6, R3, texture[1], 2D;',
        'MOVR o[COLR], R6;',
        'END']) + '\n'

def depalx():
    """Palette lookup from a framebuffer, texel = pixel, any shift (3rd Birthday's glow: its 8888
    screen read as CLUT32 with shift 12, then a 4444 buffer read as CLUT16 with shift 0). Texture 0
    is the framebuffer (each pixel's channels widened to 8 bits), texture 1 the palette as a
    1-pixel-high row. Per vertex: TEX0.xy the source uv, TEX0.z the palette start, TEX0.w 1 / palette
    width; the fog value (TEX1.x) is mask + 1 (a power of two). Constant varyings: TEX2 per channel
    the divisor from 8 bits down to the format's bits (8888: 1), TEX3 per channel its weight in the
    value the index is taken from (16-bit: its place in the pixel; 8888: 1 for the byte holding the
    index, 0 for the others), TEX4.x = 2^shift (within that value). Integers are rounded first:
    varyings may not arrive exact. index = floor(value / 2^shift) mod (mask + 1) + start."""
    return '\n'.join([
        '!!ARBfp1.0', 'OPTION NV_fragment_program2;',
        '#const c[0] = 255 0.5 0.0009765625 0',
        'MOVR R2, f[TEX0];',
        'TEX R0, R2, texture[0], 2D;',
        # 8-bit channels, then the format's bits (divided by powers of two: exact).
        'MADR R0, R0, c[0].x, c[0].y;',
        'FLRR R0, R0;',
        'MOVR R10, f[TEX2];',
        'ADDR R10, R10, c[0].y;',
        'FLRR R10, R10;',
        'RCPR R12.x, R10.x;',
        'RCPR R12.y, R10.y;',
        'RCPR R12.z, R10.z;',
        'RCPR R12.w, R10.w;',
        'MULR R0, R0, R12;',
        'ADDR R0, R0, c[0].z;',
        'FLRR R0, R0;',
        # The value, then >> shift.
        'MOVR R11, f[TEX3];',
        'ADDR R11, R11, c[0].y;',
        'FLRR R11, R11;',
        'DP4R R3.x, R0, R11;',
        'MOVR R7, f[TEX4];',
        'ADDR R7.x, R7.x, c[0].y;',
        'FLRR R7.x, R7.x;',
        'RCPR R13.y, R7.x;',
        'MULR R13.w, R13.y, c[0].y;',
        'MADR R3.x, R3.x, R13.y, R13.w;',
        'FLRR R3.x, R3.x;',
        # & mask (mod mask + 1), + start, palette fetch.
        'MOVR R4, f[TEX1];',
        'ADDR R4.x, R4.x, c[0].y;',
        'FLRR R4.x, R4.x;',
        'RCPR R5.x, R4.x;',
        'MULR R13.z, R5.x, c[0].y;',
        'MADR R5.y, R3.x, R5.x, R13.z;',
        'FLRR R5.y, R5.y;',
        'MADR R3.x, -R5.y, R4.x, R3.x;',
        'ADDR R3.x, R3.x, R2.z;',
        'ADDR R3.x, R3.x, c[0].y;',
        'MULR R3.x, R3.x, R2.w;',
        'MOVR R3.y, c[0].y;',
        'TEX R6, R3, texture[1], 2D;',
        'MOVR o[COLR], R6;',
        'END']) + '\n'

def fixsub(rgba, doubling, reverse):
    """PSP-exact blend for a draw whose blend factors are both fixed colours and whose equation
    subtracts (Silent Hill Origins' bright-pass): the RSX blender rounds the two terms its own way,
    one colour channel at a time, and the tiny results (0-4) turned into coloured specks that the
    later glow passes multiply. Here the whole pixel is computed as the GE does, with blending off.
    Texture 0 is the copy of the target the draw samples (rttCopy): the source at TEX0, the
    destination pixel at (TEX0.z, TEX1.x). TEX4 = 2 * fixA + 1, TEX3 = 2 * fixB + 1 (per channel,
    0-255 units). Modulate: ((prim + 1) * tex [* 2]) >> 8; each term floor((2c + 1)(2f + 1) / 1024)."""
    a = ['!!ARBfp1.0', 'OPTION NV_fragment_program2;',
         '#const c[0] = 255 0.5 1 0',
         '#const c[1] = 0.0009765625 0.00390625 2 0.003921568627',
         'MOVR R1, f[COL0];',
         'TXP R0, f[TEX0], texture[0], 2D;',
         'MOVR R5, f[TEX0];',
         'MOVR R6, f[TEX1];',
         'MOVR R7, c[0].w;',
         'MOVR R7.x, R5.z;',
         'MOVR R7.y, R6.x;',
         'TEX R8, R7, texture[0], 2D;',
         # To 8-bit values.
         'MADR R0, R0, c[0].x, c[0].y;', 'FLRR R0, R0;',
         'MADR R1, R1, c[0].x, c[0].y;', 'FLRR R1, R1;',
         'MADR R8, R8, c[0].x, c[0].y;', 'FLRR R8, R8;',
         # Modulate.
         'ADDR R2, R1, c[0].z;',
         'MULR R3, R2, R0;']
    if doubling:
        a.append('MULR R3.xyz, R3, c[1].z;')
    a += ['MULR R3, R3, c[1].y;', 'FLRR R3, R3;', 'MINR R3, R3, c[0].x;']
    if not rgba:
        a.append('MOVR R3.w, R1.w;')
    # Source term.
    a += ['MADR R4.xyz, R3, c[1].z, c[0].z;',
          'MOVR R9, f[TEX4];',
          'MULR R4.xyz, R4, R9;',
          'MULR R4.xyz, R4, c[1].x;', 'FLRR R4.xyz, R4;',
          # Destination term.
          'MADR R10.xyz, R8, c[1].z, c[0].z;',
          'MOVR R11, f[TEX3];',
          'MULR R10.xyz, R10, R11;',
          'MULR R10.xyz, R10, c[1].x;', 'FLRR R10.xyz, R10;']
    a.append('ADDR R12.xyz, R10, -R4;' if reverse else 'ADDR R12.xyz, R4, -R10;')
    a += ['MAXR R12.xyz, R12, c[0].w;', 'MINR R12.xyz, R12, c[0].x;',
          'MOVR R12.w, R3.w;',
          'MULR R12, R12, c[1].w;',
          'MOVR o[COLR], R12;', 'END']
    return '\n'.join(a) + '\n'

def variants():
    """Yield (index, name, source). Index layout must match RSXCore::FragmentIndex()."""
    yield 0, 'clear', fragment(False, 0, False, False, False, False, clear=True)
    for tex in (0, 1):
        for func in range(5):
            if not tex and func:
                continue
            for rgba in (0, 1):
                if not tex and rgba:
                    continue
                for doubling in (0, 1):
                    for fog in (0, 1):
                        for blendmul in (0, 1):
                            index = 1 + (((tex * 5 + func) * 2 + rgba) * 2 + doubling) * 4 + fog * 2 + blendmul
                            name = 'tex%d_f%d_rgba%d_dbl%d_fog%d_mul%d' % (tex, func, rgba, doubling, fog, blendmul)
                            yield index, name, fragment(tex, func, rgba, doubling, fog, blendmul)
    yield 161, 'burn', burn()
    yield 162, 'depal', depal()
    for rgba in (0, 1):
        for doubling in (0, 1):
            for reverse in (0, 1):
                yield 163 + rgba * 4 + doubling * 2 + reverse, 'fixsub_rgba%d_dbl%d_rev%d' % (rgba, doubling, reverse), fixsub(rgba, doubling, reverse)
    yield 171, 'depal16', depal16()
    yield 172, 'fxaa', fxaa()
    yield 173, 'fxaa_light', fxaa(True)
    yield 174, 'depalx', depalx()

def fxaa(light=False):
    """Edge smoothing of the finished picture (FXAA, the classic console version): luma of the
    pixel and its four diagonal neighbours (bilinear fetches half a texel away), the edge direction
    from them, then two and four samples along that direction; the four-sample blend unless its
    luma falls outside the neighbourhood's range (then the two-sample one). Texture 0 is the
    picture (bilinear, clamped); TEX0.xy the pixel's texture coordinate; TEX4.xy = 1 / texture size
    (constant varying). light: Edge smoothing "Light" - the search along the edge at most 2 texels
    (not 8) and only half of the smoothed colour mixed into the pixel's own, so small sharp details
    such as text stay crisper."""
    span = '2' if light else '8'
    return '\n'.join([
        '!!ARBfp1.0', 'OPTION NV_fragment_program2;',
        '#const c[0] = 0.299 0.587 0.114 0',
        '#const c[1] = 0.03125 0.0078125 ' + span + ' 0.5',
        '#const c[2] = -0.16666667 0.16666667 0.25 0',
        '#const c[3] = 0.5 -0.5 -0.5 0.5',
        'MOVR R10, f[TEX0];',
        'MOVR R11, f[TEX4];',
        # The four diagonal neighbours (NW, NE, SW, SE) and the pixel itself.
        'MADR R1.xy, R11, -c[1].w, R10;',
        'MADR R2.xy, R11, c[3], R10;',
        'MADR R3.xy, R11, c[3].zwzw, R10;',
        'MADR R4.xy, R11, c[1].w, R10;',
        'TEX R1, R1, texture[0], 2D;',
        'TEX R2, R2, texture[0], 2D;',
        'TEX R3, R3, texture[0], 2D;',
        'TEX R4, R4, texture[0], 2D;',
        'TEX R9, R10, texture[0], 2D;',
        'DP3R R6.x, R1, c[0];',
        'DP3R R6.y, R2, c[0];',
        'DP3R R6.z, R3, c[0];',
        'DP3R R6.w, R4, c[0];',
        'DP3R R7.x, R9, c[0];',
        # Luma range around the pixel.
        'MINR R7.y, R6.x, R6.y;',
        'MINR R7.w, R6.z, R6.w;',
        'MINR R7.y, R7.y, R7.w;',
        'MINR R7.y, R7.y, R7.x;',
        'MAXR R7.z, R6.x, R6.y;',
        'MAXR R7.w, R6.z, R6.w;',
        'MAXR R7.z, R7.z, R7.w;',
        'MAXR R7.z, R7.z, R7.x;',
        # Edge direction: x = (SW + SE) - (NW + NE), y = (NW + SW) - (NE + SE).
        'ADDR R8.x, R6.z, R6.w;',
        'ADDR R8.y, R6.x, R6.y;',
        'ADDR R12.x, R8.x, -R8.y;',
        'ADDR R8.z, R6.x, R6.z;',
        'ADDR R8.w, R6.y, R6.w;',
        'ADDR R12.y, R8.z, -R8.w;',
        # Scaled so its shorter component is ~1, at most 8 texels, then in texture units.
        'ADDR R13.x, R8.x, R8.y;',
        'MULR R13.x, R13.x, c[1].x;',
        'MAXR R13.x, R13.x, c[1].y;',
        'MINR R13.y, |R12.x|, |R12.y|;',
        'ADDR R13.y, R13.y, R13.x;',
        'RCPR R13.y, R13.y;',
        'MULR R12.xy, R12, R13.y;',
        'MINR R12.xy, R12, c[1].z;',
        'MAXR R12.xy, R12, -c[1].z;',
        'MULR R12.xy, R12, R11;',
        # Two samples at -1/6 and +1/6 of the direction (A), two more at -1/2 and +1/2 (B).
        'MADR R1.xy, R12, c[2].x, R10;',
        'MADR R2.xy, R12, c[2].y, R10;',
        'MADR R3.xy, R12, -c[1].w, R10;',
        'MADR R4.xy, R12, c[1].w, R10;',
        'TEX R1, R1, texture[0], 2D;',
        'TEX R2, R2, texture[0], 2D;',
        'TEX R3, R3, texture[0], 2D;',
        'TEX R4, R4, texture[0], 2D;',
        'ADDR R1, R1, R2;',
        'MULR R1, R1, c[1].w;',
        'ADDR R3, R3, R4;',
        'MULR R3, R3, c[2].z;',
        'MADR R3, R1, c[1].w, R3;',
        # B unless its luma is outside the range (then A).
        'DP3R R13.z, R3, c[0];',
        'SLTR R13.w, R13.z, R7.y;',
        'SGTR R14.x, R13.z, R7.z;',
        'ADDR R13.w, R13.w, R14.x;',
        'ADDR R5, R1, -R3;',
        'MADR R5, R5, R13.w, R3;',
    ] + ([
        # Light: halfway between the pixel's own colour and the smoothed one.
        'ADDR R6, R5, -R9;',
        'MADR R5, R6, c[1].w, R9;',
    ] if light else []) + [
        'MOVR R5.w, R9.w;',
        'MOVR o[COLR], R5;',
        'END']) + '\n'

def assemble(cgcomp, kind, source):
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, 'in.asm')
        out = os.path.join(d, 'out.bin')
        open(src, 'w').write(source)
        r = subprocess.run([cgcomp, '-' + kind, '-a', src, out], capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            raise SystemExit('cgcomp failed for:\n%s\n%s%s' % (source, r.stdout, r.stderr))
        return open(out, 'rb').read()

def emit(name, data):
    lines = ['alignas(16) static const unsigned char %s[%d] = {' % (name, len(data))]
    for i in range(0, len(data), 16):
        lines.append('\t' + ', '.join('0x%02x' % b for b in data[i:i + 16]) + ',')
    lines.append('};')
    return '\n'.join(lines)

def main():
    cgcomp, out = sys.argv[1], sys.argv[2]
    parts = ['// Generated by PS3/rsx/make-shaders.py with cgcomp -a. Do not edit.', '#pragma once', '']
    parts.append(emit('rsx_vp_main', assemble(cgcomp, 'v', VP)))
    parts.append(emit('rsx_vp_tl', assemble(cgcomp, 'v', tl_program())))
    parts.append(emit('rsx_vp_tlmin', assemble(cgcomp, 'v', TL_MIN)))
    # Self-test stages (RSXTestTL): position through a temp, position straight to the output,
    # T&L without the light loop.
    parts.append(emit('rsx_vp_tlpos', assemble(cgcomp, 'v', TL_POS)))
    parts.append(emit('rsx_vp_tlposdirect', assemble(cgcomp, 'v', TL_POS_DIRECT)))
    parts.append(emit('rsx_vp_tlnolights', assemble(cgcomp, 'v', tl_program(0))))
    table = {}
    for index, name, source in variants():
        sym = 'rsx_fp_' + name
        parts.append('// %d: %s' % (index, name))
        parts.append(emit(sym, assemble(cgcomp, 'f', source)))
        table[index] = sym
    count = max(table) + 1
    parts.append('static const int RSX_FRAGMENT_PROGRAM_COUNT = %d;' % count)
    parts.append('static const int RSX_FRAGMENT_BURN = 161;')
    parts.append('static const int RSX_FRAGMENT_DEPAL = 162;')
    parts.append('static const int RSX_FRAGMENT_FIXSUB = 163;')
    parts.append('static const int RSX_FRAGMENT_DEPAL16 = 171;')
    parts.append('static const int RSX_FRAGMENT_FXAA = 172;')
    parts.append('static const int RSX_FRAGMENT_FXAA_LIGHT = 173;')
    parts.append('static const int RSX_FRAGMENT_DEPALX = 174;')
    parts.append('static const unsigned char *const rsx_fp_table[%d] = {' % count)
    for i in range(count):
        parts.append('\t%s,' % table.get(i, 'nullptr'))
    parts.append('};')
    open(out, 'w').write('\n'.join(parts) + '\n')
    print('%d fragment programs written to %s' % (len(table), out))

if __name__ == '__main__':
    main()
