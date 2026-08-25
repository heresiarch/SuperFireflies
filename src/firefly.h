#ifndef FIREFLY_H
#define FIREFLY_H

#include <avr/io.h>
#include <stdint.h>


/*
 * ============================================================
 * FLAGS
 * ============================================================
 */

#define FLAG_WAVE       (1 << 0)
#define FLAG_TIMER      (1 << 1)
#define FLAG_UPDATE     (1 << 2)
#define FLAG_ISNIGHT    (1 << 3)
#define FLAG_MEASURE    (1 << 4)

/*
 * MEASURE_INTERVAL: PIT ticks (~125ms each) between LDR measurements.
 * 375 ticks × 125ms = ~47 seconds between day/night checks.
 */
#define MEASURE_INTERVAL    ((uint16_t)(1 * 375))

/*
 * LDR_THRESHOLD: ADC value (0–1023) above which it is considered dark.
 * Higher value = darker threshold. Adjust for your LDR/resistor combo.
 */
#define LDR_THRESHOLD       512


/*
 * ============================================================
 * LED ROWS — ATtiny412 Pin Assignment
 * ============================================================
 *
 * Pin 2: PA6 — Row 3
 * Pin 4: PA1 — Row 0
 * Pin 5: PA2 — Row 1
 * Pin 7: PA3 — Row 2
 *
 * PA7 (Pin 3) — LDR / ADC (AIN7)
 * PA0 (Pin 6) — UPDI (reserved)
 */

#define R0  (1 << PIN1_bp)   /* PA1 */
#define R1  (1 << PIN2_bp)   /* PA2 */
#define R2  (1 << PIN3_bp)   /* PA3 */
#define R3  (1 << PIN6_bp)   /* PA6 */

/* Mask of all charlieplex pins */
#define LED_MASK    (R0 | R1 | R2 | R3)

/* LDR pin */
#define LDR_PIN     PIN7_bp
#define LDR_MUXPOS  ADC_MUXPOS_AIN7_gc


/*
 * ============================================================
 * FIREFLY STRUCTURE
 * ============================================================
 *
 * Must remain exactly 8 bytes.
 */

typedef struct
{
    uint16_t wave_end;
    uint16_t wave_ptr;
    uint16_t hungry;
    uint16_t energy;
} firefly_t;

typedef firefly_t *firefly_p;


/*
 * ============================================================
 * DDR TABLE
 * ============================================================
 *
 * 4 rows × 4 entries each:
 *   [row_drive, led_a_ddr, led_b_ddr, led_c_ddr]
 *
 * Charlieplex: row pin drives high, each LED is between
 * the row pin and one of the other 3 pins.
 * DDR entry = row pin OR target pin (both as outputs).
 */

extern const uint8_t ddr_data[16];


/*
 * ============================================================
 * PWM STAGE DDR MASKS
 * ============================================================
 *
 * Cumulative DIR masks applied by the compare ISRs of the
 * current charlieplex row. Written by the TCA0 OVF ISR.
 *
 *   cmp_ddr[0] — applied at CMP0: brightest LED on
 *   cmp_ddr[1] — applied at CMP1: brightest + 2nd on
 *   cmp_ddr[2] — applied at CMP2: all three on
 *
 * Only the stages belonging to active fireflies are armed;
 * the remaining compare interrupts are disabled per row so
 * an inactive firefly (brightness 0, which negates to 0)
 * cannot fire a compare match at count 0.
 */

extern volatile uint8_t cmp_ddr[3];


/*
 * ============================================================
 * GLOBAL STATE (extern declarations)
 * ============================================================
 */

extern volatile uint8_t  flags;
extern volatile uint8_t  reset_cause;
extern          firefly_t fireflies[12];


/*
 * ============================================================
 * FUNCTION DECLARATIONS
 * ============================================================
 */

void     init(void);
uint16_t update_fireflies(void);
void     measure_isnight(void);

void     load_seed(void);
void     save_seed(void);

void     tca0_start(void);
void     tca0_stop(void);
void     tcb0_start(void);
void     tcb0_stop(void);

uint16_t pit_setup(uint16_t time);


#endif /* FIREFLY_H */
