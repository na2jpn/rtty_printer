/*
  PS RTTY PRINTER
  Target: Seeed Studio XIAO ESP32S3
  FW: 1.00
  Build: 2026-10-03

  Wiring
  -------
  D0  : AUDIO ADC input
  D2  : FEED switch -> GND
  D3  : MODE switch -> GND
  D6  : UART TX -> Printer TTL RX (yellow)
  D7  : UART RX <- Printer TTL TX (green, optional)
  D8  : STATUS LED -> 1k -> LED -> GND
  3V3 : Audio bias network
  GND : Common GND
  5V  : External 5V through 1N5819

  Printer TTL: 9600 8N1
  RTTY: 45.45 baud / 170 Hz shift
  MARK: 2125 Hz
  SPACE: 2295 Hz
*/

#include <Arduino.h>
#include <math.h>

static const char *FW_VERSION = "1.01";
static const char *BUILD_DATE = "2026-10-04";

// ----- Pins -----
static const int PIN_AUDIO = D0;
static const int PIN_FEED  = D2;
static const int PIN_MODE  = D3;
static const int PIN_PTX   = D6;
static const int PIN_PRX   = D7;
static const int PIN_LED   = D8;

// ----- Printer -----
HardwareSerial Printer(1);
static const uint32_t PRINTER_BAUD = 9600;

// ----- RTTY -----
static const float SAMPLE_RATE = 8000.0f;
static const uint32_t SAMPLE_INTERVAL_US = 125; // 8 kHz
static const int BLOCK_N = 64;                 // 8 ms
static const float MARK_HZ  = 2125.0f;
static const float SPACE_HZ = 2295.0f;
static const float RTTY_BAUD = 45.45f;
static const uint32_t BIT_US = (uint32_t)(1000000.0f / RTTY_BAUD + 0.5f);

// Detection thresholds. Can be tuned later if needed.
static const float MIN_RMS = 5.0f;
static const float MIN_TONE_TO_ENERGY = 4.0f;
static const float MIN_DOMINANCE = 1.15f;

// ----- Line printing -----
static const size_t PRINT_LINE_MAX = 32;
static const uint32_t LINE_SILENCE_MS = 2000;
char lineBuf[PRINT_LINE_MAX + 1];
size_t lineLen = 0;
uint32_t lastTextMs = 0;

// ----- Mode -----
enum RunMode {
  MODE_AUDIO_NORMAL = 0,
  MODE_AUDIO_REVERSE,
  MODE_USB
};
RunMode runMode = MODE_AUDIO_NORMAL;

// ----- Goertzel state -----
float coeffMark;
float coeffSpace;
float s1Mark = 0, s2Mark = 0;
float s1Space = 0, s2Space = 0;
float blockEnergy = 0;
int blockCount = 0;
float dcEstimate = 2048.0f;

enum Tone {
  TONE_INVALID = -1,
  TONE_SPACE = 0,
  TONE_MARK = 1
};

Tone currentTone = TONE_INVALID;
Tone previousTone = TONE_INVALID;
uint32_t lastStrongToneUs = 0;
uint32_t lastCarrierMs = 0;

// ----- Baudot decoder -----
bool lettersShift = true;
bool receivingChar = false;
int rxStage = 0;
uint8_t rxCode = 0;
uint32_t nextBitSampleUs = 0;

// ITA2 / Baudot
const char LETTERS[32] = {
  '\0','E','\n','A',' ','S','I','U',
  '\r','D','R','J','N','F','C','K',
  'T','Z','L','W','H','Y','P','Q',
  'O','B','G','\0','M','X','V','\0'
};

const char FIGURES[32] = {
  '\0','3','\n','-',' ','\'','8','7',
  '\r','$','4','\0',',','!',':','(',
  '5','"',')','2','#','6','0','1',
  '9','?','&','\0','.','/',';','\0'
};

// ----- Buttons -----
bool prevFeed = HIGH;
bool prevMode = HIGH;
uint32_t lastFeedChangeMs = 0;
uint32_t lastModeChangeMs = 0;

// ---------- Printer helpers ----------

void printerNewLine() {
  Printer.print("\r\n");
}

void flushLine() {
  if (lineLen == 0) return;
  lineBuf[lineLen] = '\0';
  Printer.print(lineBuf);
  printerNewLine();
  lineLen = 0;
}

void addPrintable(char c) {
  if (c == '\r') return;

  if (c == '\n') {
    flushLine();
    return;
  }

  if (c < 32 || c > 126) return;

  if (lineLen >= PRINT_LINE_MAX) {
    flushLine();
  }

  lineBuf[lineLen++] = c;
  lastTextMs = millis();
}

void printMode() {
  Printer.print("MODE: ");
  switch (runMode) {
    case MODE_AUDIO_NORMAL:
      Printer.println("AUDIO NORMAL");
      break;
    case MODE_AUDIO_REVERSE:
      Printer.println("AUDIO REVERSE");
      break;
    case MODE_USB:
      Printer.println("USB");
      break;
  }
}

void printBanner() {
  // ESC/POS initialize
  Printer.write(0x1B);
  Printer.write('@');
  delay(50);

  Printer.println("PS RTTY PRINTER");
  Printer.print("FW: ");
  Printer.println(FW_VERSION);
  Printer.print("BUILD: ");
  Printer.println(BUILD_DATE);
  Printer.println("MCU: XIAO ESP32S3");
  Printer.println("TTL: 9600 8N1");
  Printer.println("RTTY: 45.45 / 170");
  Printer.println("MARK 2125 / SPACE 2295");
  Printer.println("RX SENS: HIGH / RMS 5.0");
  printMode();
  Printer.println("READY");
  printerNewLine();
}

// ---------- Baudot ----------

void processBaudot(uint8_t code) {
  code &= 0x1F;

  if (code == 31) { // LTRS
    lettersShift = true;
    return;
  }

  if (code == 27) { // FIGS
    lettersShift = false;
    return;
  }

  char c = lettersShift ? LETTERS[code] : FIGURES[code];
  if (c != '\0') {
    addPrintable(c);
  }
}

// ---------- RTTY bit timing ----------

Tone toneForBitSample(uint32_t nowUs) {
  if ((uint32_t)(nowUs - lastStrongToneUs) <= 20000) {
    return currentTone;
  }
  return TONE_INVALID;
}

void startCharacter(uint32_t nowUs) {
  receivingChar = true;
  rxStage = 0;
  rxCode = 0;
  nextBitSampleUs = nowUs + BIT_US / 2;
}

void serviceRTTYBitTiming() {
  if (!receivingChar) return;

  uint32_t nowUs = micros();
  if ((int32_t)(nowUs - nextBitSampleUs) < 0) return;

  Tone t = toneForBitSample(nowUs);

  if (t == TONE_INVALID) {
    receivingChar = false;
    return;
  }

  if (rxStage == 0) {
    if (t != TONE_SPACE) {
      receivingChar = false;
      return;
    }
    rxStage = 1;
    nextBitSampleUs += BIT_US;
    return;
  }

  if (rxStage >= 1 && rxStage <= 5) {
    int bitIndex = rxStage - 1;
    if (t == TONE_MARK) {
      rxCode |= (1U << bitIndex);
    }
    rxStage++;
    nextBitSampleUs += BIT_US;
    return;
  }

  if (rxStage == 6) {
    if (t == TONE_MARK) {
      processBaudot(rxCode);
    }
    receivingChar = false;
  }
}

// ---------- Audio / Goertzel ----------

void resetGoertzel() {
  s1Mark = s2Mark = 0;
  s1Space = s2Space = 0;
  blockEnergy = 0;
  blockCount = 0;
}

float goertzelPower(float s1, float s2, float coeff) {
  return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

void finishAudioBlock(uint32_t nowUs) {
  float pMark = goertzelPower(s1Mark, s2Mark, coeffMark);
  float pSpace = goertzelPower(s1Space, s2Space, coeffSpace);
  float rms = sqrtf(blockEnergy / (float)BLOCK_N);

  float hi = (pMark > pSpace) ? pMark : pSpace;
  float lo = (pMark > pSpace) ? pSpace : pMark;
  if (lo < 1.0f) lo = 1.0f;

  bool strongEnough =
      (rms >= MIN_RMS) &&
      (blockEnergy > 1.0f) &&
      ((hi / blockEnergy) >= MIN_TONE_TO_ENERGY) &&
      ((hi / lo) >= MIN_DOMINANCE);

  previousTone = currentTone;

  if (!strongEnough) {
    currentTone = TONE_INVALID;
  } else {
    Tone physicalTone = (pMark > pSpace) ? TONE_MARK : TONE_SPACE;

    if (runMode == MODE_AUDIO_REVERSE) {
      currentTone = (physicalTone == TONE_MARK) ? TONE_SPACE : TONE_MARK;
    } else {
      currentTone = physicalTone;
    }

    lastStrongToneUs = nowUs;
    lastCarrierMs = millis();
  }

  if (!receivingChar &&
      runMode != MODE_USB &&
      previousTone == TONE_MARK &&
      currentTone == TONE_SPACE) {
    startCharacter(nowUs);
  }

  resetGoertzel();
}

void addAudioSample(int raw, uint32_t nowUs) {
  dcEstimate += 0.0008f * ((float)raw - dcEstimate);
  float x = (float)raw - dcEstimate;

  blockEnergy += x * x;

  float s0m = x + coeffMark * s1Mark - s2Mark;
  s2Mark = s1Mark;
  s1Mark = s0m;

  float s0s = x + coeffSpace * s1Space - s2Space;
  s2Space = s1Space;
  s1Space = s0s;

  blockCount++;
  if (blockCount >= BLOCK_N) {
    finishAudioBlock(nowUs);
  }
}

void serviceAudioSampling() {
  static uint32_t nextSampleUs = 0;
  if (nextSampleUs == 0) nextSampleUs = micros();

  uint32_t nowUs = micros();

  while ((int32_t)(nowUs - nextSampleUs) >= 0) {
    if (runMode != MODE_USB) {
      int raw = analogRead(PIN_AUDIO);
      addAudioSample(raw, nextSampleUs);
    }

    nextSampleUs += SAMPLE_INTERVAL_US;

    if ((int32_t)(nowUs - nextSampleUs) > 2000) {
      nextSampleUs = nowUs + SAMPLE_INTERVAL_US;
      resetGoertzel();
      break;
    }

    nowUs = micros();
  }
}

// ---------- USB passthrough ----------

void serviceUSBMode() {
  if (runMode != MODE_USB) return;

  while (Serial.available()) {
    int c = Serial.read();
    if (c >= 0) {
      Printer.write((uint8_t)c);
      digitalWrite(PIN_LED, HIGH);
      lastCarrierMs = millis();
    }
  }
}

// ---------- Buttons ----------

void changeMode() {
  flushLine();

  if (runMode == MODE_AUDIO_NORMAL) {
    runMode = MODE_AUDIO_REVERSE;
  } else if (runMode == MODE_AUDIO_REVERSE) {
    runMode = MODE_USB;
  } else {
    runMode = MODE_AUDIO_NORMAL;
  }

  receivingChar = false;
  currentTone = TONE_INVALID;
  previousTone = TONE_INVALID;
  lettersShift = true;
  resetGoertzel();

  printerNewLine();
  printMode();
  Printer.println("READY");
}

void serviceButtons() {
  uint32_t now = millis();

  bool feed = digitalRead(PIN_FEED);
  if (feed != prevFeed && (now - lastFeedChangeMs) > 30) {
    lastFeedChangeMs = now;
    prevFeed = feed;
    if (feed == LOW) {
      flushLine();
      printerNewLine();
    }
  }

  bool mode = digitalRead(PIN_MODE);
  if (mode != prevMode && (now - lastModeChangeMs) > 30) {
    lastModeChangeMs = now;
    prevMode = mode;
    if (mode == LOW) {
      changeMode();
    }
  }
}

// ---------- Setup / loop ----------

void setup() {
  pinMode(PIN_FEED, INPUT_PULLUP);
  pinMode(PIN_MODE, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  analogReadResolution(12);
#if defined(ADC_11db)
  analogSetPinAttenuation(PIN_AUDIO, ADC_11db);
#endif

  Serial.begin(115200);
  Printer.begin(PRINTER_BAUD, SERIAL_8N1, PIN_PRX, PIN_PTX);

  coeffMark  = 2.0f * cosf(2.0f * PI * MARK_HZ  / SAMPLE_RATE);
  coeffSpace = 2.0f * cosf(2.0f * PI * SPACE_HZ / SAMPLE_RATE);

  delay(700);
  printBanner();

  resetGoertzel();
  lastTextMs = millis();
}

void loop() {
  serviceButtons();
  serviceUSBMode();

  if (runMode != MODE_USB) {
    serviceAudioSampling();
    serviceRTTYBitTiming();
  }

  if (lineLen > 0 && (millis() - lastTextMs) >= LINE_SILENCE_MS) {
    flushLine();
  }

  bool active = (millis() - lastCarrierMs) < 120;
  digitalWrite(PIN_LED, active ? HIGH : LOW);
}
