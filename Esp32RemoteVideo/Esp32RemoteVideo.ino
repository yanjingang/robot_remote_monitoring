/* 
 *  esp32-cam + wsserver 制作4g图传模块
 *    https://blog.yanjingang.com/?p=6598 
 */

#include "esp_camera.h"
#include <WiFi.h>
#include "Arduino.h"
#include <WebSocketsClient.h>   //需安装ws库：https://www.arduinolibraries.info/libraries/web-sockets
//#include <ArduinoMqttClient.h>
//#include "base64.h"


// 相机设置
//#define CAMERA_MODEL_WROVER_KIT // Has PSRAM
//#define CAMERA_MODEL_ESP_EYE // Has PSRAM
//#define CAMERA_MODEL_M5STACK_PSRAM // Has PSRAM
//#define CAMERA_MODEL_M5STACK_V2_PSRAM // M5Camera version B Has PSRAM
//#define CAMERA_MODEL_M5STACK_WIDE // Has PSRAM
//#define CAMERA_MODEL_M5STACK_ESP32CAM // No PSRAM
#define CAMERA_MODEL_AI_THINKER // Has PSRAM
//#define CAMERA_MODEL_TTGO_T_JOURNAL // No PSRAM


// wifi设置
const char* SSID = "Mars.Y"; //MARS-4G
const char* PASSWD = "Lovezhu1314";


// 相机引脚
#include "camera_pins.h"
// 相机server声明
void startCameraServer();


// 其他参数
int LED_CAMERA = 4; // 闪光灯
int LED_PIN = 33; // RST按钮旁板载红色LED


// ws视频流server
const char* socket_url = "yanjingang.com";
int socket_port = 8878; //8079;
WebSocketsClient webSocket;
long lastSendStream = 0;   //末次上报时间
int socketStatus = 0;      //ws连接状态(0未连接，1已连接)
int streamTaskStatus = 0;  //task状态(0未执行，1执行中)

/*
// mqtt参数
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);
const char broker[] = "ahytdaa.iot.gz.baidubce.com";
int port = 1883;
const char userName[] = "thingidp@ahytdaa|cam1|0|MD5";      //{adp_type}@{IoTCoreId}|{DeviceKey}|{timestamp}|{algorithm_type}
const char passWord[] = "c2e73ae81a590671851b0dd6a3ebf4cc"; //md5({device_key}&{timestamp}&{algorithm_type}{device_secret})
const char clientId[] = "esp32-cam1";     // uniqut
const char pubTopic[]  = "cam1/stream";  // 注意：baidu iotcore的自定义topic不能以'/'开头!
*/


void setup() {
  Serial.begin(115200);
  //Serial.setDebugOutput(true);
  Serial.println();

  // 相机参数
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
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_HQVGA; //XGA(1024x768)、SVGA(800x600)、VGA(640x480)、CIF(400x296)、QVGA(320x240)、HQVGA(240x176)、QQVGA(160x120)
  config.jpeg_quality = 10;
  config.fb_count = 1;


  // 相机初始化 camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }
  sensor_t * s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1); // flip it back
    s->set_brightness(s, 1); // up the brightness just a bit
    s->set_saturation(s, -2); // lower the saturation
  }
  
  #if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
  #endif


  // 连接wifi
  WiFi.begin(SSID, PASSWD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected!");


  // 启动相机视频流服务
  /*//局域网wifi 视频流 server停止启动，改为使用下方推送到远端ws服务器
  startCameraServer();
  Serial.print("Camera Ready! http://");
  Serial.print(WiFi.localIP());
  Serial.println("\n");
  */

  
  // 视频流推送ws服务器初始化
  webSocket.begin(socket_url, socket_port);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  webSocket.enableHeartbeat(15000, 3000, 2); 
  

  /*
  // 连接mqtt
  mqttClient.setId(clientId);
  mqttClient.setUsernamePassword(userName, passWord);
  //mqttClient.setTxPayloadSize(512);  //默认只能发256字节，需要调大buf区大小
  while (!mqttClient.connect(broker, port)) {
    Serial.print("MQTT connection failed! Error code = ");
    Serial.println(mqttClient.connectError());
    Serial.print(".");
    delay(500);
  }
  Serial.println("MQTT connected!\n");
  */


  // 红led灯点亮
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
}



void loop() {
  // 推送视频流到WS服务器
  webSocket.loop();
  uint64_t now = millis();  
  if(now - lastSendStream > 100 && socketStatus && streamTaskStatus==0) { //仅在连接ws 且 无其他异步任务执行时运行
      lastSendStream = now;
      /*// 同步推流
      camera_fb_t * fb = NULL;
      // Take Picture with Camera
      fb = esp_camera_fb_get();  
      if(!fb) {
        Serial.println("Camera capture failed");
        return;
      }else{
        webSocket.sendBIN(fb->buf, fb->len);
        Serial.print("Send Image to WsServer: len ");
        Serial.println(fb->len);
        esp_camera_fb_return(fb);
      }*/
      // 异步推流任务
      streamTaskStatus = 1; //任务执行中
      xTaskCreate(
        &sendStream,   // Task function.
        "sendStream", // String with name of task.
        10240,     // Stack size in bytes.
        NULL,      // Parameter passed as input of the task
        1,         // Priority of the task.
        NULL);     // Task handle.
      
  }
}


//视频流推送服务器线程
void sendStream(void *parameter){
  camera_fb_t * fb = NULL;
  // Take Picture with Camera
  fb = esp_camera_fb_get();  
  if(!fb) {
    Serial.println("Camera capture failed");
  }else{
    if(socketStatus){ //ws已连接
      webSocket.sendBIN(fb->buf, fb->len);
      Serial.print("Send Image to WsServer: len ");
      Serial.println(fb->len);
    }
    /*
    String base64Buf = base64::encode(fb->buf, fb->len); 
    mqttClient.beginMessage(pubTopic);
    mqttClient.print(base64Buf);
    int ret = mqttClient.endMessage();
    Serial.print("Send Image to MQTT: ret=");
    Serial.print(ret);
    Serial.print(" len=");
    Serial.println(base64Buf.length());
    //Serial.println(base64Buf);
    */
    
    esp_camera_fb_return(fb);
  }

  streamTaskStatus=0; //当前任务结束
  vTaskDelete(NULL);
}


// 视频流ws server事件
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED: //接收到连接断开消息
        socketStatus = 0;
        Serial.printf("[WS] Disconnected!\n");
        break;
    case WStype_CONNECTED:  //接收到连接成功消息
        socketStatus = 1;
        Serial.printf("[WS] Connected to url: %s\n", payload);
        webSocket.sendTXT("camlogin");
        break;
    case WStype_TEXT: //接收到文本消息
        Serial.printf("[WS] get text: %s\n", payload);
        break;
    case WStype_BIN:  //接收到二进制流消息
        Serial.printf("[WS] get bin\n");
        break;
    case WStype_PING: // 当接收到服务器发送来的PING消息，会自动向服务器发送PONG消息
        Serial.printf("[WS] get ping\n");
        break;
    case WStype_PONG: // 客户端向服务器发送PING消息后，服务器会向客户端发送PONG消息
        Serial.printf("[WS] get pong\n");
        break;
    }
}
