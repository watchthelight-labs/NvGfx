/*
 * NvGfx (N-81): framebuffer fetch emulated by copies.
 *
 * With framebuffer fetch off (WindowServer always, apps under NVMTL_NO_FBFETCH=1) a fragment shader that reads its
 * colour attachments ([[color(n)]]: an input attachment at set 3, binding 192+n) read unbound descriptors, and
 * CoreAnimation's material composite (fixed_frag_lpf_cpf: Dock, notifications) drew flat 32 px tiles. Turning fetch on
 * in WindowServer put every colour image in input-attachment form and blanked the desktop (N-80).
 *
 * Instead, only such a pipeline is changed: its shader reads a sampled 2D image at the pixel's own position
 * (OpImageFetch at FragCoord.xy, Lod 0) instead of the input attachment, and before each of its draws the encoder ends
 * the pass, copies the attachment to a scratch image, resumes the pass and binds the copy at set 3. Every other pipeline
 * and render pass stays as it was.
 *
 * nvgfx_spirv_fetch_to_texture() rewrites the module. It returns a malloc'd module (or NULL if it cannot, e.g. a
 * multisampled subpass input) and its size in words.
 */
#ifndef NVGFX_FETCH_COPY_H
#define NVGFX_FETCH_COPY_H
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

enum { NVS_OpNop = 0, NVS_OpEntryPoint = 15, NVS_OpCapability = 17, NVS_OpTypeInt = 21, NVS_OpTypeFloat = 22,
       NVS_OpTypeVector = 23, NVS_OpTypeImage = 25, NVS_OpTypePointer = 32, NVS_OpConstant = 43, NVS_OpFunction = 54,
       NVS_OpVariable = 59, NVS_OpLoad = 61, NVS_OpDecorate = 71, NVS_OpMemberDecorate = 72, NVS_OpDecorationGroup = 73,
       NVS_OpGroupDecorate = 74, NVS_OpGroupMemberDecorate = 75, NVS_OpVectorShuffle = 79, NVS_OpImageFetch = 95,
       NVS_OpImageRead = 98, NVS_OpConvertFToS = 110, NVS_OpDecorateId = 332, NVS_OpDecorateString = 5632,
       NVS_OpMemberDecorateString = 5633 };
#define NVS_DIM_2D 1u
#define NVS_DIM_SUBPASS 6u
#define NVS_DEC_BUILTIN 11u
#define NVS_DEC_INPUT_ATTACHMENT_INDEX 43u
#define NVS_BUILTIN_FRAGCOORD 15u
#define NVS_CAP_INPUT_ATTACHMENT 40u
#define NVS_SC_INPUT 1u

typedef struct { uint32_t *w; size_t n, cap; int oom; } nvs_out;
static void nvs_put(nvs_out *o, const uint32_t *src, size_t n)
{
    if (o->oom) return;
    if (o->n + n > o->cap) {
        size_t cap = o->cap ? o->cap : 1024; while (cap < o->n + n) cap *= 2;
        uint32_t *g = (uint32_t *)realloc(o->w, cap * 4); if (!g) { o->oom = 1; return; }
        o->w = g; o->cap = cap;
    }
    memcpy(o->w + o->n, src, n * 4); o->n += n;
}
static void nvs_ins(nvs_out *o, uint32_t op, int nops, ...)
{
    uint32_t b[16]; b[0] = ((uint32_t)(nops + 1) << 16) | op;
    va_list a; va_start(a, nops); for (int i = 0; i < nops && i < 15; i++) b[i + 1] = va_arg(a, uint32_t); va_end(a);
    nvs_put(o, b, (size_t)nops + 1);
}

static uint32_t *nvgfx_spirv_fetch_to_texture(const uint32_t *in, size_t nw, size_t *outw)
{
    *outw = 0;
    if (nw < 5 || in[0] != 0x07230203u) return NULL;
    uint32_t bound = in[3];
    // ---- scan
    uint32_t t_float = 0, t_v2f = 0, t_v4f = 0, t_int = 0, t_v2i = 0, t_ptr_in_v4f = 0, v_fc = 0;
    size_t first_fn = 0, last_annot = 0, first_type = 0, entry = 0; int n_entry = 0;
    uint32_t sub_types[16]; int n_sub = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffffu;
        if (!wc || i + wc > nw) return NULL;
        switch (op) {
        case NVS_OpEntryPoint: entry = i; n_entry++; break;
        case NVS_OpTypeFloat: if (wc >= 3 && in[i + 2] == 32 && !t_float) t_float = in[i + 1]; break;
        case NVS_OpTypeInt: if (wc >= 4 && in[i + 2] == 32 && (!t_int || in[i + 3] == 1)) t_int = in[i + 1]; break;
        case NVS_OpTypeImage:
            if (wc >= 9 && in[i + 3] == NVS_DIM_SUBPASS) {
                if (in[i + 6] != 0) return NULL;          // multisampled subpass input: not emulated
                if (n_sub < 16) sub_types[n_sub++] = in[i + 1];
            }
            break;
        case NVS_OpDecorate:
            if (wc >= 4 && in[i + 2] == NVS_DEC_BUILTIN && in[i + 3] == NVS_BUILTIN_FRAGCOORD) v_fc = in[i + 1];
            break;
        case NVS_OpFunction: if (!first_fn) first_fn = i; break;
        default: break;
        }
        if (op == NVS_OpDecorate || op == NVS_OpMemberDecorate || op == NVS_OpDecorationGroup || op == NVS_OpGroupDecorate ||
            op == NVS_OpGroupMemberDecorate || op == NVS_OpDecorateId || op == NVS_OpDecorateString || op == NVS_OpMemberDecorateString)
            last_annot = i + wc;
        if (!first_type && op >= 19 && op <= 39) first_type = i;   // OpTypeVoid..OpTypeForwardPointer: the types section starts
        i += wc;
    }
    if (!n_sub || n_entry != 1 || !first_fn || !t_float) return NULL;
    // vectors (after the scalars are known)
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffffu;
        if (op == NVS_OpTypeVector && wc >= 4) {
            if (in[i + 2] == t_float && in[i + 3] == 2 && !t_v2f) t_v2f = in[i + 1];
            if (in[i + 2] == t_float && in[i + 3] == 4 && !t_v4f) t_v4f = in[i + 1];
            if (t_int && in[i + 2] == t_int && in[i + 3] == 2 && !t_v2i) t_v2i = in[i + 1];
        }
        if (op == NVS_OpTypePointer && wc >= 4 && in[i + 2] == NVS_SC_INPUT) { /* matched below once v4f is known */ }
        i += wc;
    }
    if (t_v4f) for (size_t i = 5; i < nw; ) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffffu;
        if (op == NVS_OpTypePointer && wc >= 4 && in[i + 2] == NVS_SC_INPUT && in[i + 3] == t_v4f) { t_ptr_in_v4f = in[i + 1]; break; }
        i += wc;
    }
    // ids: 1 = a subpass image type, 2 = a loaded subpass image, 3 = a pointer to one, 4 = a variable of such a pointer
    uint32_t *is_sub = (uint32_t *)calloc(bound, sizeof(uint32_t));
    uint32_t *remap = (uint32_t *)calloc(bound, sizeof(uint32_t));   // subpass type -> an identical sampled 2D type already declared
    if (!is_sub || !remap) { free(is_sub); free(remap); return NULL; }
    for (int k = 0; k < n_sub; k++) if (sub_types[k] < bound) is_sub[sub_types[k]] = 1;
    for (int k = 0; k < n_sub; k++) {
        uint32_t st = 0;   // the subpass type's sampled type
        for (size_t i = 5; i < nw; ) { uint32_t wc = in[i] >> 16; if ((in[i] & 0xffffu) == NVS_OpTypeImage && in[i + 1] == sub_types[k]) { st = in[i + 2]; break; } i += wc; }
        for (size_t i = 5; i < nw && st; ) {
            uint32_t wc = in[i] >> 16;
            if ((in[i] & 0xffffu) == NVS_OpTypeImage && wc >= 9 && in[i + 2] == st && in[i + 3] == NVS_DIM_2D && in[i + 4] == 0 &&
                in[i + 5] == 0 && in[i + 6] == 0 && in[i + 7] == 1 && in[i + 8] == 0 /* Unknown */ && wc == 9) { remap[sub_types[k]] = in[i + 1]; break; }
            i += wc;
        }
    }
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffffu;
        if (op == NVS_OpTypePointer && wc >= 4 && in[i + 3] < bound && is_sub[in[i + 3]] == 1 && in[i + 1] < bound) is_sub[in[i + 1]] = 3;
        i += wc;
    }
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffffu;
        if (op == NVS_OpVariable && wc >= 4 && in[i + 1] < bound && is_sub[in[i + 1]] == 3 && in[i + 2] < bound) is_sub[in[i + 2]] = 4;
        i += wc;
    }
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffffu;
        if (op == NVS_OpLoad && wc >= 4 && in[i + 1] < bound && is_sub[in[i + 1]] == 1 && in[i + 2] < bound) is_sub[in[i + 2]] = 2;
        i += wc;
    }
    // ---- new ids
    const uint32_t n_float = 0, need_int = !t_int;
    (void)n_float;
    uint32_t id_int = t_int ? t_int : bound++;
    uint32_t id_v2f = t_v2f ? t_v2f : bound++;
    uint32_t id_v4f = t_v4f ? t_v4f : bound++;
    uint32_t id_v2i = t_v2i ? t_v2i : bound++;
    uint32_t id_ptr = t_ptr_in_v4f ? t_ptr_in_v4f : bound++;
    uint32_t id_fc = v_fc ? v_fc : bound++;
    uint32_t id_zero = bound++;
    // a FragCoord that exists is assumed to be an Input v4float variable, as the translator emits it
    // ---- rebuild
    nvs_out o = { 0 };
    nvs_put(&o, in, 5);
    size_t insert_types_at = first_fn;   // new types, constant and variable go just before the first function
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffffu;
        if (i == insert_types_at) {
            if (need_int) nvs_ins(&o, NVS_OpTypeInt, 3, id_int, 32u, 1u);
            if (!t_v2f) nvs_ins(&o, NVS_OpTypeVector, 3, id_v2f, t_float, 2u);
            if (!t_v4f) nvs_ins(&o, NVS_OpTypeVector, 3, id_v4f, t_float, 4u);
            if (!t_v2i) nvs_ins(&o, NVS_OpTypeVector, 3, id_v2i, id_int, 2u);
            if (!t_ptr_in_v4f) nvs_ins(&o, NVS_OpTypePointer, 3, id_ptr, NVS_SC_INPUT, id_v4f);
            if (!v_fc) nvs_ins(&o, NVS_OpVariable, 3, id_ptr, id_fc, NVS_SC_INPUT);
            nvs_ins(&o, NVS_OpConstant, 3, id_int, id_zero, 0u);
        }
        if (op == NVS_OpCapability && wc == 2 && in[i + 1] == NVS_CAP_INPUT_ATTACHMENT) { i += wc; continue; }
        if (op == NVS_OpDecorate && wc >= 4 && in[i + 2] == NVS_DEC_INPUT_ATTACHMENT_INDEX) { i += wc; continue; }
        if (op == NVS_OpDecorate && wc == 3 && in[i + 1] < bound && is_sub[in[i + 1]] == 4 && in[i + 2] == 24 /* NonWritable */) { i += wc; continue; }
        if (op == NVS_OpTypeImage && wc >= 9 && in[i + 3] == NVS_DIM_SUBPASS && remap[in[i + 1]]) { i += wc; continue; }   // its twin stays
        if (op == NVS_OpTypePointer && wc >= 4 && in[i + 3] < bound && remap[in[i + 3]]) {
            uint32_t b[4] = { in[i], in[i + 1], in[i + 2], remap[in[i + 3]] }; nvs_put(&o, b, 4); i += wc; continue; }
        if (op == NVS_OpLoad && wc >= 4 && in[i + 1] < bound && remap[in[i + 1]]) {
            uint32_t b[16]; memcpy(b, in + i, (size_t)(wc < 16 ? wc : 16) * 4); b[1] = remap[in[i + 1]]; nvs_put(&o, b, wc); i += wc; continue; }
        if (op == NVS_OpEntryPoint && !v_fc) {
            uint32_t hdr = ((wc + 1) << 16) | op;
            nvs_put(&o, &hdr, 1); nvs_put(&o, in + i + 1, wc - 1); nvs_put(&o, &id_fc, 1);
            i += wc; continue;
        }
        if (op == NVS_OpTypeImage && wc >= 9 && in[i + 3] == NVS_DIM_SUBPASS) {
            uint32_t b[16]; memcpy(b, in + i, (size_t)(wc < 16 ? wc : 16) * 4);
            b[3] = NVS_DIM_2D; b[7] = 1;   // a sampled 2D image, read with OpImageFetch
            nvs_put(&o, b, wc); i += wc; continue;
        }
        if (op == NVS_OpImageRead && wc >= 5 && in[i + 3] < bound && is_sub[in[i + 3]] == 2) {
            uint32_t a = bound++, s = bound++, c = bound++;
            nvs_ins(&o, NVS_OpLoad, 3, id_v4f, a, id_fc);
            nvs_ins(&o, NVS_OpVectorShuffle, 6, id_v2f, s, a, a, 0u, 1u);
            nvs_ins(&o, NVS_OpConvertFToS, 3, id_v2i, c, s);
            nvs_ins(&o, NVS_OpImageFetch, 6, in[i + 1], in[i + 2], in[i + 3], c, 0x2u /* Lod */, id_zero);
            i += wc; continue;
        }
        nvs_put(&o, in + i, wc);
        if (!v_fc && last_annot && i + wc == last_annot) nvs_ins(&o, NVS_OpDecorate, 3, id_fc, NVS_DEC_BUILTIN, NVS_BUILTIN_FRAGCOORD);
        i += wc;
    }
    free(is_sub); free(remap);
    if (o.oom || (!v_fc && !last_annot)) { free(o.w); return NULL; }
    o.w[3] = bound;
    *outw = o.n;
    (void)first_type;
    return o.w;
}
#endif
