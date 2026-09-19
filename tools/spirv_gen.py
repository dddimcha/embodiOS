#!/usr/bin/env python3
"""Hand-assembled SPIR-V generator for embodiOS Vulkan compute shaders.

No GLSL compiler is available in the build environment (glslc/glslangValidator
are not installable), so this script emits SPIR-V 1.3 binary modules directly.

Shaders generated:
  (a) matmul_f32  : C[m*n] = A[m*k] * B[k*n], all fp32, one invocation per
                    element of C, local_size_x = 64, push constants {m,k,n}.
  (b) matmul_q8_0 : A is ggml Q8_0 blocks (u16 fp16 scale + 32 x int8 per
                    34-byte block, row-major over m x k), B and C fp32.
                    Dequantization (incl. fp16->fp32 bit conversion) happens
                    in-shader. k must be a multiple of 32.

Output: kernel/ai/vk_shaders.h with `static const uint32_t` arrays so both the
host validation binary (tools/host_test_vulkan.c) and the kernel-side driver
layer (kernel/ai/vk_device.c) compile the exact same words.

Validation: the emitted modules are exercised end-to-end on lavapipe by
tools/host_test_vulkan.c (vkCreateComputePipelines rejects invalid SPIR-V).
"""

import os
import struct
import sys

# ----------------------------------------------------------------------------
# SPIR-V opcodes (subset, from the SPIR-V 1.3 specification)
# ----------------------------------------------------------------------------
OP_NAME = 5
OP_MEMBER_NAME = 6
OP_EXT_INST_IMPORT = 11
OP_MEMORY_MODEL = 14
OP_ENTRY_POINT = 15
OP_EXECUTION_MODE = 16
OP_CAPABILITY = 17
OP_TYPE_VOID = 19
OP_TYPE_BOOL = 20
OP_TYPE_INT = 21
OP_TYPE_FLOAT = 22
OP_TYPE_VECTOR = 23
OP_TYPE_RUNTIME_ARRAY = 29
OP_TYPE_STRUCT = 30
OP_TYPE_POINTER = 32
OP_TYPE_FUNCTION = 33
OP_CONSTANT_TRUE = 41
OP_CONSTANT_FALSE = 42
OP_CONSTANT = 43
OP_FUNCTION = 54
OP_FUNCTION_END = 56
OP_VARIABLE = 59
OP_LOAD = 61
OP_STORE = 62
OP_ACCESS_CHAIN = 65
OP_DECORATE = 71
OP_MEMBER_DECORATE = 72
OP_COMPOSITE_EXTRACT = 81
OP_CONVERT_S_TO_F = 111
OP_CONVERT_U_TO_F = 112
OP_BITCAST = 124
OP_F_NEGATE = 127
OP_I_ADD = 128
OP_F_ADD = 129
OP_I_SUB = 130
OP_I_MUL = 132
OP_F_MUL = 133
OP_U_DIV = 134
OP_SELECT = 169
OP_I_EQUAL = 170
OP_I_NOT_EQUAL = 171
OP_U_GREATER_THAN_EQUAL = 174
OP_U_LESS_THAN = 176
OP_SHIFT_RIGHT_LOGICAL = 194
OP_SHIFT_RIGHT_ARITHMETIC = 195
OP_SHIFT_LEFT_LOGICAL = 196
OP_BITWISE_OR = 197
OP_BITWISE_AND = 199
OP_LOOP_MERGE = 246
OP_SELECTION_MERGE = 247
OP_LABEL = 248
OP_BRANCH = 249
OP_BRANCH_CONDITIONAL = 250
OP_RETURN = 253

# Capabilities
CAP_SHADER = 1

# Addressing / memory models
ADDR_LOGICAL = 0
MEM_GLSL450 = 1

# Execution model / mode
EXEC_GL_COMPUTE = 5
MODE_LOCAL_SIZE = 17

# Storage classes
SC_INPUT = 1
SC_PRIVATE = 6
SC_FUNCTION = 7
SC_PUSH_CONSTANT = 9
SC_STORAGE_BUFFER = 12

# Decorations
DEC_BLOCK = 2
DEC_ARRAY_STRIDE = 6
DEC_BUILTIN = 11
DEC_NON_WRITABLE = 24
DEC_NON_READABLE = 25
DEC_BINDING = 33
DEC_DESCRIPTOR_SET = 34
DEC_OFFSET = 35

# Built-ins
BUILTIN_GLOBAL_INVOCATION_ID = 28

# Control masks
NONE = 0

LOCAL_SIZE_X = 64


def _str_words(s):
    """Encode a string as SPIR-V literal-string words (NUL-terminated, padded)."""
    b = s.encode("utf-8") + b"\x00"
    b += b"\x00" * ((4 - len(b) % 4) % 4)
    return list(struct.unpack("<%dI" % (len(b) // 4), b))


def _f32_words(v):
    return [struct.unpack("<I", struct.pack("<f", v))[0]]


class Module:
    """Minimal SPIR-V module assembler."""

    def __init__(self):
        self._next_id = 1
        self.sec_capabilities = []
        self.sec_memory_model = []
        self.sec_entry_points = []
        self.sec_exec_modes = []
        self.sec_names = []
        self.sec_decorations = []
        self.sec_types = []      # types, constants, global variables (in order)
        self.sec_functions = []
        self._type_cache = {}
        self._const_cache = {}

    # -- id management ------------------------------------------------------
    def new_id(self):
        i = self._next_id
        self._next_id += 1
        return i

    # -- raw emission -------------------------------------------------------
    @staticmethod
    def _inst(section, opcode, *operands):
        section.append(((1 + len(operands)) << 16) | opcode)
        section.extend(operands)

    def inst(self, section, opcode, *operands):
        self._inst(section, opcode, *operands)

    # -- types --------------------------------------------------------------
    def type_void(self):
        if "void" not in self._type_cache:
            i = self.new_id()
            self.inst(self.sec_types, OP_TYPE_VOID, i)
            self._type_cache["void"] = i
        return self._type_cache["void"]

    def type_bool(self):
        if "bool" not in self._type_cache:
            i = self.new_id()
            self.inst(self.sec_types, OP_TYPE_BOOL, i)
            self._type_cache["bool"] = i
        return self._type_cache["bool"]

    def type_int(self, width, signed):
        key = ("int", width, signed)
        if key not in self._type_cache:
            i = self.new_id()
            self.inst(self.sec_types, OP_TYPE_INT, i, width, signed)
            self._type_cache[key] = i
        return self._type_cache[key]

    def type_float(self, width):
        key = ("float", width)
        if key not in self._type_cache:
            i = self.new_id()
            self.inst(self.sec_types, OP_TYPE_FLOAT, i, width)
            self._type_cache[key] = i
        return self._type_cache[key]

    def type_vector(self, comp, count):
        key = ("vec", comp, count)
        if key not in self._type_cache:
            i = self.new_id()
            self.inst(self.sec_types, OP_TYPE_VECTOR, i, comp, count)
            self._type_cache[key] = i
        return self._type_cache[key]

    def type_runtime_array(self, elem, stride):
        key = ("rta", elem)
        if key not in self._type_cache:
            i = self.new_id()
            self.inst(self.sec_types, OP_TYPE_RUNTIME_ARRAY, i, elem)
            self.inst(self.sec_decorations, OP_DECORATE, i, DEC_ARRAY_STRIDE, stride)
            self._type_cache[key] = i
        return self._type_cache[key]

    def type_struct(self, members, offsets, block=False):
        key = ("struct", tuple(members))
        if key not in self._type_cache:
            i = self.new_id()
            self.inst(self.sec_types, OP_TYPE_STRUCT, i, *members)
            if block:
                self.inst(self.sec_decorations, OP_DECORATE, i, DEC_BLOCK)
            for idx, off in enumerate(offsets):
                self.inst(self.sec_decorations, OP_MEMBER_DECORATE, i, idx,
                          DEC_OFFSET, off)
            self._type_cache[key] = i
        return self._type_cache[key]

    def type_pointer(self, storage_class, pointee):
        key = ("ptr", storage_class, pointee)
        if key not in self._type_cache:
            i = self.new_id()
            self.inst(self.sec_types, OP_TYPE_POINTER, i, storage_class, pointee)
            self._type_cache[key] = i
        return self._type_cache[key]

    def type_function(self, ret, params=()):
        key = ("fn", ret, tuple(params))
        if key not in self._type_cache:
            i = self.new_id()
            self.inst(self.sec_types, OP_TYPE_FUNCTION, i, ret, *params)
            self._type_cache[key] = i
        return self._type_cache[key]

    # -- constants ------------------------------------------------------------
    def const_u32(self, value):
        key = ("cu32", value & 0xFFFFFFFF)
        if key not in self._const_cache:
            t = self.type_int(32, 0)
            i = self.new_id()
            self.inst(self.sec_types, OP_CONSTANT, t, i, value & 0xFFFFFFFF)
            self._const_cache[key] = i
        return self._const_cache[key]

    def const_i32(self, value):
        key = ("ci32", value & 0xFFFFFFFF)
        if key not in self._const_cache:
            t = self.type_int(32, 1)
            i = self.new_id()
            self.inst(self.sec_types, OP_CONSTANT, t, i, value & 0xFFFFFFFF)
            self._const_cache[key] = i
        return self._const_cache[key]

    def const_f32(self, value):
        key = ("cf32", struct.pack("<f", value))
        if key not in self._const_cache:
            t = self.type_float(32)
            i = self.new_id()
            self.inst(self.sec_types, OP_CONSTANT, t, i, *_f32_words(value))
            self._const_cache[key] = i
        return self._const_cache[key]

    # -- globals ----------------------------------------------------------------
    def global_variable(self, ptr_type, storage_class, name=None):
        i = self.new_id()
        self.inst(self.sec_types, OP_VARIABLE, ptr_type, i, storage_class)
        if name:
            self.name(i, name)
        return i

    # -- decorations ------------------------------------------------------------
    def decorate(self, target, decoration, *extra):
        self.inst(self.sec_decorations, OP_DECORATE, target, decoration, *extra)

    def name(self, target, s):
        self.inst(self.sec_names, OP_NAME, target, *_str_words(s))

    def member_name(self, target, member, s):
        self.inst(self.sec_names, OP_MEMBER_NAME, target, member, *_str_words(s))

    # -- module assembly ----------------------------------------------------------
    def add_capability(self, cap):
        self.inst(self.sec_capabilities, OP_CAPABILITY, cap)

    def set_memory_model(self, addressing, model):
        self.inst(self.sec_memory_model, OP_MEMORY_MODEL, addressing, model)

    def add_entry_point(self, exec_model, fn_id, name, interfaces):
        self.inst(self.sec_entry_points, OP_ENTRY_POINT, exec_model, fn_id,
                  *_str_words(name), *interfaces)

    def add_execution_mode(self, fn_id, mode, *literals):
        self.inst(self.sec_exec_modes, OP_EXECUTION_MODE, fn_id, mode, *literals)

    def add_function(self, body_words):
        self.sec_functions.extend(body_words)

    def assemble(self):
        words = [0x07230203, 0x00010300, 0x0008000B, self._next_id, 0]
        for sec in (self.sec_capabilities, self.sec_memory_model,
                    self.sec_entry_points, self.sec_exec_modes,
                    self.sec_names, self.sec_decorations,
                    self.sec_types, self.sec_functions):
            words.extend(sec)
        return words

    def to_bytes(self):
        return struct.pack("<%dI" % len(self.assemble()), *self.assemble())


class FunctionBuilder:
    """Imperative builder for a single SPIR-V function with structured CF."""

    def __init__(self, mod, ret_type, fn_type, name=None):
        self.mod = mod
        self.fn_id = mod.new_id()
        if name:
            mod.name(self.fn_id, name)
        self.blocks = []          # list of (label_id, [words])
        self._cur = None
        self.ret_type = ret_type
        self.fn_type = fn_type
        self._first_block = True
        # function header
        self._header = []
        Module._inst(self._header, OP_FUNCTION, ret_type, self.fn_id, NONE, fn_type)

    def new_label(self):
        return self.mod.new_id()

    def begin_block(self, label):
        words = []
        Module._inst(words, OP_LABEL, label)
        self.blocks.append((label, words))
        self._cur = words
        self._first_block = False

    def emit(self, opcode, *operands):
        Module._inst(self._cur, opcode, *operands)

    def result(self, result_type, opcode, *operands):
        i = self.mod.new_id()
        self.emit(opcode, result_type, i, *operands)
        return i

    def declare_local(self, ptr_type, storage_class=SC_FUNCTION):
        """OpVariable for a function-local; MUST be emitted in the first block."""
        assert self._cur is not None and len(self.blocks) == 1, \
            "function locals must be declared in the entry block"
        i = self.mod.new_id()
        self.emit(OP_VARIABLE, ptr_type, i, storage_class)
        return i

    def build_words(self):
        words = list(self._header)
        for _, w in self.blocks:
            words.extend(w)
        Module._inst(words, OP_FUNCTION_END)
        return words


# ----------------------------------------------------------------------------
# Shared prologue: module scaffolding + buffers + push constants + entry block
# ----------------------------------------------------------------------------
class ShaderCtx:
    def __init__(self, a_is_u32_words):
        m = Module()
        self.m = m
        m.add_capability(CAP_SHADER)
        m.set_memory_model(ADDR_LOGICAL, MEM_GLSL450)

        # base types
        self.t_void = m.type_void()
        self.t_bool = m.type_bool()
        self.t_u32 = m.type_int(32, 0)
        self.t_i32 = m.type_int(32, 1)
        self.t_f32 = m.type_float(32)
        self.t_v3u32 = m.type_vector(self.t_u32, 3)

        # SSBO struct types (std430, Block-decorated)
        t_rta_f32 = m.type_runtime_array(self.t_f32, 4)
        self.t_buf_f32 = m.type_struct([t_rta_f32], [0], block=True)
        t_ptr_buf_f32 = m.type_pointer(SC_STORAGE_BUFFER, self.t_buf_f32)

        # A element type: f32 for matmul_f32; u32 word view for matmul_q8_0
        # (Q8_0 blocks are 34 bytes and not word-aligned, so the shader does
        # manual byte extraction from u32 words).
        self.a_is_u32_words = a_is_u32_words
        if a_is_u32_words:
            t_rta_a = m.type_runtime_array(self.t_u32, 4)
            t_buf_a = m.type_struct([t_rta_a], [0], block=True)
            t_ptr_buf_a = m.type_pointer(SC_STORAGE_BUFFER, t_buf_a)
        else:
            t_ptr_buf_a = t_ptr_buf_f32

        self.var_a = m.global_variable(t_ptr_buf_a, SC_STORAGE_BUFFER, "bufA")
        m.decorate(self.var_a, DEC_DESCRIPTOR_SET, 0)
        m.decorate(self.var_a, DEC_BINDING, 0)
        m.decorate(self.var_a, DEC_NON_WRITABLE)

        self.var_b = m.global_variable(t_ptr_buf_f32, SC_STORAGE_BUFFER, "bufB")
        m.decorate(self.var_b, DEC_DESCRIPTOR_SET, 0)
        m.decorate(self.var_b, DEC_BINDING, 1)
        m.decorate(self.var_b, DEC_NON_WRITABLE)

        self.var_c = m.global_variable(t_ptr_buf_f32, SC_STORAGE_BUFFER, "bufC")
        m.decorate(self.var_c, DEC_DESCRIPTOR_SET, 0)
        m.decorate(self.var_c, DEC_BINDING, 2)
        m.decorate(self.var_c, DEC_NON_READABLE)

        # push constants { u32 m, k, n }
        self.t_pc = m.type_struct([self.t_u32] * 3, [0, 4, 8], block=True)
        t_ptr_pc = m.type_pointer(SC_PUSH_CONSTANT, self.t_pc)
        self.var_pc = m.global_variable(t_ptr_pc, SC_PUSH_CONSTANT, "pc")
        self.t_ptr_pc_u32 = m.type_pointer(SC_PUSH_CONSTANT, self.t_u32)

        # gl_GlobalInvocationID
        t_ptr_in_v3u32 = m.type_pointer(SC_INPUT, self.t_v3u32)
        self.var_gid = m.global_variable(t_ptr_in_v3u32, SC_INPUT,
                                         "gl_GlobalInvocationID")
        m.decorate(self.var_gid, DEC_BUILTIN, BUILTIN_GLOBAL_INVOCATION_ID)

        # pointers used by bodies
        self.t_ptr_ssbo_f32 = m.type_pointer(SC_STORAGE_BUFFER, self.t_f32)
        self.t_ptr_ssbo_u32 = m.type_pointer(SC_STORAGE_BUFFER, self.t_u32)
        self.t_ptr_fn_u32 = m.type_pointer(SC_FUNCTION, self.t_u32)
        self.t_ptr_fn_f32 = m.type_pointer(SC_FUNCTION, self.t_f32)
        self.t_fn_void = m.type_function(self.t_void)

        # frequently used constants
        self.c0 = m.const_u32(0)
        self.c1 = m.const_u32(1)
        self.c2 = m.const_u32(2)
        self.f0 = m.const_f32(0.0)

    # -- common instruction helpers -----------------------------------------
    def load_pc_field(self, fb, member_const):
        p = fb.result(self.t_ptr_pc_u32, OP_ACCESS_CHAIN, self.var_pc,
                      member_const)
        return fb.result(self.t_u32, OP_LOAD, p)

    def begin_main(self, local_ptr_types=()):
        """Emit function header + entry block (locals come first).

        local_ptr_types: pointer types for function-local OpVariables, which
        must be the first instructions of the entry block.

        Returns (fb, labels dict, ids dict) with idx/m/k/n/oob computed and
        the in-bounds branch emitted; body continues at labels['work'].
        ids["locals"] holds the declared local variable ids in order.
        """
        m = self.m
        fb = FunctionBuilder(m, self.t_void, self.t_fn_void, "main")
        labels = {k: fb.new_label() for k in
                  ("entry", "work", "ret")}
        fb.begin_block(labels["entry"])
        ids = {}
        ids["locals"] = [fb.declare_local(p) for p in local_ptr_types]
        ids["gid"] = fb.result(self.t_v3u32, OP_LOAD, self.var_gid)
        ids["idx"] = fb.result(self.t_u32, OP_COMPOSITE_EXTRACT,
                               ids["gid"], 0)
        ids["m"] = self.load_pc_field(fb, self.c0)
        ids["k"] = self.load_pc_field(fb, self.c1)
        ids["n"] = self.load_pc_field(fb, self.c2)
        total = fb.result(self.t_u32, OP_I_MUL, ids["m"], ids["n"])
        ids["oob"] = fb.result(self.t_bool, OP_U_GREATER_THAN_EQUAL,
                               ids["idx"], total)
        fb.emit(OP_SELECTION_MERGE, labels["ret"], NONE)
        fb.emit(OP_BRANCH_CONDITIONAL, ids["oob"], labels["ret"],
                labels["work"])
        return fb, labels, ids

    def finish(self, fb):
        m = self.m
        m.add_function(fb.build_words())
        m.add_entry_point(EXEC_GL_COMPUTE, fb.fn_id, "main", [self.var_gid])
        m.add_execution_mode(fb.fn_id, MODE_LOCAL_SIZE, LOCAL_SIZE_X, 1, 1)
        return m


# ----------------------------------------------------------------------------
# (a) matmul_f32:  C[idx] = sum_kk A[i*k+kk] * B[kk*n+j],  idx = i*n + j
# ----------------------------------------------------------------------------
def build_matmul_f32():
    ctx = ShaderCtx(a_is_u32_words=False)
    m = ctx.m
    fb, lb, ids = ctx.begin_main(local_ptr_types=[ctx.t_ptr_fn_f32,
                                                  ctx.t_ptr_fn_u32])
    acc, kk = ids["locals"]
    u32, f32, boolt = ctx.t_u32, ctx.t_f32, ctx.t_bool
    idx, k, n = ids["idx"], ids["k"], ids["n"]

    lh, lc, lbd, lct, lx = (fb.new_label() for _ in range(5))

    # work block: i = idx / n; j = idx - i*n; acc = 0; kk = 0
    fb.begin_block(lb["work"])
    i = fb.result(u32, OP_U_DIV, idx, n)
    in_ = fb.result(u32, OP_I_MUL, i, n)
    j = fb.result(u32, OP_I_SUB, idx, in_)
    fb.emit(OP_STORE, acc, ctx.f0)
    fb.emit(OP_STORE, kk, ctx.c0)
    fb.emit(OP_BRANCH, lh)

    # loop header
    fb.begin_block(lh)
    fb.emit(OP_LOOP_MERGE, lx, lct, NONE)
    fb.emit(OP_BRANCH, lc)

    # condition: kk < k
    fb.begin_block(lc)
    kkv = fb.result(u32, OP_LOAD, kk)
    cond = fb.result(boolt, OP_U_LESS_THAN, kkv, k)
    fb.emit(OP_BRANCH_CONDITIONAL, cond, lbd, lx)

    # body: acc += A[i*k+kk] * B[kk*n+j]
    fb.begin_block(lbd)
    ik = fb.result(u32, OP_I_MUL, i, k)
    aidx = fb.result(u32, OP_I_ADD, ik, kkv)
    pa = fb.result(ctx.t_ptr_ssbo_f32, OP_ACCESS_CHAIN, ctx.var_a, ctx.c0, aidx)
    av = fb.result(f32, OP_LOAD, pa)
    kkn = fb.result(u32, OP_I_MUL, kkv, n)
    bidx = fb.result(u32, OP_I_ADD, kkn, j)
    pb = fb.result(ctx.t_ptr_ssbo_f32, OP_ACCESS_CHAIN, ctx.var_b, ctx.c0, bidx)
    bv = fb.result(f32, OP_LOAD, pb)
    accv = fb.result(f32, OP_LOAD, acc)
    prod = fb.result(f32, OP_F_MUL, av, bv)
    acc2 = fb.result(f32, OP_F_ADD, accv, prod)
    fb.emit(OP_STORE, acc, acc2)
    fb.emit(OP_BRANCH, lct)

    # continue: kk++
    fb.begin_block(lct)
    kkv2 = fb.result(u32, OP_LOAD, kk)
    kk3 = fb.result(u32, OP_I_ADD, kkv2, ctx.c1)
    fb.emit(OP_STORE, kk, kk3)
    fb.emit(OP_BRANCH, lh)

    # exit: C[idx] = acc
    fb.begin_block(lx)
    accf = fb.result(f32, OP_LOAD, acc)
    pc = fb.result(ctx.t_ptr_ssbo_f32, OP_ACCESS_CHAIN, ctx.var_c, ctx.c0, idx)
    fb.emit(OP_STORE, pc, accf)
    fb.emit(OP_BRANCH, lb["ret"])

    fb.begin_block(lb["ret"])
    fb.emit(OP_RETURN)

    ctx.finish(fb)
    return m


# ----------------------------------------------------------------------------
# (b) matmul_q8_0: A = ggml Q8_0 blocks (fp16 scale + 32 x int8, 34 B/block)
# ----------------------------------------------------------------------------
Q8_0_BLOCK_BYTES = 34
QK8_0 = 32


def _fp16_to_fp32(ctx, fb, h):
    """Emit branch-free fp16 (u32 bits) -> fp32 conversion. Returns f32 id.

    mag = exp==0  ? mant * 2^-24                    (denormal/zero, exact)
        : exp==31 ? inf/nan bit pattern
        :           normal: bitcast(((exp+112)<<23) | (mant<<13))
    result = sign ? -mag : mag
    """
    m = ctx.m
    u32, f32, boolt = ctx.t_u32, ctx.t_f32, ctx.t_bool
    c = {
        "15": m.const_u32(15), "1": ctx.c1, "10": m.const_u32(10),
        "0x1f": m.const_u32(0x1F), "0x3ff": m.const_u32(0x3FF),
        "112": m.const_u32(112), "23": m.const_u32(23), "13": m.const_u32(13),
        "inf": m.const_u32(0x7F800000),
    }
    f_2m24 = m.const_f32(5.9604644775390625e-08)  # 2^-24

    sign = fb.result(u32, OP_SHIFT_RIGHT_LOGICAL, h, c["15"])
    sign = fb.result(u32, OP_BITWISE_AND, sign, c["1"])
    exp = fb.result(u32, OP_SHIFT_RIGHT_LOGICAL, h, c["10"])
    exp = fb.result(u32, OP_BITWISE_AND, exp, c["0x1f"])
    mant = fb.result(u32, OP_BITWISE_AND, h, c["0x3ff"])

    # normal path
    e32 = fb.result(u32, OP_I_ADD, exp, c["112"])
    e32s = fb.result(u32, OP_SHIFT_LEFT_LOGICAL, e32, c["23"])
    m13 = fb.result(u32, OP_SHIFT_LEFT_LOGICAL, mant, c["13"])
    bits_n = fb.result(u32, OP_BITWISE_OR, e32s, m13)
    fn = fb.result(f32, OP_BITCAST, bits_n)

    # denormal / zero path: mant * 2^-24 (exact in fp32)
    fmant = fb.result(f32, OP_CONVERT_U_TO_F, mant)
    fd = fb.result(f32, OP_F_MUL, fmant, f_2m24)

    # inf/nan path
    bits_i = fb.result(u32, OP_BITWISE_OR, c["inf"], m13)
    fi = fb.result(f32, OP_BITCAST, bits_i)

    zero = m.const_u32(0)
    c31 = m.const_u32(31)
    is_denorm = fb.result(boolt, OP_I_EQUAL, exp, zero)
    is_inf = fb.result(boolt, OP_I_EQUAL, exp, c31)
    mag1 = fb.result(f32, OP_SELECT, is_inf, fi, fn)
    mag = fb.result(f32, OP_SELECT, is_denorm, fd, mag1)

    has_sign = fb.result(boolt, OP_I_NOT_EQUAL, sign, zero)
    neg = fb.result(f32, OP_F_NEGATE, mag)
    return fb.result(f32, OP_SELECT, has_sign, neg, mag)


def _load_u16_le(ctx, fb, byte_off):
    """Load a little-endian u16 at an arbitrary byte offset of SSBO A."""
    m = ctx.m
    u32 = ctx.t_u32
    c2 = m.const_u32(2)
    c3 = m.const_u32(3)
    c4 = m.const_u32(4)
    cmask = m.const_u32(0xFFFF)
    woff = fb.result(u32, OP_SHIFT_RIGHT_LOGICAL, byte_off, c2)
    pw = fb.result(ctx.t_ptr_ssbo_u32, OP_ACCESS_CHAIN, ctx.var_a, ctx.c0, woff)
    w = fb.result(u32, OP_LOAD, pw)
    bsel = fb.result(u32, OP_BITWISE_AND, byte_off, c2)
    sh = fb.result(u32, OP_SHIFT_LEFT_LOGICAL, bsel, c3)
    shifted = fb.result(u32, OP_SHIFT_RIGHT_LOGICAL, w, sh)
    return fb.result(u32, OP_BITWISE_AND, shifted, cmask)


def _load_i8(ctx, fb, byte_off):
    """Load a signed i8 at an arbitrary byte offset of SSBO A. Returns i32."""
    m = ctx.m
    u32, i32 = ctx.t_u32, ctx.t_i32
    c2 = m.const_u32(2)
    c3 = m.const_u32(3)
    cmask = m.const_u32(0xFF)
    c24 = m.const_i32(24)
    woff = fb.result(u32, OP_SHIFT_RIGHT_LOGICAL, byte_off, c2)
    pw = fb.result(ctx.t_ptr_ssbo_u32, OP_ACCESS_CHAIN, ctx.var_a, ctx.c0, woff)
    w = fb.result(u32, OP_LOAD, pw)
    bsel = fb.result(u32, OP_BITWISE_AND, byte_off, c3)
    sh = fb.result(u32, OP_SHIFT_LEFT_LOGICAL, bsel, c3)
    shifted = fb.result(u32, OP_SHIFT_RIGHT_LOGICAL, w, sh)
    byte = fb.result(u32, OP_BITWISE_AND, shifted, cmask)
    # sign-extend: (i32)(byte << 24) >> 24 (arithmetic)
    qb = fb.result(i32, OP_BITCAST, byte)
    q24 = fb.result(i32, OP_SHIFT_LEFT_LOGICAL, qb, c24)
    return fb.result(i32, OP_SHIFT_RIGHT_ARITHMETIC, q24, c24)


def build_matmul_q8_0():
    ctx = ShaderCtx(a_is_u32_words=True)
    m = ctx.m
    fb, lb, ids = ctx.begin_main(local_ptr_types=[ctx.t_ptr_fn_f32,
                                                  ctx.t_ptr_fn_u32,
                                                  ctx.t_ptr_fn_f32,
                                                  ctx.t_ptr_fn_u32])
    acc, bi, bacc, jj = ids["locals"]
    u32, f32, boolt = ctx.t_u32, ctx.t_f32, ctx.t_bool
    idx, k, n = ids["idx"], ids["k"], ids["n"]

    c5 = m.const_u32(5)
    c32 = m.const_u32(QK8_0)
    c34 = m.const_u32(Q8_0_BLOCK_BYTES)

    (oh, oc, obd, ih, ic, ibd, ict, ix, oct_, ox) = (fb.new_label()
                                                    for _ in range(10))

    # work: i = idx/n; j = idx%n; nb = k>>5; row_base = i*nb*34
    fb.begin_block(lb["work"])
    i = fb.result(u32, OP_U_DIV, idx, n)
    in_ = fb.result(u32, OP_I_MUL, i, n)
    j = fb.result(u32, OP_I_SUB, idx, in_)
    nb = fb.result(u32, OP_SHIFT_RIGHT_LOGICAL, k, c5)
    inb = fb.result(u32, OP_I_MUL, i, nb)
    row_base = fb.result(u32, OP_I_MUL, inb, c34)
    fb.emit(OP_STORE, acc, ctx.f0)
    fb.emit(OP_STORE, bi, ctx.c0)
    fb.emit(OP_BRANCH, oh)

    # outer header (block loop)
    fb.begin_block(oh)
    fb.emit(OP_LOOP_MERGE, ox, oct_, NONE)
    fb.emit(OP_BRANCH, oc)

    fb.begin_block(oc)
    biv = fb.result(u32, OP_LOAD, bi)
    ocond = fb.result(boolt, OP_U_LESS_THAN, biv, nb)
    fb.emit(OP_BRANCH_CONDITIONAL, ocond, obd, ox)

    # outer body: block_base, d = fp16 scale
    fb.begin_block(obd)
    bi34 = fb.result(u32, OP_I_MUL, biv, c34)
    block_base = fb.result(u32, OP_I_ADD, row_base, bi34)
    hb = _load_u16_le(ctx, fb, block_base)
    d = _fp16_to_fp32(ctx, fb, hb)
    fb.emit(OP_STORE, bacc, ctx.f0)
    fb.emit(OP_STORE, jj, ctx.c0)
    fb.emit(OP_BRANCH, ih)

    # inner header (element loop, 32 per block)
    fb.begin_block(ih)
    fb.emit(OP_LOOP_MERGE, ix, ict, NONE)
    fb.emit(OP_BRANCH, ic)

    fb.begin_block(ic)
    jjv = fb.result(u32, OP_LOAD, jj)
    icond = fb.result(boolt, OP_U_LESS_THAN, jjv, c32)
    fb.emit(OP_BRANCH_CONDITIONAL, icond, ibd, ix)

    # inner body: bacc += float(qs[jj]) * B[(bi*32+jj)*n + j]
    fb.begin_block(ibd)
    base2 = fb.result(u32, OP_I_ADD, block_base, ctx.c2)
    boff = fb.result(u32, OP_I_ADD, base2, jjv)
    q = _load_i8(ctx, fb, boff)
    fq = fb.result(f32, OP_CONVERT_S_TO_F, q)
    bi32 = fb.result(u32, OP_I_MUL, biv, c32)
    row = fb.result(u32, OP_I_ADD, bi32, jjv)
    rown = fb.result(u32, OP_I_MUL, row, n)
    bidx = fb.result(u32, OP_I_ADD, rown, j)
    pb = fb.result(ctx.t_ptr_ssbo_f32, OP_ACCESS_CHAIN, ctx.var_b, ctx.c0, bidx)
    bv = fb.result(f32, OP_LOAD, pb)
    baccv = fb.result(f32, OP_LOAD, bacc)
    prod = fb.result(f32, OP_F_MUL, fq, bv)
    nbacc = fb.result(f32, OP_F_ADD, baccv, prod)
    fb.emit(OP_STORE, bacc, nbacc)
    fb.emit(OP_BRANCH, ict)

    fb.begin_block(ict)
    jjv2 = fb.result(u32, OP_LOAD, jj)
    jj3 = fb.result(u32, OP_I_ADD, jjv2, ctx.c1)
    fb.emit(OP_STORE, jj, jj3)
    fb.emit(OP_BRANCH, ih)

    # inner exit: acc += d * bacc
    fb.begin_block(ix)
    baccf = fb.result(f32, OP_LOAD, bacc)
    db = fb.result(f32, OP_F_MUL, d, baccf)
    accv = fb.result(f32, OP_LOAD, acc)
    acc2 = fb.result(f32, OP_F_ADD, accv, db)
    fb.emit(OP_STORE, acc, acc2)
    fb.emit(OP_BRANCH, oct_)

    fb.begin_block(oct_)
    biv2 = fb.result(u32, OP_LOAD, bi)
    bi3 = fb.result(u32, OP_I_ADD, biv2, ctx.c1)
    fb.emit(OP_STORE, bi, bi3)
    fb.emit(OP_BRANCH, oh)

    # outer exit: C[idx] = acc
    fb.begin_block(ox)
    accf = fb.result(f32, OP_LOAD, acc)
    pc = fb.result(ctx.t_ptr_ssbo_f32, OP_ACCESS_CHAIN, ctx.var_c, ctx.c0, idx)
    fb.emit(OP_STORE, pc, accf)
    fb.emit(OP_BRANCH, lb["ret"])

    fb.begin_block(lb["ret"])
    fb.emit(OP_RETURN)

    ctx.finish(fb)
    return m


# ----------------------------------------------------------------------------
# Output: kernel/ai/vk_shaders.h
# ----------------------------------------------------------------------------
HEADER_TEMPLATE = """/* Vulkan compute shaders for embodiOS — GENERATED FILE, do not edit.
 *
 * Generated by tools/spirv_gen.py (hand-assembled SPIR-V 1.3, no GLSL
 * compiler available in the build environment). Regenerate with:
 *     python3 tools/spirv_gen.py
 *
 * Modules:
 *   vk_spv_matmul_f32   : C[m*n] = A[m*k] * B[k*n], fp32, local_size_x = 64,
 *                         push constants {{u32 m, u32 k, u32 n}}.
 *   vk_spv_matmul_q8_0  : same, but A is ggml Q8_0 blocks (34 B per 32
 *                         values: u16 fp16 scale + 32 x int8), dequantized
 *                         in-shader; k must be a multiple of 32.
 *
 * Validated end-to-end on lavapipe by tools/host_test_vulkan.c.
 */

#ifndef EMBODIOS_VK_SHADERS_H
#define EMBODIOS_VK_SHADERS_H

#include <stdint.h>

{arrays}

#endif /* EMBODIOS_VK_SHADERS_H */
"""


def _format_array(name, words):
    lines = ["static const uint32_t %s[] = {" % name]
    for off in range(0, len(words), 8):
        chunk = words[off:off + 8]
        lines.append("    " + ", ".join("0x%08xu" % w for w in chunk) + ",")
    lines.append("};")
    lines.append("static const uint32_t %s_word_count = %du;" %
                 (name, len(words)))
    return "\n".join(lines)


def _sanity_check(name, words):
    assert words[0] == 0x07230203, "%s: bad magic" % name
    assert (words[1] >> 16) == 1 and (words[1] & 0xFF00) == 0x0300, \
        "%s: bad version" % name
    assert words[3] > 1, "%s: bad id bound" % name
    # walk the instruction stream to verify structural integrity
    pos = 5
    while pos < len(words):
        wc = words[pos] >> 16
        assert wc >= 1, "%s: zero word-count at %d" % (name, pos)
        pos += wc
    assert pos == len(words), "%s: trailing garbage" % name
    # exactly one entry point, one function
    ops = [words[p] & 0xFFFF for p in _iter_inst_offsets(words)]
    assert ops.count(OP_ENTRY_POINT) == 1
    assert ops.count(OP_FUNCTION) == 1
    assert ops.count(OP_FUNCTION_END) == 1


def _iter_inst_offsets(words):
    pos = 5
    while pos < len(words):
        yield pos
        pos += words[pos] >> 16


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(script_dir)
    out_path = (sys.argv[1] if len(sys.argv) > 1
                else os.path.join(repo_root, "kernel", "ai", "vk_shaders.h"))

    shaders = [
        ("vk_spv_matmul_f32", build_matmul_f32().assemble()),
        ("vk_spv_matmul_q8_0", build_matmul_q8_0().assemble()),
    ]
    arrays = []
    for name, words in shaders:
        _sanity_check(name, words)
        arrays.append(_format_array(name, words))
        print("%s: %d words (%d bytes)" % (name, len(words), 4 * len(words)))
        # External validation when spirv-tools is available (optional).
        import shutil
        import subprocess
        import tempfile
        spirv_val = shutil.which("spirv-val")
        if spirv_val:
            with tempfile.NamedTemporaryFile(suffix=".spv", delete=False) as tf:
                tf.write(struct.pack("<%dI" % len(words), *words))
                tmp = tf.name
            r = subprocess.run([spirv_val, "--target-env", "vulkan1.1", tmp],
                               capture_output=True, text=True)
            os.unlink(tmp)
            if r.returncode != 0:
                sys.stderr.write("spirv-val FAILED for %s:\n%s" % (name, r.stderr))
                sys.exit(1)
            print("%s: spirv-val OK (vulkan1.1)" % name)

    with open(out_path, "w") as f:
        f.write(HEADER_TEMPLATE.format(arrays="\n\n".join(arrays)))
    print("wrote %s" % out_path)


if __name__ == "__main__":
    main()
