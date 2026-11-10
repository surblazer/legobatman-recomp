#!/usr/bin/env python3
"""LEGO Batman port: copy of rashevskyv/cvhd-recomp tools/xenos2glsl.py (BSD-3 like the SDK) with additions marked
"LEGO Batman port": the address register a0 (maxas/maxasf, c[N+a0] operands: skinning) and setTexLOD.

Xenos shader microcode -> GLSL for uam (deko3d), from the ReXGlue SDK's ucode disassembly
(the `shader_<hash>.ucode.vert|frag` files its --dump_shaders writes; the trace backend writes
them too). The semantics follow the SDK's shader interpreter (BSD-3); the text grammar follows
its translator_disasm.cpp. Only what the game's shaders use is implemented; anything else stops
with a clear message so the gap is visible (docs/native-renderer-plan.md, M2).

Interface of the generated GLSL (the deko3d backend relies on it):
  vertex:   layout(location = a) in vec4 in_a      one per vfetch, in instruction order; the
            sidecar JSON lists (slot, offset, format, stride, signed, integer) per attribute;
            binding 0 = uniform Consts { vec4 c[256]; } (the ALU constants c0..c255);
            binding 1 = uniform Sys (ndc_scale, ndc_offset, alpha test, bool constants);
            layout(location = k) out vec4 o_k      the interpolators the shader exports.
  fragment: layout(location = k) in vec4 i_k       the interpolators it reads (r_k at start);
            layout(binding = n) uniform sampler2D tf_n (1D/3D/Cube by the fetch's dimension);
            layout(location = k) out vec4 oC_k; the alpha test is applied from Sys.

    python3 tools/xenos2glsl.py in.ucode.vert [-o out.vert] [--json out.json] [--check]
    python3 tools/xenos2glsl.py --all <dump dir> --out <dir> [--check]   (every dump; --check runs uam)
"""
import argparse
import json
import os
import re
import subprocess
import sys

VECTOR_OPS = {'add', 'mul', 'max', 'min', 'seq', 'sgt', 'sge', 'sne', 'frc', 'trunc', 'floor', 'mad',
              'cndeq', 'cndge', 'cndgt', 'dp4', 'dp3', 'dp2add', 'max4', 'setp_eq_push', 'setp_ne_push',
              'setp_gt_push', 'setp_ge_push', 'kill_eq', 'kill_gt', 'kill_ge', 'kill_ne', 'dst', 'cube', 'maxa'}
SCALAR_OPS = {'adds', 'adds_prev', 'muls', 'muls_prev', 'muls_prev2', 'maxs', 'mins', 'seqs', 'sgts', 'sges',
              'snes', 'frcs', 'truncs', 'floors', 'exp', 'logc', 'log', 'rcpc', 'rcpf', 'rcp', 'rsqc', 'rsqf',
              'rsq', 'maxas', 'maxasf', 'subs', 'subs_prev', 'setp_eq', 'setp_ne', 'setp_gt', 'setp_ge',
              'setp_inv', 'setp_pop', 'setp_clr', 'setp_rstr', 'kills_eq', 'kills_gt', 'kills_ge', 'kills_ne',
              'kills_one', 'sqrt', 'mulsc', 'addsc', 'subsc', 'sin', 'cos', 'retain_prev'}
TWO_COMPONENT_SCALAR = {'adds', 'muls', 'maxs', 'mins', 'subs', 'maxas', 'maxasf'}  # one operand, two components
TWO_OPERAND_SCALAR = {'mulsc', 'addsc', 'subsc'}  # a constant and a register
REPLICATED = {'dp4', 'dp3', 'dp2add', 'max4', 'setp_eq_push', 'setp_ne_push', 'setp_gt_push', 'setp_ge_push',
              'kill_eq', 'kill_gt', 'kill_ge', 'kill_ne'}


# LEGO Batman port: ALU ops whose result may be Inf/NaN from finite operands (FLT_MAX from the clamping variants
# overflows when multiplied), and ops whose result is finite whatever the operands (comparisons, kills).
NONFINITE_OPS = {'rcp', 'rcpc', 'rcpf', 'rsq', 'rsqc', 'rsqf', 'log', 'logc', 'exp', 'sqrt', 'setp_clr'}
# XENOS_FINITE_SPECIAL=1 (perf, 2026-11-10): rcp/rsq/log/exp return +-2^60 where Xenos returns +-Inf (rcpf/rsqf
# already flush Inf to 0). 0 * 2^60 = 0 is what Xenos gives for 0 * Inf, so the multiplies downstream need no exact
# helper; non-zero * 2^60 stays huge and saturates like Inf. Differs only where Xenos already made Inf/NaN garbage
# (Inf - Inf, three huge factors). Then only sqrt (of a negative) and setp_clr (FLT_MAX) are taint sources.
FINITE_SPECIAL = os.environ.get('XENOS_FINITE_SPECIAL') == '1'
# LEGO Batman port: vertex shaders from the disc (tools/shader-extract). D3D rewrites their vfetch instructions when
# the game binds them (order, format, offset, stride, destination swizzle), so the translation keeps only what the
# rewrite preserves: each fetch's destination register and written components. Each fetch writes them from
# in_<index> through the per-component source the backend passes in uniform VFetch (binding 2): 0..3 = x..w,
# 4 = 0.0, 5 = 1.0, from the bound shader's own vfetch. The attribute layout comes from the bound shader too; the
# sidecar JSON lists (reg, mask) per in_<index> (tools/scripts/vs_generic_key.py, the .gmap file).
GENERIC_FETCH = os.environ.get('XENOS_GENERIC_FETCH') == '1'
MAX_GENERIC_FETCHES = 32
if FINITE_SPECIAL:
    NONFINITE_OPS = {'sqrt', 'setp_clr'}
FINITE_OPS = {'seq', 'sgt', 'sge', 'sne', 'seqs', 'sgts', 'sges', 'snes', 'kill_eq', 'kill_gt', 'kill_ge', 'kill_ne',
              'kills_eq', 'kills_gt', 'kills_ge', 'kills_ne', 'kills_one', 'setp_eq', 'setp_ne', 'setp_gt', 'setp_ge'}


class Translator:
    def __init__(self, text, stage, name):
        self.stage = stage  # 'vert' or 'frag'
        self.name = name
        self.lines = text.splitlines()
        self.body = []        # GLSL statements of the current block
        self.blocks = []      # (cf_index, kind, statements, extra) in order
        self.attrs = []       # vertex attributes: dict(slot, offset, format, stride, signed, integer)
        self.textures = {}    # slot -> dimension ('2D', '1D', '3D', 'Cube')
        self.max_reg = -1
        self.reads_before_write = set()
        self.written = {}     # register index -> components written so far (per component: `r0.z = ...`
                              # before `tfetch2D r0, r0.xy` must not hide the interpolator in r0.xy)
        self.exports = set()  # 'oPos', 'o0'.., 'oC0'.., 'oPts', 'oDepth'
        self.has_jump = False
        self.has_loop = False
        self.has_kill = False
        self.uses_bools = False
        self.stride_words = 0
        self.pending_serialize = False
        self.indent = '  '
        self.uses_a0 = False     # LEGO Batman port: address register (maxas / c[N+a0])
        self.tex_lod = None      # LEGO Batman port: setTexLOD source expression for UseRegisterLOD fetches
        # LEGO Batman port: which register components may hold Inf/NaN (see solve_taint). None = unknown: every
        # multiply uses the exact Xenos helpers (first pass, or XENOS_EXACT_MULS=1).
        self.tainted = None
        self.ps_tainted = True
        self.edges = []          # (dest components, source operands, op, saturate, scalar part) of every ALU part
        self._last_src = None
        self.op_taint = []

    # ----- helpers ---------------------------------------------------------------------------
    def err(self, msg, line=''):
        raise SystemExit('%s: %s%s' % (self.name, msg, ('\n  ' + line) if line else ''))

    def note_reg(self, idx):
        self.max_reg = max(self.max_reg, idx)

    def reg_read(self, idx, comps):
        self.note_reg(idx)
        if not set(comps) <= self.written.get(idx, set()):
            self.reads_before_write.add(idx)

    def reg_write(self, idx, comps):
        self.note_reg(idx)
        self.written.setdefault(idx, set()).update(comps)

    def parse_result(self, tok):
        """'r0.__z_' -> (target, [per-component source char or None]). Target: 'r3', 'o1', 'oPos', 'oC0'."""
        # LEGO Batman port: an export to a register this stage does not have prints with no register ('.x___'):
        # the hardware drops it. Target '' = computed, written nowhere (the scalar part still sets ps).
        if not tok or tok.startswith('.'):
            return '', [None] * 4
        m = re.match(r'^(r|o|oPos|oPts|oC|oDepth|eA|eM)(\d*)(?:\.([xyzw01_]+))?$', tok)
        if not m:
            self.err('bad result operand %r' % tok)
        kind, idx, mask = m.groups()
        target = kind + idx
        if mask is None:
            comps = ['x', 'y', 'z', 'w']
        elif mask == '_':
            comps = [None] * 4
        else:
            if len(mask) != 4:
                self.err('bad result mask %r' % tok)
            comps = [None if ch == '_' else ch for ch in mask]
        if kind == 'r':
            self.reg_write(int(idx), ['xyzw'[i] for i, ch in enumerate(comps) if ch is not None])
        elif kind in ('eA', 'eM'):
            self.err('memexport is not supported')
        else:
            self.exports.add(target)
        return target, comps

    def parse_source(self, tok, count):
        """Source operand -> GLSL expression with exactly `count` components (1, 2 or 4)."""
        m = re.match(r'^(-?)(r|c)(_abs)?(?:\[(\d+)\]|(\d+)|\[(\d+)\+a[L0]\]|\[a[L0]\])(?:\.([xyzw]+))?$', tok)
        if not m:
            self.err('bad source operand %r' % tok)
        neg, kind, is_abs, idx_abs, idx_plain, idx_rel, swz = m.groups()
        relative = tok.find('aL') >= 0  # gh: c[N+aL] inside a loop
        rel_a0 = tok.find('a0') >= 0  # LEGO Batman port: c[N+a0], the address register (skinning bone matrices)
        idx = int(idx_abs if idx_abs is not None else idx_plain if idx_plain is not None else idx_rel or 0)
        if (relative or rel_a0) and kind != 'c':
            self.err('aL/a0 on a register is not supported')
        self._last_src = ('c',)
        if swz is None:
            swz = 'xyzw'
        if len(swz) == 1:
            swz = swz * 4  # the disassembler prints one letter for a scalar operand: that component
        elif len(swz) == 2:
            swz = swz  # two-component scalar operand
        if kind == 'r':
            self.reg_read(idx, swz[:count] if count in (1, 2) else swz[:4])
            self._last_src = ('r', idx, frozenset(swz[:count] if count in (1, 2) else swz[:4]))
            base = 'r%d' % idx
        elif relative:
            base = 'c[%d + aL]' % idx
        elif rel_a0:
            self.uses_a0 = True
            base = 'c[clamp(%d + a0, 0, 255)]' % idx
        else:
            base = 'c[%d]' % idx
        if count == 1:
            expr = '%s.%s' % (base, swz[0])
        elif count == 2:
            expr = '%s.%s' % (base, swz[:2])
        else:
            expr = '%s.%s' % (base, swz[:4]) if swz != 'xyzw' else base
        if is_abs:
            expr = 'abs(%s)' % expr
        if neg:
            expr = '(-%s)' % expr
        return expr

    def write_masked(self, target, comps, value_var, is_export):
        """Assign value_var (vec4) into target by the result mask."""
        out = []
        if not target:
            return out
        if comps == ['x', 'y', 'z', 'w']:
            out.append('%s = %s;' % (target, value_var))
            return out
        for i, ch in enumerate(comps):
            dst = 'xyzw'[i]
            if ch is None:
                continue  # exports start at zero: an unwritten component stays 0
            if ch == '0':
                out.append('%s.%s = 0.0;' % (target, dst))
            elif ch == '1':
                out.append('%s.%s = 1.0;' % (target, dst))
            else:
                out.append('%s.%s = %s.%s;' % (target, dst, value_var, ch))
        return out

    # ----- instructions ----------------------------------------------------------------------
    def emit_fetch_vertex(self, op, rest):
        # vfetch_full r1.xyz1, r0.x, vf95, DataFormat=..., Stride=7, Signed=true, NumFormat=integer, PrefetchCount=7
        # vfetch_mini r0, Offset=3, DataFormat=..., Signed=true, NumFormat=integer
        parts = [p.strip() for p in rest.split(',')]
        kv = {}
        slot = None
        pos = 1
        if op == 'vfetch_full':
            self.parse_source(parts[1], 1)  # the index register (r0.x): the host supplies vertex ids
        target, comps = self.parse_result(parts[0])
        if op == 'vfetch_full':
            if not parts[2].startswith('vf'):
                self.err('vfetch_full without a fetch constant', rest)
            slot = int(parts[2][2:])
            pos = 3
        for p in parts[pos:]:
            if '=' in p:
                k, v = p.split('=', 1)
                kv[k] = v
            else:
                self.err('unexpected vfetch attribute %r' % p, rest)
        if op == 'vfetch_full':
            self.cur_slot = slot
            self.stride_words = int(kv.get('Stride', '0'))
        attr = {
            'index': len(self.attrs),
            'slot': self.cur_slot,
            'offset_words': int(kv.get('Offset', '0')),
            'format': kv.get('DataFormat', 'FMT_UNDEFINED'),
            'stride_words': self.stride_words,
            'signed': kv.get('Signed') == 'true',
            'integer': kv.get('NumFormat') == 'integer',
            'exp_adjust': int(kv.get('ExpAdjust', '0')),
        }
        self.attrs.append(attr)
        var = 'in_%d' % attr['index']
        if GENERIC_FETCH:
            if not target.startswith('r'):
                self.err('generic vfetch to %s' % target, rest)
            attr['reg'] = int(target[1:])
            attr['mask'] = sum(1 << i for i, ch in enumerate(comps) if ch is not None)
            if attr['index'] >= MAX_GENERIC_FETCHES:
                self.err('more than %d vfetches' % MAX_GENERIC_FETCHES, rest)
            for i, ch in enumerate(comps):
                if ch is not None:
                    self.body.append('%s.%s = vsel(%s, vswz[%d].%s);' % (target, 'xyzw'[i], var, attr['index'], 'xyzw'[i]))
            return
        if FINITE_SPECIAL:  # vertex data is an Inf/NaN source the taint analysis does not track: made finite here
            var = 'clamp(%s, -XBIG, XBIG)' % var
            fin = 'vf_%d' % self.tmp_counter()
            self.body.append('vec4 %s = %s;' % (fin, var))
            var = fin
        self.body.extend(self.write_masked(target, comps, var, False))

    @staticmethod
    def fin(expr):
        # FINITE_SPECIAL: a texel can be Inf/NaN (a float render target written with an overflowing value, a half-float
        # target turns anything over 65504 into Inf): made finite as it enters the shader. The fast shader set still
        # flickered in the intro cutscenes with every constant clamped (2026-11-10).
        return 'clamp(%s, -XBIG, XBIG)' % expr if FINITE_SPECIAL else expr

    def emit_fetch_texture(self, op, rest):
        # tfetch2D r0, r0.xy, tf0[, FetchValidOnly=false, ...]
        parts = [p.strip() for p in rest.split(',')]
        dim = {'tfetch1D': '1D', 'tfetch2D': '2D', 'tfetch3D': '3D', 'tfetchCube': 'Cube'}.get(op)
        if dim is None:
            self.err('unsupported fetch %s' % op, rest)
        ncomp = {'1D': 1, '2D': 2, '3D': 3, 'Cube': 3}[dim]
        # Sources before the result: `tfetch2D r0, r0.xy` reads r0 (an interpolator) before writing it.
        coord = self.parse_source(parts[1], ncomp if ncomp != 3 else 4)
        target, comps = self.parse_result(parts[0])
        if dim == 'Cube':
            # LEGO Batman port: no cube textures in the backend yet. The coordinate is the cube op's (tc, sc, face)
            # result; read (sc, tc) from a 2D binding, which the backend fills with a black placeholder for cube
            # fetch constants (objects render, without the reflection).
            coord = '(%s).yx' % coord
            dim = '2D'
        elif ncomp == 3:
            coord = '(%s).xyz' % coord
        if dim == '1D':
            # The Xenos has no 1D textures: a 1D fetch reads a 2D texture of height 1 (Xenia does the
            # same), so the sampler is 2D and the row is 0.
            coord = 'vec2(%s, 0.5)' % coord
            dim = '2D'
        if not parts[2].startswith('tf'):
            self.err('tfetch without a fetch constant', rest)
        slot = int(parts[2][2:])
        prev = self.textures.get(slot)
        if prev and prev != dim:
            self.err('texture slot %d used as %s and %s' % (slot, prev, dim))
        self.textures[slot] = dim
        use_register_lod = False  # LEGO Batman port: UseRegisterLOD=true samples at the setTexLOD level
        unnormalized = False      # LEGO Batman port: texel coordinates (vertex shaders reading tables from a texture)
        offset = {'OffsetX': 0.0, 'OffsetY': 0.0, 'OffsetZ': 0.0}
        for p in parts[3:]:
            k = p.split('=')[0]
            if k == 'UseRegisterLOD':
                use_register_lod = p.split('=')[1].strip() == 'true'
                continue
            if k == 'UnnormalizedTextureCoords':
                unnormalized = p.split('=')[1].strip() == 'true'
                continue
            if k in offset:
                offset[k] = float(p.split('=')[1])
                continue
            if k not in ('FetchValidOnly', 'UseComputedLOD', 'MagFilter', 'MinFilter', 'MipFilter', 'AnisoFilter'):
                self.err('unsupported tfetch attribute %r' % p, rest)
        tmp = 'tex_%d' % self.tmp_counter()
        if unnormalized:
            # texelFetch at the texel holding the coordinate (+ the fetch offset, in texels), clamped to the level.
            if op == 'tfetchCube':
                self.err('unnormalized cube fetch', rest)
            n = {'1D': 1, '2D': 2, '3D': 3}[{'tfetch1D': '1D', 'tfetch2D': '2D', 'tfetch3D': '3D'}[op]]
            raw = self.parse_source(parts[1], n if n != 3 else 4)
            off = [offset['OffsetX'], offset['OffsetY'], offset['OffsetZ']]
            if n == 1:
                tc = 'ivec2(clamp(int(floor((%s) + %r)), 0, textureSize(tf_%d, 0).x - 1), 0)' % (raw, off[0], slot)
            else:
                vt = 'vec%d' % n
                it = 'ivec%d' % n
                comp = '.xyz' if n == 3 else ''
                tc = 'clamp(%s(floor((%s)%s + %s(%s))), %s(0), textureSize(tf_%d, 0) - 1)' % (
                    it, raw, comp, vt, ', '.join(repr(o) for o in off[:n]), it, slot)
            self.body.append('vec4 %s = %s;' % (tmp, self.fin('texelFetch(tf_%d, %s, 0)' % (slot, tc))))
            self.body.extend(self.write_masked(target, comps, tmp, False))
            return
        if use_register_lod:
            self.body.append('vec4 %s = %s;' % (tmp, self.fin('textureLod(tf_%d, %s, tex_lod)' % (slot, coord))))
            self.body.extend(self.write_masked(target, comps, tmp, False))
            return
        # Fragment: texture() = the Xenos computed LOD (the faithful translation; what Xenia emits). 0.258-0.276 used
        # textureLod(.., 0) here on the theory that implicit derivatives in the pc loop's divergent flow made the green
        # halos; 0.269 found them in unpredicated fetches instead. With one uploaded level, mipFilter None and min == mag
        # filters on every texture seen, both forms pick the same texel; the compiled TEX differs in the LOD-mode field
        # only, and the console A/B (0.272) showed fewer blinking sprites with the computed LOD. Vertex fetches have no
        # derivatives: level 0.
        if self.stage == 'frag':
            self.body.append('vec4 %s = %s;' % (tmp, self.fin('texture(tf_%d, %s)' % (slot, coord))))
        else:
            self.body.append('vec4 %s = %s;' % (tmp, self.fin('textureLod(tf_%d, %s, 0.0)' % (slot, coord))))
        self.body.extend(self.write_masked(target, comps, tmp, False))

    _tmp = 0

    def tmp_counter(self):
        Translator._tmp += 1
        return Translator._tmp

    # ----- LEGO Batman port: exact (Xenos 0 * x = 0) multiplies only where an operand may be Inf/NaN ----------------
    # The exact helpers cost mul+min+compare+select per component and block FFMA fusion (the busiest pixel shaders were
    # ~3x larger). Maxwell's own FMZ multiply is not equivalent (the console flickered with it, 2026-11-10), so the
    # helpers stay, but only for operands that can be non-finite: results of rcp/rsq/log/exp/sqrt and what is computed
    # from them. Constants, interpolants, fetches, comparisons and saturated results are finite.
    def exact(self):
        return self.tainted is None or any(self.op_taint[:2])

    def mul4(self, x, y):
        return ('xmul(%s, %s)' if self.exact() else '(%s * %s)') % (x, y)

    def mul1(self, x, y):
        return ('xmuls(%s, %s)' if self.exact() else '(%s * %s)') % (x, y)

    def dot(self, n, x, y):
        if self.exact():
            return 'xdot%d(%s, %s)' % (n, x, y)
        sw = {4: '', 3: '.xyz', 2: '.xy'}[n]
        return 'dot((%s)%s, (%s)%s)' % (x, sw, y, sw)

    def src_tainted(self, src):
        if self.tainted is None:
            return True
        if src == 'ps':
            return self.ps_tainted
        return src[0] == 'r' and any((src[1], c) in self.tainted for c in src[2])

    def vector_expr(self, op, a):
        """GLSL expression (vec4) of a vector op with operand expressions a[0..2]."""
        if op == 'add': return '(%s + %s)' % (a[0], a[1])
        if op == 'mul': return self.mul4(a[0], a[1])
        if op == 'max': return 'max(%s, %s)' % (a[0], a[1])
        if op == 'min': return 'min(%s, %s)' % (a[0], a[1])
        if op == 'seq': return 'vec4(equal(%s, %s))' % (a[0], a[1])
        if op == 'sgt': return 'vec4(greaterThan(%s, %s))' % (a[0], a[1])
        if op == 'sge': return 'vec4(greaterThanEqual(%s, %s))' % (a[0], a[1])
        if op == 'sne': return 'vec4(notEqual(%s, %s))' % (a[0], a[1])
        if op == 'frc': return 'fract(%s)' % a[0]
        if op == 'trunc': return 'trunc(%s)' % a[0]
        if op == 'floor': return 'floor(%s)' % a[0]
        if op == 'mad':
            if self.exact():
                return 'xmad(%s, %s, %s)' % (a[0], a[1], a[2])
            return '(%s * %s + %s)' % (a[0], a[1], a[2])
        if op == 'cndeq': return 'mix(%s, %s, vec4(notEqual(%s, vec4(0.0))))' % (a[1], a[2], a[0])
        if op == 'cndge': return 'mix(%s, %s, vec4(lessThan(%s, vec4(0.0))))' % (a[1], a[2], a[0])
        if op == 'cndgt': return 'mix(%s, %s, vec4(lessThanEqual(%s, vec4(0.0))))' % (a[1], a[2], a[0])
        if op == 'dp4': return 'vec4(%s)' % self.dot(4, a[0], a[1])
        if op == 'dp3': return 'vec4(%s)' % self.dot(3, a[0], a[1])
        if op == 'dp2add': return 'vec4(%s + (%s).x)' % (self.dot(2, a[0], a[1]), a[2])
        if op == 'max4':
            return 'vec4(max(max((%s).x, (%s).y), max((%s).z, (%s).w)))' % (a[0], a[0], a[0], a[0])
        if op in ('setp_eq_push', 'setp_ne_push', 'setp_gt_push', 'setp_ge_push'):
            cmp = {'setp_eq_push': '== 0.0', 'setp_ne_push': '!= 0.0', 'setp_gt_push': '> 0.0',
                   'setp_ge_push': '>= 0.0'}[op]
            self.extra_pred = 'p0 = ((%s).w == 0.0 && (%s).w %s);' % (a[0], a[1], cmp)
            return 'vec4(((%s).x == 0.0 && (%s).x %s) ? 0.0 : (%s).x + 1.0)' % (a[0], a[1], cmp, a[0])
        if op in ('kill_eq', 'kill_gt', 'kill_ge', 'kill_ne'):
            fn = {'kill_eq': 'equal', 'kill_gt': 'greaterThan', 'kill_ge': 'greaterThanEqual',
                  'kill_ne': 'notEqual'}[op]
            self.has_kill = True
            self.extra_kill = 'if (any(%s(%s, %s))) discard;' % (fn, a[0], a[1])
            return 'vec4(float(any(%s(%s, %s))))' % (fn, a[0], a[1])
        if op == 'cube':
            # Xenos cube (src0 = v.zzxy, src1 = v.yxzz): (tc, sc, 2 * major axis, face), Xenia's convention.
            v = '(%s).zwx' % a[0]
            ax, ay, az = 'abs(%s.x)' % v, 'abs(%s.y)' % v, 'abs(%s.z)' % v
            x, y, z = v + '.x', v + '.y', v + '.z'
            zmaj = '(%s >= %s && %s >= %s)' % (az, ax, az, ay)
            ymaj = '(%s >= %s)' % (ay, ax)
            tc = '(%s ? -%s : %s ? (%s < 0.0 ? -%s : %s) : -%s)' % (zmaj, y, ymaj, y, z, z, y)
            sc = '(%s ? (%s < 0.0 ? -%s : %s) : %s ? %s : (%s < 0.0 ? %s : -%s))' % (zmaj, z, x, x, ymaj, x, x, z, z)
            ma = '(%s ? %s : %s ? %s : %s)' % (zmaj, z, ymaj, y, x)
            face = '(%s ? (%s < 0.0 ? 5.0 : 4.0) : %s ? (%s < 0.0 ? 3.0 : 2.0) : (%s < 0.0 ? 1.0 : 0.0))' % (zmaj, z, ymaj, y, x)
            return 'vec4(%s, %s, 2.0 * %s, %s)' % (tc, sc, ma, face)
        if op == 'dst':
            return 'vec4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)' % (a[0], a[1], a[0], a[1])
        self.err('vector op %s is not implemented' % op)

    def scalar_expr(self, op, a):
        """GLSL float expression of a scalar op; a = operand expressions (1 or 2 floats); sets self.extra_pred."""
        if op in ('adds', 'addsc'): return '(%s + %s)' % (a[0], a[1])
        if op == 'adds_prev': return '(%s + ps)' % a[0]
        if op in ('muls', 'mulsc'): return self.mul1(a[0], a[1])
        if op == 'muls_prev': return self.mul1(a[0], 'ps')
        if op == 'maxs': return 'max(%s, %s)' % (a[0], a[1])
        if op == 'mins': return 'min(%s, %s)' % (a[0], a[1])
        if op in ('subs', 'subsc'): return '(%s - %s)' % (a[0], a[1])
        if op == 'subs_prev': return '(%s - ps)' % a[0]
        if op == 'seqs': return 'float(%s == 0.0)' % a[0]
        if op == 'sgts': return 'float(%s > 0.0)' % a[0]
        if op == 'sges': return 'float(%s >= 0.0)' % a[0]
        if op == 'snes': return 'float(%s != 0.0)' % a[0]
        if op == 'frcs': return 'fract(%s)' % a[0]
        if op == 'truncs': return 'trunc(%s)' % a[0]
        if op == 'floors': return 'floor(%s)' % a[0]
        if FINITE_SPECIAL:
            # LEGO Batman port (perf): Inf becomes +-2^60, so 0 * it is 0 with a plain multiply (see FINITE_SPECIAL).
            if op == 'exp': return 'min(exp2(%s), XBIG)' % a[0]
            if op in ('log', 'logc'): return 'max(log2(%s), -XBIG)' % a[0]
            if op in ('rcp', 'rcpc'): return 'clamp(1.0 / %s, -XBIG, XBIG)' % a[0]
            if op in ('rsq', 'rsqc'): return 'min(inversesqrt(%s), XBIG)' % a[0]
        if op == 'exp': return 'exp2(%s)' % a[0]
        if op in ('log', 'logc'): return 'log2(%s)' % a[0]
        if op == 'rcp': return '(1.0 / %s)' % a[0]
        if op == 'rcpc': return 'xclampinf(1.0 / %s)' % a[0]
        if op == 'rcpf': return 'xflushinf(1.0 / %s)' % a[0]
        if op == 'rsq': return 'inversesqrt(%s)' % a[0]
        if op == 'rsqc': return 'xclampinf(inversesqrt(%s))' % a[0]
        if op == 'rsqf': return 'xflushinf(inversesqrt(%s))' % a[0]
        if op == 'sqrt': return 'sqrt(%s)' % a[0]
        if op == 'sin': return 'sin(%s)' % a[0]
        if op == 'cos': return 'cos(%s)' % a[0]
        if op in ('setp_eq', 'setp_ne', 'setp_gt', 'setp_ge'):
            cmp = {'setp_eq': '== 0.0', 'setp_ne': '!= 0.0', 'setp_gt': '> 0.0', 'setp_ge': '>= 0.0'}[op]
            self.extra_pred = 'p0 = (%s %s);' % (a[0], cmp)
            return 'float(!p0)'
        if op == 'setp_inv':
            self.extra_pred = 'p0 = (%s == 1.0);' % a[0]
            return '(p0 ? 0.0 : ((%s == 0.0) ? 1.0 : %s))' % (a[0], a[0])
        if op == 'setp_pop':
            self.extra_pred = 'p0 = ((%s - 1.0) <= 0.0);' % a[0]
            return '(p0 ? 0.0 : (%s - 1.0))' % a[0]
        if op == 'setp_clr':
            self.extra_pred = 'p0 = false;'
            return '3.402823466e+38'
        if op == 'setp_rstr':
            self.extra_pred = 'p0 = (%s == 0.0);' % a[0]
            return '(p0 ? 0.0 : %s)' % a[0]
        if op in ('kills_eq', 'kills_gt', 'kills_ge', 'kills_ne', 'kills_one'):
            cmp = {'kills_eq': '== 0.0', 'kills_gt': '> 0.0', 'kills_ge': '>= 0.0', 'kills_ne': '!= 0.0',
                   'kills_one': '== 1.0'}[op]
            self.has_kill = True
            self.extra_kill = 'if (%s %s) discard;' % (a[0], cmp)
            return 'float(%s %s)' % (a[0], cmp)
        if op == 'retain_prev': return 'ps'
        if op in ('maxas', 'maxasf'):  # LEGO Batman port, Xenia's interpreter semantics
            # dest = max(src.x, src.y); a0 = src.x rounded to nearest (maxas) or floored (maxasf), clamped to -256..255
            self.uses_a0 = True
            rnd = ('floor(%s + 0.5)' if op == 'maxas' else 'floor(%s)') % a[0]
            self.extra_pred = 'a0 = int(clamp(%s, -256.0, 255.0));' % rnd
            return 'max(%s, %s)' % (a[0], a[1])
        self.err('scalar op %s is not implemented' % op)

    def dest_comps(self, target, comps):
        if not target or not target.startswith('r') or not target[1:].isdigit():
            return frozenset()
        idx = int(target[1:])
        return frozenset((idx, 'xyzw'[i]) for i, ch in enumerate(comps) if ch is not None and ch not in '01')

    def solve_taint(self):
        """Fixpoint over the recorded ALU parts (flow-insensitive, so loops and branches are covered)."""
        tainted, ps = set(), False
        changed = True
        while changed:
            changed = False
            for dests, srcs, op, sat, scalar in self.edges:
                src_t = any((ps if x == 'ps' else x[0] == 'r' and any((x[1], c) in tainted for c in x[2]))
                            for x in srcs)
                t = op in NONFINITE_OPS or (src_t and op not in FINITE_OPS)
                if sat:
                    t = False  # clamp(Inf) is 1, and Maxwell's min/max return the non-NaN operand
                if t:
                    if not dests <= tainted:
                        tainted |= dests
                        changed = True
                    if scalar and not ps:
                        ps = True
                        changed = True
        return tainted, ps

    def emit_alu(self, vec, sca, pred):
        """vec/sca: (opname, saturate, result token, [operand tokens]) or None; pred: None/True/False."""
        self.extra_pred = None
        self.extra_kill = None
        stmts = []
        vres = None
        sres = None
        if vec:
            op, sat, res, ops = vec
            a, srcs = [], []
            for o in ops:
                a.append(self.parse_source(o, 4))  # sources before the result (read-before-write analysis)
                srcs.append(self._last_src)
            target, comps = self.parse_result(res)
            self.op_taint = [self.src_tainted(x) for x in srcs]
            self.edges.append((self.dest_comps(target, comps), srcs, op, sat, False))
            expr = self.vector_expr(op, a)
            if sat:
                expr = 'clamp(%s, 0.0, 1.0)' % expr
            vres = (target, comps, expr)
        if sca:
            op, sat, res, ops = sca
            if op in TWO_OPERAND_SCALAR:
                a = [self.parse_source(ops[0], 1)]
                srcs = [self._last_src]
                a.append(self.parse_source(ops[1], 1))
                srcs.append(self._last_src)
            elif op in TWO_COMPONENT_SCALAR:
                two = self.parse_source(ops[0], 2)
                a = ['(%s).x' % two, '(%s).y' % two]
                srcs = [self._last_src, self._last_src]
            elif ops:
                a = [self.parse_source(ops[0], 1)]
                srcs = [self._last_src]
            else:
                a = []
                srcs = []
            if op.endswith('_prev'):
                srcs.append('ps')
            target, comps = self.parse_result(res)
            self.op_taint = [self.src_tainted(x) for x in srcs]
            self.edges.append((self.dest_comps(target, comps), srcs, op, sat, True))
            expr = self.scalar_expr(op, a)
            if sat:
                expr = 'clamp(%s, 0.0, 1.0)' % expr
            sres = (target, comps, expr)
        # Both parts read their operands before either writes (one instruction).
        n = self.tmp_counter()
        if vres:
            stmts.append('vec4 v%d = %s;' % (n, vres[2]))
        if sres:
            stmts.append('float s%d = %s;' % (n, sres[2]))
        if self.extra_pred:
            stmts.append(self.extra_pred)
        if sres:
            stmts.append('ps = s%d;' % n)
        if vres:
            stmts.extend(self.write_masked(vres[0], vres[1], 'v%d' % n, False))
        if sres:
            stmts.extend(self.write_masked(sres[0], sres[1], 'vec4(s%d)' % n, False))
        if self.extra_kill:
            stmts.append(self.extra_kill)
        if pred is not None:
            stmts = ['if (%sp0) {' % ('' if pred else '!')] + ['  ' + s for s in stmts] + ['}']
        self.body.extend(stmts)

    # ----- parsing ---------------------------------------------------------------------------
    def parse_alu_part(self, text):
        """'mad_sat oPos.xy__, r0.yzzz, c32.xyyy' -> (op, sat, result, [operands])"""
        m = re.match(r'^([a-z0-9_]+?)(_sat)?\s+(\S*?)(?:\s*,\s*(.*))?$', text.strip())
        if not m:
            self.err('bad ALU text %r' % text)
        op, sat, res, rest = m.groups()
        ops = [o.strip() for o in rest.split(',')] if rest else []
        return op, bool(sat), res, ops

    def strip_pred(self, text):
        text = text.strip()
        if text.startswith('(p0)'):
            return True, text[4:].strip()
        if text.startswith('(!p0)'):
            return False, text[5:].strip()
        return None, text

    def translate(self):
        cf = []  # (index, text)
        instr_lines = []
        pending_alu = None  # (vec, sca, pred)
        serialize_next = False

        def flush_alu():
            nonlocal pending_alu
            if pending_alu:
                self.emit_alu(*pending_alu)
                pending_alu = None

        blocks = []  # list of dict(index, kind, pred, cond, target, stmts)
        cur = None

        def end_block():
            nonlocal cur
            flush_alu()
            if cur is not None:
                cur['stmts'] = self.body
                blocks.append(cur)
            self.body = []
            cur = None

        for raw in self.lines:
            line = raw.rstrip()
            if not line.strip():
                continue
            m = re.match(r'^/\*\s*(\d+)\.(\d+)\s*\*/\s*(.*)$', line)
            if m:  # control-flow instruction
                end_block()
                idx = int(m.group(1)) * 2 + int(m.group(2))
                text = m.group(3).split('//')[0].strip()
                pred, text = self.strip_pred(text)
                op = text.split()[0] if text else ''
                if op in ('exec', 'exece'):
                    cond = None
                    rest = text[len(op):].strip()
                    if rest.startswith('b') or rest.startswith('!b'):
                        self.uses_bools = True
                        cond = rest.split(',')[0].strip()
                    cur = {'index': idx, 'kind': 'exec', 'end': op == 'exece', 'pred': pred, 'cond': cond}
                elif op == 'jmp' or op == 'cjmp':
                    self.has_jump = True
                    m2 = re.match(r'^c?jmp\s+(?:(!?b\d+),\s*)?L(\d+)$', text)
                    if not m2:
                        self.err('bad jump', line)
                    if m2.group(1):
                        self.uses_bools = True
                    blocks.append({'index': idx, 'kind': 'jmp', 'pred': pred, 'cond': m2.group(1),
                                   'target': int(m2.group(2)), 'stmts': []})
                elif op in ('alloc', 'cnop', 'nop'):
                    blocks.append({'index': idx, 'kind': 'nop', 'stmts': []})
                elif op in ('loop', 'endloop'):
                    # gh: structured loops over an integer constant (count | start << 8 | step << 16);
                    # aL is the loop register the body indexes the constants with (c[1+aL]).
                    m2 = re.match(r'^(loop|endloop)\s+i(\d+),\s*L(\d+)$', text)
                    if not m2:
                        self.err('bad loop', line)
                    self.has_loop = True
                    blocks.append({'index': idx, 'kind': op, 'loop': int(m2.group(2)), 'stmts': []})
                elif op in ('call', 'ccall', 'ret'):
                    self.err('control flow %s is not supported' % op, line)
                else:
                    self.err('unknown control flow %r' % text, line)
                continue
            m = re.match(r'^/\*\s*(\d+)\s*\*/\s*(.*)$', line)
            if m:
                text = m.group(2).strip()
            else:
                text = line.strip()
                if text.startswith('label L'):
                    # A jump target: the next control-flow instruction has this index already.
                    end_block()
                    continue
            if text == 'serialize':
                continue
            if text.startswith('+'):  # the scalar co-issue of the pending ALU instruction
                if not pending_alu:
                    self.err('scalar co-issue without a vector part', line)
                pred, body = self.strip_pred(text[1:])
                pending_alu = (pending_alu[0], self.parse_alu_part(body), pending_alu[2])
                continue
            flush_alu()
            pred, body = self.strip_pred(text)
            if cur is None:
                self.err('instruction outside an exec block', line)
            op = body.split()[0] if body else ''
            # A predicated fetch ("(p0) tfetch1D r2.zyxw, ...") runs only when the predicate holds. It was emitted
            # unconditionally: in PS B3EC630B's four-tap path (scaled and rotated paletted sprites) a transparent tap
            # had its zeroed colour overwritten by palette entry 0 (green / red): the halos (0.141-0.268).
            start = len(self.body)
            if op in ('vfetch_full', 'vfetch_mini'):
                self.emit_fetch_vertex(op, body[len(op):].strip())
            elif op == 'setTexLOD':  # LEGO Batman port: `setTexLOD r1.y` sets the LOD of UseRegisterLOD fetches
                self.body.append('tex_lod = %s;' % self.parse_source(body[len(op):].strip(), 1))
            elif op.startswith('tfetch') or op in ('getTextureBorderColorFrac', 'getTextureComputedLod',
                                                     'getTextureGradients', 'getTextureWeights', 'setTextureLod',
                                                     'setTextureGradientsHorz', 'setTextureGradientsVert'):
                if not op.startswith('tfetch'):
                    self.err('texture instruction %s is not supported' % op, line)
                self.emit_fetch_texture(op, body[len(op):].strip())
            if pred is not None and len(self.body) > start and (op in ('vfetch_full', 'vfetch_mini') or op.startswith('tfetch')):
                inner = self.body[start:]
                del self.body[start:]
                self.body.append('if (%sp0) {' % ('' if pred else '!'))
                self.body.extend('  ' + l for l in inner)
                self.body.append('}')
                continue
            if op in ('vfetch_full', 'vfetch_mini') or op.startswith('tfetch') or op == 'setTexLOD':  # LEGO Batman port
                continue
            if op == 'nop':
                continue
            elif op in VECTOR_OPS or (op.endswith('_sat') and op[:-4] in VECTOR_OPS):
                pending_alu = (self.parse_alu_part(body), None, pred)
            elif op in SCALAR_OPS or (op.endswith('_sat') and op[:-4] in SCALAR_OPS):
                pending_alu = (None, self.parse_alu_part(body), pred)
            else:
                self.err('unknown instruction %r' % body, line)
        end_block()
        return self.generate(blocks)

    # ----- output ----------------------------------------------------------------------------
    def generate(self, blocks):
        out = ['#version 460', '// cvhd xenos2glsl: %s (%s)' % (self.name, self.stage)]
        ind = '  '
        if self.stage == 'vert':
            # The same position in a depth pre-pass and in a colour pass (two shaders) must match bit for bit
            # (hcu-switch: otherwise a LEQUAL test fails, missing meshes and z-fighting).
            out.append('invariant gl_Position;')
            for a in self.attrs:
                out.append('layout(location = %d) in vec4 in_%d;  // vf%d offset %d %s' % (
                    a['index'], a['index'], a['slot'], a['offset_words'], a['format']))
            # Vertex texture fetch (LEGO Batman port: VS 8A67D3A2495FEAFD samples tf16): the backend binds them to the
            # vertex stage.
            for slot in sorted(self.textures):
                out.append('layout(binding = %d) uniform sampler%s tf_%d;' % (slot, self.textures[slot], slot))
        else:
            for k in sorted(self.reads_before_write):
                out.append('layout(location = %d) in vec4 i_%d;' % (k, k))
            for slot in sorted(self.textures):
                out.append('layout(binding = %d) uniform sampler%s tf_%d;' % (slot, self.textures[slot], slot))
        out.append('layout(std140, binding = 0) uniform Consts { vec4 c[256]; };')
        out.append('layout(std140, binding = 1) uniform Sys { vec4 ndc_scale; vec4 ndc_offset; vec4 alpha; uvec4 bools[2]; uvec4 loops[8]; };')
        if GENERIC_FETCH and self.stage == 'vert' and self.attrs:
            out.append('layout(std140, binding = 2) uniform VFetch { ivec4 vswz[%d]; };' % MAX_GENERIC_FETCHES)
            out.append('float vsel(vec4 v, int s) { return s < 4 ? v[s] : float(s - 4); }')
        interps = sorted(int(e[1:]) for e in self.exports if re.match(r'^o\d+$', e))
        colors = sorted(int(e[2:]) for e in self.exports if e.startswith('oC'))
        if self.stage == 'vert':
            for k in interps:
                out.append('layout(location = %d) out vec4 o_%d;' % (k, k))
        else:
            for k in colors:
                out.append('layout(location = %d) out vec4 oC_%d;' % (k, k))
        # LEGO Batman port: Xenos (Shader Model 3) multiplication: +-0 times anything, Inf and NaN included, is 0, as
        # the SDK's SPIR-V translator does (ZeroIfAnyOperandIsZero) for mul, mad, dp2add/dp3/dp4 and muls.
        # XENOS_NATIVE_ZERO_WINS=1 (tools/switch-env/stage-shaders.sh): plain * and dot(), for the patched uam run with
        # UAM_MUL_ZERO_WINS=1, which marks every float MUL/MAD/DP "dnz" = Maxwell FMUL/FFMA.FMZ, the hardware's own
        # D3D9 rule. One instruction instead of mul+min+compare+select per component (GPU-bound frames, 2026-11-10).
        if os.environ.get('XENOS_NATIVE_ZERO_WINS') == '1':
            out.append('vec4 xmul(vec4 a, vec4 b) { return a * b; }')
            out.append('float xmuls(float a, float b) { return a * b; }')
            out.append('float xdot4(vec4 a, vec4 b) { return dot(a, b); }')
            out.append('float xdot3(vec4 a, vec4 b) { return dot(a.xyz, b.xyz); }')
            out.append('float xdot2(vec4 a, vec4 b) { return dot(a.xy, b.xy); }')
            out.append('vec4 xmad(vec4 a, vec4 b, vec4 c) { return a * b + c; }')
        elif os.environ.get('XENOS_STRICT_HELPERS') == '1':
            out.append('vec4 xmul(vec4 a, vec4 b) { return mix(a * b, vec4(0.0), equal(min(abs(a), abs(b)), vec4(0.0))); }')
            out.append('float xmuls(float a, float b) { return (min(abs(a), abs(b)) == 0.0) ? 0.0 : a * b; }')
            out.append('float xdot4(vec4 a, vec4 b) { vec4 p = xmul(a, b); return p.x + p.y + p.z + p.w; }')
            out.append('float xdot3(vec4 a, vec4 b) { vec4 p = xmul(a, b); return p.x + p.y + p.z; }')
            out.append('float xdot2(vec4 a, vec4 b) { vec4 p = xmul(a, b); return p.x + p.y; }')
            out.append('vec4 xmad(vec4 a, vec4 b, vec4 c) { return xmul(a, b) + c; }')
        else:
            # Cheaper exact forms (perf, 2026-11-10): from finite operands a plain product is NaN only for 0 * Inf, where
            # Xenos gives 0; so compute the plain (fused) product and replace a NaN. mul/mad = 3 instructions instead of
            # 4-5, dp4 = 10 instead of 19. Differs from Xenos only for NaN inputs (0 or c instead of NaN) and for
            # Inf - Inf in a mad (c instead of NaN). XENOS_STRICT_HELPERS=1 restores the min/compare forms.
            out.append('vec4 xmul(vec4 a, vec4 b) { vec4 p = a * b; return mix(p, vec4(0.0), isnan(p)); }')
            out.append('float xmuls(float a, float b) { float p = a * b; return isnan(p) ? 0.0 : p; }')
            out.append('vec4 xmad(vec4 a, vec4 b, vec4 c) { vec4 p = a * b + c; return mix(p, c, isnan(p)); }')
            out.append('float xmads(float a, float b, float c) { float p = a * b + c; return isnan(p) ? c : p; }')
            out.append('float xdot4(vec4 a, vec4 b) { return xmads(a.w, b.w, xmads(a.z, b.z, xmads(a.y, b.y, xmuls(a.x, b.x)))); }')
            out.append('float xdot3(vec4 a, vec4 b) { return xmads(a.z, b.z, xmads(a.y, b.y, xmuls(a.x, b.x))); }')
            out.append('float xdot2(vec4 a, vec4 b) { return xmads(a.y, b.y, xmuls(a.x, b.x)); }')
        # rcp/rsq with the "c" (clamp infinity to +-FLT_MAX) and "f" (flush infinity to 0) variants, as the SDK does.
        out.append('float xclampinf(float v) { return isinf(v) ? sign(v) * 3.402823466e38 : v; }')
        out.append('const float XBIG = 1.152921504606847e18;  // 2^60')
        out.append('float xflushinf(float v) { return isinf(v) ? 0.0 * sign(v) : v; }')
        out.append('void main() {')
        for i in range(self.max_reg + 1):
            if self.stage == 'frag' and i in self.reads_before_write:
                out.append(ind + 'vec4 r%d = i_%d;' % (i, i))
            else:
                out.append(ind + 'vec4 r%d = vec4(0.0);' % i)
        for e in sorted(self.exports):
            out.append(ind + 'vec4 %s = vec4(0.0);' % e)
        out.append(ind + 'bool p0 = false;')
        out.append(ind + 'float ps = 0.0;')
        out.append(ind + 'int a0 = 0;')         # LEGO Batman port: address register (maxas, c[N+a0])
        out.append(ind + 'float tex_lod = 0.0;')  # LEGO Batman port: setTexLOD

        def block_cond(b):
            conds = []
            if b.get('pred') is not None:
                conds.append('p0' if b['pred'] else '!p0')
            if b.get('cond'):
                c = b['cond']
                neg = c.startswith('!')
                n = int(c.lstrip('!b'))
                # LEGO Batman port: `!(uint) != 0u` was invalid GLSL for negated conditions; compare instead.
                conds.append('((bools[%d][%d] >> %du) & 1u) %s 0u' % (n // 128, (n % 128) // 32, n % 32, '==' if neg else '!='))
            return ' && '.join(conds)

        if self.has_loop:
            if self.has_jump:
                self.err('loops together with jumps are not supported')
            out.append(ind + 'int aL = 0;')
        if not self.has_jump:
            for b in blocks:
                if b['kind'] == 'loop':
                    n = b['loop']
                    out.append(ind + '{ uint lc = loops[%d][%d]; int lcount = int(lc & 255u); aL = int((lc >> 8) & 255u); '
                                     'int lstep = int(int((lc >> 16) & 255u) << 24) >> 24;' % (n // 4, n % 4))
                    out.append(ind + 'for (int li = 0; li < lcount; ++li) {')
                    continue
                if b['kind'] == 'endloop':
                    out.append(ind + 'aL += lstep; } }')
                    continue
                if b['kind'] != 'exec':
                    continue
                cond = block_cond(b)
                if cond:
                    out.append(ind + 'if (%s) {' % cond)
                    out.extend(ind + '  ' + s for s in b['stmts'])
                    out.append(ind + '}')
                else:
                    out.extend(ind + s for s in b['stmts'])
        else:
            out.append(ind + 'int pc = 0;')
            out.append(ind + 'while (pc >= 0) {')
            out.append(ind + '  switch (pc) {')
            for i, b in enumerate(blocks):
                out.append(ind + '  case %d: {' % b['index'])
                nxt = blocks[i + 1]['index'] if i + 1 < len(blocks) else -1
                if b['kind'] == 'exec':
                    cond = block_cond(b)
                    if cond:
                        out.append(ind + '    if (%s) {' % cond)
                        out.extend(ind + '      ' + s for s in b['stmts'])
                        out.append(ind + '    }')
                    else:
                        out.extend(ind + '    ' + s for s in b['stmts'])
                    out.append(ind + '    pc = %d;' % (-1 if b['end'] else nxt))
                elif b['kind'] == 'jmp':
                    cond = block_cond(b)
                    if cond:
                        out.append(ind + '    pc = (%s) ? %d : %d;' % (cond, b['target'], nxt))
                    else:
                        out.append(ind + '    pc = %d;' % b['target'])
                else:
                    out.append(ind + '    pc = %d;' % nxt)
                out.append(ind + '  } break;')
            out.append(ind + '  default: pc = -1; break;')
            out.append(ind + '  }')
            out.append(ind + '}')
        if self.stage == 'vert':
            out.append(ind + 'gl_Position = vec4(oPos.xy * ndc_scale.xy + ndc_offset.xy * oPos.w, '
                             'oPos.z * ndc_scale.z + ndc_offset.z * oPos.w, oPos.w);')
            for k in interps:
                out.append(ind + ('o_%d = clamp(o%d, -XBIG, XBIG);' if FINITE_SPECIAL else 'o_%d = o%d;') % (k, k))
        else:
            if colors:
                out.append(ind + '// Alpha test (RB_COLORCONTROL): alpha.x enable, alpha.y function, alpha.z reference.')
                out.append(ind + 'if (alpha.x > 0.5) {')
                out.append(ind + '  uint f = uint(alpha.y); float a = oC0.w; bool pass = f == 7u || (f == 1u && a < alpha.z) || '
                                 '(f == 2u && a == alpha.z) || (f == 3u && a <= alpha.z) || (f == 4u && a > alpha.z) || '
                                 '(f == 5u && a != alpha.z) || (f == 6u && a >= alpha.z);')
                out.append(ind + '  if (!pass) discard;')
                out.append(ind + '}')
            for k in colors:
                out.append(ind + 'oC_%d = oC%d;' % (k, k))
        out.append('}')
        meta = {
            'name': self.name, 'stage': self.stage,
            'attributes': self.attrs if self.stage == 'vert' else [],
            'textures': {str(k): v for k, v in sorted(self.textures.items())},
            'interpolators': interps, 'inputs': sorted(self.reads_before_write) if self.stage == 'frag' else [],
            'colors': colors, 'has_jump': self.has_jump, 'uses_bools': self.uses_bools, 'kill': self.has_kill,
        }
        return '\n'.join(out) + '\n', meta


def geometry_glsl(name, interps):
    """A pass-through geometry shader for Xenos rectangle lists: 3 vertices per rectangle, the
    4th = v1 + v2 - v0 (positions and every interpolator extrapolated). deko3d's fill-rectangle
    mode does the same on the console; Eden ignores that mode, so the backend binds this shader
    for rectangle-list draws (cvar deko3d_fill_rectangle selects the hardware mode)."""
    out = ['#version 460', '// cvhd xenos2glsl: %s (geom: rectangle lists)' % name,
           'layout(triangles) in;', 'layout(triangle_strip, max_vertices = 4) out;']
    for k in interps:
        out.append('layout(location = %d) in vec4 g_%d[];' % (k, k))
        out.append('layout(location = %d) out vec4 o_%d;' % (k, k))
    out.append('void main() {')
    out.append('  for (int i = 0; i < 3; i++) {')
    out.append('    gl_Position = gl_in[i].gl_Position;')
    for k in interps:
        out.append('    o_%d = g_%d[i];' % (k, k))
    out.append('    EmitVertex();')
    out.append('  }')
    out.append('  gl_Position = gl_in[1].gl_Position + gl_in[2].gl_Position - gl_in[0].gl_Position;')
    for k in interps:
        out.append('  o_%d = g_%d[1] + g_%d[2] - g_%d[0];' % (k, k, k, k))
    out.append('  EmitVertex();')
    out.append('  EndPrimitive();')
    out.append('}')
    return '\n'.join(out) + '\n'


def translate_file(path):
    stage = 'vert' if path.endswith('.vert') else 'frag' if path.endswith('.frag') else None
    if stage is None:
        raise SystemExit('%s: name must end in .vert or .frag' % path)
    name = os.path.basename(path).replace('.ucode.', '.')
    text = open(path, encoding='utf-8').read()
    if os.environ.get('XENOS_EXACT_MULS') == '1':
        return Translator(text, stage, name).translate()
    first = Translator(text, stage, name)
    first.translate()
    t = Translator(text, stage, name)
    t.tainted, t.ps_tainted = first.solve_taint()
    return t.translate()


def uam_check(glsl_path, stage):
    uam = os.environ.get('UAM', '/opt/devkitpro/tools/bin/uam')
    out = glsl_path + '.dksh'
    r = subprocess.run([uam, '-s', stage, '-o', out, glsl_path], capture_output=True, text=True)
    if r.returncode != 0:
        return False, (r.stdout + r.stderr).strip()
    return True, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('input', nargs='?')
    ap.add_argument('-o', '--out')
    ap.add_argument('--json')
    ap.add_argument('--all', help='translate every *.ucode.vert/frag in this directory')
    ap.add_argument('--out-dir', help='output directory for --all')
    ap.add_argument('--check', action='store_true', help='compile the output with uam')
    args = ap.parse_args()
    if args.all:
        files = sorted(f for f in os.listdir(args.all) if f.endswith('.ucode.vert') or f.endswith('.ucode.frag'))
        os.makedirs(args.out_dir or args.all, exist_ok=True)
        ok = 0
        for f in files:
            src = os.path.join(args.all, f)
            try:
                glsl, meta = translate_file(src)
            except SystemExit as e:
                print('FAIL %s: %s' % (f, e))
                continue
            stem = f.replace('.ucode.', '.')
            dst = os.path.join(args.out_dir or args.all, stem + '.glsl')
            open(dst, 'w', newline='\n').write(glsl)
            open(dst.replace('.glsl', '.json'), 'w', newline='\n').write(json.dumps(meta, indent=1))
            if args.check:
                good, msg = uam_check(dst, meta['stage'])
                print('%s %s%s' % ('OK  ' if good else 'FAIL', stem, '' if good else ': ' + msg))
                ok += good
            else:
                ok += 1
            if meta['stage'] == 'vert':
                # The rectangle-list geometry shader of this vertex shader (same hash, .geom).
                gdst = os.path.join(args.out_dir or args.all, stem.replace('.vert', '.geom') + '.glsl')
                open(gdst, 'w', newline='\n').write(geometry_glsl(meta['name'], meta['interpolators']))
                if args.check:
                    good, msg = uam_check(gdst, 'geom')
                    if not good:
                        print('FAIL %s: %s' % (os.path.basename(gdst), msg))
        print('%d of %d translated' % (ok, len(files)))
        return
    glsl, meta = translate_file(args.input)
    if args.out:
        open(args.out, 'w', newline='\n').write(glsl)
        if args.check:
            good, msg = uam_check(args.out, meta['stage'])
            print('OK' if good else 'FAIL: ' + msg)
    else:
        sys.stdout.write(glsl)
    if args.json:
        open(args.json, 'w', newline='\n').write(json.dumps(meta, indent=1))


if __name__ == '__main__':
    main()
