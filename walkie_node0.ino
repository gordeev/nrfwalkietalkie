/*
  NRF Walkie-Talkie (PTT) for XIAO ESP32-C3 / XIAO ESP32-S3
  ----------------------------------------------------------
  NODE 0 firmware

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

  ВАЖНО:
  - Для PA/LNA модуля nRF24 добавь 470uF + 100nF между 3V3 и GND прямо у модуля.
  - Этот файл прошивать в первое устройство (NODE_ID = 0).
*/

#include <Arduino.h>
#include <SPI.h>
#include <RF24.h>

#define NODE_ID 0

// ---- Пины ----
static const int PIN_NRF_CE = D2;
static const int PIN_NRF_CSN = D1;
static const int PIN_NRF_SCK = D8;
static const int PIN_NRF_MISO = D9;
static const int PIN_NRF_MOSI = D10;

static const int PIN_MIC = A0;
static const int PIN_PTT = D3;
static const int PIN_SPK = D6; // PWM output to amplifier input

// ---- Аудио/радио параметры ----
static const uint16_t SAMPLE_RATE_HZ = 8000; // 8kHz речь
static const uint16_t SAMPLE_PERIOD_US = 1000000UL / SAMPLE_RATE_HZ;
static const uint8_t AUDIO_PAYLOAD = 24; // 24 байта аудио в пакет

struct AudioPacket {
  uint8_t seq;
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

bool pttPressed() { return digitalRead(PIN_PTT) == LOW; }

void rxPush(uint8_t v) {
  if (rxCount >= RX_BUF_SIZE)
    return;
  rxBuf[rxHead] = v;
  rxHead = (rxHead + 1) % RX_BUF_SIZE;
  rxCount++;
}

bool rxPop(uint8_t &v) {
  if (!rxCount)
    return false;
  v = rxBuf[rxTail];
  rxTail = (rxTail + 1) % RX_BUF_SIZE;
  rxCount--;
  return true;
}

void audioPlaybackTick() {
  uint32_t now = micros();
  while ((int32_t)(now - nextPlayUs) >= 0) {
    nextPlayUs += SAMPLE_PERIOD_US;
    uint8_t s = 128; // silence midpoint
    rxPop(s);
    ledcWrite(0, s);
    now = micros();
  }
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

  for (uint8_t i = 0; i < AUDIO_PAYLOAD; i++) {
    while ((int32_t)(micros() - nextCaptureUs) < 0) {
    }
    nextCaptureUs += SAMPLE_PERIOD_US;

    uint16_t adc = analogRead(PIN_MIC); // 0..4095
    p.pcm[i] = (uint8_t)(adc >> 4);     // 8-bit PCM
  }

  radio.write(&p, sizeof(p)); // без ACK для минимальной задержки
}

void rxAudioPackets() {
  while (radio.available()) {
    AudioPacket p{};
    radio.read(&p, sizeof(p));
    for (uint8_t i = 0; i < AUDIO_PAYLOAD; i++)
      rxPush(p.pcm[i]);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_PTT, INPUT_PULLUP);
  pinMode(PIN_MIC, INPUT);

  analogReadResolution(12);

  // PWM audio out: 8-bit @ 160kHz carrier
  ledcSetup(0, 160000, 8);
  ledcAttachPin(PIN_SPK, 0);
  ledcWrite(0, 128);

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
  radio.setDataRate(RF24_1MBPS);
  radio.setPALevel(RF24_PA_MAX);

  radio.openReadingPipe(1, ADDR[NODE_ID]);
  radio.openWritingPipe(ADDR[1 - NODE_ID]);
  radio.startListening();

  nextPlayUs = micros();
}

void loop() {
  audioPlaybackTick();

  if (pttPressed()) {
    if (!txMode)
      enterTx();
    txAudioChunk();
  } else {
    if (txMode)
      enterRx();
    rxAudioPackets();
  }
}
