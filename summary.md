# ESP32-C3 周辺機器ドライバ実装 summary

## ピン配置
- IO7: LED (WS2812B)
- IO5: SwitchA
- IO6: SwitchB
- IO2: SDA / IO3: SCL (I2C, SC7A20HTRとES8311で共有)
- IO4: ES_LRCK / IO21: ES_SCLK / IO20: ES_DOUT (ES8311, MCLK・DIN未配線)
- IO0: Vib1 / IO10: Vib2 / IO8: Vib3 (PWM)

## 実装した関数 ([src/main.cpp](src/main.cpp))

- **`initAccelerometer()` / `readAccelerometer(x, y, z)`**
  SC7A20HTR（I2Cアドレス0x18、LIS3DH互換レジスタマップ）。WHO_AM_I(0x0F)=0x11を確認後、CTRL_REG1(0x20)=0x27で通常モード/全軸ON/10Hzに設定。読み取りはOUT_X_L(0x28)にMSB(0x80)を立ててauto-increment、6byte一括読み出し→12bit左詰め2の補数に変換。

- **`initLed()` / `setLedColor(r, g, b)`**
  FastLEDライブラリでIO7のWS2812B(1個, NUM_LEDS=1)を制御。`platformio.ini`に`lib_deps = fastled/FastLED`を追加。

- **`initAudioCodec()` / `readMicSamples(buffer, maxSamples)`**
  ES8311(I2Cアドレス0x30)。I2Cではレジスタ設定のみ可能で、音声サンプル自体はI2S経由でのみ取得可能。基板にMCLKピンが無いため内部MCLKをSCLK(BCLK)から生成するモードを使用し、マイク(ADC)専用構成（DAC/再生経路は無効のまま）でEspressif公式esp-adfの`es8311_codec_init`/`es8311_start`を簡略移植。I2Sはレガシー`driver/i2s.h`（`i2s_driver_install`/`i2s_set_pin`/`i2s_read`）を使用、16kHz/16bit/monoがデフォルト。

- **`readSwitches(swA, swB)`**
  IO5/IO6を`INPUT_PULLUP`で読み取り。押下時にLOWになるためtrueに反転して返す。

- **`initVibrationMotors()` / `setVibration(vib1, vib2, vib3)`**
  LEDC(PWM)でIO0/IO10/IO8を駆動。5kHz/8bit解像度(duty 0-255)。この環境の`framework-arduinoespressif32`が旧チャンネル指定API(`ledcSetup`+`ledcAttachPin`+`ledcWrite(channel,...)`)だったため、それに合わせて実装（新しいpin指定API`ledcAttach(pin,...)`は未対応だったため使用不可）。

## 検証状況
`pio run -e esp32-c3-devkitc-02`でビルド成功を確認済み（RAM 4.5%, Flash 23.1%）。実機での動作確認は未実施。

## 未検証・要調整の可能性がある点
- ES8311のレジスタ初期化シーケンスは実機未検証。電源シーケンスやマイクゲイン(現在REG14=0x1A)などボード固有の調整が必要になる可能性がある。
- サンプルレート/ビット幅/チャンネル数を変える場合は`ES_SAMPLE_RATE_HZ`および`i2sConfig`を変更する。
- スイッチの配線がプルアップ前提（押下でLOW）。プルダウン配線の場合は`readSwitches`のロジック反転が必要。
