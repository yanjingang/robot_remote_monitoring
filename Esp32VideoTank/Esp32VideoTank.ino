/* 
 *  esp32-cam + l298n + mqtt + ws制作4g图传坦克
 *    https://blog.yanjingang.com/?p=6598 
 */

#include "esp_camera.h"
#include <WiFi.h>
#include "Arduino.h"
#include <ArduinoMqttClient.h>
#include <Arduino_JSON.h>
#include "pwm_control.h"
#include <WebSocketsClient.h>   //需安装ws库：https://www.arduinolibraries.info/libraries/web-sockets


// 相机设置
//#define CAMERA_MODEL_WROVER_KIT // Has PSRAM
//#define CAMERA_MODEL_ESP_EYE // Has PSRAM
//#define CAMERA_MODEL_M5STACK_PSRAM // Has PSRAM
//#define CAMERA_MODEL_M5STACK_V2_PSRAM // M5Camera version B Has PSRAM
//#define CAMERA_MODEL_M5STACK_WIDE // Has PSRAM
//#define CAMERA_MODEL_M5STACK_ESP32CAM // No PSRAM
#define CAMERA_MODEL_AI_THINKER // Has PSRAM
//#define CAMERA_MODEL_TTGO_T_JOURNAL // No PSRAM

// 相机server声明
void startCameraServer();


// wifi设置
const char* ssid = "MARS-Y";
const char* password = "Lovezhu1314";

// 网络时间获取
const char *ntpServer1 = "us.pool.ntp.org"; //网络时间服务器
const char *ntpServer2 = "time2.cloud.tencent.com"; //网络时间服务器
const char *ntpServer3 = "ntp1.aliyun.com"; //网络时间服务器
const long gmtOffset_sec = 8 * 3600;    //时区设置函数，东八区（UTC/GMT+8:00）写成8*3600  
const int daylightOffset_sec = 0;   //夏令时填写3600，否则填0
long START_WORD_TIMESTAMP = 0;  //网络接通时的世界时间戳


// 相机引脚
#include "camera_pins.h"


// 马达参数
// GPIO引脚（IO0用于烧录时接地；IO1/3用于TX/RX串口通信；IO14/15/16端口空代码运行就会高电平，像是被什么占用了，没找到原因前先用剩下的io口）
int DC_LEFT1 = 4; // Left 1
int DC_LEFT2 = 2; // Left 2
int DC_RIGHT1 = 13; // Right 1
int DC_RIGHT2 = 12; // Right 2
// PWM通道（0-15按顺序用即可。注意：0通道0定时器被相机使用了，需要避开）
int PWM_LEFT1 = 15; // Left 1
int PWM_LEFT2 = 14; // Left 2
int PWM_RIGHT1 = 13; // Right 1
int PWM_RIGHT2 = 12; // Right 2
// 其他参数
int CAMERA_LED = 4; // Light灯
int PWM_SPEED_LEFT = 800;  //速度
int PWM_SPEED_RIGHT = 800;  //速度
String motorStatus = "";        //底盘当前状态
String lastMotorStatus = "";   //底盘末次上报状态


// mqtt参数
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);
const char broker[] = "amzvemj.iot.gz.baidubce.com";
int port = 1883;
const char pubTopic[]  = "tank1/data";
const char subTopic[]  = "tank1/cmd";  // 注意：baidu iotcore的自定义topic不能以'/'开头!


// ws视频流server
const char* socket_url = "yanjingang.com";
int socket_port = 8079;
WebSocketsClient webSocket;
long lastSendStream = 0;   //末次上报时间
int socketStatus = 0;     //ws连接状态(0未连接，1已连接)
int taskStatus = 0;       //rask状态(0未执行，1执行中)


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
  /*// if PSRAM IC present, init with UXGA resolution and higher JPEG quality for larger pre-allocated frame buffer.
  if(psramFound()){
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }*/
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
  // drop down frame size for higher initial frame rate
  //s->set_framesize(s, FRAMESIZE_HQVGA);  //XGA(1024x768)、SVGA(800x600)、VGA(640x480)、CIF(400x296)、QVGA(320x240)、HQVGA(240x176)、QQVGA(160x120)

  #if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
  #endif


  // 连接wifi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected!");
  
  // 世界时间同步
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2, ntpServer3);
  if(START_WORD_TIMESTAMP == 0){
    static char res[19] = "";
    START_WORD_TIMESTAMP = getWordTimestamp(res);
    Serial.print("getWordTimestamp: ");
    Serial.println(res);
    Serial.println(START_WORD_TIMESTAMP);
    Serial.println("");
  }
  
  

  // 启动相机视频流服务
  /*//局域网wifi 视频流 server停止启动，改为使用下方推送到远端ws服务器
  startCameraServer();
  Serial.print("Camera Ready! http://");
  Serial.print(WiFi.localIP());
  Serial.println("\n");
  */
  
  // 视频流推送ws服务器初始化
  webSocket.begin(socket_url,socket_port);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  webSocket.enableHeartbeat(15000, 3000, 2); 
  

  
  // 初始化马达引脚
  PWM_Init(PWM_LEFT1, DC_LEFT1);//通道0-15, GPIO
  PWM_Init(PWM_LEFT2, DC_LEFT2);
  PWM_Init(PWM_RIGHT1, DC_RIGHT1);
  PWM_Init(PWM_RIGHT2, DC_RIGHT2);
  //STOP
  runMotor(0, 0); // x/y=0代表横纵向静止不动
  


  // 连接mqtt
  // unique client ID
  mqttClient.setId("esp32-tank1");
  // username and password for authentication
  //    username = {adp_type}@{IoTCoreId}|{DeviceKey}|{timestamp}|{algorithm_type}
  //    password = md5({device_key}&{timestamp}&{algorithm_type}{device_secret})
  mqttClient.setUsernamePassword("thingidp@amzvemj|tank1|0|MD5", "b2c233b4b1d09a42ab4c8293794ec25a");
  //mqttClient.setUsernamePassword("thingidp@amzvemj|tank1|1658244929|MD5", "8809d3fe13eb6f1bd37a5dfec50f0f77");
  while (!mqttClient.connect(broker, port)) {
    Serial.print("MQTT connection failed! Error code = ");
    Serial.println(mqttClient.connectError());
    Serial.print(".");
    delay(500);
  }
  Serial.println("MQTT connected!\n");

  // 订阅topic
  // message receive callback
  mqttClient.onMessage(onReceiveMessage);
  // subscribe to a topic
  mqttClient.subscribe(subTopic);
  // mqttClient.unsubscribe(subTopic);
  Serial.print("Waiting for messages on topic: ");
  Serial.println(subTopic);

}



void loop() {
  // 世界时间同步
  if(START_WORD_TIMESTAMP == 0){
    static char res[19] = "";
    START_WORD_TIMESTAMP = getWordTimestamp(res);
    Serial.println(res);
    Serial.println(START_WORD_TIMESTAMP);
  }
  
  // 接收mqtt指令
  mqttClient.poll();

  
  // 上报底盘状态
  if (motorStatus != lastMotorStatus) {
    lastMotorStatus = motorStatus;

    // data
    JSONVar pubdata;
    pubdata["status"] = motorStatus;
    pubdata["ts"] = START_WORD_TIMESTAMP + millis()/1000;
    String strdata = JSON.stringify(pubdata);
    
    Serial.print("pub msg to topic: ");
    Serial.print(pubTopic);
    Serial.print(" ");
    Serial.println(strdata);

    // send message, the Print interface can be used to set the message contents
    mqttClient.beginMessage(pubTopic);
    mqttClient.print(strdata);  //{"status": "Forward"}
    mqttClient.endMessage();
    Serial.println("pub done!");
  }

  //更新websocket信息
  webSocket.loop();
  uint64_t now = millis();
  if(now - lastSendStream > 100 && socketStatus && taskStatus==0) { //仅在连接ws 且 无其他异步任务执行时运行
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
      taskStatus = 1; //任务执行中
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
    esp_camera_fb_return(fb);
  }
  
  //vTaskDelay(100 / portTICK_RATE_MS);

  taskStatus=0; //当前任务结束
  
  vTaskDelete(NULL);
  
}


// 获取世界时间
time_t getWordTimestamp(char *res){
    //static char res[19];
    
    // get time
    struct tm timeinfo;
    int ms = 5000; //超时时间
    if (!getLocalTime(&timeinfo, ms)){
        Serial.println("Failed to getLocalTime.");
        return 0;
    }
    //Serial.println(&timeinfo, "%F %T %A"); // 格式化输出
    
    // format time
    time_t timep = mktime(&timeinfo);
    //Serial.println(timep);
    
    // format time
    sprintf(res, "%d-%.2d-%.2d %.2d:%.2d:%.2d",
              timeinfo.tm_year + 1900,
              timeinfo.tm_mon + 1,
              timeinfo.tm_mday, 
              timeinfo.tm_hour,
              timeinfo.tm_min, 
              timeinfo.tm_sec);
    //Serial.println(res);
    
    return timep;
}


// 订阅消息的回调
void onReceiveMessage(int messageSize) {
  // print topic and contents
  Serial.print("sub msg: ");
  Serial.print(mqttClient.messageTopic());
  Serial.print(", length ");
  Serial.print(messageSize);
  Serial.print(" bytes, data: ");

  // get msg
  String msg = "";
  while (mqttClient.available()) {
    msg += (char)mqttClient.read();
  }
  Serial.println(msg);
  
  // json decode
  JSONVar cmd = JSON.parse(msg);
  //Serial.println(cmd);
  if (JSON.typeof(cmd) == "undefined") {
    Serial.println("json parsing failed!");
    //return;
  }
  int x = 0;
  int y = 0;
  if (cmd.hasOwnProperty("x")) {
    x = (int)cmd["x"];
  }
  if (cmd.hasOwnProperty("y")) {
    y = (int)cmd["y"];
  }

  // 移动底盘
  runMotor(x, y);
}


// 执行底盘移动指令
int pwmMax = 25;  //xy单方向的最大值
void runMotor(int x, int y) {
  Serial.print("runMotor :");
  Serial.print(x);
  Serial.print(" ");
  Serial.println(y);

  //指令处理
  //  原点{"x":0,"y":0}, 前进y>0,后退y<0；右转x>0,左转x<0
  if (y >= 2 && x >= 2) { //前+右
    motorStatus = "RightForward";
    Serial.println(motorStatus);
    //forward + right 向前右转
    PWM_Control(PWM_LEFT1, PWM_SPEED_LEFT);   //pwm调速
    PWM_Control(PWM_LEFT2, 0);
    PWM_Control(PWM_RIGHT1, PWM_SPEED_RIGHT * (pwmMax - x)/pwmMax);   //pwm调速
    PWM_Control(PWM_RIGHT2, 0);
  }
  else if (y >= 2 && x <= -2) { //前+左
    motorStatus = "LeftForward";
    Serial.println(motorStatus);
    //forward + left 向前左转
    PWM_Control(PWM_LEFT1, PWM_SPEED_LEFT * (0 - x)/pwmMax);   //pwm调速
    PWM_Control(PWM_LEFT2, 0);
    PWM_Control(PWM_RIGHT1, PWM_SPEED_RIGHT);   //pwm调速
    PWM_Control(PWM_RIGHT2, 0);
  }
  else if (y <= -2 && x <= -2) { //后+左
    motorStatus = "LeftBackward";
    Serial.println(motorStatus);
    //back + left 向左后转
    PWM_Control(PWM_LEFT1, 0);
    PWM_Control(PWM_LEFT2, PWM_SPEED_LEFT);
    PWM_Control(PWM_RIGHT1, 0);
    PWM_Control(PWM_RIGHT2, PWM_SPEED_RIGHT * (0 - x)/pwmMax);
  }
  else if (y <= -2 && x >= 2) { //后+右
    motorStatus = "RightBackward";
    Serial.println(motorStatus);
    //back + right 向右后转
    PWM_Control(PWM_LEFT1, 0);
    PWM_Control(PWM_LEFT2, PWM_SPEED_LEFT * (pwmMax - x)/pwmMax);
    PWM_Control(PWM_RIGHT1, 0);
    PWM_Control(PWM_RIGHT2, PWM_SPEED_RIGHT);
  }
  else if (y >= 2) { //前
    motorStatus = "Forward";
    Serial.println(motorStatus);
    PWM_Control(PWM_LEFT1, PWM_SPEED_LEFT);   //pwm调速
    PWM_Control(PWM_LEFT2, 0);
    PWM_Control(PWM_RIGHT1, PWM_SPEED_RIGHT);   //pwm调速
    PWM_Control(PWM_RIGHT2, 0);
  }
  else if (y <= -2) { //后
    motorStatus = "Backward";
    Serial.println(motorStatus);
    PWM_Control(PWM_LEFT1, 0);
    PWM_Control(PWM_LEFT2, PWM_SPEED_LEFT);
    PWM_Control(PWM_RIGHT1, 0);
    PWM_Control(PWM_RIGHT2, PWM_SPEED_RIGHT);
  }
  else if (x <= -2) { //左
    motorStatus = "TurnLeft";
    Serial.println(motorStatus);
    PWM_Control(PWM_LEFT1, 0);
    PWM_Control(PWM_LEFT2, PWM_SPEED_LEFT);
    PWM_Control(PWM_RIGHT1, PWM_SPEED_RIGHT);
    PWM_Control(PWM_RIGHT2, 0);
  }
  else if (x >= 2) { //右
    motorStatus = "TurnRight";
    Serial.println(motorStatus);
    PWM_Control(PWM_LEFT1, PWM_SPEED_LEFT);   //pwm调速
    PWM_Control(PWM_LEFT2, 0);
    PWM_Control(PWM_RIGHT1, 0);
    PWM_Control(PWM_RIGHT2, PWM_SPEED_RIGHT);
  }
  else if (y == 0 && x == 0) {  //停
    motorStatus = "Stop";
    Serial.println(motorStatus);
    PWM_Control(PWM_LEFT1, 0);
    PWM_Control(PWM_LEFT2, 0);
    PWM_Control(PWM_RIGHT1, 0);
    PWM_Control(PWM_RIGHT2, 0);
  }
}


// 视频流ws server事件
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
        socketStatus = 0;
        Serial.printf("[WS] Disconnected!\n");
        break;
    case WStype_CONNECTED: 
        socketStatus = 1;
        Serial.printf("[WS] Connected to url: %s\n", payload);
        webSocket.sendTXT("camlogin");
        break;
    case WStype_TEXT:
        Serial.printf("[WS] get text: %s\n", payload);
        break;
    case WStype_BIN:
        break;
    case WStype_PING:
        Serial.printf("[WS] get ping\n");
        break;
    case WStype_PONG:
        Serial.printf("[WS] get pong\n");
        break;
    }
}
