#include <Arduino.h>
#include <Wire.h>
#include <FastLED.h>
#include <driver/i2s.h>

// Pin Connections
// IO7: LED
// IO5: SwitchA
// IO6: SwitchB
// IO2: SDA
// IO3: SCL
// IO4: ES_LRCK
// IO21: ES_SCLK
// IO20: ES_DOUT
// IO10: Vib2
// IO8: Vib3
// IO0: Vib1

// I2C deivces:
// - SC7A20HTR (Accelerometer)
// - ES8311 (Audio Codec)

// ---- I2C pins / addresses ----
static const int PIN_SDA = 2;
static const int PIN_SCL = 3;
static const uint8_t SC7A20_ADDR = 0x18;
static const uint8_t ES8311_ADDR = 0x30;

// ---- SC7A20HTR (accelerometer) registers ----
static const uint8_t SC7A20_REG_WHO_AM_I = 0x0F;
static const uint8_t SC7A20_WHO_AM_I_VALUE = 0x11;
static const uint8_t SC7A20_REG_CTRL_REG1 = 0x20;
static const uint8_t SC7A20_REG_OUT_X_L = 0x28;

// ---- WS2812B (FastLED) ----
static const int PIN_LED = 7;
static const int NUM_LEDS = 1;
static CRGB leds[NUM_LEDS];

// ---- Switches ----
static const int PIN_SWITCH_A = 5;
static const int PIN_SWITCH_B = 6;

// ---- Vibration motors (PWM via LEDC) ----
static const int PIN_VIB1 = 0;
static const int PIN_VIB2 = 10;
static const int PIN_VIB3 = 8;
static const int VIB_PWM_FREQ_HZ = 5000;
static const int VIB_PWM_RESOLUTION_BITS = 8; // duty range 0-255
static const uint8_t VIB1_LEDC_CHANNEL = 0;
static const uint8_t VIB2_LEDC_CHANNEL = 1;
static const uint8_t VIB3_LEDC_CHANNEL = 2;

// ---- ES8311 (audio codec) I2S pins / format ----
static const int PIN_ES_LRCK = 4;
static const int PIN_ES_SCLK = 21;
static const int PIN_ES_DOUT = 20; // codec ADC output -> ESP32 I2S data in
static const i2s_port_t ES_I2S_PORT = I2S_NUM_0;
static const uint32_t ES_SAMPLE_RATE_HZ = 16000;

// put function declarations here:
static void i2cWriteReg8(uint8_t devAddr, uint8_t reg, uint8_t value);
static bool i2cReadRegs(uint8_t devAddr, uint8_t reg, uint8_t *buf, size_t length);

bool initAccelerometer();
bool readAccelerometer(int16_t &x, int16_t &y, int16_t &z);

void initLed();
void setLedColor(uint8_t r, uint8_t g, uint8_t b);

bool initAudioCodec();
size_t readMicSamples(int16_t *buffer, size_t maxSamples);

void readSwitches(bool &swA, bool &swB);

void initVibrationMotors();
void setVibration(uint8_t vib1, uint8_t vib2, uint8_t vib3);

void setup() {
  Serial.begin(115200);
  Wire.begin(PIN_SDA, PIN_SCL);

  initAccelerometer();
  initLed();
  initAudioCodec();
  pinMode(PIN_SWITCH_A, INPUT_PULLUP);
  pinMode(PIN_SWITCH_B, INPUT_PULLUP);
  initVibrationMotors();
}

void loop() {
  // Each driver function above is intended to be called on demand from
  // application logic; nothing runs automatically here.
}

// put function definitions here:

// ---- Shared I2C helpers ----
static void i2cWriteReg8(uint8_t devAddr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(devAddr);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

static bool i2cReadRegs(uint8_t devAddr, uint8_t reg, uint8_t *buf, size_t length) {
  Wire.beginTransmission(devAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  size_t received = Wire.requestFrom((int)devAddr, (int)length);
  if (received != length) {
    return false;
  }
  for (size_t i = 0; i < length; i++) {
    buf[i] = Wire.read();
  }
  return true;
}

// ---- SC7A20HTR accelerometer ----
bool initAccelerometer() {
  uint8_t whoAmI = 0;
  if (!i2cReadRegs(SC7A20_ADDR, SC7A20_REG_WHO_AM_I, &whoAmI, 1) || whoAmI != SC7A20_WHO_AM_I_VALUE) {
    return false;
  }
  // Normal mode, 10Hz output data rate, X/Y/Z axes enabled.
  i2cWriteReg8(SC7A20_ADDR, SC7A20_REG_CTRL_REG1, 0x27);
  return true;
}

bool readAccelerometer(int16_t &x, int16_t &y, int16_t &z) {
  uint8_t buf[6];
  // MSB of the register address enables auto-increment across OUT_X_L..OUT_Z_H.
  if (!i2cReadRegs(SC7A20_ADDR, SC7A20_REG_OUT_X_L | 0x80, buf, sizeof(buf))) {
    return false;
  }
  // Data is 12-bit, left-justified in a 16-bit little-endian pair.
  x = (int16_t)((buf[1] << 8) | buf[0]) >> 4;
  y = (int16_t)((buf[3] << 8) | buf[2]) >> 4;
  z = (int16_t)((buf[5] << 8) | buf[4]) >> 4;
  return true;
}

// ---- WS2812B RGB LED ----
void initLed() {
  FastLED.addLeds<WS2812B, PIN_LED, GRB>(leds, NUM_LEDS);
  FastLED.clear();
  FastLED.show();
}

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
  leds[0] = CRGB(r, g, b);
  FastLED.show();
}

// ---- ES8311 audio codec (microphone input) ----
bool initAudioCodec() {
  // Register init sequence adapted from Espressif esp-adf's es8311_codec_init/
  // es8311_start, trimmed to an ADC(mic)-only path since this board has no
  // MCLK pin and no DAC/DIN wiring. Internal MCLK is derived from SCLK (BCLK).
  i2cWriteReg8(ES8311_ADDR, 0x44, 0x08); // GPIO_REG44: I2C noise immunity
  i2cWriteReg8(ES8311_ADDR, 0x44, 0x08); // written twice per reference driver

  i2cWriteReg8(ES8311_ADDR, 0x01, 0x30); // CLK_MANAGER_REG01
  i2cWriteReg8(ES8311_ADDR, 0x02, 0x10); // CLK_MANAGER_REG02: MCLK = 256 * Fs derived from BCLK
  i2cWriteReg8(ES8311_ADDR, 0x03, 0x10); // CLK_MANAGER_REG03: adc osr
  i2cWriteReg8(ES8311_ADDR, 0x16, 0x24); // ADC_REG16
  i2cWriteReg8(ES8311_ADDR, 0x04, 0x10); // CLK_MANAGER_REG04: dac osr (unused but reset value)
  i2cWriteReg8(ES8311_ADDR, 0x05, 0x00); // CLK_MANAGER_REG05: adc/dac clock divider
  i2cWriteReg8(ES8311_ADDR, 0x0B, 0x00); // SYSTEM_REG0B
  i2cWriteReg8(ES8311_ADDR, 0x0C, 0x00); // SYSTEM_REG0C
  i2cWriteReg8(ES8311_ADDR, 0x10, 0x1F); // SYSTEM_REG10
  i2cWriteReg8(ES8311_ADDR, 0x11, 0x7F); // SYSTEM_REG11
  i2cWriteReg8(ES8311_ADDR, 0x00, 0x80); // RESET_REG00: reset

  // ESP32 drives BCLK/WS as I2S master, so ES8311 operates as slave.
  uint8_t reset_reg = 0;
  i2cReadRegs(ES8311_ADDR, 0x00, &reset_reg, 1);
  reset_reg &= 0xBF; // slave mode
  i2cWriteReg8(ES8311_ADDR, 0x00, reset_reg);

  i2cWriteReg8(ES8311_ADDR, 0x01, 0x3F);
  // Select internal MCLK source = SCLK (BCLK) pin, since MCLK is not wired.
  uint8_t clk01 = 0;
  i2cReadRegs(ES8311_ADDR, 0x01, &clk01, 1);
  clk01 |= 0x80;
  i2cWriteReg8(ES8311_ADDR, 0x01, clk01);

  i2cWriteReg8(ES8311_ADDR, 0x13, 0x10); // SYSTEM_REG13
  i2cWriteReg8(ES8311_ADDR, 0x1B, 0x0A); // ADC_REG1B
  i2cWriteReg8(ES8311_ADDR, 0x1C, 0x6A); // ADC_REG1C

  // Enable ADC (mic) path only; DAC path stays powered down (SDPIN bit6=1).
  i2cWriteReg8(ES8311_ADDR, 0x09, 0x40); // SDPIN_REG09: DAC interface disabled
  i2cWriteReg8(ES8311_ADDR, 0x0A, 0x00); // SDPOUT_REG0A: ADC interface enabled

  i2cWriteReg8(ES8311_ADDR, 0x17, 0xBF); // ADC_REG17: adc volume
  i2cWriteReg8(ES8311_ADDR, 0x0E, 0x02); // SYSTEM_REG0E
  i2cWriteReg8(ES8311_ADDR, 0x12, 0x00); // SYSTEM_REG12
  i2cWriteReg8(ES8311_ADDR, 0x14, 0x1A); // SYSTEM_REG14: analog mic pga gain
  i2cWriteReg8(ES8311_ADDR, 0x0D, 0x01); // SYSTEM_REG0D: power up analog
  i2cWriteReg8(ES8311_ADDR, 0x15, 0x40); // ADC_REG15: adc ramp rate
  i2cWriteReg8(ES8311_ADDR, 0x37, 0x08); // DAC_REG37
  i2cWriteReg8(ES8311_ADDR, 0x45, 0x00); // GP_REG45
  i2cWriteReg8(ES8311_ADDR, 0x44, 0x58); // GPIO_REG44: internal reference signal

  i2s_config_t i2sConfig = {};
  i2sConfig.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  i2sConfig.sample_rate = ES_SAMPLE_RATE_HZ;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2sConfig.dma_buf_count = 4;
  i2sConfig.dma_buf_len = 256;
  i2sConfig.use_apll = false;

  if (i2s_driver_install(ES_I2S_PORT, &i2sConfig, 0, nullptr) != ESP_OK) {
    return false;
  }

  i2s_pin_config_t pinConfig = {};
  pinConfig.bck_io_num = PIN_ES_SCLK;
  pinConfig.ws_io_num = PIN_ES_LRCK;
  pinConfig.data_out_num = I2S_PIN_NO_CHANGE; // no DAC/speaker path wired
  pinConfig.data_in_num = PIN_ES_DOUT;

  return i2s_set_pin(ES_I2S_PORT, &pinConfig) == ESP_OK;
}

size_t readMicSamples(int16_t *buffer, size_t maxSamples) {
  size_t bytesRead = 0;
  i2s_read(ES_I2S_PORT, buffer, maxSamples * sizeof(int16_t), &bytesRead, portMAX_DELAY);
  return bytesRead / sizeof(int16_t);
}

// ---- Switches ----
void readSwitches(bool &swA, bool &swB) {
  // INPUT_PULLUP: pressed = LOW, so invert to report true when pressed.
  swA = digitalRead(PIN_SWITCH_A) == LOW;
  swB = digitalRead(PIN_SWITCH_B) == LOW;
}

// ---- Vibration motors ----
void initVibrationMotors() {
  ledcSetup(VIB1_LEDC_CHANNEL, VIB_PWM_FREQ_HZ, VIB_PWM_RESOLUTION_BITS);
  ledcSetup(VIB2_LEDC_CHANNEL, VIB_PWM_FREQ_HZ, VIB_PWM_RESOLUTION_BITS);
  ledcSetup(VIB3_LEDC_CHANNEL, VIB_PWM_FREQ_HZ, VIB_PWM_RESOLUTION_BITS);
  ledcAttachPin(PIN_VIB1, VIB1_LEDC_CHANNEL);
  ledcAttachPin(PIN_VIB2, VIB2_LEDC_CHANNEL);
  ledcAttachPin(PIN_VIB3, VIB3_LEDC_CHANNEL);
}

void setVibration(uint8_t vib1, uint8_t vib2, uint8_t vib3) {
  ledcWrite(VIB1_LEDC_CHANNEL, vib1);
  ledcWrite(VIB2_LEDC_CHANNEL, vib2);
  ledcWrite(VIB3_LEDC_CHANNEL, vib3);
}
