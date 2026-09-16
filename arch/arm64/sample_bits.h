/* Deterministic normalized-audio conversion without kernel FP instructions.
 * Single-input routes preserve float bits; fan-in mixes saturating Q1.31.
 * NaN/Inf are silenced at conversion boundaries. */
#ifndef ARM64_SAMPLE_BITS_H
#define ARM64_SAMPLE_BITS_H
#include <stdint.h>
static inline int32_t sample_q31(uint32_t f) {
  unsigned e = (f >> 23) & 255u;
  if (e == 255 || e < 96)
    return 0;
  uint64_t m = (f & 0x7fffffu) | 0x800000u;
  if (e >= 127)
    m = 2147483648ull;
  else if (e >= 119)
    m <<= e - 119;
  else
    m >>= 119 - e;
  if (f >> 31)
    return m >= 2147483648ull ? INT32_MIN : -(int32_t)m;
  return m >= 2147483647ull ? INT32_MAX : (int32_t)m;
}
static inline uint32_t sample_float(int64_t q) {
  if (q > INT32_MAX)
    q = INT32_MAX;
  if (q < INT32_MIN)
    q = INT32_MIN;
  if (!q)
    return 0;
  uint32_t sign = q < 0 ? 0x80000000u : 0, m = (uint32_t)(q < 0 ? -q : q);
  unsigned bit = 0;
  for (uint32_t n = m; n >>= 1;)
    ++bit;
  uint32_t sig = bit > 23 ? m >> (bit - 23) : m << (23 - bit);
  return sign | ((bit + 96u) << 23) | (sig & 0x7fffffu);
}
#endif
