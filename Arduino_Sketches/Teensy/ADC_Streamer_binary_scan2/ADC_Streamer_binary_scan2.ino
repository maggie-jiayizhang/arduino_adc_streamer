/*
 * ADC_Streamer_binary_scan2.ino
 * ------------------------------------------------
 * Teensy 4.x sketch that keeps the MG24 serial API and binary block format.
 *
 * Goals:
 *   - Same command names as the MG24 sketch:
 *       channels, ground, repeat, buffer, ref, osr, gain,
 *       run, stop, status, mcu, help
 *   - Same binary block framing as the MG24 sketch:
 *       [0xAA][0x55][countL][countH]
 *       + count * uint16 samples (LE)
 *       + avg_dt_us (uint16 LE)
 *       + block_start_us (uint32 LE)
 *       + block_end_us   (uint32 LE)
 *   - The host can distinguish devices using printMcu().
 *
 * Important Teensy 4.0 notes:
 *   - The ADC hardware uses 3.3V reference only in this implementation.
 *     Therefore "ref 1.2" returns an error.
 *   - "osr 2|4|8" is mapped to ADC hardware averaging 2/4/8.
 *   - "gain 1|2|3|4" is kept for API compatibility and status reporting,
 *     but Teensy 4.0 ADC has no equivalent analog gain setting here.
 *
 * Commands (terminated by '*'):
 *   channels 14,15,16,17,18*
 *   ground 19*
 *   ground true*
 *   ground false*
 *   repeat 20*
 *   buffer 10*
 *   ref 3.3*
 *   ref vdd*
 *   osr 2*
 *   osr 4*
 *   osr 8*
 *   gain 1*
 *   gain 2*
 *   gain 3*
 *   gain 4*
 *   run*
 *   run 100*
 *   stop*
 *   status*
 *   mcu*
 *   help*
 */

#include <Arduino.h>
#include <ADC.h>
#include <ADC_util.h>

// ---------------------------------------------------------------------
// Limits & defaults
// ---------------------------------------------------------------------

const uint8_t  MAX_SEQUENCE_LEN   = 16;
const uint32_t BAUD_RATE          = 460800;
const uint32_t MAX_SAMPLES_BUFFER = 32000;   // 64 kB (uint16_t samples)
const uint16_t MAX_SCAN_ENTRIES   = 65535;   // informational on Teensy
const uint16_t MAX_REPEAT_COUNT   = 100;
const uint16_t ADC_WARMUP_SWEEPS  = 48;

// ---------------------------------------------------------------------
// Command framing constants
// ---------------------------------------------------------------------

static const char     CMD_TERMINATOR = '*';
static const uint16_t MAX_CMD_LENGTH = 512;

// ---------------------------------------------------------------------
// Reference enum kept compatible with MG24 sketch status output
// ---------------------------------------------------------------------

enum analog_references {
  AR_INTERNAL1V2,
  AR_EXTERNAL_1V25,
  AR_VDD,
  AR_08VDD
};

// ---------------------------------------------------------------------
// Teensy ADC instance
// ---------------------------------------------------------------------

ADC adc;

// ---------------------------------------------------------------------
// Configuration state
// ---------------------------------------------------------------------

uint8_t  channelSequence[MAX_SEQUENCE_LEN];
uint8_t  channelCount        = 0;

int      groundPin           = 0;
bool     useGroundBeforeEach = false;

uint16_t repeatCount         = 1;
uint16_t adcBuffer[MAX_SAMPLES_BUFFER];

uint16_t samplesPerSweep     = 0;   // sent to host (no ground samples)
uint16_t scanEntriesPerSweep = 0;   // logical entries including ground
uint16_t sweepsPerBlock      = 1;

analog_references currentRef = AR_VDD;
static uint8_t     g_osr_cmd = 2;
static uint8_t     g_gain_cmd = 1;
static uint8_t     g_adc_averages = 2;

static ADC_CONVERSION_SPEED g_conv_speed = ADC_CONVERSION_SPEED::HIGH_SPEED;
static ADC_SAMPLING_SPEED   g_samp_speed = ADC_SAMPLING_SPEED::VERY_HIGH_SPEED;

static bool g_configDirty = true;

// ---------------------------------------------------------------------
// Run state
// ---------------------------------------------------------------------

bool     isRunning     = false;
bool     timedRun      = false;
uint32_t runStopMillis = 0;

// ---------------------------------------------------------------------
// Serial input buffer
// ---------------------------------------------------------------------

String inputLine;

// ---------------------------------------------------------------------
// Timing measurement: block timing
// ---------------------------------------------------------------------

uint32_t blockStartMicros = 0;

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

String toLowerTrim(const String &s) {
  String t = s;
  t.trim();
  t.toLowerCase();
  return t;
}

void splitCommand(const String &line, String &cmd, String &args) {
  int idx = line.indexOf(' ');
  if (idx < 0) {
    cmd = line;
    args = "";
  } else {
    cmd = line.substring(0, idx);
    args = line.substring(idx + 1);
  }
  cmd.trim();
  args.trim();
}

void doDummyRead() {
  delayMicroseconds(10);
}

bool isValidAnalogPin(int pin) {
  if (pin < 0 || pin > 255) return false;
#if defined(ARDUINO_TEENSY40) || defined(CORE_TEENSY)
  return (digitalPinToAnalogInput(pin) >= 0);
#else
  return true;
#endif
}

// ---------------------------------------------------------------------
// Command acknowledgment helper
// ---------------------------------------------------------------------

void sendCommandAck(bool ok, const String &args) {
  if (ok) {
    if (args.length() > 0) {
      Serial.print(F("#OK "));
      Serial.println(args);
    } else {
      Serial.println(F("#OK"));
    }
  } else {
    if (args.length() > 0) {
      Serial.print(F("#NOT_OK "));
      Serial.println(args);
    } else {
      Serial.println(F("#NOT_OK"));
    }
  }

  Serial.flush();
  delay(5);
}

// ---------------------------------------------------------------------
// Helper: calculate logical scan entries per sweep
// ---------------------------------------------------------------------

uint16_t calcScanEntryCount(uint16_t rep) {
  if (channelCount == 0) return 0;

  uint16_t total = 0;
  int prevChan = -1;

  for (uint8_t i = 0; i < channelCount; ++i) {
    uint8_t chan = channelSequence[i];
    bool isNew = (i == 0) || (chan != prevChan);

    if (useGroundBeforeEach && isNew) {
      total += 1;
    }

    total += rep;
    prevChan = chan;
  }

  return total;
}

// ---------------------------------------------------------------------
// Derived configuration recomputation
// ---------------------------------------------------------------------

bool recomputeDerivedConfig() {
  if (channelCount == 0) {
    samplesPerSweep = 0;
    scanEntriesPerSweep = 0;
    sweepsPerBlock = 1;
    g_configDirty = true;
    return true;
  }

  scanEntriesPerSweep = calcScanEntryCount(repeatCount);
  samplesPerSweep = (uint16_t)channelCount * (uint16_t)repeatCount;

  if (samplesPerSweep == 0) {
    sweepsPerBlock = 1;
  } else {
    uint32_t maxSweepsByBuffer = MAX_SAMPLES_BUFFER / (uint32_t)samplesPerSweep;
    if (maxSweepsByBuffer == 0) maxSweepsByBuffer = 1;
    if (sweepsPerBlock == 0) sweepsPerBlock = 1;
    if (sweepsPerBlock > maxSweepsByBuffer) {
      sweepsPerBlock = (uint16_t)maxSweepsByBuffer;
    }
  }

  g_configDirty = true;
  return true;
}

// ---------------------------------------------------------------------
// Binary block output helpers
// ---------------------------------------------------------------------

const uint8_t SWEEP_MAGIC1 = 0xAA;
const uint8_t SWEEP_MAGIC2 = 0x55;

void sendSweepHeader(uint16_t totalSamples) {
  uint8_t header[4];
  header[0] = SWEEP_MAGIC1;
  header[1] = SWEEP_MAGIC2;
  header[2] = (uint8_t)(totalSamples & 0xFF);
  header[3] = (uint8_t)(totalSamples >> 8);
  Serial.write(header, 4);
}

void sendBlock(uint16_t sampleCount, uint32_t blockStartUs, uint32_t blockEndUs) {
  if (sampleCount == 0) return;

  uint32_t totalSamples = sampleCount;
  if (totalSamples > MAX_SAMPLES_BUFFER) {
    totalSamples = MAX_SAMPLES_BUFFER;
  }

  uint32_t totalTimeUs = blockEndUs - blockStartUs;   // wrap-safe unsigned math
  uint32_t avgSampleDtUs = (totalSamples > 0) ? (totalTimeUs / totalSamples) : 0;

  uint16_t avgSampleDtUs16 = (avgSampleDtUs <= 65535u)
                               ? (uint16_t)avgSampleDtUs
                               : (uint16_t)65535u;

  sendSweepHeader((uint16_t)totalSamples);
  Serial.write((uint8_t*)adcBuffer, (size_t)(totalSamples * sizeof(uint16_t)));

  uint8_t rateBytes[2];
  rateBytes[0] = (uint8_t)(avgSampleDtUs16 & 0xFF);
  rateBytes[1] = (uint8_t)(avgSampleDtUs16 >> 8);
  Serial.write(rateBytes, 2);

  uint8_t tsBytes[8];
  tsBytes[0] = (uint8_t)(blockStartUs & 0xFF);
  tsBytes[1] = (uint8_t)((blockStartUs >> 8) & 0xFF);
  tsBytes[2] = (uint8_t)((blockStartUs >> 16) & 0xFF);
  tsBytes[3] = (uint8_t)((blockStartUs >> 24) & 0xFF);
  tsBytes[4] = (uint8_t)(blockEndUs & 0xFF);
  tsBytes[5] = (uint8_t)((blockEndUs >> 8) & 0xFF);
  tsBytes[6] = (uint8_t)((blockEndUs >> 16) & 0xFF);
  tsBytes[7] = (uint8_t)((blockEndUs >> 24) & 0xFF);
  Serial.write(tsBytes, 8);
}

// ---------------------------------------------------------------------
// Teensy ADC configuration
// ---------------------------------------------------------------------

void applyADCConfig() {
  adc.adc0->setResolution(12);
  adc.adc0->setAveraging(g_adc_averages);
  adc.adc0->setConversionSpeed(g_conv_speed);
  adc.adc0->setSamplingSpeed(g_samp_speed);
  adc.adc0->setReference(ADC_REFERENCE::REF_3V3);
  g_configDirty = false;
}

uint16_t readSingleSample(uint8_t pin) {
  return (uint16_t)adc.adc0->analogRead(pin);
}

// ---------------------------------------------------------------------
// Warm-up sweeps before real capture
// ---------------------------------------------------------------------

void discardWarmupSweeps(uint16_t warmupSweeps) {
  if (channelCount == 0 || samplesPerSweep == 0) return;

  if (g_configDirty) {
    applyADCConfig();
  }

  for (uint16_t s = 0; s < warmupSweeps; ++s) {
    int prevChan = -1;

    for (uint8_t i = 0; i < channelCount; ++i) {
      uint8_t chan = channelSequence[i];
      bool isNew = (i == 0) || (chan != prevChan);

      if (useGroundBeforeEach && isNew) {
        (void)readSingleSample((uint8_t)groundPin);
      }

      for (uint16_t r = 0; r < repeatCount; ++r) {
        (void)readSingleSample(chan);
      }

      prevChan = chan;
    }
  }
}

// ---------------------------------------------------------------------
// Capture one block into adcBuffer
// ---------------------------------------------------------------------

void doOneBlock() {
  if (!isRunning || channelCount == 0 || samplesPerSweep == 0) return;

  if (g_configDirty) {
    applyADCConfig();
  }

  uint32_t totalSamples = (uint32_t)sweepsPerBlock * (uint32_t)samplesPerSweep;
  if (totalSamples > MAX_SAMPLES_BUFFER) {
    totalSamples = MAX_SAMPLES_BUFFER;
  }

  blockStartMicros = micros();
  uint32_t idx = 0;

  for (uint16_t s = 0; s < sweepsPerBlock && idx < totalSamples; ++s) {
    int prevChan = -1;

    for (uint8_t i = 0; i < channelCount && idx < totalSamples; ++i) {
      uint8_t chan = channelSequence[i];
      bool isNew = (i == 0) || (chan != prevChan);

      if (useGroundBeforeEach && isNew) {
        (void)readSingleSample((uint8_t)groundPin);
      }

      for (uint16_t r = 0; r < repeatCount && idx < totalSamples; ++r) {
        adcBuffer[idx++] = readSingleSample(chan);
      }

      prevChan = chan;
    }
  }

  uint32_t blockEndMicros = micros();
  sendBlock((uint16_t)idx, blockStartMicros, blockEndMicros);
}

// ---------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------

bool handleChannels(const String &args) {
  channelCount = 0;
  int len = args.length();
  int i = 0;

  while (i < len && channelCount < MAX_SEQUENCE_LEN) {
    while (i < len && (args[i] == ' ' || args[i] == ',' || args[i] == '\t')) {
      i++;
    }
    if (i >= len) break;

    int start = i;
    while (i < len && args[i] != ' ' && args[i] != ',' && args[i] != '\t') {
      i++;
    }

    String token = args.substring(start, i);
    token.trim();
    if (token.length() == 0) continue;

    int val = token.toInt();
    if (val < 0 || val > 255) {
      Serial.println(F("# ERROR: channel out of range (0-255)"));
      continue;
    }
    if (!isValidAnalogPin(val)) {
      Serial.print(F("# ERROR: channel pin is not a valid Teensy analog pin: "));
      Serial.println(val);
      continue;
    }

    channelSequence[channelCount++] = (uint8_t)val;
  }

  if (channelCount == 0) {
    Serial.println(F("# ERROR: no valid channels parsed."));
    recomputeDerivedConfig();
    return false;
  }

  for (uint8_t k = 0; k < channelCount; ++k) {
    pinMode(channelSequence[k], INPUT);
  }

  return recomputeDerivedConfig();
}

bool handleGround(const String &args) {
  if (args.length() == 0) {
    Serial.println(F("# ERROR: ground requires an argument (pin number or true/false)"));
    return false;
  }

  String a = toLowerTrim(args);

  if (a == "true") {
    if (!isValidAnalogPin(groundPin)) {
      Serial.println(F("# ERROR: current ground pin is not a valid Teensy analog pin."));
      return false;
    }
    useGroundBeforeEach = true;
  } else if (a == "false") {
    useGroundBeforeEach = false;
  } else {
    int pin = a.toInt();
    if (pin < 0 || pin > 255) {
      Serial.println(F("# ERROR: ground pin out of range (0-255)"));
      return false;
    }
    if (!isValidAnalogPin(pin)) {
      Serial.println(F("# ERROR: ground pin is not a valid Teensy analog pin."));
      return false;
    }

    groundPin = pin;
    pinMode(groundPin, INPUT);
    useGroundBeforeEach = true;
  }

  return recomputeDerivedConfig();
}

bool handleRepeat(const String &args) {
  if (args.length() == 0) {
    Serial.println(F("# ERROR: repeat requires a positive integer"));
    return false;
  }

  long val = args.toInt();
  if (val <= 0) val = 1;
  if (val > MAX_REPEAT_COUNT) val = MAX_REPEAT_COUNT;

  repeatCount = (uint16_t)val;
  return recomputeDerivedConfig();
}

bool handleBuffer(const String &args) {
  if (args.length() == 0) {
    Serial.println(F("# ERROR: buffer requires a positive integer (sweeps per block)"));
    return false;
  }

  long val = args.toInt();
  if (val <= 0) val = 1;

  sweepsPerBlock = (uint16_t)val;
  return recomputeDerivedConfig();
}

bool handleRef(const String &args) {
  if (args.length() == 0) {
    Serial.println(F("# ERROR: ref requires a value (1.2, 3.3, vdd)"));
    return false;
  }

  String a = toLowerTrim(args);

  if (a == "1.2" || a == "1v2") {
    Serial.println(F("# ERROR: Teensy 4.0 does not support internal 1.2V ADC reference. Use ref 3.3 or ref vdd."));
    return false;
  } else if (a == "3.3" || a == "vdd") {
    currentRef = AR_VDD;
    g_configDirty = true;
  } else {
    Serial.println(F("# ERROR: only ref 3.3 and ref vdd are supported on Teensy 4.0."));
    return false;
  }

  doDummyRead();
  return true;
}

bool handleOsr(const String &args) {
  if (args.length() == 0) {
    Serial.println(F("# ERROR: osr requires an integer (2,4,8)"));
    return false;
  }

  long o = args.toInt();
  if (o == 2 || o == 4 || o == 8) {
    g_osr_cmd = (uint8_t)o;
    g_adc_averages = (uint8_t)o;
    g_configDirty = true;
    doDummyRead();
    return true;
  }

  Serial.println(F("# ERROR: osr must be 2, 4, or 8"));
  return false;
}

bool handleGain(const String &args) {
  if (args.length() == 0) {
    Serial.println(F("# ERROR: gain requires an integer (1,2,3,4)"));
    return false;
  }

  long g = args.toInt();
  if (g == 1 || g == 2 || g == 3 || g == 4) {
    g_gain_cmd = (uint8_t)g;
    doDummyRead();
    return true;
  }

  Serial.println(F("# ERROR: gain must be 1, 2, or 3, or 4"));
  return false;
}

bool handleRun(const String &args) {
  if (channelCount == 0) {
    Serial.println(F("# ERROR: no channels configured. Use 'channels ...' first."));
    return false;
  }

  if (args.length() == 0) {
    isRunning = true;
    timedRun = false;
  } else {
    long ms = args.toInt();
    if (ms <= 0) {
      isRunning = true;
      timedRun = false;
    } else {
      isRunning = true;
      timedRun = true;
      runStopMillis = millis() + (uint32_t)ms;
    }
  }

  if (!recomputeDerivedConfig()) {
    isRunning = false;
    timedRun = false;
    return false;
  }

  g_configDirty = true;
  discardWarmupSweeps(ADC_WARMUP_SWEEPS);
  return true;
}

void handleStop() {
  isRunning = false;
  timedRun = false;
}

// ---------------------------------------------------------------------
// Status / help
// ---------------------------------------------------------------------

void printMcu() {
  Serial.println(F("# TEENSY40"));
}

void printStatus() {
  Serial.println(F("# -------- STATUS --------"));
  Serial.print(F("# running: "));
  Serial.println(isRunning ? F("true") : F("false"));

  Serial.print(F("# timedRun: "));
  Serial.println(timedRun ? F("true") : F("false"));

  Serial.print(F("# channels (count="));
  Serial.print(channelCount);
  Serial.println(F("):"));
  Serial.print(F("#   "));
  for (uint8_t i = 0; i < channelCount; i++) {
    Serial.print(channelSequence[i]);
    if (i + 1 < channelCount) Serial.print(F(","));
  }
  Serial.println();

  Serial.print(F("# repeatCount: "));
  Serial.println(repeatCount);

  Serial.print(F("# groundPin: "));
  Serial.println(groundPin);

  Serial.print(F("# useGroundBeforeEach: "));
  Serial.println(useGroundBeforeEach ? F("true") : F("false"));

  Serial.print(F("# adcReference: "));
  switch (currentRef) {
    case AR_INTERNAL1V2:   Serial.println(F("INTERNAL1V2")); break;
    case AR_EXTERNAL_1V25: Serial.println(F("EXTERNAL_1V25")); break;
    case AR_VDD:           Serial.println(F("VDD")); break;
    case AR_08VDD:         Serial.println(F("0.8*VDD")); break;
    default:               Serial.println(F("UNKNOWN")); break;
  }

  Serial.print(F("# osr (high-speed): "));
  Serial.println(g_osr_cmd);

  Serial.print(F("# gain: "));
  Serial.print(g_gain_cmd);
  Serial.println(F("x"));

  Serial.print(F("# samplesPerSweep (sent): "));
  Serial.println(samplesPerSweep);

  Serial.print(F("# scanEntriesPerSweep (IADC incl. ground): "));
  Serial.println(scanEntriesPerSweep);

  Serial.print(F("# sweepsPerBlock: "));
  Serial.println(sweepsPerBlock);

  Serial.print(F("# MAX_SAMPLES_BUFFER: "));
  Serial.println(MAX_SAMPLES_BUFFER);

  Serial.print(F("# MAX_SCAN_ENTRIES (hardware limit): "));
  Serial.println(MAX_SCAN_ENTRIES);

  Serial.println(F("# -------------------------"));
}

void printHelp() {
  Serial.println(F("# Commands:"));
  Serial.println(F("#   channels 0,1,1,1,2,2,3,4,5"));
  Serial.println(F("#   ground 2              (set ground pin)"));
  Serial.println(F("#   ground true|false     (insert ground dummy before each new channel)"));
  Serial.println(F("#   repeat 20             (samples per channel per sweep)"));
  Serial.println(F("#   buffer 10             (sweeps per binary block)"));
  Serial.println(F("#   ref 1.2 | 3.3 | vdd   (set ADC reference)"));
  Serial.println(F("#   osr 2|4|8             (set high-speed oversampling)"));
  Serial.println(F("#   gain 1|2|3|4          (set analog gain multiplier)"));
  Serial.println(F("#   run                   (continuous until 'stop')"));
  Serial.println(F("#   run 100               (~100 ms time-limited run)"));
  Serial.println(F("#   stop                  (stop running)"));
  Serial.println(F("#   status                (show configuration)"));
  Serial.println(F("#   mcu                   (print MCU name for GUI detection)"));
  Serial.println(F("#   help                  (this message)"));
  Serial.println(F("# Note: on Teensy 4.0, ref 1.2 returns an error; osr maps to averaging; gain is compatibility-only."));
}

// ---------------------------------------------------------------------
// Command dispatcher
// ---------------------------------------------------------------------

void handleLine(const String &lineRaw) {
  String line = lineRaw;
  line.trim();
  if (line.length() == 0) return;

  String cmd, args;
  splitCommand(line, cmd, args);
  cmd.toLowerCase();

  bool ok = true;

  if      (cmd == "channels") { ok = handleChannels(args); }
  else if (cmd == "ground")   { ok = handleGround(args); }
  else if (cmd == "repeat")   { ok = handleRepeat(args); }
  else if (cmd == "buffer")   { ok = handleBuffer(args); }
  else if (cmd == "ref")      { ok = handleRef(args); }
  else if (cmd == "osr")      { ok = handleOsr(args); }
  else if (cmd == "gain")     { ok = handleGain(args); }
  else if (cmd == "run")      { ok = handleRun(args); }
  else if (cmd == "stop")     { handleStop(); ok = true; }
  else if (cmd == "status")   { printStatus(); ok = true; }
  else if (cmd == "mcu")      { printMcu(); ok = true; }
  else if (cmd == "help")     { printHelp(); ok = true; }
  else {
    Serial.print(F("# ERROR: unknown command '"));
    Serial.print(cmd);
    Serial.println(F("'. Type 'help'."));
    ok = false;
  }

  sendCommandAck(ok, args);
}

// ---------------------------------------------------------------------
// setup() and loop()
// ---------------------------------------------------------------------

void setup() {
  Serial.begin(BAUD_RATE);
  while (!Serial) {
    ;
  }

  (void)recomputeDerivedConfig();
  applyADCConfig();
  g_configDirty = true;
}

void loop() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();

    if (c == '\r' || c == '\n') {
      continue;
    }

    if (c == CMD_TERMINATOR) {
      if (inputLine.length() > 0) {
        handleLine(inputLine);
        inputLine = "";
      }
      continue;
    }

    inputLine += c;

    if (inputLine.length() > MAX_CMD_LENGTH) {
      inputLine = "";
      Serial.println(F("# ERROR: input line too long; cleared."));
    }
  }

  if (isRunning && timedRun) {
    uint32_t now = millis();
    if ((int32_t)(now - runStopMillis) >= 0) {
      isRunning = false;
      timedRun = false;
      return;
    }
  }

  if (isRunning) {
    doOneBlock();
  }
}
