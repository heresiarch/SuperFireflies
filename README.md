# SuperFireflies

A firefly lamp for the ATtiny412. Twelve LEDs simulate independent fireflies
that glow, fade, and loosely synchronize with each other — no fixed blink
pattern, each run looks different.

Port of the [Firefly2026](../Firefly2026) project (ATtiny45 @ 8 MHz) to the
ATtiny412 @ 8 MHz (16 MHz oscillator / 2), preserving identical PWM timing
and wave playback characteristics while using the newer chip's cleaner
sleep management and simpler code.

Based on the original concept from the
[mikrocontroller.net thread](https://www.mikrocontroller.net/topic/99803?page=single)
by H. Reddmann.

---

## Hardware

| Item | Detail |
|------|--------|
| MCU | ATtiny412 @ 8 MHz (16 MHz oscillator, /2 prescaler) |
| Package | 8-pin SOIC |
| LEDs | 12, wired as antiparallel pairs on 4 pins (charlieplexing) |
| LDR | PA7 / AIN7 — capacitor discharge method for day/night detection |
| Power | Battery (3V coin cell or 2xAA); deep-sleep between flashes |
| Programmer | UPDI (PA0, pin 6) — any UPDI-capable programmer (SNAP, SerialUPDI, etc.) |

---

## Pinout

```
              ATtiny412
            8-Pin SOIC

         ┌──────────────┐
  VDD  1 │●             │ 8  GND
  PA6  2 │  LED Row 3   │ 7  PA3 — LED Row 2
  PA7  3 │  LDR (AIN7)  │ 6  PA0 — UPDI (reserved)
  PA1  4 │  LED Row 0   │ 5  PA2 — LED Row 1
         └──────────────┘
```

| Pin | Port | Function | Direction |
|-----|------|----------|-----------|
| 1 | VDD | Power supply (1.8–5.5V) | — |
| 2 | PA6 | LED Row 3 (charlieplex) | Output (when active) |
| 3 | PA7 | LDR / ADC input (AIN7) | I/O (charge/measure) |
| 4 | PA1 | LED Row 0 (charlieplex) | Output (when active) |
| 5 | PA2 | LED Row 1 (charlieplex) | Output (when active) |
| 6 | PA0 | UPDI programming | Reserved |
| 7 | PA3 | LED Row 2 (charlieplex) | Output (when active) |
| 8 | GND | Ground | — |

---

## LED Wiring — Charlieplex Matrix

Four pins drive 12 LEDs in a charlieplex arrangement. Each LED sits between
two pins, with its polarity determining which one it is.

```
         PA1    PA2    PA3    PA6
          │      │      │      │
  Row 0:  ├──►D1─┤  ├──►D2─┤  ├──►D3─┤
  (PA1 H) │      │      │      │
  Row 1:  ├──◄D4─┤  ├──►D5─┤  ├──►D6─┤
  (PA2 H) │      │      │      │
  Row 2:  ├──◄D7─┤  ├──◄D8─┤  ├──►D9─┤
  (PA3 H) │      │      │      │
  Row 3:  ├──◄D10┤  ├──◄D11┤  ├──◄D12┤
  (PA6 H) │      │      │      │
```

Each row: one pin drives HIGH, the other 3 pins selectively sink current
(set as output LOW) to light individual LEDs. Direction of the LED (anode
towards row pin or away) determines which of the 12 positions it occupies.

**Current limiting:** Each LED needs a series resistor (47–220 Ohm depending
on LED type and desired brightness). In the original design, the short duty
cycle from multiplexing provides implicit current limiting with low-value
resistors.

---

## LDR Circuit

```
              10k          ┌───────┬───────┐
PA7/AIN7 ────/\/\/─────────┤       │       │
                         ┌───┐   ┌───┐
                         │LDR│   │10n│
                         └───┘   └───┘
                           │       │
                          GND     GND
```

PA7 connects through a 10k series resistor to a node that carries the
LDR and a 10 nF capacitor, both in parallel to GND.

**Measurement principle (capacitor charge/discharge):**
1. PA7 driven HIGH — charges the 10 nF cap through the 10k (tau ≈ 100 µs).
2. PA7 switched to high-impedance input — with no current through the
   10k, the pin reads the node voltage directly.
3. The cap discharges through the LDR; the ADC reads the remaining
   voltage after a short fixed delay.

A **high ADC value** means the capacitor discharged slowly (LDR is high
impedance) — it is **dark** (night mode active, fireflies enabled).

A **low ADC value** means fast discharge (LDR is low impedance) — it is
**bright** (day mode, fireflies disabled, deep sleep).

The threshold is defined as `LDR_THRESHOLD` in `firefly.h` (default: 512).
Adjust based on your LDR and desired light sensitivity.

---

## Building and Flashing

The project uses [PlatformIO](https://platformio.org/). No Arduino framework
is used — this is bare-metal AVR-libc.

```bash
# Build
pio run

# Flash (SerialUPDI on /dev/ttyUSB0)
pio run --target upload

# Flash fuses (needed once for 20MHz + WDT)
pio run --target fuses
```

### Programmer Setup

The default configuration uses SerialUPDI on `/dev/ttyUSB0`. To change
the programmer or port, edit `platformio.ini`:

```ini
upload_protocol = serialupdi
upload_flags =
    -P
    /dev/ttyUSB0
```

Other supported programmers: `snap_updi`, `pickit4_updi`, `atmelice_updi`.

---

## Fuse Configuration

| Fuse | Value | Meaning |
|------|-------|---------|
| WDTCFG | 0x0B | WDT enabled, normal mode, 8s timeout (crash guard) |
| BODCFG | 0x00 | Brown-out detection disabled |
| OSCCFG | 0x01 | 16 MHz internal oscillator (divided to 8 MHz in software) |
| TCD0CFG | 0x00 | Default (TCD0 not used) |
| SYSCFG0 | 0xC9 | Reset pin as RESET (safe for UPDI recovery), CRC disabled |
| APPEND | 0x00 | No append section |
| BOOTEND | 0x00 | No bootloader section |

**Important:** SYSCFG0 = 0xC9 keeps PA0 as a reset/UPDI pin. This ensures
you can always reprogram the chip without an HV programmer.

---

## How It Works

### Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                    FAST DOMAIN (ISRs)                    │
│                                                         │
│  TCA0 (488 Hz overflow, 122 Hz per LED)                 │
│  ├─ OVF: all LEDs off, read waves, sort, set PORTOUT    │
│  ├─ CMP0: turn ON brightest LED (fires first)           │
│  ├─ CMP1: add medium LED                                │
│  └─ CMP2: add dimmest LED (fires last)                  │
│                                                         │
│  One CMP channel armed per active firefly, per row      │
│                                                         │
├─────────────────────────────────────────────────────────┤
│                    SLOW DOMAIN (main loop)              │
│                                                         │
│  RTC/PIT ──► pit_tick ──► update_fireflies()           │
│              (16ms–1s)        energy/hungry logic       │
│                                                         │
│  Periodic ──► measure_isnight() ──► day/night switch   │
│                                                         │
│  WDT (8s) ──► crash guard (hardware reset)             │
└─────────────────────────────────────────────────────────┘
```

### Single-Timer PWM (Same Architecture as Original)

The firmware uses a single timer (TCA0) for both wave playback and
charlieplex PWM — the same approach as the original ATtiny45 project.
TCA0 runs at 8 MHz / 64 / 256 = 488 Hz overflow rate. With 4
charlieplex rows cycling, each LED refreshes at 122 Hz — identical
to the original.

Wave pointers advance once per row visit, giving 122 Hz per firefly.
The longer of the two wave tables shipped by the wave editor is used
(1654 samples), which is what makes the animations slow and smooth
rather than nervous — the author doubled the sample count, not the
playback period.

Each overflow handles one row of 3 fireflies:
1. Reads the current wave sample for each firefly
2. Sorts brightness values descending
3. Negates them (converts to "time until turn-on")
4. Programs CMP0/CMP1/CMP2 with the negated values
5. Sets PORTOUT for the row drive pin

The three TCA0 compare channels then **progressively turn LEDs ON**:
- CMP0 fires first (negated brightest = smallest value) → brightest LED on
- CMP1 fires next → medium LED joins
- CMP2 fires last (negated dimmest = largest value) → dimmest LED joins
- All LEDs stay on until the next overflow resets everything

This progressive turn-on approach eliminates ghost glow completely:
LEDs start OFF at each overflow and only get activated by their
specific compare match. No LED is ever briefly pulsed unintentionally.

### Anti-Ghost Design

Charlieplex matrices are prone to ghost lighting — parasitic current
through floating pins can dimly illuminate unintended LEDs. The
progressive turn-on approach avoids this entirely:

1. **OVF ISR clears both DIR and OUT** — all pins become high-impedance
   inputs with no output driver. Zero current flows.
2. **OUT is set to row drive value** — but DIR is still 0, so no pin
   actually drives anything yet.
3. **CMP ISRs enable DIR only for active LEDs** — pins are only made
   outputs when it's time for their specific LED to conduct.
4. **Only channels with an active firefly are armed** — see below.

This means at no point during the PWM cycle does an unintended LED
get even a brief pulse of current.

### Per-Row Compare Channel Arming

The negation `compare = 0 - brightness` maps raw values 1…255 to
compare points 255…1, but raw **0 maps to 0** — the earliest possible
compare point in the cycle. An idle firefly would therefore trigger a
compare match at count 0 and, through the cumulative DIR mask, switch
the active LED of the same row to full brightness for the entire
cycle. The result is binary on/off behaviour with no visible dimming
whenever a row contains an idle firefly — which is the normal steady
state once the startup burst settles.

To prevent this, the OVF ISR arms **one compare channel per active
firefly** and disables the remaining channels for that row via
`TCA0.SINGLE.INTCTRL`:

| Active fireflies in row | Armed channels |
|---|---|
| 3 | CMP0, CMP1, CMP2 |
| 2 | CMP0, CMP1 |
| 1 | CMP0 |
| 0 | none (row stays dark) |

Because brightness values are sorted descending before negation, the
active fireflies are always the leading entries, so the armed channels
are always contiguous from CMP0.

All four interrupt flags are cleared at the very end of the OVF ISR.
This suppresses compare matches that elapsed while the ISR was still
running and prevents them from leaking into the next row's cycle —
the same purpose the original serves with its `TIFR` write.

### Wave Playback Rate

Each overflow processes one row (3 fireflies). With 4 rows cycling,
each firefly's wave pointer advances once every 4 overflows =
488 / 4 = **122 Hz per firefly** — identical to the original. The
longest wave (293 samples) plays for ~2.4 seconds.

### Firefly Simulation

Each firefly has:
- `wave_ptr` — current position in a brightness wave
- `wave_end` — end address of the current wave segment
- `hungry` — how long until the firefly can flash again
- `energy` — accumulated energy from neighbours

When a firefly is idle (`wave_ptr == 0`, `hungry == 0`),
`update_fireflies()` picks one of 32 wave segments at random using a
32-bit LFSR, activates it, and distributes energy to the next fireflies
in the ring (full, half, quarter...). The `hungry` counter prevents
immediate re-flashing, creating natural spacing.

### Sleep Modes and Power

| Condition | Sleep Mode | Active Peripherals | Current |
|-----------|-----------|-------------------|---------|
| Fireflies active | IDLE | TCA0, PIT | ~3–4 mA |
| Between flashes (night) | STANDBY | PIT only | ~0.7 µA |
| Daytime (waiting) | STANDBY | PIT only | ~0.7 µA |
| LDR measurement | STANDBY | ADC (RUNSTDBY) + PIT | ~0.7 µA + ADC |

In STANDBY the chip draws only the 32.768 kHz ULP oscillator current.
With a CR2032 coin cell (~220 mAh), theoretical standby life exceeds
the battery's self-discharge limit.

### Watchdog Crash Recovery

The WDT is configured via fuse (WDTCFG = 0x0B) as a pure crash guard
with an 8-second timeout. The main loop kicks it (`wdr`) every iteration.
If main() hangs, the WDT resets the chip after 8 seconds. No dual-purpose
interrupt/timing — that's handled by the PIT.

### Wave Table

The wave data is generated by H. Reddmann's WaveEditor tool. The file
`wave.h` contains:
- `wave[1654]` — raw brightness samples (0–255)
- `wave_data[32]` — descriptors pointing into `wave[]` with start, end,
  and energy values

The wave editor output is used directly (PROGMEM removed for ATtiny412's
memory-mapped flash). The number of waves is always a power of 2
(typically 32). Sample count is typically ~1600.

---

## Resource Usage

```
Flash: 3804 / 4096 bytes (92.9%)
RAM:   111  / 256  bytes (43.4%)
```

The wave table (1654 bytes) dominates flash usage. If larger wave tables
are needed, consider the ATtiny1614 (16 KB flash, same pinout family).

---

## Source Layout

| File | Contents |
|------|----------|
| `src/firefly.h` | Types, macros, flag definitions, pin assignments, extern declarations |
| `src/firefly.c` | DDR table, EEPROM, init(), TCA0 ISRs (OVF+CMP), update_fireflies() |
| `src/main.c` | Main loop, LDR measurement, PIT ISR, sleep management |
| `src/lfsr32.c/h` | 32-bit LFSR pseudo-random number generator |
| `src/wave.h` | Wave table data (1654 bytes) and 32 wave descriptors |
| `platformio.ini` | Build configuration, fuses, upload settings |

---

## Differences from Firefly2026 (ATtiny45)

| Aspect | ATtiny45 (old) | ATtiny412 (new) |
|--------|---------------|-----------------|
| Clock | 8 MHz | 8 MHz (16 MHz osc / 2) |
| PWM frequency | 122 Hz per LED | 122 Hz per LED (identical) |
| PWM method | Timer0 OVF + single OCR reprogram | TCA0 OVF + 3 CMP channels (no reprogram) |
| PWM approach | Progressive LED turn-ON | Same progressive turn-ON (anti-ghost) |
| Wave sample rate | 122 Hz | 122 Hz (identical) |
| Wave timer | Same as PWM timer | Same as PWM timer (TCA0 only) |
| Sleep timing | WDT (dual-purpose) | RTC/PIT (dedicated, RUNSTDBY) |
| Crash guard | WDT interrupt+reset trick | WDT pure reset (8s fuse) |
| Flash access | PROGMEM + pgm_read | Direct pointer (memory-mapped) |
| Pin access | PORTB/DDRB (SBI/CBI) | VPORTA (single-cycle) |
| ADC measurement | CPU polling in ADC noise reduction | RUNSTDBY (CPU sleeps during conversion) |
| Programming | ISP (SPI) | UPDI (single-wire) |
| Assembler | pgm_read_word_inc macro | None |

---

## License

Same as the original Firefly2026 project.
