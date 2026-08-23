#include "lfsr32.h"

#define LFSR_POLYNOMIAL 0xC3A8AD09UL

uint32_t seed;


/*
 * Galois LFSR with configurable polynomial.
 *
 * Shifts the 32-bit seed right one bit per iteration.
 * If the ejected bit is 1, XOR with polynomial.
 * Collects ejected bits into the result.
 */
uint16_t lfsr_poly(uint8_t count, uint32_t polynomial)
{
    uint16_t result = 0;

    while (count != 0)
    {
        uint8_t feedback = (uint8_t)(seed & 1U);

        seed >>= 1;

        if (feedback != 0)
        {
            seed ^= polynomial;
        }

        result = (uint16_t)((result << 1) | feedback);

        --count;
    }

    return result;
}


/*
 * LFSR with default polynomial.
 */
uint16_t lfsr(uint8_t count)
{
    return lfsr_poly(count, LFSR_POLYNOMIAL);
}
