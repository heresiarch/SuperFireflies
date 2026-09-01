#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <stdint.h>

#include "firefly.h"


/*
 * ============================================================
 * RTC PIT ISR — Periodic wakeup from STANDBY
 * ============================================================
 */

ISR(RTC_PIT_vect)
{
    /* Plain byte store — see firefly.h on why this is its own byte. */
    pit_tick = 1;
    RTC.PITINTFLAGS = RTC_PI_bm;
}


/*
 * ============================================================
 * LDR NIGHT DETECTION — ADC in STANDBY
 * ============================================================
 *
 * LDR circuit:
 *
 *              10k          ┌──────┬──────┐
 * PA7/AIN7 ───/\/\/─────────┤      │      │
 *                          LDR    10nF
 *                           │      │
 *                          GND    GND
 *
 * PA7 connects through a 10k series resistor to a node that
 * has the LDR and a 10nF capacitor in parallel to GND.
 *
 * Measurement (capacitor charge/discharge method):
 *   1. Drive PA7 HIGH — charges the cap through the 10k.
 *   2. Make PA7 high-impedance input — with no current through
 *      the 10k, the pin reads the node voltage directly.
 *   3. The cap discharges through the LDR:
 *        bright → low LDR resistance → fast discharge → low ADC
 *        dark   → high LDR resistance → slow discharge → high ADC
 *   4. A high ADC value means dark — night mode active.
 *
 * Note: the 10k sits between the pin and the ADC sample-and-hold
 * cap, so it raises the source impedance during sampling. Only a
 * coarse day/night threshold is needed, so this is acceptable;
 * ADC_SAMPCAP reduces the sample cap to help settling.
 */

void measure_isnight(void)
{
    uint16_t ldr_value;

    /* Clear night flag first. */
    flags &= ~FLAG_ISNIGHT;

    /*
     * Charge the 10 nF capacitor by driving PA7 HIGH.
     * Charge path is PA7 → 10k → cap, tau = 10k × 10nF = 100 µs.
     */
    PORTA.DIRSET = (1 << LDR_PIN);
    PORTA.OUTSET = (1 << LDR_PIN);

    /*
     * Configure ADC0:
     * - AIN7 (PA7)
     * - VDD reference
     * - 10-bit resolution
     * - Prescaler /16 → 20MHz/16 = 1.25 MHz ADC clock
     * - RUNSTDBY for noise reduction
     */
    ADC0.CTRLA = ADC_ENABLE_bm;
    ADC0.CTRLB = 0;  /* No accumulation */
    ADC0.CTRLC = ADC_PRESC_DIV16_gc | ADC_REFSEL_VDDREF_gc | ADC_SAMPCAP_bm;
    ADC0.CTRLD = 0;
    ADC0.MUXPOS = LDR_MUXPOS;

    /*
     * The RESRDY interrupt stays DISABLED throughout.
     *
     * Every conversion below is awaited by polling INTFLAGS. An
     * enabled RESRDY interrupt would fire the moment the flag is
     * set and clear it inside the ISR, so the polling loop would
     * never observe it and would spin forever — taking the whole
     * lamp down with it, since main then never reaches its
     * watchdog kick.
     */
    ADC0.INTCTRL = 0;

    /*
     * Run several conversions while charging the capacitor.
     * Each conversion takes ~13 ADC clocks = ~10.4 µs.
     * 16 conversions ≈ 166 µs charge time.
     */
    for (uint8_t i = 0; i < 16; i++)
    {
        ADC0.COMMAND = ADC_STCONV_bm;

        /* Wait for conversion complete. */
        while (!(ADC0.INTFLAGS & ADC_RESRDY_bm));
        (void)ADC0.RES;  /* Discard result, clear flag. */
    }

    /*
     * Make PA7 high-impedance. The capacitor now discharges
     * through the LDR only (the 10k leads to a high-impedance
     * input, so no current flows that way and the pin reads
     * the node voltage directly).
     */
    PORTA.DIRCLR = (1 << LDR_PIN);
    PORTA.OUTCLR = (1 << LDR_PIN);

    /*
     * Short delay to allow discharge through LDR.
     * With 10nF and LDR ~10k (bright): tau = 100 µs
     * With 10nF and LDR ~1M (dark): tau = 10 ms
     * A few hundred µs is enough to differentiate.
     * Run 4 dummy ADC conversions (~42 µs total) as delay.
     */
    for (uint8_t i = 0; i < 4; i++)
    {
        ADC0.COMMAND = ADC_STCONV_bm;
        while (!(ADC0.INTFLAGS & ADC_RESRDY_bm));
        (void)ADC0.RES;
    }

    /*
     * Perform the actual measurement.
     *
     * Polled, not slept through. A conversion takes ~26 µs at
     * this prescaler, so there is nothing meaningful to save by
     * sleeping, and sleeping here was actively unsafe: the PIT
     * runs with RUNSTDBY and could wake us before the conversion
     * finished, leaving a stale ADC0.RES to be read as a valid
     * light level.
     */
    ADC0.COMMAND = ADC_STCONV_bm;
    while (!(ADC0.INTFLAGS & ADC_RESRDY_bm));

    ldr_value = ADC0.RES;

    /*
     * High ADC = cap still charged = LDR high impedance = dark = night.
     */
    if (ldr_value > LDR_THRESHOLD)
    {
        flags |= FLAG_ISNIGHT;
    }

    /*
     * Disable ADC for power savings.
     */
    ADC0.CTRLA = 0;

    /* PA7 safely as input, no pull-up. */
    PORTA.DIRCLR = (1 << LDR_PIN);
    PORTA.OUTCLR = (1 << LDR_PIN);
}

/*
 * No ADC0_RESRDY ISR exists on purpose — see the note in
 * measure_isnight(). All conversions are awaited by polling.
 */


/*
 * ============================================================
 * MAIN
 * ============================================================
 */

int main(void)
{
    init();

    sei();

    uint16_t timeout = 0;
    uint16_t measure = 0;

    while (1)
    {
        /*
         * Kick the WDT (crash guard).
         * WDT is fuse-enabled at 2s timeout.
         * If main() hangs, the WDT resets the chip.
         */
        __asm__ __volatile__("wdr");

        /*
         * Safety clamp.
         */
        if (timeout > 0x7FFF)
        {
            timeout = 0;
        }

        /*
         * ====================================================
         * PIT WAKEUP
         * ====================================================
         */
        if (pit_tick)
        {
            pit_tick = 0;

            if (timeout == 0)
            {
                /*
                 * NIGHT — generate firefly activity.
                 */
                if (flags & FLAG_ISNIGHT)
                {
                    timeout = update_fireflies();

                    /*
                     * Count down to next LDR measurement.
                     */
                    if (measure <= timeout)
                    {
                        measure = MEASURE_INTERVAL;
                        flags |= FLAG_MEASURE;
                    }
                    else
                    {
                        measure -= timeout;
                    }
                }
                /*
                 * DAY — no fireflies, just wait for next measurement.
                 */
                else
                {
                    measure = MEASURE_INTERVAL;
                    timeout = measure;
                    flags |= FLAG_MEASURE;
                }
            }

            /*
             * Program PIT for next wakeup.
             */
            timeout -= pit_setup(timeout);
        }

        /*
         * ====================================================
         * FIREFLIES ACTIVE — IDLE sleep
         * ====================================================
         *
         * TCA0 and TCB0 run. CPU sleeps in IDLE.
         * Timer ISRs continue driving the LEDs.
         */
        if (wave_active)
        {
            /* Ensure timers are running. */
            tca0_start();
            tcb0_start();

            /* IDLE sleep — timers stay active. */
            SLPCTRL.CTRLA = SLPCTRL_SMODE_IDLE_gc | SLPCTRL_SEN_bm;
            sleep_cpu();
        }
        /*
         * ====================================================
         * NO FIREFLIES — STANDBY sleep
         * ====================================================
         *
         * Stop timers. Only PIT runs (from 32.768 kHz ULP).
         * Current draw: ~0.7 µA.
         */
        else
        {
            /* Stop PWM and wave timers. */
            tca0_stop();
            tcb0_stop();

            /* All LED pins off. */
            VPORTA.DIR = 0;
            VPORTA.OUT = 0;

            /*
             * LDR measurement if due.
             */
            if (flags & FLAG_MEASURE)
            {
                flags &= ~FLAG_MEASURE;
                measure_isnight();
            }

            /* STANDBY sleep — only PIT wakes us. */
            SLPCTRL.CTRLA = SLPCTRL_SMODE_STDBY_gc | SLPCTRL_SEN_bm;
            sleep_cpu();
        }
    }
}
