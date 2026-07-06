#goobeywoobey gpu- A discrete-logic graphics processor 

**160×128 pixels · 8-bit color (RGB332) · ~3 Mpix/s hardware fill · double-buffered VRAM · built from ~19 jellybean 74HC chips + 62256 SRAM on a 30×40 cm perfboard.**

Host ESP32 issues draw commands. Output ESP32 scans the frame out. Everything in between which includes but is not limited to
address generation, write engine, VRAM, bus arbitration, double buffering is done by 
discrete logic. No microcontroller touches a pixel between command and scanout.

```
                                THE GPU (discrete logic)
             ┌─────────────────────────────────────────────────────────┐
 ESP32 #1    │  ┌─────────┐    ┌──────────────┐    ┌───────┐            │    ESP32 #2
 "HOST"      │  │ 3× 595  │──▶│ 4× HC163      │──▶│2× 541 │─┐          │    "SCANOUT"
 draw cmds ──┼─▶│ cmd regs│   │ 16-bit fill   │   │ (fill │ │ ADDR     │
 SPI 10 MHz  │  │ AH AL PX│    │ addr counter  │   │  side)│ │ BUS      │  ┌─────────┐
             │  └────┬────┘    │ loadable,     │   └───────┘ ├──▶┌────┐ │  │ browser │
 CP bursts ──┼─▶ WE gating───▶│ auto-increment│             │   │VRAM│ │  │ viewer  │
 (~3 MHz,    │   (74HC32/04)   └──────────────┘   ┌───────┐  │   │ A  │ │  │ over    │
  1 clk =    │                                    │2× 541 │  │   │────│ │  │ WiFi AP │
  1 pixel)   │        ┌──────────────┐            │ (scan │──┘   │VRAM│ │  └─────────┘
             │        │ 2× HC393     │───────────▶│  side)│     │ B  │ │       ▲
             │        │ 15-bit scan  │            └───────┘      └──┬─┘ │       │
 pixel clk ◀─┼────────│ addr counter │◀── SCANCLK ──────────────────┼───┼── 8-bit parallel
             │        └──────────────┘            74HC74 swap FF ───┘  │   read @ ~1.5 MHz
             └─────────────────────────────────────────────────────────┘
```

# How does it work

 ##.
 0# Memory map
- Screen: **160 wide × 128 tall** (matches the ST7735 in landscape 1:1), 1 byte per
  pixel, RGB332 (3 bits red, 3 green, 2 blue). Address = `y*160 + x`, the hardware just uses a linear counter. One frame =
  20480 bytes; each 32 KB 62256 holds a frame with 12 KB spare.
- Address bus `AB0..AB14`, data bus `DB0..DB7`, shared by fill and scan sides,
  tri-state arbitrated by the 74HC541 pairs. Only one side owns the bus at a time
  (`FILL_EN` selects).

### Fill engine (the main part ig)
1. Host shifts 24 bits over SPI into the 595 chain: `[PIXEL][ADDR_LO][ADDR_HI]`,
   pulses RCLK.
2. Host pulls `/PE` low, pulses CP once → the HC163 chain **parallel-loads** the
   start address.
3. Host raises `FILL_EN`:
   - fill-side 541s drive the address bus, scan-side 541s go Hi-Z,
   - the PIXEL 595 drives the data bus, SRAM `/OE` is forced high,
   - `/WE = CP OR /FILL_EN` → every CP low phase is a write strobe.
4. Host streams **N bare clock pulses** (tight IRAM loop, ~3 MHz). Each pulse:
   CP low → write pixel at current address; CP rising edge → write commits **and**
   the counter increments. **One pulse = one pixel.** The address rolls across row
   boundaries, so a full-screen clear is a single 20480-pulse burst (~7 ms).
5. Host drops `FILL_EN` → bus returns to the scan side.

Why this goes vroom vroom fast: a CPU writing through shift registers pays 24+ SPI bits + latch +
strobe per pixel (~4 µs). The fill engine pays that **once per run**, then 1 clock
per pixel. Measured speedup on full-screen clears: **>10×** (the demo firmware
benchmarks it live).

### Scan-out thingy
- The HC393 ripple chain generates sequential addresses 0..20479. ESP32 #2 pulses
  `SCAN_RST`, then clocks `SCANCLK` (~1.4 MHz) and reads the SRAM data bus directly
  on 8 GPIOs which is one full byte per clock, ~15 ms per frame (~65 fps capture).
- ESP32 #2 displays every frame **1:1 on the 1.8" ST7735 TFT** (embedded
  zero-dependency driver, landscape 160×128 = the whole panel) **and** runs a WiFi
  access point (`FALLOUT-GPU`) serving the same frame as an 8-bit BMP 

### Arbitration / vsync (2 wires thingy between the ESP32s)
- `BUSY` (host → scanout): high while the host owns the GPU bus. Scanout never
  starts a frame while BUSY.
- `FRAME_ACT` (scanout → host): high during a frame capture. Host waits for it to
  drop before drawing. Result: no torn frames even in single-buffer mode.

### Double buffering (stage 2)
Both SRAMs share the address/data bus. A 74HC74 flip-flop (`SEL`) steers `/WE` to
the back buffer and `/OE` to the front buffer through four OR gates ('32):

| signal | equation | effect |
|---|---|---|
| `/WE_A` | `/WE_F OR SEL` | A written only when SEL=0 |
| `/WE_B` | `/WE_F OR /SEL` | B written only when SEL=1 |
| `/OE_A` | `FILL_EN OR /SEL` | A scanned only when SEL=1 |
| `/OE_B` | `FILL_EN OR SEL` | B scanned only when SEL=0 |

Host toggles SEL with one `SWAP` pulse between frames. Draw time is hidden: the
viewer always sees a complete frame while the next one is drawn.

## Specs for pure flex

| Spec | Value |
|---|---|
| Resolution / color | 160×128, 8 bpp RGB332 |
| VRAM | 2× 32 KB SRAM, double buffered (20 KB/frame + 12 KB spare each) |
| Fill rate | ~3 Mpix/s burst (1 pixel/clock), tunable to 5 MHz |
| Random pixel writes | ~220 k/s (24-bit command + 1 clock) |
| Scan-out | 8-bit parallel, ~65 fps capture, ST7735 TFT + WiFi browser |
| Logic | 19 ICs: 595/163/393/541/32/04/74 + 62256 |
| Clocks | Fully static design|
| Big H haul | sprite blitter (HC245 + 3rd counter), register file (7× HC574), autonomous 25 MHz clock domain (crystals + HC04), HW cursor blink (HC123) |

## Pitch framing
- The ESP32s are *peripherals*: one is the "driver/command generator", one is the
  "monitor cable". The GPU itself does address generation, write sequencing, VRAM,
  bus arbitration, double-buffer steering is a 74-series logic you can point at.
