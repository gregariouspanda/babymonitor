/*
 * ESP32 (CH340 dev board) + Arducam Mini 2MP (OV2640) -> WiFi remote monitor
 *
 * Endpoints (HTTP Basic auth):
 *   /            simple viewer page
 *   /capture     single JPEG snapshot
 *   /stream      MJPEG live stream
 *   /size?v=0..3 0=320x240 1=640x480 2=800x600 3=1024x768
 *
 * WIRING (ESP32 VSPI -> Arducam Mini)
 *   CS   -> GPIO5      SDA -> GPIO21
 *   MOSI -> GPIO23     SCL -> GPIO22
 *   MISO -> GPIO19     VCC -> 3V3
 *   SCK  -> GPIO18     GND -> GND
 *
 * SETUP
 *   1. Install the "ArduCAM" library (Arduino Library Manager) and ESP32 board package.
 *   2. Edit  Arduino/libraries/ArduCAM/memorysaver.h : comment out every
 *      #define line, then leave ONLY   #define OV2640_MINI_2MP
 *   3. Board: "ESP32 Dev Module". CH340 driver may be needed on your OS.
 *   4. Set WIFI_SSID / WIFI_PASS / AUTH_* below, upload, open Serial Monitor
 *      @115200 to see the IP, then browse to http://<ip>/
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <SPI.h>
#include <ArduCAM.h>
#include "memorysaver.h"

#if !(defined OV2640_MINI_2MP)
#error Select OV2640_MINI_2MP in ArduCAM/memorysaver.h
#endif

// ---- user config ----
const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PASS = "YOUR_PASSWORD";
const char* AUTH_USER = "admin";
const char* AUTH_PASS = "change-me";   // CHANGE THIS

// ---- pins ----
const int PIN_CS = 5, PIN_SCK = 18, PIN_MISO = 19, PIN_MOSI = 23;
const int PIN_SDA = 21, PIN_SCL = 22;

ArduCAM myCAM(OV2640, PIN_CS);
WebServer server(80);

const uint8_t SIZES[] = {OV2640_320x240, OV2640_640x480, OV2640_800x600, OV2640_1024x768};

// Capture one JPEG into a malloc'd buffer. Caller must free(*out). Returns length or 0.
size_t captureJpeg(uint8_t** out) {
  myCAM.flush_fifo();
  myCAM.clear_fifo_flag();
  myCAM.start_capture();

  uint32_t t0 = millis();
  while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK)) {
    if (millis() - t0 > 3000) return 0;
    delay(1);
  }

  uint32_t len = myCAM.read_fifo_length();
  if (len == 0 || len >= 0x7FFFF) { myCAM.clear_fifo_flag(); return 0; }

  uint8_t* buf = (uint8_t*)malloc(len);
  if (!buf) { myCAM.clear_fifo_flag(); return 0; }

  myCAM.CS_LOW();
  myCAM.set_fifo_burst();
  for (uint32_t i = 0; i < len; i++) buf[i] = SPI.transfer(0x00);
  myCAM.CS_HIGH();
  myCAM.clear_fifo_flag();

  // Trim to the real JPEG: from FFD8 to first FFD9.
  uint32_t s = 0, e = 0;
  while (s + 1 < len && !(buf[s] == 0xFF && buf[s + 1] == 0xD8)) s++;
  for (e = s + 2; e + 1 < len && !(buf[e] == 0xFF && buf[e + 1] == 0xD9); e++);
  if (s + 1 >= len || e + 1 >= len) { free(buf); return 0; }
  size_t jlen = e + 2 - s;
  if (s) memmove(buf, buf + s, jlen);
  *out = buf;
  return jlen;
}

bool authed() {
  if (server.authenticate(AUTH_USER, AUTH_PASS)) return true;
  server.requestAuthentication();
  return false;
}

void handleRoot() {
  if (!authed()) return;
  server.send(200, "text/html",
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<body style='background:#111;color:#eee;font-family:sans-serif;text-align:center'>"
    "<h3>ESP32 Camera</h3><img src='/stream' style='max-width:100%'><p>"
    "<a href='/capture' style='color:#6cf'>snapshot</a> | size: "
    "<a href='/size?v=0' style='color:#6cf'>320</a> "
    "<a href='/size?v=1' style='color:#6cf'>640</a> "
    "<a href='/size?v=2' style='color:#6cf'>800</a> "
    "<a href='/size?v=3' style='color:#6cf'>1024</a></p></body>");
}

void handleCapture() {
  if (!authed()) return;
  uint8_t* buf; size_t n = captureJpeg(&buf);
  if (!n) { server.send(503, "text/plain", "capture failed"); return; }
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(n);
  server.send(200, "image/jpeg", "");
  server.sendContent((const char*)buf, n);
  free(buf);
}

void handleStream() {
  if (!authed()) return;
  WiFiClient client = server.client();
  client.print("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n"
               "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
  while (client.connected()) {
    uint8_t* buf; size_t n = captureJpeg(&buf);
    if (!n) { delay(50); continue; }
    client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)n);
    size_t sent = 0;
    while (sent < n && client.connected()) {
      size_t w = client.write(buf + sent, min((size_t)1436, n - sent));
      if (!w) break;
      sent += w;
    }
    client.print("\r\n");
    free(buf);
    delay(1);
  }
}

void handleSize() {
  if (!authed()) return;
  int v = server.arg("v").toInt();
  if (v < 0 || v > 3) { server.send(400, "text/plain", "v must be 0..3"); return; }
  myCAM.OV2640_set_JPEG_size(SIZES[v]);
  delay(100);
  server.sendHeader("Location", "/");
  server.send(302);
}

void halt(const char* msg) {
  Serial.println(msg);
  while (true) delay(1000);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(PIN_SDA, PIN_SCL);
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  SPI.setFrequency(4000000);

  myCAM.write_reg(0x07, 0x80);  // reset CPLD
  delay(100);
  myCAM.write_reg(0x07, 0x00);
  delay(100);

  myCAM.write_reg(ARDUCHIP_TEST1, 0x55);
  if (myCAM.read_reg(ARDUCHIP_TEST1) != 0x55) halt("SPI error: check CS/MOSI/MISO/SCK wiring");

  uint8_t vid, pid;
  myCAM.wrSensorReg8_8(0xff, 0x01);
  myCAM.rdSensorReg8_8(0x0A, &vid);
  myCAM.rdSensorReg8_8(0x0B, &pid);
  if (vid != 0x26 || (pid != 0x41 && pid != 0x42)) halt("OV2640 not found: check SDA/SCL wiring");

  myCAM.set_format(JPEG);
  myCAM.InitCAM();
  myCAM.OV2640_set_JPEG_size(OV2640_640x480);
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print('.'); }
  Serial.printf("\nOpen http://%s/\n", WiFi.localIP().toString().c_str());

  server.on("/", handleRoot);
  server.on("/capture", handleCapture);
  server.on("/stream", handleStream);
  server.on("/size", handleSize);
  server.begin();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {   // simple auto-reconnect
    WiFi.reconnect();
    delay(2000);
  }
  server.handleClient();
}
