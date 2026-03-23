/*
  NRF Walkie-Talkie (PTT) with optional OLED
  for XIAO ESP32-C3 / XIAO ESP32-S3
  -----------------------------------------

  Подключение (рекомендованное):
  nRF24L01 PA/LNA:
    VCC  -> 3V3 (НЕ 5V!)
    GND  -> GND
    CE   -> D2
    CSN  -> D1
    SCK  -> D8
    MISO -> D9
    MOSI -> D10

  MAX9814:
    VCC  -> 3V3
    GND  -> GND
    OUT  -> A0

  Кнопка PTT:
    один контакт -> D3
    второй -> GND
    (в коде INPUT_PULLUP)

  Аудио выход (PWM):
    D6 -> (1k резистор) -> узел PWM_AUDIO
    PWM_AUDIO -> (100nF на GND)   // простой RC-фильтр
    PWM_AUDIO -> (1uF последовательно) -> IN усилителя
    GND ESP32 -> GND усилителя
    Усилитель -> динамик 4-8 Ом

  OLED (опционально, SSD1306 I2C 128x64):
    VCC -> 3V3
    GND -> GND
    SDA -> SDA XIAO (штатный I2C)
    SCL -> SCL XIAO (штатный I2C)

  ВАЖНО:
  - Для PA/LNA модуля nRF24 добавь 470uF + 100nF между 3V3 и GND прямо у модуля.
  - NODE_ID на двух устройствах должен быть разный: 0 и 1.
  - Это полудуплекс PTT: нажал кнопку -> говоришь, отпустил -> слушаешь.
*/

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>
#if defined(ARDUINO_ARCH_ESP32)
#if __has_include("esp32-hal-ledc.h")
#include "esp32-hal-ledc.h"
#endif
#include <esp_wifi.h>
#if __has_include("esp32-hal-bt.h")
#include "esp32-hal-bt.h"
#endif
#if __has_include("esp_bt.h")
#include <esp_bt.h>
#endif
#endif

// ---- Опции проекта ----
#define NODE_ID 0              // на втором устройстве поставить 1
#define USE_OLED 1             // 1 = экран включен, 0 = без экрана
#define ENABLE_SIMPLE_CRYPT 1  // 1 = простое XOR-шифрование

#if USE_OLED
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#endif

// ---- Пины ----
static const int PIN_NRF_CE = D2;
static const int PIN_NRF_CSN = D1;
static const int PIN_NRF_SCK = D8;
static const int PIN_NRF_MISO = D9;
static const int PIN_NRF_MOSI = D10;

static const int PIN_MIC = A0;
static const int PIN_PTT = D3;
static const int PIN_SPK = D6;  // PWM output to amplifier input

// ---- Аудио/радио параметры ----
static const uint16_t SAMPLE_RATE_HZ = 8000;
static const uint16_t SAMPLE_PERIOD_US = 1000000UL / SAMPLE_RATE_HZ;
static const uint8_t AUDIO_PAYLOAD = 24;

// Для nRF24 всегда <= 32 байт payload
struct AudioPacket {
  uint8_t seq;
  uint8_t nonce;
  uint8_t pcm[AUDIO_PAYLOAD];
};

RF24 radio(PIN_NRF_CE, PIN_NRF_CSN);
const uint8_t ADDR[2][6] = {"W0LK0", "W0LK1"};

static uint8_t txSeq = 0;
static bool txMode = false;

// RX ring buffer
static const uint16_t RX_BUF_SIZE = 1024;
uint8_t rxBuf[RX_BUF_SIZE];
uint16_t rxHead = 0, rxTail = 0, rxCount = 0;

uint32_t nextPlayUs = 0;
uint32_t nextCaptureUs = 0;
uint32_t lastUiMs = 0;
uint32_t rxPackets = 0;
uint32_t txPackets = 0;

#if USE_OLED
Adafruit_SSD1306 display(128, 64, &Wire, -1);
#endif

bool pttPressed() { return digitalRead(PIN_PTT) == LOW; }

void disableBuiltinRadios() {
#if defined(ARDUINO_ARCH_ESP32)
  // Если какой-то код успел поднять Wi-Fi, корректно гасим стек.
  esp_wifi_stop();
  esp_wifi_deinit();

  // В Arduino-ядре для ESP32 это самый совместимый способ остановить BT.
#if __has_include("esp32-hal-bt.h")
  btStop();
#endif
#if __has_include("esp_bt.h")
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
#endif
#endif
}

void rxPush(uint8_t v) {
  if (rxCount >= RX_BUF_SIZE) return;
  rxBuf[rxHead] = v;
  rxHead = (rxHead + 1) % RX_BUF_SIZE;
  rxCount++;
}

bool rxPop(uint8_t& v) {
  if (!rxCount) return false;
  v = rxBuf[rxTail];
  rxTail = (rxTail + 1) % RX_BUF_SIZE;
  rxCount--;
  return true;
}

void audioPlaybackTick() {
  uint32_t now = micros();
  while ((int32_t)(now - nextPlayUs) >= 0) {
    nextPlayUs += SAMPLE_PERIOD_US;
    uint8_t s = 128;  // silence midpoint
    rxPop(s);
    ledcWrite(PIN_SPK, s);
    now = micros();
  }
}

// Простой PRNG для XOR-маски. Это не "сильная крипта", но трафик не читается в лоб.
static inline uint8_t prng8(uint8_t& state) {
  state ^= (uint8_t)(state << 3);
  state ^= (uint8_t)(state >> 5);
  state ^= (uint8_t)(state << 1);
  return state;
}

void cryptBuffer(uint8_t* data, uint8_t len, uint8_t seq, uint8_t nonce) {
#if ENABLE_SIMPLE_CRYPT
  // Ключ: одинаковый на обоих девайсах
  static const uint8_t KEY[8] = {0x41, 0x72, 0x44, 0x75, 0x69, 0x6E, 0x6F, 0x21};
  uint8_t s = (uint8_t)(KEY[NODE_ID & 7] ^ seq ^ nonce ^ 0xA5);
  for (uint8_t i = 0; i < len; i++) {
    s = prng8(s);
    data[i] ^= (uint8_t)(s ^ KEY[i & 7]);
  }
#else
  (void)data;
  (void)len;
  (void)seq;
  (void)nonce;
#endif
}

void enterTx() {
  txMode = true;
  radio.stopListening();
  nextCaptureUs = micros();
}

void enterRx() {
  txMode = false;
  radio.startListening();
}

void txAudioChunk() {
  AudioPacket p{};
  p.seq = txSeq++;
  p.nonce = (uint8_t)esp_random();

  for (uint8_t i = 0; i < AUDIO_PAYLOAD; i++) {
    while ((int32_t)(micros() - nextCaptureUs) < 0) {}
    nextCaptureUs += SAMPLE_PERIOD_US;

    uint16_t adc = analogRead(PIN_MIC);  // 0..4095
    p.pcm[i] = (uint8_t)(adc >> 4);      // 8-bit PCM
  }

  cryptBuffer(p.pcm, AUDIO_PAYLOAD, p.seq, p.nonce);

  if (radio.write(&p, sizeof(p))) {
    txPackets++;
  }
}

void rxAudioPackets() {
  while (radio.available()) {
    AudioPacket p{};
    radio.read(&p, sizeof(p));
    cryptBuffer(p.pcm, AUDIO_PAYLOAD, p.seq, p.nonce);
    for (uint8_t i = 0; i < AUDIO_PAYLOAD; i++) rxPush(p.pcm[i]);
    rxPackets++;
  }
}

#if USE_OLED
void drawUI() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("NODE: ");
  display.println(NODE_ID);

  display.print("MODE: ");
  display.println(txMode ? "TX" : "RX");

  display.print("PTT : ");
  display.println(pttPressed() ? "PRESSED" : "RELEASED");

  display.print("BUF : ");
  display.print(rxCount);
  display.print("/");
  display.println(RX_BUF_SIZE);

  display.print("TXP : ");
  display.println(txPackets);
  display.print("RXP : ");
  display.println(rxPackets);

#if ENABLE_SIMPLE_CRYPT
  display.println("CRYPT: SIMPLE XOR");
#else
  display.println("CRYPT: OFF");
#endif

  display.display();
}
#endif

void setup() {
  Serial.begin(115200);
  disableBuiltinRadios();

  pinMode(PIN_PTT, INPUT_PULLUP);
  pinMode(PIN_MIC, INPUT);

  analogReadResolution(12);

  // PWM audio out: 8-bit @ 160kHz carrier
  ledcAttach(PIN_SPK, 160000, 8);
  ledcWrite(PIN_SPK, 128);

  SPI.begin(PIN_NRF_SCK, PIN_NRF_MISO, PIN_NRF_MOSI, PIN_NRF_CSN);

  if (!radio.begin()) {
    while (true) {
      Serial.println("nRF24 not found!");
      delay(1000);
    }
  }

  radio.setAutoAck(false);
  radio.setCRCLength(RF24_CRC_8);
  radio.setPayloadSize(sizeof(AudioPacket));
  radio.setChannel(90);
  radio.setDataRate(RF24_250KBPS);
  radio.setPALevel(RF24_PA_MAX);

  radio.openReadingPipe(1, ADDR[NODE_ID]);
  radio.openWritingPipe(ADDR[1 - NODE_ID]);
  radio.startListening();

  nextPlayUs = micros();

#if USE_OLED
  Wire.begin();
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Walkie boot...");
    display.display();
    delay(400);
  }
#endif
}

void loop() {
  audioPlaybackTick();

  if (pttPressed()) {
    if (!txMode) enterTx();
    txAudioChunk();
  } else {
    if (txMode) enterRx();
    rxAudioPackets();
  }

#if USE_OLED
  if (millis() - lastUiMs > 150) {
    lastUiMs = millis();
    drawUI();
  }
#endif
}
