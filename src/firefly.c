#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <stdint.h>

#include "firefly.h"
#include "lfsr32.h"
#include "wave.h"


/*
 * ============================================================
 * DDR TABLE — ATtiny412 charlieplex
 * ============================================================
 *
 * 4 rows, each row has:
 *   [PORTOUT drive value, DDR for LED A, DDR for LED B, DDR for LED C]
 *
 * Row pin drives HIGH (via PORTOUT), the other 3 pins are
 * selectively set as outputs (LOW) to sink current through LEDs.
 *
 * R0=PA1, R1=PA2, R2=PA3, R3=PA6
 */

const uint8_t ddr_data[16] =
{
    /* Row 0: PA1 drives high */
    R0, R0 | R1, R0 | R2, R0 | R3,

    /* Row 1: PA2 drives high */
    R1, R1 | R0, R1 | R2, R1 | R3,

    /* Row 2: PA3 drives high */
    R2, R2 | R0, R2 | R1, R2 | R3,

    /* Row 3: PA6 drives high */
    R3, R3 | R0, R3 | R1, R3 | R2
};


/*
 * ============================================================
 * GLOBAL STATE
 * ============================================================
 */

volatile uint8_t flags = 0;

volatile uint8_t reset_cause = 0;

firefly_t fireflies[12];

volatile pwm_buffer_t pwm_buf;


/*
 * Internal state for TCB0 wave advancement.
 */

static firefly_p _fly_ptr;
static uint8_t   _ddr_idx;


/*
 * Hungry threshold for update_fireflies().
 */

static uint16_t hungry_threshold = 0;


/*
 * Current row index for TCA0 PWM ISR.
 */

static volatile uint8_t _row_idx;


/*
 * ============================================================
 * EEPROM ACCESS via NVMCTRL
 * ============================================================
 */

#define EEPROM_START  ((volatile uint8_t *)0x1400)


void load_seed(void)
{
    uint8_t *dst = (uint8_t *)&seed;

    for (uint8_t i = 0; i < 4; i++)
    {
        dst[i] = EEPROM_START[i];
    }
}


void save_seed(void)
{
    uint8_t *src = (uint8_t *)&seed;

    /* Wait for previous operation. */
    while (NVMCTRL.STATUS & NVMCTRL_EEBUSY_bm);

    /* Clear page buffer. */
    _PROTECTED_WRITE_SPM(NVMCTRL.CTRLA, NVMCTRL_CMD_PAGEBUFCLR_gc);

    /* Write bytes to page buffer. */
    for (uint8_t i = 0; i < 4; i++)
    {
        EEPROM_START[i] = src[i];
    }

    /* Erase + write page. */
    _PROTECTED_WRITE_SPM(NVMCTRL.CTRLA, NVMCTRL_CMD_PAGEERASEWRITE_gc);

    /* Wait for completion. */
    while (NVMCTRL.STATUS & NVMCTRL_EEBUSY_bm);
}


/*
 * ============================================================
 * TIMER CONTROL
 * ============================================================
 */

void tca0_start(void)
{
    TCA0.SINGLE.CTRLA = TCA_SINGLE_CLKSEL_DIV2_gc | TCA_SINGLE_ENABLE_bm;
}


void tca0_stop(void)
{
    TCA0.SINGLE.CTRLA = 0;
    TCA0.SINGLE.CNT = 0;
}


void tcb0_start(void)
{
    TCB0.CTRLA = TCB_CLKSEL_CLKDIV2_gc | TCB_ENABLE_bm;
}


void tcb0_stop(void)
{
    TCB0.CTRLA = 0;
    TCB0.CNT = 0;
}


/*
 * ============================================================
 * PIT SETUP — RTC Periodic Interrupt Timer
 * ============================================================
 *
 * Programs the PIT for the next wakeup interval.
 * Returns the actual number of PIT ticks programmed.
 *
 * The PIT runs from 32.768 kHz, so:
 *   CYC4     = ~122 us
 *   CYC8     = ~244 us
 *   CYC16    = ~488 us
 *   CYC32    = ~977 us
 *   CYC64    = ~1.95 ms
 *   CYC128   = ~3.9 ms
 *   CYC256   = ~7.8 ms
 *   CYC512   = ~15.6 ms
 *   CYC1024  = ~31.25 ms
 *   CYC2048  = ~62.5 ms
 *   CYC4096  = ~125 ms
 *   CYC8192  = ~250 ms
 *   CYC16384 = ~500 ms
 *   CYC32768 = ~1 s
 *
 * 'time' is in the same units as update_fireflies() returns
 * (inherited from the original WDT tick concept).
 * The original WDT ticks corresponded to power-of-2 intervals
 * starting at ~16ms (tick=1). Our PIT base unit is ~125ms.
 *
 * To match the original variable-sleep behavior:
 * We find the largest PIT period that fits within 'time',
 * program the PIT, and return how many units were consumed.
 * The caller subtracts and re-calls on the next wakeup.
 */

uint16_t pit_setup(uint16_t time)
{
    /*
     * Match original wdt_setup() algorithm:
     * Find largest power-of-2 that fits within 'time'.
     *
     * PIT periods mapped to tick units (base = ~16ms):
     *   CYC512   = 1 tick   (~15.6 ms)
     *   CYC1024  = 2 ticks  (~31.25 ms)
     *   CYC2048  = 4 ticks  (~62.5 ms)
     *   CYC4096  = 8 ticks  (~125 ms)
     *   CYC8192  = 16 ticks (~250 ms)
     *   CYC16384 = 32 ticks (~500 ms)
     *   CYC32768 = 64 ticks (~1 s)
     *
     * This matches the original WDT timing where 1 tick ≈ 16ms.
     */

    uint8_t period;
    uint16_t result;

    if (time >= 64)
    {
        period = RTC_PERIOD_CYC32768_gc;
        result = 64;
    }
    else if (time >= 32)
    {
        period = RTC_PERIOD_CYC16384_gc;
        result = 32;
    }
    else if (time >= 16)
    {
        period = RTC_PERIOD_CYC8192_gc;
        result = 16;
    }
    else if (time >= 8)
    {
        period = RTC_PERIOD_CYC4096_gc;
        result = 8;
    }
    else if (time >= 4)
    {
        period = RTC_PERIOD_CYC2048_gc;
        result = 4;
    }
    else if (time >= 2)
    {
        period = RTC_PERIOD_CYC1024_gc;
        result = 2;
    }
    else
    {
        period = RTC_PERIOD_CYC512_gc;
        result = 1;
    }

    /* Wait for sync before writing PIT registers. */
    while (RTC.PITSTATUS & RTC_CTRLBUSY_bm);

    RTC.PITCTRLA = period | RTC_PITEN_bm;

    return result;
}


/*
 * ============================================================
 * TCA0 OVERFLOW ISR — Row Switch
 * ============================================================
 *
 * Fires at ~39 kHz (20MHz / 2 / 256).
 * Switches charlieplex row and loads pre-computed brightness
 * values into CMP registers.
 */

ISR(TCA0_OVF_vect)
{
    /* All LEDs off immediately. */
    VPORTA.DIR = 0;

    /* Advance to next row. */
    uint8_t row = _row_idx;
    row = (row + 1) & 0x03;
    _row_idx = row;

    /* Load pre-computed values. */
    uint8_t cmp0 = pwm_buf.brightness[row][0];
    uint8_t cmp1 = pwm_buf.brightness[row][1];
    uint8_t cmp2 = pwm_buf.brightness[row][2];

    /* Set compare registers. */
    TCA0.SINGLE.CMP0 = cmp0;
    TCA0.SINGLE.CMP1 = cmp1;
    TCA0.SINGLE.CMP2 = cmp2;

    /* Drive the row pin high, others low. */
    VPORTA.OUT = pwm_buf.ddr_row[row][0];

    /* Turn on all 3 LEDs (set DDR for all active pins). */
    VPORTA.DIR = pwm_buf.ddr_row[row][1];

    /* Clear interrupt flag. */
    TCA0.SINGLE.INTFLAGS = TCA_SINGLE_OVF_bm;
}


/*
 * ============================================================
 * TCA0 CMP0 ISR — Turn off brightest LED
 * ============================================================
 */

ISR(TCA0_CMP0_vect)
{
    VPORTA.DIR = pwm_buf.ddr_row[_row_idx][2];
    TCA0.SINGLE.INTFLAGS = TCA_SINGLE_CMP0_bm;
}


/*
 * ============================================================
 * TCA0 CMP1 ISR — Turn off 2nd LED
 * ============================================================
 */

ISR(TCA0_CMP1_vect)
{
    VPORTA.DIR = pwm_buf.ddr_row[_row_idx][3];
    TCA0.SINGLE.INTFLAGS = TCA_SINGLE_CMP1_bm;
}


/*
 * ============================================================
 * TCA0 CMP2 ISR — Turn off dimmest LED (all off)
 * ============================================================
 */

ISR(TCA0_CMP2_vect)
{
    VPORTA.DIR = 0;
    TCA0.SINGLE.INTFLAGS = TCA_SINGLE_CMP2_bm;
}


/*
 * ============================================================
 * TCB0 ISR — Wave Advancement (~488 Hz)
 * ============================================================
 *
 * Advances wave pointers for 3 fireflies per call.
 * Pre-computes sorted brightness and DDR masks for TCA0.
 * Cycles through all 4 groups (12 fireflies) every 4 calls
 * = ~122 Hz effective per-firefly sample rate.
 */

ISR(TCB0_INT_vect)
{
    firefly_p fly = _fly_ptr;
    uint8_t ddr_index = _ddr_idx;

    uint8_t a, b, c;
    uint8_t a_ddr, b_ddr, c_ddr;
    uint8_t tmp;
    const uint8_t *wp;

    /*
     * Read wave samples and advance pointers.
     */

    /* Firefly A */
    wp = (const uint8_t *)(fly[0].wave_ptr);
    if (wp != 0)
    {
        a = *wp;
        if (++wp >= (const uint8_t *)(fly[0].wave_end))
            wp = 0;
        fly[0].wave_ptr = (uint16_t)wp;
    }
    else
    {
        a = 0;
    }

    /* Firefly B */
    wp = (const uint8_t *)(fly[1].wave_ptr);
    if (wp != 0)
    {
        b = *wp;
        if (++wp >= (const uint8_t *)(fly[1].wave_end))
            wp = 0;
        fly[1].wave_ptr = (uint16_t)wp;
    }
    else
    {
        b = 0;
    }

    /* Firefly C */
    wp = (const uint8_t *)(fly[2].wave_ptr);
    if (wp != 0)
    {
        c = *wp;
        if (++wp >= (const uint8_t *)(fly[2].wave_end))
            wp = 0;
        fly[2].wave_ptr = (uint16_t)wp;
    }
    else
    {
        c = 0;
    }

    /*
     * Set FLAG_WAVE if any firefly in this group is active.
     */
    if (fly[0].wave_ptr | fly[1].wave_ptr | a | b | c |
        fly[2].wave_ptr)
    {
        flags |= FLAG_WAVE;
    }

    /*
     * Load DDR data for current row.
     * ddr_data[ddr_index + 0] = PORTOUT row drive
     * ddr_data[ddr_index + 1] = DDR for LED A
     * ddr_data[ddr_index + 2] = DDR for LED B
     * ddr_data[ddr_index + 3] = DDR for LED C
     */
    uint8_t row_drive = ddr_data[ddr_index];
    a_ddr = ddr_data[ddr_index + 1];
    b_ddr = ddr_data[ddr_index + 2];
    c_ddr = ddr_data[ddr_index + 3];

    /*
     * Advance to next row group.
     */
    fly += 3;
    ddr_index = (ddr_index + 4) & 0x0F;

    /*
     * Completed all 12 fireflies?
     */
    if (ddr_index == 0)
    {
        uint8_t f = flags;

        if (!(f & FLAG_WAVE))
        {
            f &= ~FLAG_TIMER;
        }

        f &= ~FLAG_WAVE;
        flags = f;

        fly = (firefly_p)&fireflies[0];
    }

    _fly_ptr = fly;
    _ddr_idx = ddr_index;

    /*
     * Sort brightness values descending.
     */
    if (b < c)
    {
        tmp = b; b = c; c = tmp;
        tmp = b_ddr; b_ddr = c_ddr; c_ddr = tmp;
    }

    if (a < b)
    {
        tmp = a; a = b; b = tmp;
        tmp = a_ddr; a_ddr = b_ddr; b_ddr = tmp;

        if (b < c)
        {
            tmp = b; b = c; c = tmp;
            tmp = b_ddr; b_ddr = c_ddr; c_ddr = tmp;
        }
    }

    /*
     * Build cumulative DDR states.
     * At start: all 3 LEDs on (a_ddr | b_ddr | c_ddr)
     * After CMP0 (brightest off): b_ddr | c_ddr
     * After CMP1 (2nd off): c_ddr only
     * After CMP2 (dimmest off): 0
     *
     * If all brightness values are 0, set all DDR to 0
     * to prevent ghost glow from brief pin driver activation.
     */
    uint8_t ddr_all;
    uint8_t ddr_after_a;
    uint8_t ddr_after_b;

    if (a == 0)
    {
        /* No LED active in this row. */
        ddr_all = 0;
        ddr_after_a = 0;
        ddr_after_b = 0;
        row_drive = 0;
    }
    else
    {
        /*
         * Only include DDR bits for LEDs with brightness > 0.
         * LEDs with brightness 0 must not be driven at all.
         */
        if (b == 0)
        {
            b_ddr = 0;
            c_ddr = 0;
        }
        else if (c == 0)
        {
            c_ddr = 0;
        }

        ddr_all = a_ddr | b_ddr | c_ddr;
        ddr_after_a = b_ddr | c_ddr;
        ddr_after_b = c_ddr;
    }

    /*
     * Compute row index from ddr_index (which already advanced).
     * Current row = (ddr_index / 4 - 1) & 3
     * But since ddr_index already advanced, the row we just
     * computed brightness for is ((ddr_index - 4) / 4) & 3.
     */
    uint8_t row = ((ddr_index - 4) & 0x0F) >> 2;
    if (ddr_index == 0) row = 3;  /* wrap case */

    /*
     * Store into PWM buffer for TCA0 to pick up.
     * Brightness stored directly as CMP values.
     * Higher brightness = LED on longer = higher CMP value.
     */
    pwm_buf.brightness[row][0] = a;
    pwm_buf.brightness[row][1] = b;
    pwm_buf.brightness[row][2] = c;

    pwm_buf.ddr_row[row][0] = row_drive;
    pwm_buf.ddr_row[row][1] = ddr_all;
    pwm_buf.ddr_row[row][2] = ddr_after_a;
    pwm_buf.ddr_row[row][3] = ddr_after_b;

    /* Clear interrupt flag. */
    TCB0.INTFLAGS = TCB_CAPT_bm;
}


/*
 * ============================================================
 * UPDATE FIREFLIES — Energy / Hungry Algorithm
 * ============================================================
 *
 * Same logic as Firefly2026 original.
 * Returns time (in PIT ticks of ~125ms) until next update.
 */

uint16_t update_fireflies(void)
{
    firefly_p fly;
    firefly_p cur;

    /*
     * Update waves — assign new waves to idle fireflies.
     * Limit: only one firefly per row group can start per update.
     * This prevents triplet lockstep.
     */
    fly = (firefly_p)&fireflies[0];

    for (uint8_t i = 12; i > 0; i--)
    {
        if ((fly->wave_ptr == 0) &&
            (fly->hungry == 0))
        {
            /*
             * Select one of the 32 waves randomly.
             */
            const wave_data_t *wd = &wave_data[lfsr(5)];

            cur = fly;

            /* Wave start pointer. */
            uint16_t ptr = (uint16_t)(wd->wave_ptr);

            /* Wave end pointer. */
            cur->wave_end = (uint16_t)(wd->wave_end);

            /* Atomically activate wave. */
            cli();
            cur->wave_ptr = ptr;
            flags |= FLAG_TIMER;
            sei();

            /* Energy for this wave. */
            uint16_t energy = wd->energy;

            /*
             * Mutual feeding.
             * First firefly gets full energy.
             * Next gets half, then quarter, etc.
             */
            while (energy > 0)
            {
                cur->energy += energy;

                if (++cur >= &fireflies[12])
                    cur = (firefly_p)&fireflies[0];

                energy /= 2;
            }
        }

        fly++;
    }

    /*
     * Calculate feeding step.
     */
    uint16_t food = 0xFFFF;
    uint16_t threshold = hungry_threshold;

    fly = (firefly_p)&fireflies[0];

    for (uint8_t i = 12; i > 0; i--)
    {
        uint16_t hungry = fly->hungry;

        if (fly->energy > 0)
        {
            if ((hungry == 0) ||
                (hungry > threshold))
            {
                uint16_t max = (uint16_t)(0xFFFF - fly->energy);

                if (hungry < max)
                    hungry += fly->energy;
                else
                    hungry = 0xFFFF;

                if (threshold < 0xFFF0)
                    threshold += 3;
            }
            else if (threshold > 1)
            {
                threshold -= 2;
            }
        }

        if (hungry < food)
            food = hungry;

        fly->hungry = hungry;
        fly->energy = 0;
        fly++;
    }

    hungry_threshold = threshold;

    /*
     * Feeding — subtract minimum from all.
     */
    fly = (firefly_p)&fireflies[0];

    for (uint8_t i = 12; i > 0; i--)
    {
        fly->hungry -= food;
        fly++;
    }

    /*
     * Return PIT cycles until next update.
     */
    return food;
}


/*
 * ============================================================
 * INITIALIZATION
 * ============================================================
 */

void init(void)
{
    /*
     * Disable the main clock prescaler.
     * The ATtiny412 boots with a /6 prescaler enabled (3.33 MHz).
     * We need full 20 MHz. CCP-protected write.
     */
    _PROTECTED_WRITE(CLKCTRL.MCLKCTRLB, 0);

    /*
     * Read and clear reset cause.
     */
    reset_cause = RSTCTRL.RSTFR;
    RSTCTRL.RSTFR = reset_cause;

    /*
     * Initialize global state.
     */
    _fly_ptr = (firefly_p)&fireflies[0];
    _ddr_idx = 0;
    _row_idx = 0;
    flags = 0;

    /*
     * PORT — all pins input, no pull-ups.
     * Clear output register.
     */
    PORTA.DIR = 0;
    PORTA.OUT = 0;

    /* Disable pull-ups on all pins. */
    PORTA.PIN1CTRL = 0;
    PORTA.PIN2CTRL = 0;
    PORTA.PIN3CTRL = 0;
    PORTA.PIN6CTRL = 0;
    PORTA.PIN7CTRL = 0;

    /*
     * Disable unused peripherals for power savings.
     * On ATtiny412: USART0, SPI0, TWI0, TCD0, ADC0 (initially), AC0.
     */
    /* Analog comparator off. */
    AC0.CTRLA = 0;

    /* ADC disabled initially. */
    ADC0.CTRLA = 0;

    /*
     * TCA0 — Normal mode, prescaler /2, PER=255.
     * 20MHz / 2 / 256 = ~39 kHz overflow rate.
     * 4 rows → ~9.7 kHz per LED.
     * Initially stopped.
     */
    TCA0.SINGLE.CTRLA = 0;  /* stopped */
    TCA0.SINGLE.CTRLB = TCA_SINGLE_WGMODE_NORMAL_gc;
    TCA0.SINGLE.PER = 255;
    TCA0.SINGLE.CMP0 = 0;
    TCA0.SINGLE.CMP1 = 0;
    TCA0.SINGLE.CMP2 = 0;
    TCA0.SINGLE.CNT = 0;
    TCA0.SINGLE.INTCTRL = TCA_SINGLE_OVF_bm |
                           TCA_SINGLE_CMP0_bm |
                           TCA_SINGLE_CMP1_bm |
                           TCA_SINGLE_CMP2_bm;
    TCA0.SINGLE.INTFLAGS = TCA_SINGLE_OVF_bm |
                            TCA_SINGLE_CMP0_bm |
                            TCA_SINGLE_CMP1_bm |
                            TCA_SINGLE_CMP2_bm;

    /*
     * TCB0 — Periodic Interrupt mode.
     * Clock: CLK_PER/2 = 10 MHz.
     * Period: 10000000 / 488 ≈ 20492 ticks → ~488 Hz.
     * With 4 rows cycling, each firefly gets updated at ~122 Hz.
     * Initially stopped.
     */
    TCB0.CTRLA = 0;  /* stopped */
    TCB0.CTRLB = TCB_CNTMODE_INT_gc;
    TCB0.CCMP = 20492;
    TCB0.CNT = 0;
    TCB0.INTCTRL = TCB_CAPT_bm;
    TCB0.INTFLAGS = TCB_CAPT_bm;

    /*
     * RTC / PIT — Periodic Interrupt Timer.
     * Clock source: internal 32.768 kHz ULP oscillator.
     * Initial period: CYC4096 (~125 ms).
     * RUNSTDBY enabled so it wakes from STANDBY.
     */
    while (RTC.STATUS & RTC_CTRLABUSY_bm);
    RTC.CLKSEL = RTC_CLKSEL_INT32K_gc;

    while (RTC.PITSTATUS & RTC_CTRLBUSY_bm);
    RTC.PITCTRLA = RTC_PERIOD_CYC4096_gc | RTC_PITEN_bm;
    RTC.PITINTCTRL = RTC_PI_bm;

    /*
     * SLPCTRL — default to STANDBY.
     */
    SLPCTRL.CTRLA = SLPCTRL_SMODE_STDBY_gc | SLPCTRL_SEN_bm;

    /*
     * LFSR seed from EEPROM.
     */
    load_seed();

    /* Seed must never be 0 (LFSR degenerate state). */
    if (seed == 0)
        seed = 0xDEADBEEF;

    /* Advance seed and save back. */
    lfsr_poly(32, 0x8140C9D5);
    save_seed();
}
