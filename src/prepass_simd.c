#include <stdint.h>
#include <string.h>

uint16_t organ_leading_indent_swar(const uint8_t *p, uint32_t len) {
    uint16_t i = 0;
    while (i + 8 <= len) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        uint64_t sp_mask = w ^ 0x2020202020202020ULL;
        uint64_t ht_mask = w ^ 0x0909090909090909ULL;
        uint64_t zero_sp = (sp_mask - 0x0101010101010101ULL) & ~sp_mask & 0x8080808080808080ULL;
        uint64_t zero_ht = (ht_mask - 0x0101010101010101ULL) & ~ht_mask & 0x8080808080808080ULL;
        uint64_t neither = ~(zero_sp | zero_ht);
        uint64_t bad = neither & 0x8080808080808080ULL;
        if (bad == 0) { i += 8; continue; }
        i += (uint16_t)(__builtin_ctzll(bad) / 8);
        return i;
    }
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    return i;
}
