# XIAO ESP32S3 を標準採用する理由と、ESP32C3 を使用する場合の注意

PS RTTY PRINTER R1.2 は、**Seeed Studio XIAO ESP32S3（通常版）を標準MCU**として設計・実機確認しています。

XIAO ESP32C3でも、RTTY 45.45 baud / 170 Hz の受信、GoertzelによるMARK/SPACE検出、ITA2復号、TTLサーマルプリンター出力という処理そのものは十分実現可能です。

ただし、**R1.2基板へESP32C3をそのまま差し替えることは推奨しません。**

## 結論

- **標準・動作確認済み：XIAO ESP32S3**
- **ESP32C3：改造・実験用途。動作保証対象外**
- R1.2基板をそのまま使用する場合は、ESP32S3を使用してください。

S3とC3の価格差は小さく、頒布基板で再現性やサポート性を優先する場合は、S3を選ぶメリットの方が大きいと判断しています。

## なぜ XIAO ESP32S3 なのか

### 1. R1.2基板・FirmwareをS3で設計・実機確認している

現在の標準ピン割り当ては次の通りです。

| 機能 | XIAO ESP32S3 |
|---|---|
| AUDIO ADC | D0 |
| FEED | D2 |
| MODE | D3 |
| Printer TX | D6 |
| Printer RX | D7 |
| RX STATUS LED | D8 |
| 電源 | 5V / 3V3 / GND |

USBによるFirmware書き込み、USBシリアル、TTLプリンター出力、オーディオADC入力もS3実機で確認しています。

### 2. S3のD0は素直なADC入力として使用できる

XIAO ESP32S3では D0 = GPIO1 で、ADC入力として使用できます。

R1.2ではD0を約1.65Vへバイアスして、交流音声をADCへ入力します。

```text
                 3V3
                  |
                 47k
                  |
AUDIO--1uF--1k---+---- D0
                  |
                 47k
                  |
                 GND
```

### 3. ESP32C3ではD0がストラッピングピン

XIAO ESP32C3では D0 = GPIO2 / ADC1_CH2 ですが、GPIO2はESP32-C3のストラッピングピンです。

ESP32-C3ではGPIO2 / GPIO8 / GPIO9が起動条件に関係するため、外付け回路による電位の影響で、書き込みや起動へ影響する可能性があります。

PS RTTY PRINTER R1.2ではD0へ47kΩ / 47kΩのバイアス回路が常時接続されるため、C3でD0をそのままAUDIO ADCとして使う構成は標準採用しません。

### 4. D8もC3ではストラッピングピン

R1.2ではD8をRX STATUS LEDとして使用しています。

XIAO ESP32C3では D8 = GPIO8 で、GPIO8もストラッピングピンです。

そのため、R1.2の

```text
D8 -- 1k -- LED -- GND
```

という回路も、C3へそのまま置き換える前提にはしない方が安全です。

C3版を作る場合はSTATUS LEDをD4またはD5など、ストラッピングピンではないGPIOへ移すことを推奨します。

### 5. S3には処理能力とメモリの余裕がある

XIAO ESP32S3は、

- 最大240MHz
- デュアルコア
- 8MB PSRAM
- 8MB Flash
- 9系統のADC

を持ちます。

XIAO ESP32C3は、

- 最大160MHz
- シングルコア
- 400KB SRAM
- 4MB Flash
- 4系統のADC

です。

現在のRTTY復調だけならC3でも十分な性能があります。

ただし今後、

- 受信判定の高度化
- AGC / 自動レベル判定
- OLED表示
- スペクトラム / 同調表示
- Wi-Fi設定
- ロギング
- 他モードへの拡張

などを追加する場合、S3の余裕が有利です。

## どうしても XIAO ESP32C3 を使う場合

C3でも実験は可能ですが、**R1.2との完全なピン互換として扱わないでください。**

推奨変更は次の通りです。

| 機能 | R1.2 / S3 | C3での推奨 |
|---|---:|---:|
| AUDIO ADC | D0 | **D1** |
| FEED | D2 | D2 |
| MODE | D3 | D3 |
| Printer TX | D6 | D6 |
| Printer RX | D7 | D7 |
| RX STATUS LED | D8 | **D4 または D5** |
| 5V | 5V | 5V |
| 3V3 | 3V3 | 3V3 |
| GND | GND | GND |

### AUDIO入力は D1 へ変更

XIAO ESP32C3では D1 = GPIO3 / ADC1_CH3 です。

C3版では、AUDIO ADCをD0ではなくD1へ移すことを推奨します。

```text
                 3V3
                  |
                 47k
                  |
AUDIO--1uF--1k---+---- D1
                  |
                 47k
                  |
                 GND
```

Firmware側も、

```cpp
static const int PIN_AUDIO = D0;
```

を、

```cpp
static const int PIN_AUDIO = D1;
```

へ変更します。

### STATUS LEDはD8から移動

C3ではD8がGPIO8のストラッピングピンなので、STATUS LEDは別GPIOへ移します。

例えば、

```cpp
static const int PIN_LED = D4;
```

として、

```text
D4 -- 1k -- LED -- GND
```

とします。

D4を他用途で使用する場合はD5など別の安全なGPIOへ変更してください。

### Printer TX / RX

XIAO ESP32C3でもD6 / D7はUARTのTX / RXとして使用できます。

```text
XIAO D6 / TX  ---> Printer RX
XIAO D7 / RX  <--- Printer TX
GND            --- Printer GND
```

Firmwareでは使用するボード定義に合わせてピンを明示してください。

### FirmwareはC3用に再ビルドする

S3用に生成したバイナリをC3へ書き込むことはできません。

Arduino IDE等で、

- BoardをXIAO ESP32C3へ変更
- AUDIO入力ピンをD1へ変更
- STATUS LEDピンを変更
- ADC入力感度を再確認
- USBシリアル動作を再確認

したうえで、C3用として再コンパイルしてください。

S3とC3ではADC特性も完全に同一ではないため、現在のRTTY検出閾値がそのまま最適とは限りません。

## R1.2基板をC3で使用する場合

R1.2はS3用です。

そのままC3を載せる場合、少なくとも、

1. D0へ行くAUDIO配線を切り離す
2. AUDIO ADCをD1へジャンパーする
3. D8 LED回路を切り離す
4. LEDをD4またはD5等へジャンパーする
5. C3用Firmwareをビルドする
6. 起動・USB書き込み・ADCレベル・RTTY復調を改めて確認する

必要があります。

そのため、**基板を頒布して通常使用する場合はESP32S3を推奨します。**


## 推奨方針

通常製作・基板頒布では、

> **Seeed Studio XIAO ESP32S3（通常版）を使用してください。**

とします。

ESP32C3は「性能不足だから使用不可」なのではありません。

**R1.2の回路・ピン配置・FirmwareがESP32S3を基準に設計・確認されており、C3ではD0とD8がストラッピングピンになるため、そのまま差し替えるのが適切ではない**というのが主な理由です。

## References

- Seeed Studio XIAO ESP32S3 Getting Started  
  https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/

- Seeed Studio XIAO ESP32S3 Pin Multiplexing  
  https://wiki.seeedstudio.com/xiao_esp32s3_pin_multiplexing/

- Seeed Studio XIAO ESP32C3 Getting Started  
  https://wiki.seeedstudio.com/XIAO_ESP32C3_Getting_Started/
