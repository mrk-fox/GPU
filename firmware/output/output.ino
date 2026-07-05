/*
 * FALLOUT GPU — SCANOUT (ESP32 #2, ESP32-S3)
 * Clocks the HC393 scan counter, reads the framebuffer 8 bits parallel,
 * shows it 1:1 on a 1.8" 128x160 ST7735 TFT (landscape 160x128) *and*
 * serves it to browsers over WiFi AP.
 *
 *   AP:      FALLOUT-GPU  (open)     Viewer: http://192.168.4.1/
 *
 * Board: ESP32-S3 (DevKitC-1 N16R8). Pins avoid straps (0,3,45,46),
 * octal-PSRAM pins (35-37), USB (19/20), UART0 (43/44).
 * TFT driver is self-contained — no libraries to install.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include "soc/gpio_reg.h"

#define USE_TFT     1
#define SCAN_NOPS   14        // half-period pad: 14 ~= 1.4 MHz. Lower = faster.
#define W 160                 // frame = ST7735 landscape, 1:1
#define H 128
#define FRAME_BYTES (W * H)   // 20480 (uses 20 KB of each 32 KB SRAM)

// GPU-side pins — data bus DB0..DB7 on GPIO 4,5,6,7,15,16,17,18 (one register read)
#define PIN_SCANCLK   9       // '393 counts on FALLING edge; idles HIGH
#define PIN_SCANRST  10       // active HIGH clear
#define PIN_BUSY_IN  11       // host owns GPU bus while high
#define PIN_FRAMEACT 12       // we assert while capturing
#define PIN_LINK_RX  13       // stats UART from host

// TFT: 1.8" 128x160 ST7735, module pins GND VCC SCL SDA RES DC CS BL.
// SCL = SPI clock, SDA = SPI MOSI. VCC and BL -> 3V3, GND -> GND. SPI3/HSPI.
#define TFT_SCK    40         // SCL
#define TFT_MOSI   41         // SDA
#define TFT_CS     42
#define TFT_DC      2
#define TFT_RST     1         // RES
#define TFT_HZ     15000000   // ST7735 spec ~15 MHz; many panels take 26M — tune later
#define TFT_MADCTL 0x60       // landscape (MX|MV). Mirrored? 0xA0. R/B swapped? add |0x08
#define TFT_INVERT 0          // set 1 if image looks negative (some ST7735S panels)
#define TFT_XOFS   0          // panel RAM offsets; a few modules need 1-2 here
#define TFT_YOFS   0

#define MASK(p) (1UL << (p))
#define HI(p) REG_WRITE(GPIO_OUT_W1TS_REG, MASK(p))
#define LO(p) REG_WRITE(GPIO_OUT_W1TC_REG, MASK(p))

static uint8_t capBuf[FRAME_BYTES];
static uint8_t frontBuf[FRAME_BYTES];
static uint8_t bmpHdr[1078];
static WebServer server(80);
static float capFps = 0;
static char hostStats[96] = "S,boot,0,0";

static inline uint8_t pkByte(uint32_t raw) {
  return ((raw >> 4) & 0x0F)           // DB0..DB3 = GPIO4..7
       | (((raw >> 15) & 0x0F) << 4);  // DB4..DB7 = GPIO15..18
}

IRAM_ATTR static void captureFrame() {
  HI(PIN_FRAMEACT);
  HI(PIN_SCANRST);                     // async clear -> address 0
  for (int i = 0; i < 20; i++) asm volatile("nop");
  LO(PIN_SCANRST);
  for (int i = 0; i < 20; i++) asm volatile("nop");
  const uint32_t m = MASK(PIN_SCANCLK);
  for (uint32_t k = 0; k < FRAME_BYTES; k++) {
    capBuf[k] = pkByte(REG_READ(GPIO_IN_REG));   // read pixel at current addr
    REG_WRITE(GPIO_OUT_W1TC_REG, m);             // falling edge -> addr+1
    for (int i = 0; i < SCAN_NOPS; i++) asm volatile("nop");  // ripple + tAA settle
    REG_WRITE(GPIO_OUT_W1TS_REG, m);
    for (int i = 0; i < 2; i++) asm volatile("nop");
  }
  LO(PIN_FRAMEACT);
}

// ------------------------------------------------------------- minimal ST7735
#if USE_TFT
SPIClass tftSpi(HSPI);
static uint16_t lut565[256];           // RGB332 -> byte-swapped RGB565
static uint16_t rowBuf[W];

static void tftCmd(uint8_t c) {
  digitalWrite(TFT_DC, LOW);  digitalWrite(TFT_CS, LOW);
  tftSpi.write(c);
  digitalWrite(TFT_CS, HIGH); digitalWrite(TFT_DC, HIGH);
}
static void tftData(const uint8_t *d, size_t n) {
  digitalWrite(TFT_CS, LOW);
  tftSpi.writeBytes(d, n);
  digitalWrite(TFT_CS, HIGH);
}
static void tftData1(uint8_t d) { tftData(&d, 1); }

static void tftWindow(int x0, int y0, int x1, int y1) {
  uint8_t d[4];
  d[0] = 0; d[1] = x0 + TFT_XOFS; d[2] = 0; d[3] = x1 + TFT_XOFS;
  tftCmd(0x2A); tftData(d, 4);                 // column range
  d[1] = y0 + TFT_YOFS; d[3] = y1 + TFT_YOFS;
  tftCmd(0x2B); tftData(d, 4);                 // row range
  tftCmd(0x2C);                                // RAM write
}

static void tftInit() {
  pinMode(TFT_CS, OUTPUT);  digitalWrite(TFT_CS, HIGH);
  pinMode(TFT_DC, OUTPUT);  digitalWrite(TFT_DC, HIGH);
  pinMode(TFT_RST, OUTPUT);
  tftSpi.begin(TFT_SCK, -1, TFT_MOSI, -1);
  tftSpi.beginTransaction(SPISettings(TFT_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(TFT_RST, LOW);  delay(10);
  digitalWrite(TFT_RST, HIGH); delay(120);
  tftCmd(0x01); delay(150);              // soft reset
  tftCmd(0x11); delay(255);              // sleep out (ST7735 wants the long wait)
  tftCmd(0x3A); tftData1(0x05);          // 16 bpp
  tftCmd(0x36); tftData1(TFT_MADCTL);    // landscape 160x128
#if TFT_INVERT
  tftCmd(0x21);                          // inversion on
#else
  tftCmd(0x20);                          // inversion off
#endif
  tftCmd(0x13); delay(10);               // normal display mode
  tftCmd(0x29); delay(100);              // display on

  for (int i = 0; i < 256; i++) {        // palette LUT, pre-byteswapped
    uint8_t r = ((i >> 5) & 7) * 255 / 7;
    uint8_t g = ((i >> 2) & 7) * 255 / 7;
    uint8_t b = (i & 3) * 255 / 3;
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    lut565[i] = (c >> 8) | (c << 8);
  }
  // clear whole panel to black once
  tftWindow(0, 0, W - 1, H - 1);
  memset(rowBuf, 0, sizeof rowBuf);                       // 160 px = 320 zero bytes
  digitalWrite(TFT_CS, LOW);
  for (int y = 0; y < H; y++) tftSpi.writeBytes((uint8_t*)rowBuf, W * 2);
  digitalWrite(TFT_CS, HIGH);
}

static void tftPush(const uint8_t *fb) {       // GPU frame 1:1 on the full panel
  tftWindow(0, 0, W - 1, H - 1);
  digitalWrite(TFT_CS, LOW);
  for (int y = 0; y < H; y++) {
    const uint8_t *src = fb + y * W;
    for (int x = 0; x < W; x++) rowBuf[x] = lut565[src[x]];
    tftSpi.writeBytes((uint8_t*)rowBuf, W * 2);
  }
  digitalWrite(TFT_CS, HIGH);
}
#endif

// ------------------------------------------------------------- BMP (8bpp, RGB332 palette)
static void buildBmpHeader() {
  memset(bmpHdr, 0, sizeof bmpHdr);
  uint32_t fileSize = 1078 + FRAME_BYTES;
  bmpHdr[0]='B'; bmpHdr[1]='M';
  memcpy(bmpHdr+2,  &fileSize, 4);
  uint32_t off = 1078;            memcpy(bmpHdr+10, &off, 4);
  uint32_t ihs = 40;              memcpy(bmpHdr+14, &ihs, 4);
  int32_t  w = W;                 memcpy(bmpHdr+18, &w, 4);
  int32_t  h = -H;                memcpy(bmpHdr+22, &h, 4);   // negative = top-down
  uint16_t planes = 1;            memcpy(bmpHdr+26, &planes, 2);
  uint16_t bpp = 8;               memcpy(bmpHdr+28, &bpp, 2);
  uint32_t imgSize = FRAME_BYTES; memcpy(bmpHdr+34, &imgSize, 4);
  uint32_t colors = 256;          memcpy(bmpHdr+46, &colors, 4);
  for (int i = 0; i < 256; i++) {                // palette: index = RGB332 byte
    uint8_t r = ((i >> 5) & 7) * 255 / 7;
    uint8_t g = ((i >> 2) & 7) * 255 / 7;
    uint8_t b = (i & 3) * 255 / 3;
    bmpHdr[54 + i*4 + 0] = b; bmpHdr[54 + i*4 + 1] = g;
    bmpHdr[54 + i*4 + 2] = r; bmpHdr[54 + i*4 + 3] = 0;
  }
}

static void sendAll(WiFiClient &c, const uint8_t *p, size_t n) {
  while (n) { size_t s = c.write(p, n); if (!s) { delay(1); continue; } p += s; n -= s; }
}

// ------------------------------------------------------------- HTTP
static const char VIEWER[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FALLOUT GPU</title><style>
body{background:#0a0a0a;color:#39ff64;font-family:monospace;text-align:center;margin:0;padding:12px}
h1{font-size:20px;letter-spacing:6px;margin:8px}
canvas{image-rendering:pixelated;width:min(96vw,640px);border:2px solid #39ff64;background:#000}
#s{color:#9f9;font-size:13px;white-space:pre}
</style></head><body>
<h1>&#9608; FALLOUT GPU &#9608;</h1>
<canvas id="c" width="160" height="128"></canvas>
<div id="s">connecting...</div>
<script>
const cv=document.getElementById('c'),cx=cv.getContext('2d');
let n=0,t0=performance.now();
async function tick(){
 try{
  const r=await fetch('/frame.bmp?'+Date.now());
  const b=await r.blob();
  const im=await createImageBitmap(b);
  cx.imageSmoothingEnabled=false;cx.drawImage(im,0,0);
  n++;
 }catch(e){}
 setTimeout(tick,60);
}
async function stats(){
 try{
  const j=await(await fetch('/stats')).json();
  const fps=(n*1000/(performance.now()-t0)).toFixed(1);n=0;t0=performance.now();
  document.getElementById('s').textContent=
   'viewer '+fps+' fps | capture '+j.capFps.toFixed(1)+' fps | scene '+j.scene+
   ' | '+j.fills+' fills/s | '+(j.pixels/1e6).toFixed(2)+' Mpix/s drawn';
 }catch(e){}
 setTimeout(stats,1000);
}
tick();stats();
</script></body></html>
)HTML";

static void handleFrame() {
  server.setContentLength(1078 + FRAME_BYTES);
  server.send(200, "image/bmp", "");
  WiFiClient c = server.client();
  sendAll(c, bmpHdr, 1078);
  sendAll(c, frontBuf, FRAME_BYTES);
}

static void handleStats() {
  char scene[24] = "?"; unsigned long fills = 0, pixels = 0;
  sscanf(hostStats, "S,%23[^,],%lu,%lu", scene, &fills, &pixels);
  char js[192];
  snprintf(js, sizeof js,
    "{\"capFps\":%.1f,\"scene\":\"%s\",\"fills\":%lu,\"pixels\":%lu}",
    capFps, scene, fills, pixels);
  server.send(200, "application/json", js);
}

// ------------------------------------------------------------- main
void setup() {
  pinMode(PIN_SCANCLK, OUTPUT);  digitalWrite(PIN_SCANCLK, HIGH);
  pinMode(PIN_SCANRST, OUTPUT);  digitalWrite(PIN_SCANRST, LOW);
  pinMode(PIN_FRAMEACT, OUTPUT); digitalWrite(PIN_FRAMEACT, LOW);
  pinMode(PIN_BUSY_IN, INPUT);
  const int dbPins[8] = {4, 5, 6, 7, 15, 16, 17, 18};
  for (int p : dbPins) pinMode(p, INPUT);

  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, PIN_LINK_RX, -1);   // stats from host
  buildBmpHeader();
  memset(frontBuf, 0, FRAME_BYTES);
#if USE_TFT
  tftInit();
#endif

  WiFi.softAP("FALLOUT-GPU");                            // open AP
  Serial.printf("\nFALLOUT GPU scanout. Viewer: http://%s/\n",
                WiFi.softAPIP().toString().c_str());

  server.on("/", []() { server.send_P(200, "text/html", VIEWER); });
  server.on("/frame.bmp", handleFrame);
  server.on("/stats", handleStats);
  server.begin();
}

void loop() {
  static uint32_t frames = 0, tFps = 0;

  if (digitalRead(PIN_BUSY_IN) == LOW) {       // host not drawing -> grab a frame
    captureFrame();
    memcpy(frontBuf, capBuf, FRAME_BYTES);     // single-threaded: no race
#if USE_TFT
    tftPush(frontBuf);
#endif
    frames++;
  }
  if (millis() - tFps >= 1000) {
    capFps = frames * 1000.0f / (millis() - tFps);
    frames = 0; tFps = millis();
  }

  while (Serial1.available()) {                // collect host stats lines
    static char line[96]; static int len = 0;
    char ch = Serial1.read();
    if (ch == '\n' || len >= 95) { line[len] = 0; if (len > 2) strcpy(hostStats, line); len = 0; }
    else if (ch != '\r') line[len++] = ch;
  }

  server.handleClient();
}
