# NETLIST — FALLOUT GPU (wire-by-wire build bible)

**Rules before you solder anything**
- Whole board = **one 3.3 V rail** (`VCC` below always means 3.3 V) + common GND.
  Power comes from the HOST ESP32 devkit `3V3` pin. Tie **GND of both ESP32s and
  the board together** (≥2 wires, one at each end of the board).
- 100 nF ceramic across VCC/GND pins of **every** chip, soldered directly at the
  adapter. One bulk cap (47–100 µF) where power enters the board, one near the SRAMs.
- Color code: RED=3V3, BLACK=GND, BLUE=address bus, GREEN=data bus, YELLOW=control,
  WHITE=SPI/serial. Beep every bus group with a continuity meter before power-on.
- SOIC-on-adapter: verify adapter pin 1 corresponds to chip pin 1 before soldering.
- Unused **inputs** of soldered chips: tie to GND (or VCC where noted). Unused
  **outputs**: leave open. Chips not listed here stay in the box (don't solder spares).

## Chip roster & placement (board is 40 cm wide × 30 cm tall, columns left→right)

| Ref | Chip | Function | Column (x ≈ cm) | Stage |
|---|---|---|---|---|
| ESP1 | ESP32 devkit | HOST — commands | 2 | 1 |
| U1 | 74HC595 | load reg ADDR HIGH | 8 | 1 |
| U2 | 74HC595 | load reg ADDR LOW | 8 | 1 |
| U3 | 74HC595 | PIXEL data reg | 8 | 1 |
| U4–U7 | HC163 | fill addr counter bits 0–3 / 4–7 / 8–11 / 12–15 | 13 | 1 |
| U15 | 74HC04 | inverter (FILL_EN_N) | 13 (below U7) | 1 |
| U8 | 74HC541 | fill addr buffer AB0–7 | 18 | 1 |
| U9 | 74HC541 | fill addr buffer AB8–14 | 18 | 1 |
| U14 | 74HC32 | /WE_F = CP OR /FILL_EN | 18 (below U9) | 1 |
| U16 | 62256 SRAM | VRAM **A** | 23 (center) | 1 |
| U17 | 62256 SRAM | VRAM **B** | 23 (below U16) | 2 |
| U18 | 74HC32 | buffer steering (4 OR) | 20 (beside U16) | 2 |
| U19 | 74HC74 | SEL swap flip-flop | 20 (below U18) | 2 |
| U12 | 74HC541 | scan addr buffer AB0–7 | 28 | 1 |
| U13 | 74HC541 | scan addr buffer AB8–14 | 28 | 1 |
| U10 | 74HC393 | scan counter SA0–7 | 33 | 1 |
| U11 | 74HC393 | scan counter SA8–14 | 33 | 1 |
| ESP2 | ESP32 devkit | SCANOUT — display | 37 | 1 |

Run 3V3 along the top edge, GND along the bottom edge, drop verticals per column.

## Net glossary

| Net | Meaning | Driven by |
|---|---|---|
| SPI_MOSI / SPI_SCK / RCLK | 595 chain shift + latch | ESP1 GPIO 11 / 12 / 10 |
| CP | fill counter clock + write strobe timing | ESP1 GPIO 9 (idles HIGH) |
| PE_N | counter parallel-load enable (act. low) | ESP1 GPIO 8 |
| FILL_EN | 1 = fill side owns the bus | ESP1 GPIO 7 |
| FILL_EN_N | inverse of FILL_EN | U15 pin 2 |
| WE_F_N | gated write strobe | U14 pin 3 |
| SWAP / SEL_CLR_N | double-buffer flip / reset | ESP1 GPIO 6 / 5 |
| SEL / SEL_N | buffer-select state | U19 pin 5 / pin 6 |
| BUSY | host owns bus (to ESP2) | ESP1 GPIO 15 → ESP2 GPIO 11 |
| SCANCLK | scan counter clock (idles HIGH) | ESP2 GPIO 9 |
| SCAN_RST | scan counter clear (act. HIGH) | ESP2 GPIO 10 |
| FRAME_ACT | capture in progress (to ESP1) | ESP2 GPIO 12 → ESP1 GPIO 16 |
| LINK_TX | host stats UART 115200 | ESP1 GPIO 17 → ESP2 GPIO 13 |
| LAH0–7 / LAL0–7 | load values addr high/low | U1 / U2 outputs |
| FA0–15 | fill counter outputs | U4–U7 |
| SA0–14 | scan counter outputs | U10–U11 |
| AB0–14 | shared ADDRESS bus | U8/U9 or U12/U13 (tri-state) |
| DB0–7 | shared DATA bus | U3 (fill) or SRAM (scan) |

---

## U1 — 74HC595 (ADDR HIGH) — chain position 1
| Pin | Name | Connect to | ✓ |
|---|---|---|---|
| 16 | VCC | 3V3 | [ ] |
| 8 | GND | GND | [ ] |
| 10 | /SRCLR | 3V3 | [ ] |
| 13 | /OE | GND (always on) | [ ] |
| 14 | SER | **ESP1 GPIO11** (MOSI) | [ ] |
| 11 | SRCLK | SPI_SCK (ESP1 GPIO12) | [ ] |
| 12 | RCLK | RCLK (ESP1 GPIO10) | [ ] |
| 9 | QH' | U2 pin 14 (chain) | [ ] |
| 15 | QA | LAH0 → U6 pin 3 | [ ] |
| 1 | QB | LAH1 → U6 pin 4 | [ ] |
| 2 | QC | LAH2 → U6 pin 5 | [ ] |
| 3 | QD | LAH3 → U6 pin 6 | [ ] |
| 4 | QE | LAH4 → U7 pin 3 | [ ] |
| 5 | QF | LAH5 → U7 pin 4 | [ ] |
| 6 | QG | LAH6 → U7 pin 5 | [ ] |
| 7 | QH | LAH7 → U7 pin 6 | [ ] |

## U2 — 74HC595 (ADDR LOW) — chain position 2
| Pin | Name | Connect to | ✓ |
|---|---|---|---|
| 16/8/10 | VCC/GND//SRCLR | 3V3 / GND / 3V3 | [ ] |
| 13 | /OE | GND | [ ] |
| 14 | SER | U1 pin 9 | [ ] |
| 11 | SRCLK | SPI_SCK | [ ] |
| 12 | RCLK | RCLK | [ ] |
| 9 | QH' | U3 pin 14 (chain) | [ ] |
| 15 | QA | LAL0 → U4 pin 3 | [ ] |
| 1 | QB | LAL1 → U4 pin 4 | [ ] |
| 2 | QC | LAL2 → U4 pin 5 | [ ] |
| 3 | QD | LAL3 → U4 pin 6 | [ ] |
| 4 | QE | LAL4 → U5 pin 3 | [ ] |
| 5 | QF | LAL5 → U5 pin 4 | [ ] |
| 6 | QG | LAL6 → U5 pin 5 | [ ] |
| 7 | QH | LAL7 → U5 pin 6 | [ ] |

## U3 — 74HC595 (PIXEL) — chain position 3 (drives data bus)
| Pin | Name | Connect to | ✓ |
|---|---|---|---|
| 16/8/10 | VCC/GND//SRCLR | 3V3 / GND / 3V3 | [ ] |
| 13 | **/OE** | **FILL_EN_N** (U15 pin 2) — drives DB only during fill | [ ] |
| 14 | SER | U2 pin 9 | [ ] |
| 11 | SRCLK | SPI_SCK | [ ] |
| 12 | RCLK | RCLK | [ ] |
| 9 | QH' | n/c | [ ] |
| 15 | QA | **DB0** | [ ] |
| 1 | QB | **DB1** | [ ] |
| 2 | QC | **DB2** | [ ] |
| 3 | QD | **DB3** | [ ] |
| 4 | QE | **DB4** | [ ] |
| 5 | QF | **DB5** | [ ] |
| 6 | QG | **DB6** | [ ] |
| 7 | QH | **DB7** | [ ] |

## U4–U7 — HC163 fill address counter (16-pin each)
Common to all four: pin 16 VCC · pin 8 GND · pin 1 /CLR → 3V3 · pin 2 CLK → **CP**
· pin 9 /LOAD → **PE_N** · pin 7 ENP → 3V3.

| Chip | pin 10 ENT | pins 3,4,5,6 (D in) | pins 14,13,12,11 (QA..QD) | pin 15 RCO |
|---|---|---|---|---|
| U4 (bits 0–3) | 3V3 | LAL0,LAL1,LAL2,LAL3 | FA0,FA1,FA2,FA3 | → U5 pin 10 | 
| U5 (bits 4–7) | U4 p15 | LAL4,LAL5,LAL6,LAL7 | FA4,FA5,FA6,FA7 | → U6 pin 10 |
| U6 (bits 8–11) | U5 p15 | LAH0,LAH1,LAH2,LAH3 | FA8,FA9,FA10,FA11 | → U7 pin 10 |
| U7 (bits 12–15) | U6 p15 | LAH4,LAH5,LAH6,LAH7 | FA12,FA13,FA14,(FA15 n/c) | n/c |

Checkboxes: VCC/GND ×4 [ ][ ][ ][ ] · /CLR ×4 [ ] · CLK ×4 [ ] · /LOAD ×4 [ ] ·
ENP ×4 [ ] · ENT chain ×3 [ ] · D inputs ×16 [ ] · Q outputs ×15 [ ]

## U8 — 74HC541 fill buffer, low address byte
| Pin | Name | Connect to | ✓ |
|---|---|---|---|
| 20/10 | VCC/GND | 3V3 / GND | [ ] |
| 1 & 19 | /OE1,/OE2 | **FILL_EN_N** (both pins) | [ ] |
| 2..9 | A1..A8 | FA0..FA7 | [ ] |
| 18..11 | Y1..Y8 | **AB0..AB7** (pin18=AB0 … pin11=AB7) | [ ] |

## U9 — 74HC541 fill buffer, high address byte
| Pin | Name | Connect to | ✓ |
|---|---|---|---|
| 20/10 | VCC/GND | 3V3 / GND | [ ] |
| 1 & 19 | /OE1,/OE2 | **FILL_EN_N** | [ ] |
| 2..8 | A1..A7 | FA8..FA14 | [ ] |
| 9 | A8 | GND | [ ] |
| 18..12 | Y1..Y7 | **AB8..AB14** (pin18=AB8 … pin12=AB14) | [ ] |
| 11 | Y8 | n/c | [ ] |

## U12 — 74HC541 scan buffer, low address byte
Same as U8 but: /OE1,/OE2 (pins 1,19) = **FILL_EN** (not inverted!), A1..A8 = **SA0..SA7**, Y1..Y8 = AB0..AB7. [ ]

## U13 — 74HC541 scan buffer, high address byte
Same as U9 but: /OE1,/OE2 = **FILL_EN**, A1..A7 = **SA8..SA14**, A8 = GND, Y1..Y7 = AB8..AB14. [ ]

## U10 — 74HC393 scan counter, SA0–SA7
| Pin | Name | Connect to | ✓ |
|---|---|---|---|
| 14/7 | VCC/GND | 3V3 / GND | [ ] |
| 1 | 1CP | **SCANCLK** (ESP2 GPIO9) — counts on falling edge | [ ] |
| 2 | 1MR | **SCAN_RST** (ESP2 GPIO10) | [ ] |
| 3,4,5,6 | 1Q0..1Q3 | SA0,SA1,SA2,SA3 | [ ] |
| 13 | 2CP | SA3 (own pin 6) | [ ] |
| 12 | 2MR | SCAN_RST | [ ] |
| 11,10,9,8 | 2Q0..2Q3 | SA4,SA5,SA6,SA7 | [ ] |

## U11 — 74HC393 scan counter, SA8–SA14
| Pin | Name | Connect to | ✓ |
|---|---|---|---|
| 14/7 | VCC/GND | 3V3 / GND | [ ] |
| 1 | 1CP | SA7 (U10 pin 8) | [ ] |
| 2 | 1MR | SCAN_RST | [ ] |
| 3,4,5,6 | 1Q0..1Q3 | SA8,SA9,SA10,SA11 | [ ] |
| 13 | 2CP | SA11 (own pin 6) | [ ] |
| 12 | 2MR | SCAN_RST | [ ] |
| 11,10,9 | 2Q0..2Q2 | SA12,SA13,SA14 | [ ] |
| 8 | 2Q3 | n/c | [ ] |

## U14 — 74HC32 write-strobe gate (only gate 1 used)
| Pin | Connect to | ✓ |
|---|---|---|
| 14/7 | 3V3 / GND | [ ] |
| 1 (1A) | CP | [ ] |
| 2 (1B) | FILL_EN_N | [ ] |
| 3 (1Y) | **WE_F_N** | [ ] |
| 4,5,9,10,12,13 | GND (unused inputs) | [ ] |

## U15 — 74HC04 (only gate 1 used)
| Pin | Connect to | ✓ |
|---|---|---|
| 14/7 | 3V3 / GND | [ ] |
| 1 (1A) | FILL_EN | [ ] |
| 2 (1Y) | **FILL_EN_N** | [ ] |
| 3,5,9,11,13 | GND (unused inputs) | [ ] |

## U16 / U17 — 62256 SRAM (28-pin). U17 is STAGE 2 — wire U16 first.
| Pin | Name | Connect to | ✓ U16 | ✓ U17 |
|---|---|---|---|---|
| 28 | VCC | 3V3 | [ ] | [ ] |
| 14 | GND | GND | [ ] | [ ] |
| 10 | A0 | AB0 | [ ] | [ ] |
| 9 | A1 | AB1 | [ ] | [ ] |
| 8 | A2 | AB2 | [ ] | [ ] |
| 7 | A3 | AB3 | [ ] | [ ] |
| 6 | A4 | AB4 | [ ] | [ ] |
| 5 | A5 | AB5 | [ ] | [ ] |
| 4 | A6 | AB6 | [ ] | [ ] |
| 3 | A7 | AB7 | [ ] | [ ] |
| 25 | A8 | AB8 | [ ] | [ ] |
| 24 | A9 | AB9 | [ ] | [ ] |
| 21 | A10 | AB10 | [ ] | [ ] |
| 23 | A11 | AB11 | [ ] | [ ] |
| 2 | A12 | AB12 | [ ] | [ ] |
| 26 | A13 | AB13 | [ ] | [ ] |
| 1 | A14 | AB14 | [ ] | [ ] |
| 11 | D0 | DB0 | [ ] | [ ] |
| 12 | D1 | DB1 | [ ] | [ ] |
| 13 | D2 | DB2 | [ ] | [ ] |
| 15 | D3 | DB3 | [ ] | [ ] |
| 16 | D4 | DB4 | [ ] | [ ] |
| 17 | D5 | DB5 | [ ] | [ ] |
| 18 | D6 | DB6 | [ ] | [ ] |
| 19 | D7 | DB7 | [ ] | [ ] |
| 20 | /CE | GND (both, always selected) | [ ] | [ ] |
| 27 | /WE | **Stage 1: jumper J1 → WE_F_N** · Stage 2: U18 p3 (/WE_A). U17: U18 p6 (/WE_B) | [ ] | [ ] |
| 22 | /OE | **Stage 1: jumper J2 → FILL_EN** · Stage 2: U18 p8 (/OE_A). U17: U18 p11 (/OE_B) | [ ] | [ ] |

## U18 — 74HC32 buffer steering (STAGE 2)
| Pin | Connect to | ✓ |
|---|---|---|
| 14/7 | 3V3 / GND | [ ] |
| 1,2 → 3 | WE_F_N, SEL → **/WE_A** (U16 p27) | [ ] |
| 4,5 → 6 | WE_F_N, SEL_N → **/WE_B** (U17 p27) | [ ] |
| 9,10 → 8 | FILL_EN, SEL_N → **/OE_A** (U16 p22) | [ ] |
| 12,13 → 11 | FILL_EN, SEL → **/OE_B** (U17 p22) | [ ] |

## U19 — 74HC74 swap flip-flop (STAGE 2, FF #1 only)
| Pin | Name | Connect to | ✓ |
|---|---|---|---|
| 14/7 | VCC/GND | 3V3 / GND | [ ] |
| 1 | /1CLR | **SEL_CLR_N** (ESP1 GPIO5) | [ ] |
| 4 | /1PRE | 3V3 | [ ] |
| 3 | 1CLK | **SWAP** (ESP1 GPIO6) | [ ] |
| 2 | 1D | own pin 6 (/1Q) → toggle mode | [ ] |
| 5 | 1Q | **SEL** → U18 pins 2,13 + ESP1 GPIO4 (readback) | [ ] |
| 6 | /1Q | **SEL_N** → U18 pins 5,10 + own pin 2 | [ ] |
| 10,13 | /2PRE,/2CLR | 3V3 (FF2 unused) | [ ] |
| 11,12 | 2CLK,2D | GND | [ ] |

## ESP32 pin tables — **ESP32-S3-N16R8** boards.
S3 rules baked into these maps: never use straps **0, 3, 45, 46**; octal-PSRAM
pins **35–37** are dead on N16R8; **19/20** are USB; **43/44** are the UART0 console.

### ESP1 — HOST (S3)
| GPIO | Dir | Signal |
|---|---|---|
| 11 | out | SPI_MOSI → U1 p14 |
| 12 | out | SPI_SCK → U1,U2,U3 p11 |
| 10 | out | RCLK → U1,U2,U3 p12 |
| 9 | out | CP → U4..U7 p2 + U14 p1 |
| 8 | out | PE_N → U4..U7 p9 |
| 7 | out | FILL_EN → U15 p1, U12/U13 p1&19, U18 p9&12 (stage1: also J2) |
| 6 | out | SWAP → U19 p3 (stage 2) |
| 5 | out | SEL_CLR_N → U19 p1 (stage 2) |
| 4 | in | SEL readback ← U19 p5 (stage 2) |
| 15 | out | BUSY → ESP2 GPIO11 |
| 16 | in | FRAME_ACT ← ESP2 GPIO12 |
| 17 | out | LINK_TX → ESP2 GPIO13 |
| 3V3/GND | — | board rail + common ground (2× GND wires) |

### ESP2 — SCANOUT (S3)
| GPIO | Dir | Signal |
|---|---|---|
| 4 | in | DB0 |
| 5 | in | DB1 |
| 6 | in | DB2 |
| 7 | in | DB3 |
| 15 | in | DB4 |
| 16 | in | DB5 |
| 17 | in | DB6 |
| 18 | in | DB7 |
| 9 | out | SCANCLK → U10 p1 |
| 10 | out | SCAN_RST → U10 p2,p12 + U11 p2,p12 |
| 11 | in | BUSY ← ESP1 GPIO15 |
| 12 | out | FRAME_ACT → ESP1 GPIO16 |
| 13 | in | LINK_RX ← ESP1 GPIO17 |
| GND | — | common ground |

### ESP2 — 1.8" 128×160 ST7735 TFT (local display, 8 module pins)
Used in landscape: the GPU's 160×128 frame maps 1:1 onto the full panel.
| TFT pin | Connect to |
|---|---|
| GND | GND |
| VCC | 3V3 |
| SCL (SPI clock) | ESP2 GPIO40 |
| SDA (SPI MOSI) | ESP2 GPIO41 |
| RES (reset) | ESP2 GPIO1 |
| DC | ESP2 GPIO2 |
| CS | ESP2 GPIO42 |
| BL (backlight) | 3V3 |

If the picture is mirrored / colors R-B swapped / looks negative / shifted a
pixel or two: fix via `TFT_MADCTL`, `TFT_INVERT`, `TFT_XOFS/YOFS` in output.ino
— panel variants differ, no rewiring needed.

## Bus daisy-chain routing (solder each bit as a chain, left → right)
- **AB0..AB7**: U8.Y → U16.A → (U17.A) → U12.Y   (3–4 solder points per bit)
- **AB8..AB14**: U9.Y → U16.A → (U17.A) → U13.Y
- **DB0..DB7**: U3.Q → U16.D → (U17.D) → ESP2 GPIO
- Keep each bit's segments bundled and short; twist DB bundle with a GND wire for
  the long run to ESP2.

## Sanity checks before first power-up
1. Ohmmeter: 3V3↔GND > 1 kΩ (no shorts). Every chip VCC pin beeps to rail, GND to rail.
2. Beep each AB and DB bit end-to-end; beep ADJACENT bits against each other (must NOT beep).
3. No GPU wire may land on S3 straps 0/3/45/46, PSRAM pins 35–37, USB 19/20,
   or UART 43/44 — the pin tables above already avoid them; check anyway.
4. Boot order: flash both ESP32s BEFORE wiring power; power ESP1 first (it drives
   FILL_EN low at boot, which parks the bus on the scan side).
