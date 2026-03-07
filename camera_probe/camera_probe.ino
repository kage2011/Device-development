#include "esp_camera.h"
#include "base64.h"

// XIAO ESP32S3 Sense camera pins
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39
#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

void captureAndPrint() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("capture failed");
    return;
  }

  Serial.printf("frame %ux%u len=%u\n", fb->width, fb->height, fb->len);
  String b64 = base64::encode(fb->buf, fb->len);
  Serial.println("BEGIN_JPEG_BASE64");
  for (size_t i = 0; i < b64.length(); i += 128) {
    Serial.println(b64.substring(i, i + 128));
  }
  Serial.println("END_JPEG_BASE64");

  esp_camera_fb_return(fb);
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("camera_probe: XIAO ESP32S3 Sense");

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("esp_camera_init failed: 0x%x\n", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    Serial.println("sensor get failed");
    return;
  }

  // Color correction tuning (reduce green cast under indoor lighting)
  s->set_whitebal(s, 1);      // enable AWB
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 3);       // 0:auto, 1:sunny, 2:cloudy, 3:office, 4:home
  s->set_exposure_ctrl(s, 1);
  s->set_gain_ctrl(s, 1);
  s->set_brightness(s, 0);
  s->set_contrast(s, 1);
  s->set_saturation(s, 0);

  Serial.printf("camera ok, PID=0x%02X\n", s->id.PID);
  Serial.println("tune: WB=office, QVGA, jpgQ=12");
  Serial.println("ready: type 'shot' then Enter");
}

void loop() {
  static String cmd = "";
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      cmd.trim();
      if (cmd.equalsIgnoreCase("shot")) {
        Serial.println("ACK shot");
        captureAndPrint();
      } else if (cmd.length() > 0) {
        Serial.print("unknown cmd: ");
        Serial.println(cmd);
      }
      cmd = "";
    } else {
      cmd += c;
    }
  }
  delay(5);
}
