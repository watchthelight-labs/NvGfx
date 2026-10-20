// nvrm_cta.h: what a display's EDID says about colour depth and HDR.
//
// macOS decides whether to offer 10-bit colour and the "High Dynamic Range" switch from what the framebuffer
// advertises (IOFBTimingRange, the connection colour-depth attributes), and the framebuffer should advertise what the
// sink can take. NVKMS parses the same blocks for its own use but kapi does not pass the result on, so the
// framebuffer reads them from the raw EDID it already keeps. Three places carry the answer: the base block's video
// input byte (0x14: digital, and from EDID 1.4 the panel's bits per colour), the CTA-861 colorimetry data block
// (extended tag 5: BT.2020 RGB/YCC, DCI-P3) and the CTA-861 HDR static metadata data block (extended tag 6: the EOTFs
// the sink accepts, the metadata type, the three luminance codes of CTA-861.3).
//
// Pure C, no libc, every read bounds-checked (same rules as nvrm_eld.h): a short or malformed EDID leaves the struct
// zero, which means "nothing to advertise".
// <wtl> fork: nvgfx | author: watchthelight </wtl>

#ifndef NVRM_CTA_H
#define NVRM_CTA_H

#include <stdint.h>
#include <stddef.h>

enum {
    NVRM_CTA_EXT_COLORIMETRY = 5,
    NVRM_CTA_EXT_HDR_STATIC  = 6,
    // colorimetry data block, first byte
    NVRM_CTA_COL_BT2020_RGB  = 0x80, NVRM_CTA_COL_BT2020_YCC = 0x40, NVRM_CTA_COL_BT2020_CYCC = 0x20,
    // colorimetry data block, second byte
    NVRM_CTA_COL2_DCI_P3     = 0x80,
    // HDR static metadata block, EOTF byte
    NVRM_CTA_EOTF_SDR = 0x01, NVRM_CTA_EOTF_TRAD_HDR = 0x02, NVRM_CTA_EOTF_ST2084 = 0x04, NVRM_CTA_EOTF_HLG = 0x08,
    // HDR static metadata block, metadata type byte
    NVRM_CTA_SMD_TYPE1 = 0x01,
};

struct nvrm_cta_sink {
    uint8_t digital;            // base block byte 0x14 bit 7
    uint8_t bpc;                // 6, 8, 10, 12, 14, 16 from byte 0x14 bits 6:4 (EDID 1.4 and later), 0 when not stated
    uint8_t has_colorimetry;    // a colorimetry data block was found
    uint8_t colorimetry;        // its first byte (NVRM_CTA_COL_*)
    uint8_t colorimetry2;       // its second byte (NVRM_CTA_COL2_*)
    uint8_t has_hdr;            // an HDR static metadata data block was found
    uint8_t eotf;               // its EOTF byte (NVRM_CTA_EOTF_*)
    uint8_t smd_types;          // its metadata type byte (NVRM_CTA_SMD_*)
    uint8_t max_lum;            // CTA-861.3 codes, 0 when the block omits them: max luminance = 50 * 2^(code/32) cd/m2,
    uint8_t max_fal;            // max frame-average the same way, min luminance = max * (code/255)^2 / 100
    uint8_t min_lum;
};

static inline void nvrm_cta_zero(struct nvrm_cta_sink *s) { uint8_t *p = (uint8_t *)s; for (size_t i = 0; i < sizeof *s; i++) p[i] = 0; }

static inline void nvrm_cta_parse(const uint8_t *edid, size_t len, struct nvrm_cta_sink *s)
{
    nvrm_cta_zero(s);
    if (!edid || !s || len < 128) return;
    const uint8_t vin = edid[0x14];
    s->digital = (uint8_t)(vin >> 7);
    // EDID 1.3 uses bits 6:4 for DFP compatibility flags; only 1.4 and later state the colour depth there.
    if (s->digital && edid[18] == 1 && edid[19] >= 4) {
        const uint8_t code = (uint8_t)((vin >> 4) & 7);
        if (code >= 1 && code <= 6) s->bpc = (uint8_t)(4 + 2 * code);
    }
    const uint8_t ext_count = edid[126];
    for (uint8_t e = 1; e <= ext_count; e++) {
        const size_t base = (size_t)e * 128;
        if (base + 128 > len) break;
        const uint8_t *cea = edid + base;
        if (cea[0] != 0x02 || cea[1] < 3) continue;
        const uint8_t dtd = cea[2];
        if (dtd < 4 || dtd > 128) continue;
        size_t p = 4;
        while (p < dtd) {
            const uint8_t hdr = cea[p];
            const uint8_t tag = (uint8_t)(hdr >> 5), blen = (uint8_t)(hdr & 0x1F);
            if (p + 1 + blen > dtd) break;
            const uint8_t *body = cea + p + 1;
            if (tag == 7 && blen >= 1) {
                const uint8_t ext = body[0];
                if (ext == NVRM_CTA_EXT_COLORIMETRY && blen >= 3) {
                    s->has_colorimetry = 1; s->colorimetry = body[1]; s->colorimetry2 = body[2];
                } else if (ext == NVRM_CTA_EXT_HDR_STATIC && blen >= 3) {
                    s->has_hdr = 1; s->eotf = body[1]; s->smd_types = body[2];
                    if (blen >= 4) s->max_lum = body[3];
                    if (blen >= 5) s->max_fal = body[4];
                    if (blen >= 6) s->min_lum = body[5];
                }
            }
            p += (size_t)1 + blen;
        }
    }
}

static inline int nvrm_cta_sink_10bpc(const struct nvrm_cta_sink *s) { return s->bpc >= 10; }
static inline int nvrm_cta_sink_bt2020(const struct nvrm_cta_sink *s)
{ return s->has_colorimetry && (s->colorimetry & (NVRM_CTA_COL_BT2020_RGB | NVRM_CTA_COL_BT2020_YCC | NVRM_CTA_COL_BT2020_CYCC)); }
// HDR10 needs the PQ EOTF and static metadata type 1, the same test NVKMS's nvDpyIsHDRCapable applies to the EDID.
static inline int nvrm_cta_sink_pq(const struct nvrm_cta_sink *s)
{ return s->has_hdr && (s->eotf & NVRM_CTA_EOTF_ST2084) && (s->smd_types & NVRM_CTA_SMD_TYPE1); }

#endif
