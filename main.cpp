// --- TinyGSMデバッグログをSerialへ出力 ---
#define TINY_GSM_DEBUG Serial
#define TINY_GSM_MODEM_SIM7080

#include <Arduino.h>
#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include <Wire.h>
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"
#include <TinyGsmClient.h>

// --- ピン定義 (T-SIM7080G-S3) ---
#define I2C_SDA              15
#define I2C_SCL               7

#define BOARD_MODEM_RXD_PIN   4
#define BOARD_MODEM_TXD_PIN   5
#define BOARD_MODEM_PWR_PIN  41
#define BOARD_MODEM_DTR_PIN  42

#define PWDN_GPIO_NUM        -1
#define RESET_GPIO_NUM       18
#define XCLK_GPIO_NUM        -1 //今回使ったモジュールはXCLK配線不要なので-1

#define SIOD_GPIO_NUM         2
#define SIOC_GPIO_NUM         1

#define Y9_GPIO_NUM           9
#define Y8_GPIO_NUM          10
#define Y7_GPIO_NUM          11
#define Y6_GPIO_NUM          13
#define Y5_GPIO_NUM          21
#define Y4_GPIO_NUM          48
#define Y3_GPIO_NUM          47
#define Y2_GPIO_NUM          14

#define VSYNC_GPIO_NUM       16
#define HREF_GPIO_NUM        17
#define PCLK_GPIO_NUM        12

#define SD_MMC_CLK           38
#define SD_MMC_CMD           39
#define SD_MMC_D0            40

// --- SIMネットワーク設定 (1nce) ---
const char apn[]      = "iot.1nce.net";
const char gprsUser[] = "";
const char gprsPass[] = "";

// --- Tailscale Funnel 接続先設定 ---
const char server[]   = "*****.*****.ts.net";
const int  port       = 443;

// --- 動作間隔の設定 (ms) ---
const unsigned long CAPTURE_INTERVAL_MS = 10000; // 撮影・送信間隔 (10秒)
const unsigned long POLLING_INTERVAL_MS = 10000; // サーバーポーリング間隔 (10秒)

// --- グローバルオブジェクト ---
XPowersPMU PMU;
HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);
TinyGsmClientSecure client(modem);

bool camera_ok = false;
bool sd_ok     = false;
bool pmu_ok    = false;
bool modem_ok  = false;

bool is_streaming_requested = false;
unsigned long last_capture_time = 0;
unsigned long last_status_check_time = 0;

String sendATCmd(String cmd, uint32_t timeout = 3000) {
  Serial.print("[AT Send] ");
  Serial.println(cmd);
  
  while(SerialAT.available()) SerialAT.read();
  SerialAT.println(cmd);
  
  String response = "";
  uint32_t start = millis();
  while (millis() - start < timeout) {
    while (SerialAT.available()) {
      char c = SerialAT.read();
      response += c;
    }
  }
  
  Serial.print("[AT Resp] ");
  Serial.println(response);
  return response;
}

void setOnboardLED(bool enable) {
  if (pmu_ok) {
    PMU.setChargingLedMode(enable ? XPOWERS_CHG_LED_ON : XPOWERS_CHG_LED_OFF);
  }
}

void saveJpgToSD(camera_fb_t *fb) {
  if (!sd_ok || !fb) return;

  String path = "/photo_" + String(millis()) + ".jpg";
  File file = SD_MMC.open(path.c_str(), FILE_WRITE);
  if (file) {
    file.write(fb->buf, fb->len);
    file.close();
    Serial.printf("[SD Saved] %s (%u bytes)\n", path.c_str(), fb->len);
  } else {
    Serial.println("[SD Error] Failed to open file");
  }
}

bool checkServerStatus() {
  if (!modem_ok) return false;

  Serial.println("[SSL Socket] Connecting for GET /status...");
  if (!client.connect(server, port)) {
    Serial.println("[SSL Socket] Connection failed (GET)");
    return false;
  }

  client.print(String("GET /status HTTP/1.1\r\n") +
               "Host: " + server + "\r\n" +
               "User-Agent: ESP32-SIM7080G\r\n" +
               "Connection: close\r\n\r\n");

  unsigned long timeout = millis();
  String response = "";
  while (client.connected() && millis() - timeout < 5000) {
    while (client.available()) {
      char c = client.read();
      response += c;
      timeout = millis();
    }
  }
  client.stop();

  return (response.indexOf("\"stream\":true") != -1 || response.indexOf("\"stream\": true") != -1);
}

bool sendFrameToServer(camera_fb_t *fb) {
  if (!modem_ok || !fb) return false;

  Serial.println("[SSL Socket] Connecting for POST /upload...");
  if (!client.connect(server, port)) {
    Serial.println("[SSL Socket] Connection failed (POST)");
    return false;
  }

  client.print(String("POST /upload HTTP/1.1\r\n") +
               "Host: " + server + "\r\n" +
               "User-Agent: ESP32-SIM7080G\r\n" +
               "Content-Type: image/jpeg\r\n" +
               "Content-Length: " + String(fb->len) + "\r\n" +
               "Connection: close\r\n\r\n");

  Serial.printf("[SSL Socket] Sending JPEG (%u bytes) in 512B chunks...\n", fb->len);
  size_t bytesLeft = fb->len;
  uint8_t *bufPtr = fb->buf;
  
  while (bytesLeft > 0) {
    size_t chunkSize = (bytesLeft > 512) ? 512 : bytesLeft;
    client.write(bufPtr, chunkSize);
    bufPtr += chunkSize;
    bytesLeft -= chunkSize;
    delay(10);
  }
  Serial.println("[SSL Socket] Chunk upload complete.");

  unsigned long timeout = millis();
  String response = "";
  while (client.connected() && millis() - timeout < 8000) {
    while (client.available()) {
      char c = client.read();
      response += c;
      timeout = millis();
    }
  }
  client.stop();

  return (response.indexOf("\"stream\":true") != -1 || response.indexOf("\"stream\": true") != -1);
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0); // PC未接続時のフリーズ防止
  delay(1000);
  Serial.println("\n=== T-SIM7080G-S3 Stream System Starting ===");

  // 1. PMU (電源管理) 初期化 & 段階的電源投入
  Wire.begin(I2C_SDA, I2C_SCL);
  if (PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL)) {
    pmu_ok = true;
    PMU.enableFastPowerOn();

    // BLDO1: ESP32ロジック電源 (3.3V)
    PMU.setBLDO1Voltage(3300);
    PMU.enableBLDO1();
    delay(100);

    // ALDO3: MicroSD電源 (3.3V)
    PMU.setALDO3Voltage(3300);
    PMU.enableALDO3();
    delay(100);

    // BLDO2: アンテナ / GPS電源 (3.3V)
    PMU.setBLDO2Voltage(3300);
    PMU.enableBLDO2();
    delay(100);

    // DC3: SIM7080G モデム主電源 (3.0V)
    PMU.setDC3Voltage(3000);
    PMU.enableDC3();
    delay(500); // 電圧安定待ち

    PMU.disableTSPinMeasure();
    setOnboardLED(false);
    Serial.println("PMU Init OK!");
  }

  // 2. MicroSD 初期化
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
  if (SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_PROBING)) {
    sd_ok = true;
    Serial.println("SD Card Mount Success!");
  }

  // 3. カメラ初期化
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_QVGA;
  config.jpeg_quality = 15;

  if (psramFound()) {
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
    Serial.println("PSRAM Found!");
  } else {
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count     = 1;
  }

  if (esp_camera_init(&config) == ESP_OK) {
    camera_ok = true;
    Serial.println("Camera Init Success!");
  }

  // 4. モデムピン & 強制パワーオンパルス処理
  pinMode(BOARD_MODEM_PWR_PIN, OUTPUT);
  pinMode(BOARD_MODEM_DTR_PIN, OUTPUT);
  digitalWrite(BOARD_MODEM_DTR_PIN, LOW);

  SerialAT.begin(115200, SERIAL_8N1, BOARD_MODEM_RXD_PIN, BOARD_MODEM_TXD_PIN);
  delay(1000);

  // モデムが応答しない場合、1秒間のPWRKEYパルスで物理起動させる
  if (!modem.testAT(1000)) {
    Serial.println("Modem not responding. Pulsing PWRKEY...");
    digitalWrite(BOARD_MODEM_PWR_PIN, HIGH);
    delay(1000);
    digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
    delay(3000); // モデム内部ブートの完了待ち
  }

  // 5. ネットワーク接続
  if (modem.restart()) {
    modem.setNetworkMode(38);  // LTE Only
    modem.setPreferredMode(1); // Cat-M1
    sendATCmd("AT+CBANDCFG=\"CAT-M\",1,3,8,18,19,26");

    Serial.println("Connecting to Cellular Network...");
    if (modem.waitForNetwork(120000L) && modem.gprsConnect(apn, gprsUser, gprsPass)) {
      sendATCmd("AT+CSSLCFG=\"sslversion\",0,3");
      sendATCmd("AT+CSSLCFG=\"ignorecert\",0,1");
      sendATCmd("AT+CSSLCFG=\"sni\",0,\"" + String(server) + "\"");

      modem_ok = true;
      Serial.println("Cellular Network & SNI-TLS Socket Initialized Successfully!");
    } else {
      Serial.println("Cellular Connection Failed!");
    }
  }
}

void loop() {
  unsigned long current_time = millis();

  // 【10秒毎】静止画撮影 ＋ SD保存 ＋ (要求があれば) ストリーム送信
  if (camera_ok && (current_time - last_capture_time >= CAPTURE_INTERVAL_MS)) {
    last_capture_time = current_time;

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      // 1. SDカード保存
      saveJpgToSD(fb);

      // 2. ストリーミング要求があれば送信
      if (is_streaming_requested) {
        setOnboardLED(true);
        Serial.println("[Stream] Uploading frame to server...");
        is_streaming_requested = sendFrameToServer(fb);
        setOnboardLED(false);
      }

      esp_camera_fb_return(fb);
    }
  }

  // 【10秒毎】ストリーミング停止中のみ、視聴リクエストの有無をチェック
  if (!is_streaming_requested && (current_time - last_status_check_time >= POLLING_INTERVAL_MS)) {
    last_status_check_time = current_time;
    Serial.println("[Status Check] Polling server...");
    is_streaming_requested = checkServerStatus();
    if (is_streaming_requested) {
      Serial.println("[Status Check] Active viewer detected! Starting stream.");
    }
  }
}