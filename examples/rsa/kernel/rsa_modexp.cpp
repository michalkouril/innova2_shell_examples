// SPDX-FileCopyrightText: 2026 the innova2 contributors
// SPDX-License-Identifier: Apache-2.0
//
// rsa_modexp: batched 1024-bit modular exponentiation for RSA-2048 signing with the CRT (s_p = m^dp mod p,
// s_q = m^dq mod q); the host does the padding, m mod p / q and the recombination.
//   job k: 5 x 1024-bit words at in[k*10 .. k*10+9] (two 512-bit beats each, little-endian 64-bit limbs):
//          base (< mod), exponent, mod (odd), r2 = 2^2048 mod mod, n0' = -mod^-1 mod 2^64 (low limb of word 4)
//   result k: 1024 bits at out[k*2 .. k*2+1]; njobs a multiple of L.
// One compute unit, NE engines behind one pair of memory ports: a loader hands each engine a group of L jobs, a
// writer collects the results in the same order. An engine runs its L exponentiations in lockstep (fixed 4-bit window,
// a multiply for every nibble: the same operation sequence for every job). Montgomery multiplication: rows of one
// 64-bit limb of b, steps of U limbs of a and M (2U 64 x 64 multipliers), lanes interleaved step by step so that a
// lane's carry has L cycles and its row data and next-row quotient about L*S/U cycles to come back.
// A Montgomery product is T or T - M (T < 2M). Instead of a subtraction pass after the multiplication, the last row
// writes both, word by word as they complete (RT and RD), and the lane's choice at the end (SEL); every read of a
// register picks the right one.
#include <ap_int.h>
#include <hls_stream.h>

#ifndef RSA_U
#define RSA_U 4                           // limbs per step
#endif
#ifndef RSA_L
#define RSA_L 4                           // lanes (independent exponentiations) per engine
#endif
#ifndef RSA_NE
#define RSA_NE 2                          // engines
#endif
static const int U = RSA_U, L = RSA_L, NE = RSA_NE;
static const int S = 16;                  // limbs of 64 bits
static const int SW = S / U;              // words of U limbs
static const int NR = 21;                 // register file: 0..15 window, 16 x, 17 y, 18 base, 19 r2, 20 one
static const int RX = 16, RY = 17, RB = 18, RR2 = 19, R1 = 20;
typedef ap_uint<64> limb;
typedef ap_uint<64 * U> word;
typedef ap_uint<512> beat;

static inline limb getl(const word &w, int k) { return w.range(64 * k + 63, 64 * k); }

// flat arrays: as [NR][L][SW] HLS split them into many 28-word banks of 8 BRAM18 each
typedef word regs[NR * L * SW];
typedef ap_uint<1> sels[NR * L];
#define RI(r, l, w) (((r) * L + (l)) * SW + (w))
#define SI(r, l) ((r) * L + (l))
static inline word rd(const regs &RT, const regs &RD, const sels &SEL, int r, int l, int w)
{
#pragma HLS INLINE
  return SEL[SI(r, l)] ? RD[RI(r, l, w)] : RT[RI(r, l, w)];
}

// R[ir] = R[ia] * R[ib] * 2^-1024 mod M, every lane (R = the RT / RD / SEL triple)
static void monmul(regs &RT, regs &RD, sels &SEL, const word Mw[L][SW], const limb n0p[L], int ia, int ib, int ir)
{
#pragma HLS INLINE off
  word T[L][SW], P[L], Bc[L][SW];               // Bc: b, copied once (the register file has a read and a write port)
  limb mq[2][L], bq[2][L], top[L];
  ap_uint<67> C[L];
  ap_uint<1> bor[L];
#pragma HLS BIND_STORAGE variable=T type=ram_2p impl=lutram
#pragma HLS BIND_STORAGE variable=Bc type=ram_1wnr impl=lutram
#pragma HLS BIND_STORAGE variable=P type=ram_2p impl=lutram
#pragma HLS BIND_STORAGE variable=bor type=ram_2p impl=lutram
#pragma HLS BIND_STORAGE variable=mq type=ram_2p impl=lutram
#pragma HLS BIND_STORAGE variable=bq type=ram_2p impl=lutram
#pragma HLS BIND_STORAGE variable=top type=ram_2p impl=lutram
#pragma HLS BIND_STORAGE variable=C type=ram_2p impl=lutram
q0:
  for (int k = 0; k < L * SW; k++) {                   // copy b; row 0: b_0 and its quotient (t = 0)
#pragma HLS PIPELINE II=1
    const int l = k / SW, w = k % SW;
    const word bw = rd(RT, RD, SEL, ib, l, w);
    Bc[l][w] = bw;
    if (w == 0) {
      const limb a0 = getl(rd(RT, RD, SEL, ia, l, 0), 0), b0 = getl(bw, 0);
      bq[0][l] = b0; mq[0][l] = (limb)(a0 * b0) * n0p[l];
    }
  }
  // (row i, step w = 0..SW, lane l): steps 0..SW-1 accumulate word w; step SW folds the carry into the top limb.
  // t_new = (t + a*b_i + m*M) / 2^64: word w-1 of t_new is complete at step w (its top limb is limb 0 of step w).
rows:
  for (int it = 0; it < S * (SW + 1) * L; it++) {
#pragma HLS PIPELINE II=1
#pragma HLS DEPENDENCE variable=C inter distance=L true
#pragma HLS DEPENDENCE variable=P inter distance=L true
#pragma HLS DEPENDENCE variable=T inter distance=L*SW true
#pragma HLS DEPENDENCE variable=top inter distance=L*(SW+1) true
#pragma HLS DEPENDENCE variable=mq inter distance=L*(SW+1) true
#pragma HLS DEPENDENCE variable=bq inter distance=L*(SW+1) true
#pragma HLS DEPENDENCE variable=bor inter distance=L true
#pragma HLS DEPENDENCE variable=RT inter false
#pragma HLS DEPENDENCE variable=RD inter false
#pragma HLS DEPENDENCE variable=SEL inter false
    const int l = it % L, w = (it / L) % (SW + 1), i = it / (L * (SW + 1));
    const ap_uint<67> c = (w == 0) ? ap_uint<67>(0) : C[l];
    if (w < SW) {
      const word a = rd(RT, RD, SEL, ia, l, w), t = (i == 0) ? word(0) : T[l][w];
      const limb bi = bq[i & 1][l], m = mq[i & 1][l];
      const ap_uint<64 * U + 66> s = (ap_uint<64 * U + 66>)t + (ap_uint<64 * U + 64>)(a * bi) +
                                     (ap_uint<64 * U + 64>)(m * Mw[l][w]) + c;
      const word lw = s.range(64 * U - 1, 0);
      C[l] = s >> (64 * U);
      const word done = ((word)getl(lw, 0) << (64 * (U - 1))) | P[l];          // word w-1 of t_new
      if (w > 0) T[l][w - 1] = done;
      if (i == S - 1 && w > 0) {                       // last row: the result words, both candidates
        const ap_uint<64 * U + 1> d = (ap_uint<64 * U + 1>)done - Mw[l][w - 1] - (w == 1 ? ap_uint<1>(0) : bor[l]);
        RT[RI(ir, l, w - 1)] = done; RD[RI(ir, l, w - 1)] = d.range(64 * U - 1, 0); bor[l] = d[64 * U];
      }
      P[l] = lw >> 64;
      if (w == 0 && i + 1 < S) {                       // next row: b_{i+1} and its quotient from the new t_0
        const limb bn = getl(Bc[l][(i + 1) / U], (i + 1) % U);
        bq[(i + 1) & 1][l] = bn;
        mq[(i + 1) & 1][l] = (limb)(getl(lw, 1) + (limb)(getl(a, 0) * bn)) * n0p[l];
      }
    } else {
      const ap_uint<68> tp = (ap_uint<68>)((i == 0) ? limb(0) : top[l]) + c;
      const word done = ((word)tp.range(63, 0) << (64 * (U - 1))) | P[l];
      T[l][SW - 1] = done;
      top[l] = tp >> 64;
      if (i == S - 1) {                                // last word of the result; T >= M picks T - M
        const ap_uint<64 * U + 1> d = (ap_uint<64 * U + 1>)done - Mw[l][SW - 1] - (SW == 1 ? ap_uint<1>(0) : bor[l]);
        RT[RI(ir, l, SW - 1)] = done; RD[RI(ir, l, SW - 1)] = d.range(64 * U - 1, 0);
        SEL[SI(ir, l)] = (tp >> 64) != 0 || !d[64 * U];
      }
    }
  }
}

static void engine(hls::stream<beat> &jin, hls::stream<beat> &jout, unsigned ngroups)
{
#pragma HLS INLINE off
  regs RT, RD;
  sels SEL;
  word Mw[L][SW], E[L][SW];
  limb n0p[L];
#pragma HLS BIND_STORAGE variable=RT type=ram_2p impl=bram
#pragma HLS BIND_STORAGE variable=RD type=ram_2p impl=bram
#pragma HLS BIND_STORAGE variable=SEL type=ram_1wnr impl=lutram
#pragma HLS BIND_STORAGE variable=Mw type=ram_1wnr impl=lutram
#pragma HLS BIND_STORAGE variable=E type=ram_2p impl=lutram
grp:
  for (unsigned g = 0; g < ngroups; g++) {
  ld:
    for (int k = 0; k < L * 10; k++) {                 // per lane: base, exp, mod, r2 (2 beats each), n0', pad
#pragma HLS PIPELINE II=1
      const int l = k / 10, f = (k % 10) / 2, h = k % 2;
      const beat b = jin.read();
      for (int q = 0; q < 512 / (64 * U); q++) {
        const int wi = h * (512 / (64 * U)) + q;
        const word v = b.range(64 * U * q + 64 * U - 1, 64 * U * q);
        if (f == 0) RT[RI(RB, l, wi)] = v;
        if (f == 1) E[l][wi] = v;
        if (f == 2) Mw[l][wi] = v;
        if (f == 3) RT[RI(RR2, l, wi)] = v;
        if (f == 4 && h == 0 && q == 0) n0p[l] = getl(v, 0);
        if (f == 0) RT[RI(R1, l, wi)] = wi == 0 ? word(1) : word(0);
      }
    }
  sel0:
    for (int l = 0; l < L; l++) {
#pragma HLS PIPELINE II=3
      SEL[SI(RB, l)] = 0; SEL[SI(R1, l)] = 0; SEL[SI(RR2, l)] = 0;
    }
    monmul(RT, RD, SEL, Mw, n0p, R1, RR2, 0);                     // window[0] = R mod M (Montgomery 1)
    monmul(RT, RD, SEL, Mw, n0p, RB, RR2, 1);                     // window[1] = base in Montgomery form
  win:
    for (int k = 2; k < 16; k++) monmul(RT, RD, SEL, Mw, n0p, k - 1, 1, k);
  cp0:
    for (int k = 0; k < L * SW; k++) {
#pragma HLS PIPELINE II=1
      const int l = k / SW, w = k % SW;
      RT[RI(RX, l, w)] = RT[RI(0, l, w)]; RD[RI(RX, l, w)] = RD[RI(0, l, w)];
      if (w == 0) SEL[SI(RX, l)] = SEL[SI(0, l)];
    }
  nib:
    for (int k = 255; k >= 0; k--) {
    sq:
      for (int s = 0; s < 4; s++) monmul(RT, RD, SEL, Mw, n0p, RX, RX, RX);
    sel:
      for (int c = 0; c < L * SW; c++) {                // y = window[nibble], per lane
#pragma HLS PIPELINE II=1
        const int l = c / SW, w = c % SW;
        const word ew = E[l][k / (16 * U)];
        const ap_uint<4> nb = ew.range((k % (16 * U)) * 4 + 3, (k % (16 * U)) * 4);
        RT[RI(RY, l, w)] = rd(RT, RD, SEL, nb, l, w);
        if (w == 0) SEL[SI(RY, l)] = 0;
      }
      monmul(RT, RD, SEL, Mw, n0p, RX, RY, RX);
    }
    monmul(RT, RD, SEL, Mw, n0p, RX, R1, RX);                     // out of Montgomery form
  st:
    for (int k = 0; k < L * 2; k++) {
#pragma HLS PIPELINE II=1
      const int l = k / 2, h = k % 2;
      beat o;
      for (int q = 0; q < 512 / (64 * U); q++) o.range(64 * U * q + 64 * U - 1, 64 * U * q) = rd(RT, RD, SEL, RX, l, h * (512 / (64 * U)) + q);
      jout.write(o);
    }
  }
}

static void loader(const beat *in, hls::stream<beat> jin[NE], unsigned ngroups)
{
  for (unsigned g = 0; g < ngroups; g++)
    for (int k = 0; k < L * 10; k++) {
#pragma HLS PIPELINE II=1
      jin[g % NE].write(in[g * L * 10 + k]);
    }
}

static void writer(beat *out, hls::stream<beat> jout[NE], unsigned ngroups)
{
  for (unsigned g = 0; g < ngroups; g++)
    for (int k = 0; k < L * 2; k++) {
#pragma HLS PIPELINE II=1
      out[g * L * 2 + k] = jout[g % NE].read();
    }
}

extern "C" void rsa_modexp(const beat *in, beat *out, unsigned njobs)
{
#pragma HLS INTERFACE m_axi port=in offset=slave bundle=gmem0 depth=1024 max_read_burst_length=64
#pragma HLS INTERFACE m_axi port=out offset=slave bundle=gmem1 depth=256 max_write_burst_length=16
#pragma HLS DATAFLOW
  hls::stream<beat> jin[NE], jout[NE];
#pragma HLS STREAM variable=jin depth=L*10
#pragma HLS STREAM variable=jout depth=L*2
  const unsigned ng = njobs / L;
  loader(in, jin, ng);
eng:
  for (int e = 0; e < NE; e++) {
#pragma HLS UNROLL
    engine(jin[e], jout[e], (ng + NE - 1 - e) / NE);
  }
  writer(out, jout, ng);
}
