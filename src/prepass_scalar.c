#include <stdint.h>

uint16_t organ_leading_indent_scalar(const uint8_t *p, uint32_t len) {
    uint16_t i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    return i;
}
