#ifndef LFSR32_H
#define LFSR32_H

/*
 * ============================================================
 * 32-bit Galois LFSR — Pseudo-Random Number Generator
 * ============================================================
 *
 * Provides a fast, compact PRNG suitable for selecting random
 * wave patterns. The seed is persisted in EEPROM across power
 * cycles so each run produces a different sequence.
 *
 * lfsr(n)      — returns n random bits using default polynomial.
 * lfsr_poly(n) — returns n random bits using a given polynomial.
 *
 * The 'seed' variable is the 32-bit LFSR state. It must be
 * loaded from EEPROM before first use and saved back after
 * advancing.
 */

#include <stdint.h>

extern uint32_t seed;

uint16_t lfsr(uint8_t count);
uint16_t lfsr_poly(uint8_t count, uint32_t polynomial);

#endif
