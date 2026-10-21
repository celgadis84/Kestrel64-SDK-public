// kestrel64-sdk — music sequencer (docs/MUSIC.md).
#include <k64/k64.h>
#include <k64/music.h>

#define GAIN       20000.f              // Q15 peak of one note at full everything
#define MIN_RAMP   64                   // frames: release / volume changes (no clicks)

// 2^x, ~0.03 cents worst case (Taylor to x^5 on the fraction)
static float exp2_(float x) {
  int i = (int)x;
  if ((float)i > x) i--;
  float f = x - (float)i;
  float p = 1.f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
  return i >= 0 ? p * (float)(1u << (i > 30 ? 30 : i)) : p / (float)(1u << (-i > 30 ? 30 : -i));
}

// MIDI variable-length value: 7 bits per byte, the high bit set on all but the last byte
static u32 rd_var(const u8 **p) {
  u32 v = 0;
  u8 b;
  do { b = *(*p)++; v = v << 7 | (b & 0x7F); } while (b & 0x80);
  return v;
}

// milliseconds to output frames
static u32 frames_ms(const k64_music_t *mu, u32 ms) { return ms * mu->rate / 1000; }

// song ticks per output frame, 32.32: ppq ticks per tempo period at the output rate, times the
// speed (Q8)
static void tick_rate(k64_music_t *mu) {
  u64 r = ((u64)mu->ppq * 1000000u << 32) / ((u64)mu->tempo * mu->rate);
  mu->tick_rate = r * mu->speed >> 8;
}

void k64_music_init(k64_music_t *mu, k64_mixer_t *m, u32 first, u32 nvoices, k64_adpcm_voice_t *dec,
                    const k64_bank_t *bank, u32 rate) {
  K64_ASSERT(first + nvoices <= K64_MIXER_VOICES && nvoices);
  *mu = (k64_music_t){ .mixer = m, .bank = bank, .dec = dec, .first = first, .nvoices = nvoices,
                       .rate = rate, .speed = 256, .master = 100, .ppq = 96, .tempo = 500000 };
  tick_rate(mu);
}

// MIDI defaults: volume 100, centre pan, expression 127, bend range 2 semitones
static void reset_channels(k64_music_t *mu) {
  for (u32 c = 0; c < K64_MUSIC_CHANNELS; c++)
    mu->ch[c] = (k64_music_channel_t){ .volume = 100, .pan = 64, .expression = 127, .bend_range = 2 };
}

bool k64_music_play(k64_music_t *mu, const void *song) {
  const u8 *s = song;
  if (s[0] != 'K' || s[1] != '6' || s[2] != '4' || s[3] != 'Q' || (s[6] << 8 | s[7]) != 1) return false;
  k64_music_stop(mu);
  mu->song = s;
  mu->ppq = (u16)(s[4] << 8 | s[5]);
  mu->tempo = (u32)s[8] << 24 | s[9] << 16 | s[10] << 8 | s[11];
  tick_rate(mu);
  reset_channels(mu);
  mu->now = 0;
  mu->pos = s + 16;
  mu->loop = NULL;
  mu->next = rd_var(&mu->pos);
  mu->npending = 0;
  mu->playing = true;
  return true;
}

void k64_music_speed(k64_music_t *mu, u16 speed) {
  mu->speed = speed;
  tick_rate(mu);
}

// ---- voices -----------------------------------------------------------------------------------

// the mixer voice behind player voice i
static k64_voice_t *mvoice(k64_music_t *mu, u32 i) { return &mu->mixer->v[mu->first + i]; }

// gains: velocity, channel volume and expression, each squared (MIDI curve); pan as an
// equal-power sine/cosine pair
static void gains(k64_music_t *mu, k64_music_voice_t *v) {
  const k64_music_channel_t *c = &mu->ch[v->ch];
  float vel = v->vel * (1.f / 127), vol = c->volume * (1.f / 127), ex = c->expression * (1.f / 127);
  float g = vel * vel * vol * vol * ex * ex * v->inst->volume * mu->master * (GAIN / (127.f * 127.f));
  int p = c->pan + v->inst->pan;
  p = p < 0 ? 0 : p > 127 ? 127 : p;
  float a = p * (1.5707963f / 127);
  v->gain_l = g * k64_cosf(a);
  v->gain_r = g * k64_sinf(a);
}

// ramps the voice to its envelope target over the frames left in the stage
static void apply(k64_music_t *mu, u32 i, u32 frames) {
  k64_music_voice_t *v = &mu->v[i];
  k64_voice_volume(mvoice(mu, i), (s16)(v->level * v->gain_l), (s16)(v->level * v->gain_r), frames);
}

// step (16.16) = note pitch times the channel bend, clamped to [1, 4.0) (the mixer's limit)
static void set_step(k64_music_t *mu, u32 i) {
  k64_music_voice_t *v = &mu->v[i];
  const k64_music_channel_t *c = &mu->ch[v->ch];
  float s = v->pitch;
  if (c->bend && v->ch != 9) s *= exp2_(c->bend * (float)c->bend_range * (1.f / (8192 * 12)));
  float st = s * 65536.f;
  mvoice(mu, i)->step = st >= 262143.f ? 262143u : st < 1.f ? 1u : (u32)st;
}

// key off: the envelope ramps to zero over the release time (at least MIN_RAMP)
static void release(k64_music_t *mu, u32 i) {
  k64_music_voice_t *v = &mu->v[i];
  u32 f = frames_ms(mu, v->inst->release);
  v->stage = K64_ENV_RELEASE;
  v->held = false;
  v->level = 0;
  v->left = f < MIN_RAMP ? MIN_RAMP : f;
  apply(mu, i, v->left);
}

// voice for an instrument: a free one, else the quietest releasing one, else the oldest;
// never one that mixed ADPCM earlier in this list when the instrument is ADPCM
static int pick(k64_music_t *mu, const k64_instrument_t *in) {
  int best = -1;
  u32 score = 0;
  for (u32 i = 0; i < mu->nvoices; i++) {
    if (in->adpcm && (mu->played >> i & 1)) continue;
    k64_music_voice_t *v = &mu->v[i];
    k64_voice_t *m = mvoice(mu, i);
    u32 s;
    if (v->stage == K64_ENV_OFF || !m->on) s = 0xFFFFFFFFu;
    else if (v->stage == K64_ENV_RELEASE) s = 0x80000000u - (u32)(m->vol_l + m->vol_r);
    else s = mu->age - v->age < 0x40000000u ? mu->age - v->age : 0x3FFFFFFFu; // older first
    if (best < 0 || s > score) { best = (int)i; score = s; }
  }
  return best;
}

// instrument for a channel and key: the drum table on channel 10, else the channel's program
static const k64_instrument_t *instrument(k64_music_t *mu, u32 ch, u32 key) {
  if (ch == 9) return mu->bank->drum[key];
  const k64_instrument_t *in = mu->bank->program[mu->ch[ch].program];
  return in ? in : mu->bank->program[0];
}

// false: no voice now (ADPCM rule), retry in the next list
static bool note_on(k64_music_t *mu, u32 ch, u32 key, u32 vel, u32 end) {
  const k64_instrument_t *in = instrument(mu, ch, key);
  if (!in) return true;
  int i = pick(mu, in);
  if (i < 0) return false;
  k64_music_voice_t *v = &mu->v[i];
  k64_voice_t *m = mvoice(mu, (u32)i);
  if (v->stage != K64_ENV_OFF && m->on) mu->stats.steals++;
  *v = (k64_music_voice_t){ .release_tick = end, .age = ++mu->age, .inst = in, .ch = (u8)ch,
                            .key = (u8)key, .vel = (u8)vel, .stage = K64_ENV_ATTACK, .level = 1 };
  u32 rate = in->adpcm ? in->adpcm->rate : in->rate;
  float semis = (ch == 9 ? 0.f : (float)((int)key - in->root)) + in->fine * 0.01f;
  v->pitch = (float)rate / (float)mu->rate * exp2_(semis * (1.f / 12));
  gains(mu, v);
  v->left = frames_ms(mu, in->attack);
  s16 l = v->left ? 0 : (s16)v->gain_l, r = v->left ? 0 : (s16)v->gain_r;
  if (in->adpcm) {
    K64_ASSERT(mu->dec);
    k64_voice_play_adpcm(m, &mu->dec[i], in->adpcm, 0x10000, l, r);
    mu->played |= 1u << i;
  } else {
    k64_voice_play(m, in->pcm, in->len, in->loop_start, 0x10000, l, r);
  }
  set_step(mu, (u32)i);
  if (v->left) apply(mu, (u32)i, v->left);
  mu->stats.notes++;
  return true;
}

void k64_music_stop(k64_music_t *mu) {
  mu->playing = false;
  mu->npending = 0;
  for (u32 i = 0; i < mu->nvoices; i++)
    if (mu->v[i].stage != K64_ENV_OFF && mu->v[i].stage != K64_ENV_RELEASE) release(mu, i);
}

// applies a channel change to its sounding voices: pitch only, or gains and a volume ramp
static void channel_changed(k64_music_t *mu, u32 ch, bool pitch) {
  for (u32 i = 0; i < mu->nvoices; i++) {
    k64_music_voice_t *v = &mu->v[i];
    if (v->stage == K64_ENV_OFF || v->ch != ch) continue;
    if (pitch) { set_step(mu, i); continue; }
    gains(mu, v);
    apply(mu, i, v->left > MIN_RAMP ? v->left : MIN_RAMP);
  }
}

void k64_music_volume(k64_music_t *mu, u8 volume) {
  mu->master = volume;
  for (u32 c = 0; c < K64_MUSIC_CHANNELS; c++) channel_changed(mu, c, false);
}

// a note with no voice: kept for the next list; dropped when the pending queue is full
static void defer(k64_music_t *mu, u32 ch, u32 key, u32 vel, u32 end) {
  if (mu->npending == K64_MUSIC_PENDING) { mu->stats.dropped++; return; }
  mu->pending[mu->npending++] = (k64_music_note_t){ (u8)ch, (u8)key, (u8)vel, end };
  mu->stats.deferred++;
}

// one event at mu->pos (its tick is due)
static void event(k64_music_t *mu) {
  const u8 *p = mu->pos;
  u32 op = *p++, ch = op & 15;
  k64_music_channel_t *c = &mu->ch[ch];
  if (op < 0x80) {
    switch (op >> 4) {
    case 0: {
      u32 key = p[0], vel = p[1];
      p += 2;
      // the duration (a delta in ticks) follows the velocity: there are no note-off events
      u32 end = mu->next + rd_var(&p);
      if (!note_on(mu, ch, key, vel, end)) defer(mu, ch, key, vel, end);
      break;
    }
    case 1: c->program = *p++; break;
    case 2: c->volume = *p++; channel_changed(mu, ch, false); break;
    case 3: c->pan = *p++; channel_changed(mu, ch, false); break;
    case 4: c->bend = (s16)(p[0] << 8 | p[1]); p += 2; channel_changed(mu, ch, true); break;
    case 5: c->expression = *p++; channel_changed(mu, ch, false); break;
    case 6:
      c->sustain = *p++;
      if (!c->sustain)
        for (u32 i = 0; i < mu->nvoices; i++)
          if (mu->v[i].held && mu->v[i].ch == ch) release(mu, i);
      break;
    case 7: c->bend_range = *p++; channel_changed(mu, ch, true); break;
    }
  } else if (op == 0xF0) {
    // 24-bit microseconds per quarter note
    mu->tempo = (u32)p[0] << 16 | p[1] << 8 | p[2];
    p += 3;
    tick_rate(mu);
  } else if (op == 0xF1) {
    mu->loop = p;
  } else if (op == 0xF2 && mu->loop) {
    // loop end: back to the marked loop start
    p = mu->loop;
    mu->stats.loops++;
  } else {
    // end of song (or an unknown op): no more events; sounding notes end on their own
    mu->playing = false;
    mu->pos = p;
    return;
  }
  // the delta to the next event's tick
  mu->next += rd_var(&p);
  mu->pos = p;
}

void k64_music_tick(void *ctx, u32 frame, u32 n) {
  k64_music_t *mu = ctx;
  // integer song tick; the fraction carries over in mu->now
  u32 now = (u32)(mu->now >> 32);
  // first block of the list: the restart record resets, deferred notes get voices first
  if (frame == 0) {
    mu->played = 0;
    u32 np = mu->npending;
    mu->npending = 0;
    for (u32 k = 0; k < np; k++) {
      k64_music_note_t e = mu->pending[k];
      if (e.end <= now) mu->stats.dropped++;
      else if (!note_on(mu, e.ch, e.key, e.vel, e.end)) mu->pending[mu->npending++] = e;
    }
  }
  while (mu->playing && mu->next <= now) event(mu);

  u32 active = 0;
  for (u32 i = 0; i < mu->nvoices; i++) {
    k64_music_voice_t *v = &mu->v[i];
    k64_voice_t *m = mvoice(mu, i);
    if (v->stage == K64_ENV_OFF) continue;
    if (!m->on) { v->stage = K64_ENV_OFF; continue; }   // one-shot sample ended
    // the note's end: key off, or hold it while the sustain pedal is down
    if (v->stage < K64_ENV_RELEASE && !v->held && v->release_tick <= now) {
      if (mu->ch[v->ch].sustain) v->held = true;
      else release(mu, i);
    }
    // envelope stage done: attack -> decay -> sustain; a released voice ends here
    if (!v->left) {
      if (v->stage == K64_ENV_ATTACK) {
        v->stage = K64_ENV_DECAY;
        v->level = v->inst->sustain * (1.f / 127);
        v->left = frames_ms(mu, v->inst->decay);
        apply(mu, i, v->left);
      }
      if (v->stage == K64_ENV_DECAY && !v->left) v->stage = K64_ENV_SUSTAIN;
      if (v->stage == K64_ENV_RELEASE) { m->on = false; v->stage = K64_ENV_OFF; continue; }
    }
    // an ADPCM voice in this list: no note may restart its decoder in this list (see pick)
    if (m->adpcm) mu->played |= 1u << i;
    v->left = v->left > n ? v->left - n : 0;
    active++;
  }
  if (active > mu->stats.peak) mu->stats.peak = active;
  mu->now += mu->tick_rate * n;
}
