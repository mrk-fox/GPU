/*
 * FALLOUT GPU — HOST (ESP32 #1)
 * Drives the discrete-logic GPU: 595 command chain, HC163 fill counter,
 * write-strobe gating. Serial menu for bring-up tests + demo reel + benchmark.
 *
 * Board: ESP32-S3 (DevKitC-1 N16R8). Pins avoid S3 straps (0,3,45,46),
 * octal-PSRAM pins (35-37), USB (19/20) and UART0 (43/44).
 *
 * Serial (USB, 115200) menu:
 *   1 = M1 data-bus color test (no SRAM needed)
 *   2 = M2 counter step test (CP at 2 Hz, DMM on AB bits)
 *   3 = M2 load-pattern test (alternates 0x5555/0xAAAA)
 *   4 = M3 static test pattern (clear + rects + text)
 *   5 = demo reel auto-cycle (BOOT button = next scene)
 *   6 = benchmark (GPU fill vs CPU per-pixel), prints + draws result
 *   7 = stage-2 swap test (needs DOUBLE_BUF 1)
 */

#include <SPI.h>
#include "soc/gpio_reg.h"

// ------------------------------------------------------------- configuration
#define DOUBLE_BUF   0          // set 1 after stage-2 wiring, reflash
#define SPI_HZ       10000000   // 595 shift clock; try 20 MHz after M3 is stable
#define PULSE_NOPS   8          // fill CP half-period pad. 8~2.5MHz, 16~1.6MHz, 2~4MHz
#define W            160        // screen = ST7735 landscape, shown 1:1
#define H            128
#define ADDR(x, y)   ((uint16_t)((y) * W + (x)))   // linear fb, row stride 160

// Pins (ESP32-S3, all < 32 so single-register fast IO)
#define PIN_MOSI     11
#define PIN_SCK      12
#define PIN_RCLK     10
#define PIN_CP        9
#define PIN_PE_N      8
#define PIN_FILL_EN   7
#define PIN_SWAP      6
#define PIN_SELCLR_N  5
#define PIN_SEL_Q     4    // input
#define PIN_BUSY     15
#define PIN_FRAMEACT 16    // input
#define PIN_LINK_TX  17
#define PIN_BOOT_BTN  0

#define MASK(p)   (1UL << (p))
#define HI(p)     REG_WRITE(GPIO_OUT_W1TS_REG, MASK(p))
#define LO(p)     REG_WRITE(GPIO_OUT_W1TC_REG, MASK(p))
#define FRAME_ACTIVE() ((REG_READ(GPIO_IN_REG) >> PIN_FRAMEACT) & 1)

SPIClass spi(FSPI);   // S3: FSPI = SPI2, routed to any pin via GPIO matrix
static uint32_t statPixels = 0, statFills = 0;
static char currentScene[16] = "boot";

// ------------------------------------------------------------- low level
static inline void rclkPulse() { HI(PIN_RCLK); asm volatile("nop;nop;nop;nop"); LO(PIN_RCLK); }

// Shift [PIXEL][ADDR_LO][ADDR_HI]; first byte lands in the far chip (U3=PIXEL).
static inline void gpuCmd(uint16_t addr, uint8_t pix) {
  spi.transfer(pix);
  spi.transfer(addr & 0xFF);
  spi.transfer(addr >> 8);
  rclkPulse();
}

// Parallel-load HC163s. MUST be called with FILL_EN low (no write strobe).
static inline void gpuLoadAddr() {
  LO(PIN_PE_N);
  asm volatile("nop;nop;nop;nop");
  LO(PIN_CP);                                  // CP idles HIGH
  asm volatile("nop;nop;nop;nop;nop;nop;nop;nop");
  HI(PIN_CP);                                  // rising edge: load
  asm volatile("nop;nop;nop;nop");
  HI(PIN_PE_N);
}

// N clock pulses = N pixels written at auto-incrementing addresses.
// IRQ jitter is harmless (WE is tied to CP), so interrupts stay enabled.
IRAM_ATTR static void gpuPulses(uint32_t n) {
  const uint32_t m = MASK(PIN_CP);
  while (n--) {
    REG_WRITE(GPIO_OUT_W1TC_REG, m);           // CP low: /WE active
    for (int i = 0; i < PULSE_NOPS; i++) asm volatile("nop");
    REG_WRITE(GPIO_OUT_W1TS_REG, m);           // rising edge: commit + increment
    for (int i = 0; i < PULSE_NOPS; i++) asm volatile("nop");
  }
}

// Linear fill: n bytes starting at addr (may cross row boundaries).
static void gpuFill(uint16_t addr, uint32_t n, uint8_t color) {
  gpuCmd(addr, color);
  gpuLoadAddr();
  HI(PIN_FILL_EN);
  asm volatile("nop;nop;nop;nop;nop;nop");
  gpuPulses(n);
  LO(PIN_FILL_EN);
  statPixels += n; statFills++;
}

static inline void gpuPixel(uint8_t x, uint8_t y, uint8_t c) {
  gpuFill(ADDR(x, y), 1, c);
}

// ------------------------------------------------------------- frame protocol
static void beginFrame() {                     // take the bus, tear-free
  while (FRAME_ACTIVE()) {}                    // let current scan-out finish
  HI(PIN_BUSY);
  delayMicroseconds(2);
  if (FRAME_ACTIVE()) { /* raced into a frame: it will finish; wait it out */
    while (FRAME_ACTIVE()) {}
  }
}
static void endFrame() {
#if DOUBLE_BUF
  HI(PIN_SWAP); delayMicroseconds(1); LO(PIN_SWAP);   // flip front/back
#endif
  LO(PIN_BUSY);
}

// ------------------------------------------------------------- 2D primitives
static void clearScreen(uint8_t c) { gpuFill(0, (uint32_t)W * H, c); }  // 20480 px, one burst

static void fillRect(int x, int y, int w, int h, uint8_t c) {
  if (x < 0) { w += x; x = 0; }  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;      if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  for (int r = 0; r < h; r++) gpuFill(ADDR(x, y + r), w, c);
}

static void line(int x0, int y0, int x1, int y1, uint8_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    if (x0 >= 0 && x0 < W && y0 >= 0 && y0 < H) gpuPixel(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// RGB332 helper
static inline uint8_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (r & 0xE0) | ((g & 0xE0) >> 3) | (b >> 6);
}

// 5x7 font, rows top->bottom, low 5 bits used: "FALLOUT GPU"
struct Glyph { char ch; uint8_t rows[7]; };
static const Glyph FONT[] = {
  {'F', {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b10000}},
  {'A', {0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}},
  {'L', {0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111}},
  {'O', {0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}},
  {'U', {0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}},
  {'T', {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100}},
  {'G', {0b01111,0b10000,0b10000,0b10011,0b10001,0b10001,0b01111}},
  {'P', {0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000}},
  {' ', {0,0,0,0,0,0,0}},
};
static void drawChar(char ch, int x, int y, int s, uint8_t c) {
  for (auto &g : FONT) if (g.ch == ch) {
    for (int r = 0; r < 7; r++)
      for (int b = 0; b < 5; b++)
        if (g.rows[r] & (0b10000 >> b)) fillRect(x + b * s, y + r * s, s, s, c);
    return;
  }
}
static void drawText(const char *t, int x, int y, int s, uint8_t c) {
  for (; *t; t++, x += 6 * s) drawChar(*t, x, y, s, c);
}

// ------------------------------------------------------------- scenes
static void sceneBounce(bool init) {
  static struct { float x, y, vx, vy; int w, h; uint8_t c; } r[6];
  if (init) for (int i = 0; i < 6; i++)
    r[i] = { (float)random(10, W - 50), (float)random(10, H - 40),
             (float)random(-30, 30) / 10.f + 1.5f, (float)random(-30, 30) / 10.f + 1.f,
             random(12, 28), random(9, 22), (uint8_t)random(40, 255) };
  beginFrame();
#if DOUBLE_BUF
  clearScreen(0);
#endif
  for (auto &b : r) {
#if !DOUBLE_BUF
    fillRect((int)b.x, (int)b.y, b.w, b.h, 0);       // erase old
#endif
    b.x += b.vx; b.y += b.vy;
    if (b.x < 0 || b.x + b.w >= W) { b.vx = -b.vx; b.x += b.vx; }
    if (b.y < 0 || b.y + b.h >= H) { b.vy = -b.vy; b.y += b.vy; }
    fillRect((int)b.x, (int)b.y, b.w, b.h, b.c);
  }
  endFrame();
}

static void sceneCube(bool init) {
  static float a = 0, b = 0;
  static int16_t px[8][2]; static bool have = false;
  if (init) { have = false; a = b = 0; }
  const float V[8][3] = {{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                         {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
  const uint8_t E[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},
                            {0,4},{1,5},{2,6},{3,7}};
  int nx[8][2];
  float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
  for (int i = 0; i < 8; i++) {
    float x = V[i][0], y = V[i][1], z = V[i][2];
    float x1 = x * ca - z * sa, z1 = x * sa + z * ca;
    float y1 = y * cb - z1 * sb, z2 = y * sb + z1 * cb;
    float d = 36.f / (z2 + 3.2f);
    nx[i][0] = W / 2 + (int)(x1 * d * 1.2f); nx[i][1] = H / 2 + (int)(y1 * d);
  }
  beginFrame();
#if DOUBLE_BUF
  clearScreen(0);
#else
  if (have) for (auto &e : E)                   // erase previous wireframe
    line(px[e[0]][0], px[e[0]][1], px[e[1]][0], px[e[1]][1], 0);
#endif
  for (auto &e : E)
    line(nx[e[0]][0], nx[e[0]][1], nx[e[1]][0], nx[e[1]][1], rgb(0, 255, 128));
  endFrame();
  for (int i = 0; i < 8; i++) { px[i][0] = nx[i][0]; px[i][1] = nx[i][1]; }
  have = true; a += 0.045f; b += 0.03f;
}

static void sceneText(bool init) {
  static int off = 0;
  if (init) off = 0;
  beginFrame();
  clearScreen(rgb(0, 0, 64));
  fillRect(0, 18 + (int)(8 * sinf(off * 0.08f)), W, 3, rgb(255, 128, 0));
  drawText("FALLOUT", 17, 38, 3, rgb(255, 255, 0));   // 7 glyphs * 18 px = 126
  drawText("GPU", 44, 70, 4, rgb(0, 255, 0));         // 3 glyphs * 24 px = 72
  fillRect(0, 108 - (int)(8 * sinf(off * 0.08f)), W, 3, rgb(255, 128, 0));
  endFrame();
  off++;
}

static void runBenchmark() {
  Serial.println("[bench] GPU fill vs CPU per-pixel, one full screen each...");
  beginFrame();
  uint32_t t0 = micros();
  clearScreen(rgb(0, 64, 0));                  // hardware path
  uint32_t tGpu = micros() - t0;
  t0 = micros();
  for (int y = 0; y < H; y += 4)               // CPU path, 1/4 of rows -> x4
    for (int x = 0; x < W; x++) gpuPixel(x, y, rgb(64, 0, 0));
  uint32_t tCpu = (micros() - t0) * 4;
  endFrame();
  float mpixGpu = (float)(W * H) / tGpu, mpixCpu = (float)(W * H) / tCpu;
  float speedup = (float)tCpu / tGpu;
  Serial.printf("[bench] GPU: %lu us (%.2f Mpix/s) | CPU-style: %lu us (%.3f Mpix/s) | %.1fx faster\n",
                tGpu, mpixGpu, tCpu, mpixCpu, speedup);
  // draw result bars
  beginFrame();
  clearScreen(0);
  drawText("GPU", 8, 18, 2, rgb(0,255,0));
  fillRect(52, 18, 100, 12, rgb(0, 255, 0));
  drawText("PU", 20, 48, 2, rgb(255,0,0));     // "CPU" (no C glyph: draws PU + block)
  fillRect(8, 48, 10, 14, rgb(255,0,0));
  int cw = (int)(100.f / speedup); if (cw < 2) cw = 2;
  fillRect(52, 48, cw, 12, rgb(255, 0, 0));
  drawText("FALLOUT GPU", 14, 92, 2, rgb(255,255,0));   // 11 glyphs * 12 px = 132
  endFrame();
  char msg[48]; snprintf(msg, sizeof msg, "bench %.1fx", speedup);
  strncpy(currentScene, msg, sizeof currentScene - 1);
}

// ------------------------------------------------------------- test modes
static void testM1() {                          // no SRAM needed
  static const uint8_t cols[] = { 0xE0, 0x1C, 0x03, 0xFF, 0x00, 0xAA };
  static int i = 0;
  gpuCmd(0, cols[i]); i = (i + 1) % 6;
  HI(PIN_FILL_EN);                              // U3 drives DB continuously
  Serial.printf("[M1] DB = 0x%02X (viewer should show this solid color)\n", cols[(i+5)%6]);
  delay(1000);                                  // FILL_EN stays high on purpose
}
static void testM2step() {
  LO(PIN_FILL_EN);
  gpuCmd(0, 0); gpuLoadAddr();
  HI(PIN_FILL_EN);
  Serial.println("[M2] CP stepping 2 Hz. DMM: AB0=1Hz, AB1=0.5Hz ... (Ctrl in menu to stop)");
  while (!Serial.available()) { gpuPulses(1); delay(250); }
  LO(PIN_FILL_EN);
}
static void testM2load() {
  Serial.println("[M2] alternating load 0x5555/0xAAAA on AB (1 s each)");
  while (!Serial.available()) {
    gpuCmd(0x5555, 0); gpuLoadAddr(); HI(PIN_FILL_EN); delay(1000); LO(PIN_FILL_EN);
    gpuCmd(0xAAAA, 0); gpuLoadAddr(); HI(PIN_FILL_EN); delay(1000); LO(PIN_FILL_EN);
  }
}
static void testM3static() {
  beginFrame();
  clearScreen(rgb(32, 32, 96));
  for (int i = 0; i < 8; i++) fillRect(4 + i * 19, 8, 15, 15, (uint8_t)(i * 36 + 3));
  fillRect(0, 56, W, 2, 0xFF);
  drawText("FALLOUT GPU", 14, 72, 2, rgb(255, 255, 0));
  endFrame();
  Serial.println("[M3] pattern drawn: blue bg, 8 color squares, white line, text.");
}

// ------------------------------------------------------------- main
static int mode = 0;         // 0 idle, 5 = reel
static int scene = 0; static bool sceneInit = true;
static uint32_t lastStat = 0;

void setup() {
  // Park the GPU bus FIRST: scan side owns it, no writes possible.
  pinMode(PIN_FILL_EN, OUTPUT); digitalWrite(PIN_FILL_EN, LOW);
  pinMode(PIN_CP, OUTPUT);      digitalWrite(PIN_CP, HIGH);      // idles HIGH
  pinMode(PIN_PE_N, OUTPUT);    digitalWrite(PIN_PE_N, HIGH);
  pinMode(PIN_RCLK, OUTPUT);    digitalWrite(PIN_RCLK, LOW);
  pinMode(PIN_BUSY, OUTPUT);    digitalWrite(PIN_BUSY, LOW);
  pinMode(PIN_SWAP, OUTPUT);    digitalWrite(PIN_SWAP, LOW);
  pinMode(PIN_SELCLR_N, OUTPUT);digitalWrite(PIN_SELCLR_N, HIGH);
  pinMode(PIN_SEL_Q, INPUT);
  pinMode(PIN_FRAMEACT, INPUT);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, -1, PIN_LINK_TX);   // stats link, TX only
  spi.begin(PIN_SCK, -1, PIN_MOSI, -1);
  spi.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));

#if DOUBLE_BUF
  digitalWrite(PIN_SELCLR_N, LOW); delayMicroseconds(2);   // SEL := 0 (A=back)
  digitalWrite(PIN_SELCLR_N, HIGH);
#endif
  // Host runs no WiFi: full 3V3 regulator budget goes to the GPU rail.
  Serial.println("\nFALLOUT GPU host ready. Menu: 1=M1 2=M2step 3=M2load 4=M3 5=reel 6=bench 7=swap");
  strncpy(currentScene, "idle", sizeof currentScene);
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    LO(PIN_FILL_EN);
    switch (c) {
      case '1': mode = 1; break;
      case '2': testM2step(); mode = 0; break;
      case '3': testM2load(); mode = 0; break;
      case '4': testM3static(); mode = 0; strncpy(currentScene,"static",16); break;
      case '5': mode = 5; scene = 0; sceneInit = true; break;
      case '6': runBenchmark(); mode = 0; break;
      case '7':
#if DOUBLE_BUF
        for (int i = 0; i < 10; i++) {
          beginFrame(); clearScreen(i & 1 ? rgb(255,0,0) : rgb(0,0,255)); endFrame();
          delay(500);
        }
#else
        Serial.println("set DOUBLE_BUF 1 + reflash first");
#endif
        mode = 0; break;
    }
  }

  if (mode == 1) testM1();

  if (mode == 5) {
    if (digitalRead(PIN_BOOT_BTN) == LOW) {            // next scene
      scene = (scene + 1) % 3; sceneInit = true; delay(250);
    }
    static uint32_t autoT = 0;
    if (millis() - autoT > 12000) { autoT = millis(); scene = (scene + 1) % 3; sceneInit = true; }
    switch (scene) {
      case 0: strncpy(currentScene,"bounce",16); sceneBounce(sceneInit); break;
      case 1: strncpy(currentScene,"cube",16);   sceneCube(sceneInit);   break;
      case 2: strncpy(currentScene,"text",16);   sceneText(sceneInit);   break;
    }
    sceneInit = false;
    delay(16);
  }

  if (millis() - lastStat > 1000) {                    // stats to scanout ESP32
    lastStat = millis();
    Serial2.printf("S,%s,%lu,%lu\n", currentScene, statFills, statPixels);
    statFills = 0; statPixels = 0;
  }
}
