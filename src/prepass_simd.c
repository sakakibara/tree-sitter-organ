#include <stdint.h>
#include <string.h>

/* Exact SWAR zero-byte test: result has 0x80 set in each lane whose
 * byte is zero.  ((b & 0x7F) + 0x7F) never carries across lanes, so
 * unlike the (v - 0x01..) & ~v idiom there are no inter-lane borrow
 * false positives. */
static inline uint64_t zero_lanes(uint64_t v) {
    const uint64_t L = 0x7F7F7F7F7F7F7F7FULL;
    return ~(((v & L) + L) | v | L);
}

uint16_t organ_leading_indent_swar(const uint8_t *p, uint32_t len) {
    uint32_t i = 0;
    while (i + 8u <= len) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        uint64_t is_sp = zero_lanes(w ^ 0x2020202020202020ULL);
        uint64_t is_ht = zero_lanes(w ^ 0x0909090909090909ULL);
        uint64_t bad = ~(is_sp | is_ht) & 0x8080808080808080ULL;
        if (bad == 0) { i += 8; continue; }
        i += (uint32_t)(__builtin_ctzll(bad) / 8);
        return (uint16_t)i;
    }
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    return (uint16_t)i;
}
