# BUILD PLAN — 8 hours to a working GPU

~240 soldered wires for stage 1. Two builders ≈ 30–35 wires/hour each including
double-checking = wiring done around hour 4.5–5, leaving time for bring-up, tuning
and the double buffer. **Tick the checkbox in NETLIST.md the moment each wire is
soldered — never trust memory.**

Roles: **Builder A** (fill side, left half), **Builder B** (scan side, right half),
**Software** (can be A/B in gaps): flash firmwares first 20 min, then test at each
milestone. If you're 3 people, third person runs continuity checks + caps + preps
cut/stripped wires in batches (huge speedup).

## Hour 0:00–0:30 — Foundation (everyone)
- [ ] Flash `host.ino` → ESP1, `output.ino` → ESP2 **now, before wiring** (both
      compile with Arduino-ESP32 core, no libraries needed).
- [ ] Wire the 128×160 ST7735 TFT to ESP2 (8 module pins — GND VCC SCL SDA RES
      DC CS BL, table at the end of NETLIST.md) — it's independent of all GPU
      wiring, so do it now and every later milestone is visible on real hardware
      without opening a laptop.
- [ ] Quick sanity: ESP2 boots → TFT initializes to black, WiFi AP `FALLOUT-GPU`
      visible, browser shows viewer page. ESP1 serial monitor shows the menu.
- [ ] Tape/mark chip positions on board per NETLIST placement map.
- [ ] Solder 3V3 rail (top edge), GND rail (bottom edge), column drops.
- [ ] Solder all chip adapters' VCC/GND + 100 nF each + bulk caps.
- [ ] **SRAM 3.3 V test rig decision point**: nothing to test yet — the M1 path
      will prove it. Just note which SRAM brand goes in as U16 (start with IS62C256;
      you have 2, plus the UT62256 spare).

## Hour 0:30–2:00 — Track A: command chain · Track B: scan chain
**A:** U1→U2→U3 (595s): SPI wires, chain links, RCLK, /OE wiring. Then DB0..7
from U3 toward the SRAM site and onward to ESP2 GPIOs (B meets you halfway).
**B:** U10, U11 (393s): SCANCLK, SCAN_RST, cascades. U12, U13 (541s): SA inputs,
FILL_EN enables. AB bus segments from U12/U13 toward SRAM site.

### ✅ MILESTONE M1 (~2:00) — "first light on the data bus"
No SRAM involved: host writes a byte into the PIXEL 595 with FILL_EN high → the
byte appears on DB → ESP2 reads DB directly → viewer shows a solid color.
- ESP1 serial menu: press `1` = M1 test (cycles colors 1/s, holds FILL_EN so U3 drives DB).
- TFT and browser viewer show a full-screen color changing every second.
- **Proves:** SPI→595 chain, DB bus, ESP2 read path, both firmwares, WiFi. 🎉

## Hour 2:00–3:45 — Track A: counters · Track B: SRAM A
**A:** U4–U7 (163s): CP, PE_N, D-inputs from LAL/LAH, ENT cascade, FA outputs →
U8/U9 (541s) → AB segments. U14 ('32) + U15 ('04) glue.
**B:** U16 SRAM: all 15 AB taps, 8 DB taps, /CE→GND, jumpers J1 (/WE←WE_F_N) and
J2 (/OE←FILL_EN). Then continuity-beep the ENTIRE address and data bus end-to-end
+ adjacent-bit short check. This is the highest-wire-count zone — check twice.

### ✅ MILESTONE M2 (~3:45) — "counter counts, bus is sane" (multimeter, no SRAM needed)
- ESP1 serial menu `2`: loads address 0x0000, then steps CP at 2 Hz with FILL_EN
  high. DMM on AB0 toggles 1 Hz, AB1 half that, etc. Menu `3`: loads 0x5555 /
  0xAAAA alternating — DMM spot-check AB bits = load path works.
- **Proves:** 595→163 load, counting, cascade, fill 541s, no stuck/shorted AB bits.

## Hour 3:45–5:00 — Full pipe integration
- Connect remaining control lines: BUSY, FRAME_ACT, LINK, GND ties between ESP32s.
- Power-on checklist from NETLIST bottom. Power ESP1 first.

### ✅ MILESTONE M3 (~5:00) — "IT'S A GPU" 
- ESP1 menu `4`: clear screen to a color, draw test rects. Viewer shows them.
- ESP1 menu `5` = demo reel on: bouncing rects, wireframe cube, FALLOUT GPU text,
  live benchmark screen.
- If garbage pixels: see Debug table below — 90% it's one swapped AB/DB bit
  (pattern tells you which: vertical stripes = low AB bit, big blocks = high bit).

## Hour 5:00–6:00 — Tune + harden
- [ ] Raise fill clock: `PULSE_NOPS` in host.ino from 16 (≈2 MHz) down until
      corruption, then back off 2 steps. Expect ~3 MHz stable on perfboard.
- [ ] Raise SPI: `SPI_HZ` 10 → 20 MHz if clean.
- [ ] Raise scan clock similarly (`SCAN_NOPS` in output.ino, target ~1.5 MHz).
- [ ] Run benchmark (`6`), write the numbers on the whiteboard/pitch.
- [ ] Hot-glue or tape wire bundles; nothing must move during demos.

## Hour 6:00–7:15 — STAGE 2: double buffer (skip without guilt if behind)
- [ ] Wire U18 ('32), U19 ('74) per netlist. Wire U17 (SRAM B): 23 bus taps + 2 ctl.
- [ ] Remove jumpers J1/J2, connect U16 /WE,/OE to U18 outputs.
- [ ] host.ino: set `#define DOUBLE_BUF 1`, reflash.
- [ ] Verify: menu `7` swap test — draws red frame in back, swaps, draws blue.
      Viewer alternates clean full frames, never a half-drawn one.

## Hour 7:15–8:00 — Demo lockdown
- [ ] Demo reel on auto-cycle (BOOT button on ESP1 switches scenes manually).
- [ ] Phone + laptop connected to `FALLOUT-GPU` AP, viewer full-screen, spare phone charged.
- [ ] Pitch: specs table from README, benchmark numbers, point at the boards:
      "commands enter here → these four chips walk the addresses → this is VRAM →
      those counters scan it out. No CPU between command and pixel."
- [ ] Photo of the board now (backup if anything dies later).

---

## Debug table

| Symptom | Likely cause | Fix |
|---|---|---|
| Chip warm/hot | Bus contention: two drivers on AB or DB | Check U8/U9 vs U12/U13 enables: fill pair = FILL_EN_N, scan pair = FILL_EN. Check U3 /OE = FILL_EN_N, SRAM /OE = FILL_EN (J2) |
| Viewer shows only 595 color, never SRAM data | SRAM /OE or /CE wrong; J2 missing | J2 → FILL_EN, /CE → GND |
| Writes land at wrong X: vertical stripe pattern | Two low AB bits swapped | Stripe period 2ⁿ px identifies bit n |
| Writes land at wrong Y: rows swapped/blocks | High AB bit swapped/open | Block height 2ⁿ⁻⁸ rows identifies bit |
| Wrong colors, consistent mapping | DB bits swapped (fill vs scan side may differ!) | Compare U3→SRAM order vs SRAM→ESP2 order |
| Every 2nd pixel garbage at high fill clock | Fill clock too fast for write pulse | Increase PULSE_NOPS |
| Corruption only near count 0x00FF/0x0FFF boundaries during scan | 393 ripple settle > scan period | Increase SCAN_NOPS |
| Nothing writes at all | /WE path: U14 inputs (CP + FILL_EN_N), J1 | DMM U14 p3: idles HIGH, pulses low during fill |
| SRAM flaky at 3.3 V (rare) | 5V-only die | Swap brand (3 SRAMs on hand), or buy AS6C62256 (see list) |
| TFT mirrored / R-B swapped / negative / shifted 1–2 px | ST7735 panel variant | `TFT_MADCTL` / `TFT_INVERT` / `TFT_XOFS,YOFS` defines in output.ino — no rewiring |
| TFT stays white/blank | RES not wired, or CS/DC swapped | Check GPIO1=RES, GPIO2=DC, GPIO42=CS; then halve `TFT_HZ` |
| An ESP32-S3 won't boot/flash with board powered | A wire landed on a strap pin (0/3/45/46) or USB (19/20) | Recheck against netlist pin tables |
| Random glitches when WiFi client connects | Ground loop / rail dip | Add 2nd GND tie between ESP32s, bulk cap at ESP2 |

## Huaqiangbei shopping list — ALL OPTIONAL (design works without)
Priority order, total < ¥150:
1. **Cheap 8-ch logic analyzer clone (~¥30)** — halves every debug time. Biggest bang.
2. Resistor kit (¥10) — 10k pulldown for FILL_EN (removes boot-glitch window),
   33 Ω series for CP/SCANCLK if ringing at high clock.
3. LEDs + 470 Ω (¥5) — blinkenlights on DB via the spare '541: instant crowd appeal.
4. AS6C62256-55 ×2 (¥15) — genuinely 3 V-rated SRAM, insurance for U16/U17.
5. 74HC165 ×3 (¥5) — readback path to host (adds GPU→CPU reads = stage 3).
6. 74HC688 ×2 (¥8) — hardware end-address compare → fully autonomous fills.
7. Spare ESP32-S3 devkit (¥30) — insurance (you already own the ST7735 TFT ✓).

## Stage-3 flex (only with spare time, parts already on hand)
- **Sprite blitter**: SRAM #3 holds sprites, HC245 bridges its data to DB, third
  '393 + a '574 as page register generate source addresses → hardware copy.
- **Blinkenlights**: spare '541 from DB → LEDs (needs resistors from list).
- **25 MHz clock domain**: crystal + '04 Pierce osc + '74 divider — "GPU-internal
  clock", mostly pitch value.
- 7× '574 = register file / palette latch if you go for RGB output later.
