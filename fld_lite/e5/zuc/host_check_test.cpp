// E5.2: offline test of e52_host's request builder and reply checker against a spec-built reply.
#define main e52_main
#include "../e52_host.cpp"
#undef main
static unsigned fake_reply(unsigned id, uint8_t *o)
{
  uint8_t q[2048]; make_req(q, id); const zreq r = req_of(id); const unsigned L = r.len;
  memset(o, 0, 2048);
  memcpy(o, q + 6, 6); memcpy(o + 6, q, 6); o[12] = q[12]; o[13] = q[13];
  memcpy(o + 14, q + 14, 20); memcpy(o + 26, q + 30, 4); memcpy(o + 30, q + 26, 4); o[24] = o[25] = 0;
  const unsigned ipl = r.op == OP_CONF ? L + 0x72 : 0x62, udl = r.op == OP_CONF ? L + 0x5E : 0x4E;
  o[16] = ipl >> 8; o[17] = ipl; memcpy(o + 34, q + 36, 2); memcpy(o + 36, q + 34, 2); o[38] = udl >> 8; o[39] = udl;
  memcpy(o + 64, q + 64, 12);
  uint8_t key[16], iv[16]; keyiv(id, key, iv);
  if (r.op == OP_CONF) { zuc_xor(key, iv, q + 128, o + 128, L); return 128 + L; }
  const uint32_t m = eia3_mac(key, iv, q + 128, 8 * L); o[108] = m >> 24; o[109] = m >> 16; o[110] = m >> 8; o[111] = m; return 112;
}
int main()
{
  int bad = 0; uint8_t o[2048]; unsigned id;
  for (unsigned i = 0; i < 40; i++) {
    if (req_of(i).kind) continue;
    const unsigned len = fake_reply(i, o);
    const char *w = check_reply(o, len, id);
    if (w || id != i) { printf("reply %u rejected: %s\n", i, w ? w : "id"); bad++; }
    o[len - 1] ^= 1;                                                // corrupt the last byte: must be caught
    if (!check_reply(o, len, id)) { printf("corrupted reply %u accepted\n", i); bad++; }
  }
  GUARD = true; unsigned mal = 0; for (unsigned i = 0; i < 40; i++) mal += req_of(i).kind != 0;
  printf("guard mode: %u of 40 requests malformed\n", mal);
  printf("HOSTCHECK %s\n", bad ? "FAIL" : "PASS"); return bad;
}
