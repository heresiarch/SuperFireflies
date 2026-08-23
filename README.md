# SuperFireflies

A firefly lamp for the ATtiny412. Twelve LEDs simulate independent fireflies
that glow, fade, and loosely synchronize with each other — no fixed blink
pattern, each run looks different.

Port of the [Firefly2026](../Firefly2026) project (ATtiny45 @ 8 MHz) to the
ATtiny412 @ 20 MHz, leveraging the newer chip's superior hardware for smoother
PWM, cleaner sleep management, and simpler code.

Based on the original concept from the
[mikrocontroller.net thread](https://www.mikrocontroller.net/topic/99803?page=single)
by H. Reddmann.

---

## Hardware

| Item | Detail |
|------|--------|
| MCU | ATtiny412 @ 20 MHz (internal oscillator, OSCCFG fuse) |
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
               10k
VCC ──────────/\/\/──────┬─── PA7 / AIN7
                         │
                    ┌────┴────┐
                    │         │
                  ┌───┐     ┌───┐
                  │LDR│     │10n│
                  └───┘     └───┘
                    │         │
                    └────┬────┘
                         │
                        GND
```

The 10k resistor connects VCC to the measurement pin (PA7). The LDR and
10 nF capacitor are in parallel, both between PA7 and GND.

**Measurement principle:**
1. PA7 set as output HIGH — charges the 10 nF capacitor through the pin
   (overriding the voltage divider)
2. PA7 set as input (high-impedance) — capacitor discharges through the LDR
3. ADC reads remaining voltage on the capacitor

The 10k pull-up to VCC provides a defined voltage divider with the LDR
during steady-state, but the measurement uses the transient discharge
method for better sensitivity:

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
| WDTCFG | 0x09 | WDT enabled, normal mode, 2s timeout (crash guard) |
| BODCFG | 0x00 | Brown-out detection disabled |
| OSCCFG | 0x02 | 20 MHz internal oscillator |
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
│  TCA0 (39 kHz overflow)     TCB0 (488 Hz)              │
│  ├─ OVF: row switch         └─ Wave advancement        │
│  ├─ CMP0: LED off                (3 fireflies/call)    │
│  ├─ CMP1: LED off                Pre-computes PWM      │
│  └─ CMP2: LED off                values for TCA0       │
│                                                         │
├─────────────────────────────────────────────────────────┤
│                    SLOW DOMAIN (main loop)              │
│                                                         │
│  RTC/PIT ──► FLAG_UPDATE ──► update_fireflies()        │
│              (125ms–1s)       energy/hungry logic       │
│                                                         │
│  Periodic ──► measure_isnight() ──► day/night switch   │
│                                                         │
│  WDT (2s) ──► crash guard (hardware reset)             │
└─────────────────────────────────────────────────────────┘
```

### Two Timing Domains

**TCB0 + TCA0 — fast domain (ISRs)**

- **TCA0** runs at 20 MHz / 2 / 256 = ~39 kHz overflow rate. With 4
  charlieplex rows, each LED refreshes at ~9.7 kHz (flicker-free).
  Three compare channels (CMP0/CMP1/CMP2) provide per-LED duty cycle
  control within each row period.

- **TCB0** fires at ~488 Hz. Each ISR call advances wave pointers for
  3 fireflies, sorts brightness values, and pre-computes the DDR/CMP
  data that TCA0 will use. With 4 groups of 3, all 12 fireflies get
  updated every 4 calls = **~122 Hz effective wave sample rate**
  (identical to the original project).

**RTC/PIT — slow domain (main loop)**

The Periodic Interrupt Timer wakes the CPU from STANDBY sleep at
configurable intervals (125 ms to 1 s). On wakeup, the main loop:

1. Calls `update_fireflies()` — assigns new wave patterns, calculates
   energy transfer between neighboring fireflies, returns time until
   next update.
2. Programs the PIT for the next interval.
3. Periodically calls `measure_isnight()` to check the LDR.

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
| Fireflies active | IDLE | TCA0, TCB0, PIT | ~3–4 mA |
| Between flashes (night) | STANDBY | PIT only | ~0.7 µA |
| Daytime (waiting) | STANDBY | PIT only | ~0.7 µA |
| LDR measurement | STANDBY | ADC (RUNSTDBY) + PIT | ~0.7 µA + ADC |

In STANDBY the chip draws only the 32.768 kHz ULP oscillator current.
With a CR2032 coin cell (~220 mAh), theoretical standby life exceeds
the battery's self-discharge limit.

### Watchdog Crash Recovery

The WDT is configured via fuse (WDTCFG = 0x09) as a pure crash guard
with a 2-second timeout. The main loop kicks it (`wdr`) every iteration.
If main() hangs, the WDT resets the chip after 2 seconds. No dual-purpose
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
Flash: 3862 / 4096 bytes (94.3%)
RAM:   136  / 256  bytes (53.1%)
```

The wave table (1654 bytes) dominates flash usage. If larger wave tables
are needed, consider the ATtiny1614 (16 KB flash, same pinout family).

---

## Source Layout

| File | Contents |
|------|----------|
| `src/firefly.h` | Types, macros, flag definitions, pin assignments, extern declarations |
| `src/firefly.c` | DDR table, EEPROM, init(), TCA0/TCB0 ISRs, update_fireflies() |
| `src/main.c` | Main loop, LDR measurement, PIT ISR, sleep management |
| `src/lfsr32.c/h` | 32-bit LFSR pseudo-random number generator |
| `src/wave.h` | Wave table data (1654 bytes) and 32 wave descriptors |
| `platformio.ini` | Build configuration, fuses, upload settings |

---

## Differences from Firefly2026 (ATtiny45)

| Aspect | ATtiny45 (old) | ATtiny412 (new) |
|--------|---------------|-----------------|
| Clock | 8 MHz | 20 MHz |
| PWM frequency | ~122 Hz per LED | ~9,700 Hz per LED |
| Wave sample rate | ~122 Hz | ~122 Hz (unchanged) |
| PWM method | Software (Timer0 OVF + single OCR) | Hardware (TCA0 with 3 CMP channels) |
| Wave timer | Same as PWM timer | Separate TCB0 |
| Sleep timing | WDT (dual-purpose) | RTC/PIT (dedicated) |
| Crash guard | WDT interrupt+reset trick | WDT pure reset (fuse) |
| Flash access | PROGMEM + pgm_read | Direct pointer (memory-mapped) |
| Pin access | PORTB/DDRB (SBI/CBI) | VPORTA (single-cycle) |
| ADC measurement | CPU polling in ADC noise reduction | RUNSTDBY (CPU sleeps during conversion) |
| Programming | ISP (SPI) | UPDI (single-wire) |
| Assembler | pgm_read_word_inc macro | None |

---

## License

Same as the original Firefly2026 project.
