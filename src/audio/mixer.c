// kestrel64-sdk — voice mixer: record list builder and the CPU reference of the RSP mixer.
//
// Arithmetic (shared by the RSP overlay and k64_mixer_run_cpu, docs/AUDIO.md), per output
// frame f of a record with p = frac + (f - start) * step, i = p >> 16, x = p & 0xFFFF:
//   v   = (s[i] * 65536 + (s[i+1] - s[i]) * x) >> 16              linear interpolation
//   t   = clamp16((v * vol * 2 + 0x8000) >> 16)                    Q15 volume (vmulf)
//   vol = vol_start + (f - start) * d                              with a RAMP record (d),
//                                                                  else d = 0
//   out = clamp16(out + t)                                         per channel (vadd)
//
// VADPCM (kgfx ADPCM overlay): each ADPCM voice decodes its stream into a ring of
// K64_ADPCM_RING samples, in groups of K64_ADPCM_GROUP (16 frames) decoded ahead of the mix
// position; the voice mixes from the ring as a looped sample. A group that crosses the end of
// a looped sample is split into decode records, the part after the loop point reads the loop
// state (slot 0). Each group's last record stores the state for the next group in its slot,
// so the records of one list read only slots written by earlier lists (or earlier records of
// the same list): the list can be run twice (RSP, then the CPU reference) with one result.
#include <k64/k64.h>

#define SRCBUF_BYTES 0x460                     // RSP source buffer (kgfx.S SRCBUF)
#define ADPCM_GROUPS (K64_ADPCM_RING / K64_ADPCM_GROUP)
#define ADPCM_GFRAMES (K64_ADPCM_GROUP / 16)

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

bool k64_adpcm_load(k64_adpcm_t *a, const void *blob) {
  const u8 *b = blob;
  if (be32(b) != 0x4B364131) return false;   // "K6A1"
  a->rate = be32(b + 4); a->len = be32(b + 8); a->loop_start = be32(b + 12);
  a->nframes = be32(b + 16); a->npred = be32(b + 20);
  a->codebook = (const s16 *)(b + be32(b + 24));
  a->frames = b + be32(b + 28);
  for (u32 i = 0; i < 8; i++) a->loop_state[i] = (s16)(b[32 + i * 2] << 8 | b[33 + i * 2]);
  return true;
}

void k64_voice_play(k64_voice_t *v, const s16 *data, u32 len, u32 loop_start, u32 step,
                    s16 vol_l, s16 vol_r) {
  K64_ASSERT(step > 0 && step < 0x40000 && len > 0);
  K64_ASSERT(loop_start == K64_NO_LOOP || loop_start < len);
  v->data = data; v->len = len; v->loop_start = loop_start;
  v->idx = 0; v->frac = 0; v->step = step;
  v->vol_l = v->tgt_l = vol_l; v->vol_r = v->tgt_r = vol_r;
  v->ramp = 0;
  v->on = true;
  v->adpcm = NULL; v->left = 0;
}

void k64_voice_play_adpcm(k64_voice_t *v, k64_adpcm_voice_t *d, const k64_adpcm_t *a, u32 step,
                          s16 vol_l, s16 vol_r) {
  K64_ASSERT(((u32)d & 15) == 0 && ((u32)a->codebook & 7) == 0);
  K64_ASSERT(a->npred >= 1 && a->npred <= 8 && a->nframes > 0);
  if (a->loop_start == K64_NO_LOOP)
    K64_ASSERT(a->len < a->nframes * 16);      // the guard sample is decoded
  else
    K64_ASSERT(a->loop_start % 16 == 0 && a->loop_start < a->len && a->len == a->nframes * 16);
  d->codebook = K64_PHYS(a->codebook);
  d->codebook_len1 = a->npred * 32 - 1;
  d->mirror = K64_PHYS(&d->ring[K64_ADPCM_RING]);
  d->self = K64_PHYS(d);
  for (u32 k = 0; k < 8; k++) {
    d->state[0][k] = a->loop_state[k];
    for (u32 g = 1; g <= ADPCM_GROUPS; g++) d->state[g][k] = 0;
  }
  d->a = a; d->dec = 0; d->src = 0; d->rd = 0; d->in = 1; d->ended = false;
  d->starts++;
  k64_dcache_writeback(d, (u32)((u8 *)d->ring - (u8 *)d));
  k64_voice_play(v, d->ring, K64_ADPCM_RING, 0, step, vol_l, vol_r);
  v->adpcm = d;
  v->left = a->len;
}

void k64_voice_volume(k64_voice_t *v, s16 vol_l, s16 vol_r, u32 frames) {
  K64_ASSERT(vol_l - v->vol_l <= 32767 && v->vol_l - vol_l <= 32767);
  K64_ASSERT(vol_r - v->vol_r <= 32767 && v->vol_r - vol_r <= 32767);
  v->tgt_l = vol_l; v->tgt_r = vol_r;
  v->ramp = frames;
  if (!frames) { v->vol_l = vol_l; v->vol_r = vol_r; }
}

// Decode records for an ADPCM voice up to stream sample `need` (exclusive), whole groups.
static u32 adpcm_records(k64_adpcm_voice_t *d, k64_mixrec_t *r, u32 cap, u32 need) {
  const k64_adpcm_t *a = d->a;
  u32 nr = 0, groups = 0;
  while (d->dec < need) {
    u32 g = d->dec / K64_ADPCM_GROUP % ADPCM_GROUPS, next = 1 + (g + 1) % ADPCM_GROUPS;
    s16 *o = &d->ring[g * K64_ADPCM_GROUP];
    u32 left = ADPCM_GFRAMES;
    while (left && !d->ended) {
      if (d->src == a->nframes) {
        if (a->loop_start == K64_NO_LOOP) { d->ended = true; break; }
        d->src = a->loop_start / 16;
        d->in = 0;
      }
      u32 k = a->nframes - d->src < left ? a->nframes - d->src : left;
      bool last = k == left;
      K64_ASSERT(nr < cap);
      r[nr++] = (k64_mixrec_t){ .w = { 2, (k - 1) << 24 | K64_PHYS(a->frames + d->src * 9),
        (u32)(o == d->ring) << 31 | (last ? next : 0) << 24 | K64_PHYS(o),
        (u32)d->in << 24 | K64_PHYS(d) } };
      d->in = last ? next : 0;
      d->src += k;
      o += k * 16;
      left -= k;
    }
    d->dec += K64_ADPCM_GROUP;
    groups++;
  }
  // no decoding over samples still to be mixed
  K64_ASSERT(groups < ADPCM_GROUPS && d->dec <= d->rd + K64_ADPCM_RING);
  return nr;
}

// Records for one voice over frames [start, start + n) of a block; advances the voice.
static u32 voice_records(k64_voice_t *v, k64_mixrec_t *r, u32 cap, u32 start, u32 n) {
  u32 nr = 0;
  while (n && v->on) {
    u64 p0 = ((u64)v->idx << 16) | v->frac;
    u64 end = (u64)v->len << 16;
    bool fin = v->loop_start == K64_NO_LOOP;
    bool oneshot = v->adpcm && v->adpcm->a->loop_start == K64_NO_LOOP;
    if (oneshot && v->idx + v->left <= v->len) {  // ADPCM one-shot: ends inside the ring
      end = (u64)(v->idx + v->left) << 16;
      fin = true;
    }
    u32 nmax = (u32)((end - p0 - 1) / v->step) + 1;
    u32 count = n < nmax ? n : nmax;
    s32 dl = 0, dr = 0;
    if (v->ramp) {                             // cut at the ramp end, step per frame
      if (count > v->ramp) count = v->ramp;
      dl = (v->tgt_l - v->vol_l) / (s32)v->ramp;
      dr = (v->tgt_r - v->vol_r) / (s32)v->ramp;
    }
    u32 last = (u32)(((u32)v->frac + (count - 1) * v->step) >> 16) + 1;  // highest sample read
    u32 src = K64_PHYS(&v->data[v->idx]);
    u32 bytes = (src & 7) + (last + 1) * 2;
    K64_ASSERT(bytes <= SRCBUF_BYTES && nr + 1 < cap);
    if (dl | dr) r[nr++] = (k64_mixrec_t){ .w = { 3, (u32)(u16)dl << 16 | (u16)dr } };
    r[nr++] = (k64_mixrec_t){
      .src = (((bytes + 7) / 8 - 1) << 24) | src, .step = v->step, .frac = v->frac,
      .start = (u8)start, .count1 = (u8)(count - 1), .vol_l = v->vol_l, .vol_r = v->vol_r };
    u64 p = p0 + (u64)count * v->step;
    if (v->ramp) {
      v->vol_l = (s16)(v->vol_l + dl * (s32)count);
      v->vol_r = (s16)(v->vol_r + dr * (s32)count);
      v->ramp -= count;
      if (!v->ramp) { v->vol_l = v->tgt_l; v->vol_r = v->tgt_r; }
    }
    if (v->adpcm) {
      u32 adv = (u32)(p >> 16) - v->idx;
      v->adpcm->rd += adv;
      if (oneshot) v->left -= adv;
    }
    if (p >= end) {
      if (fin) { v->on = false; break; }
      u64 span = (u64)(v->len - v->loop_start) << 16;
      while (p >= end) p -= span;
    }
    v->idx = (u32)(p >> 16);
    v->frac = (u16)p;
    start += count;
    n -= count;
  }
  return nr;
}

// Decode records go to a scratch list while the blocks are built (a voice may start in any
// block), then in front of the mix records.
static k64_mixrec_t dec_recs[K64_MIXER_VOICES * 24];

u32 k64_mixer_build_ex(k64_mixer_t *m, k64_mixrec_t *recs, u32 cap, s16 *out, u32 frames,
                       k64_mixer_tick_fn tick, void *ctx) {
  K64_ASSERT((frames & 3) == 0);
  u32 nr = 0, nd = 0;
  const k64_adpcm_voice_t *dv[K64_MIXER_VOICES];
  u32 dec0[K64_MIXER_VOICES], gen[K64_MIXER_VOICES];
  for (u32 i = 0; i < K64_MIXER_VOICES; i++) dv[i] = NULL;
  for (u32 b = 0; b < frames; b += K64_MIXER_BLOCK) {
    u32 n = frames - b < K64_MIXER_BLOCK ? frames - b : K64_MIXER_BLOCK;
    if (tick) tick(ctx, b, n);
    for (u32 i = 0; i < K64_MIXER_VOICES; i++) {
      k64_voice_t *v = &m->v[i];
      if (!v->on || !v->adpcm) continue;
      if (dv[i] != v->adpcm) {
        // a decoder restarted inside the list would rewrite what earlier records read
        K64_ASSERT(!dv[i]);
        dv[i] = v->adpcm; dec0[i] = v->adpcm->dec; gen[i] = v->adpcm->starts;
      }
      K64_ASSERT(gen[i] == v->adpcm->starts);
      u32 last = (u32)(((u64)v->frac + (u64)(n - 1) * v->step) >> 16) + 1;
      nd += adpcm_records(v->adpcm, dec_recs + nd, K64_ARRAY_LEN(dec_recs) - nd, v->adpcm->rd + last + 1);
    }
    for (u32 i = 0; i < K64_MIXER_VOICES; i++)
      if (m->v[i].on) nr += voice_records(&m->v[i], recs + nr, cap - nr, 0, n);
    K64_ASSERT(nr < cap);
    recs[nr++] = (k64_mixrec_t){ .src = 0, .step = K64_PHYS(out + b * 2), .frac = (u16)n };
  }
  // one list may not decode over a slot it reads
  for (u32 i = 0; i < K64_MIXER_VOICES; i++)
    if (dv[i]) K64_ASSERT((dv[i]->dec - dec0[i]) / K64_ADPCM_GROUP < ADPCM_GROUPS);
  K64_ASSERT(nr < cap);
  recs[nr++] = (k64_mixrec_t){ .src = 1 };
  if (nd) {
    K64_ASSERT(nr + nd + 1 <= cap);
    for (u32 i = nr; i--;) recs[i + nd + 1] = recs[i];
    for (u32 i = 0; i < nd; i++) recs[i] = dec_recs[i];
    recs[nd] = (k64_mixrec_t){ .src = 1 };
    nr += nd + 1;
  }
  k64_dcache_writeback(recs, nr * sizeof *recs);
  return nr;
}

u32 k64_mixer_build(k64_mixer_t *m, k64_mixrec_t *recs, u32 cap, s16 *out, u32 frames) {
  return k64_mixer_build_ex(m, recs, cap, out, frames, NULL, NULL);
}

static inline s16 clamp16(s32 x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : (s16)x; }

void k64_kgfx_audio(k64_kgfx_list_t *l, const k64_mixrec_t *recs) {
  if (recs->src == 2) {
    k64_kgfx_cmd(l, (u32)K64_KGFX_OP_ADPCM << 24, K64_PHYS(recs));
    while (recs->src != 1) recs++;
    recs++;
  }
  k64_kgfx_cmd(l, (u32)K64_KGFX_OP_AUDIO << 24, K64_PHYS(recs));
}

// CPU reference of one decode record (kgfx.S adpcm overlay).
static void adpcm_cpu(const k64_mixrec_t *r) {
  k64_adpcm_voice_t *d = K64_KSEG0(r->w[3] & 0x00FFFFFF);
  const u8 *f = K64_KSEG0(r->w[1] & 0x00FFFFFF);
  s16 *o = K64_KSEG0(r->w[2] & 0x00FFFFFF), *o0 = o;
  u32 n = (r->w[1] >> 24) + 1, in = r->w[3] >> 24, out = (r->w[2] >> 24) & 0x7F;
  const s16 *cb = K64_KSEG0(d->codebook);
  s16 s[8];
  for (u32 k = 0; k < 8; k++) s[k] = d->state[in][k];
  for (; n; n--, f += 9) {
    const s16 *p0 = cb + (f[0] & 15) * 16, *p1 = p0 + 8;
    s32 res[16];
    for (u32 j = 0; j < 16; j++) {
      s32 nib = j & 1 ? f[1 + j / 2] & 15 : f[1 + j / 2] >> 4;
      res[j] = clamp16(((nib ^ 8) - 8) << (f[0] >> 4));
    }
    for (u32 h = 0; h < 2; h++) {
      const s32 *rr = res + h * 8;
      for (u32 j = 0; j < 8; j++) {
        s32 sum = p0[j] * s[6] + p1[j] * s[7] + rr[j] * 2048;
        for (u32 k = 0; k < j; k++) sum += rr[k] * p1[j - k - 1];
        o[j] = clamp16(sum >> 11);
      }
      for (u32 k = 0; k < 8; k++) s[k] = o[k];
      o += 8;
    }
  }
  k64_dcache_writeback(o0, (u32)(o - o0) * 2);
  if (r->w[2] >> 31) {
    s16 *mir = K64_KSEG0(d->mirror);
    for (u32 k = 0; k < 16; k++) mir[k] = o0[k];
    k64_dcache_writeback(mir, 32);
  }
  if (out) {
    for (u32 k = 0; k < 8; k++) d->state[out][k] = s[k];
    k64_dcache_writeback(d->state[out], 16);
  }
}

const k64_mixrec_t *k64_adpcm_run_cpu(const k64_mixrec_t *r) {
  for (; r->src == 2; r++) adpcm_cpu(r);
  return r + 1;                              // past the END of the decode records
}

void k64_mixer_run_cpu(const k64_mixrec_t *r) {
  s16 blk[K64_MIXER_BLOCK * 2];
  for (u32 i = 0; i < K64_MIXER_BLOCK * 2; i++) blk[i] = 0;
  if (r->src == 2) {
    // the RSP may have written the rings and states: drop stale lines (CPU writes to them
    // are always written back at once)
    for (const k64_mixrec_t *q = r; q->src == 2; q++) {
      k64_adpcm_voice_t *d = K64_KSEG0(q->w[3] & 0x00FFFFFF);
      k64_dcache_invalidate(d->state, (u32)((u8 *)&d->ring[K64_ADPCM_RING + 16] - (u8 *)d->state));
    }
    r = k64_adpcm_run_cpu(r);
  }
  s32 dl = 0, dr = 0;
  for (;; r++) {
    if (r->src == 1) return;
    if (r->src == 3) { dl = (s16)(r->w[1] >> 16); dr = (s16)r->w[1]; continue; }
    if (r->src == 0) {
      s16 *out = K64_KSEG0(r->step);
      for (u32 i = 0; i < r->frac * 2u; i++) { out[i] = blk[i]; blk[i] = 0; }
      k64_dcache_writeback(out, r->frac * 4u);
      continue;
    }
    const s16 *s = K64_KSEG0(r->src & 0x00FFFFFF);
    u32 p = r->frac;
    for (u32 f = r->start; f <= (u32)r->start + r->count1; f++, p += r->step) {
      u32 i = p >> 16, x = p & 0xFFFF;
      s32 v = (s32)(((s64)s[i] * 65536 + (s64)(s[i + 1] - s[i]) * (s32)x) >> 16);
      s32 vl = r->vol_l + (s32)(f - r->start) * dl, vr = r->vol_r + (s32)(f - r->start) * dr;
      s32 tl = clamp16((s32)(((s64)v * vl * 2 + 0x8000) >> 16));
      s32 tr = clamp16((s32)(((s64)v * vr * 2 + 0x8000) >> 16));
      blk[f * 2]     = clamp16(blk[f * 2] + tl);
      blk[f * 2 + 1] = clamp16(blk[f * 2 + 1] + tr);
    }
    dl = dr = 0;
  }
}
