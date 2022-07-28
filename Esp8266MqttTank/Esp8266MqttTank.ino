/* 
 *  esp8266 + l298n + mqtt 制作4g远程控制坦克
 *    https://blog.yanjingang.com/?p=6598 
 */

#include <ESP8266WiFi.h>
#include "Arduino.h"
#include <ArduinoMqttClient.h>
#include <Arduino_JSON.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Ticker.h>   //定时器


// wifi设置
const char* ssid = "Mars.Y";
const char* password = "Lovezhu1314";

// 网络时间获取
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 3600, 60000); //NTP地址
const long gmtOffset_sec = 8 * 3600;    //时区设置函数，东八区（UTC/GMT+8:00）写成8*3600 
unsigned long LAUNCH_TIMESTAMP = 0;  //网络接通时的世界时间戳


// 马达参数
int DC_LEFT1 =  D1;  //io5; // Left 1
int DC_LEFT2 =  D2;  //io4;  // Left 2
int DC_RIGHT1 = D5; //io14; // Right 1
int DC_RIGHT2 = D6; //io12; // Right 2
String motorStatus = "";        //底盘当前状态
String lastMotorStatus = "";   //底盘末次上报状态
int PWM_SPEED_LEFT = 60;
int PWM_SPEED_RIGHT = 61;     //右轮需要慢点


// 呼吸灯
Ticker tickeLamp;
int LED_PIN = LED_BUILTIN;  //D4 io2


// mqtt参数
WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);
const char broker[] = "amzvemj.iot.gz.baidubce.com";
int port = 1883;
const char pubTopic[]  = "tank1/data";
const char subTopic[]  = "tank1/cmd";  // 注意：baidu iotcore的自定义topic不能以'/'开头!

// tcp server
WiFiClient tcpClient;
//const IPAddress tcpServer(192,168,1,5);   //目标IP地址
const char tcpServer[] = "yanjingang.com";  //目标地址
uint16_t tcpPort = 8879;         //目标服务器端口号


void setup() {
  Serial.begin(115200);
  //Serial.setDebugOutput(true);
  //Serial.println();

  
  // 初始化马达引脚（注：esp板子LOW是走，HIGH是停止；拔掉电驱引脚线会导致电驱转动的诡异现象）
  digitalWrite(DC_LEFT1, HIGH);   //io5
  digitalWrite(DC_LEFT2, HIGH);   //io4
  digitalWrite(DC_RIGHT1, HIGH);   //io14
  digitalWrite(DC_RIGHT2, HIGH);   //io12 
  analogWriteRange(100); 
  analogWriteFreq(50); 
  pinMode(DC_LEFT1, OUTPUT);
  pinMode(DC_LEFT2, OUTPUT);
  pinMode(DC_RIGHT1, OUTPUT);
  pinMode(DC_RIGHT2, OUTPUT);
  //STOP
  //runMotor(0, 0); // x/y=0代表横纵向静止不动


  
  // 连接wifi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected!");
  
  // 世界时间同步
  timeClient.begin();
  timeClient.setTimeOffset(gmtOffset_sec); //+1区，偏移3600，+8区，偏移3600*8
  timeClient.update();
  LAUNCH_TIMESTAMP = timeClient.getEpochTime() - millis()/1000;  //获取时间戳-当前板子启动时间，作为初始时间
  struct tm *ptm = gmtime ((time_t *)&LAUNCH_TIMESTAMP);
  String currentDate = String(ptm->tm_year+1900) + "-" + String(ptm->tm_mon+1) + "-" + String(ptm->tm_mday) + " " + timeClient.getFormattedTime();
  Serial.print("getWordTime: ");
  Serial.println(currentDate);
  Serial.println("");
  

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

  /*
  // 建立TCP连接（对比tcp与mqtt的传输时延差别）
  while (!tcpClient.connect(tcpServer, tcpPort) || !tcpClient.connected()) {
    Serial.print("TCP connection failed! Retry...");
    delay(500);
  }
  Serial.println("TCP connected!\n");
  // login
  if(!tcpClient.write("hello")){  //{\"action\":\"login\",\"hid\":1,\"uid\":\"mars\",\"data\":{}}
     Serial.print("TCP write failed!");
  }*/

  

  // 初始化呼吸灯
  digitalWrite(LED_PIN, LOW); 
  tickeLamp.attach_ms(4000, breathingLamp);  //每4秒执行一次呼吸
   
}


// 呼吸灯
void breathingLamp(){
  //Serial.println("breathingLamp: ");
  for(int i=1023; i>0; i-=3){
    //Serial.println(i);
    analogWrite(LED_PIN, i);
    delay(100);
  }
  for(int i=0; i<=1023; i+=3){
    //Serial.println(i);
    analogWrite(LED_PIN, i);
    delay(100);
  }
}

void loop() {
  /*// 世界时间同步
  if(LAUNCH_TIMESTAMP == 0){
    static char res[19] = "";
    LAUNCH_TIMESTAMP = getWordTimestamp(res);
    Serial.println(res);
    Serial.println(LAUNCH_TIMESTAMP);
  }*/
  
  // 接收mqtt指令
  mqttClient.poll();

  
  // 上报底盘状态
  if (motorStatus != lastMotorStatus) {
    lastMotorStatus = motorStatus;

    // data
    JSONVar pubdata;
    pubdata["status"] = motorStatus;
    pubdata["ts"] = LAUNCH_TIMESTAMP + millis()/1000;
    String strdata = JSON.stringify(pubdata);
    
    Serial.print("pub msg to topic: ");
    Serial.print(pubTopic);
    Serial.print(" ");
    Serial.println(strdata);

    // send message, the Print interface can be used to set the message contents
    mqttClient.beginMessage(pubTopic);
    mqttClient.print(strdata);  //{"status": "Forward"}
    mqttClient.endMessage();
    Serial.println("pub mqtt done!");
    
    /*// sent tcp 
    if(tcpClient.connected()){
      if(!tcpClient.write(strdata.c_str())){
         Serial.print("TCP write failed!");
      }else{
        Serial.println("pub tcp done!");
      }
    }*/
  }

}

/*
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
*/


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
int runMax = 25;  //xy单方向的最大值
void runMotor(int x, int y) {
  Serial.print("runMotor :");
  Serial.print(x);
  Serial.print(" ");
  Serial.println(y);

  //指令处理
  //  原点{"x":0,"y":0}, 前进y>0,后退y<0；右转x>0,左转x<0
  //  注意：esp板子LOW是走，HIGH是停；pwm调速值也同理，值越小速度越快。需要做反向处理
  if (y >= 5 && x >= 5) { //右前
    motorStatus = "RightForward";
    Serial.println(motorStatus);
    analogWrite(DC_LEFT1, PWM_SPEED_LEFT);   //满速
    digitalWrite(DC_LEFT2, HIGH);
    analogWrite(DC_RIGHT1, PWM_SPEED_RIGHT * (1 + x/runMax));   //pwm调速-减速（值越大速度越慢）
    digitalWrite(DC_RIGHT2, HIGH);
  }
  else if (y >= 5 && x <= -5) { //左前
    motorStatus = "LeftForward";
    Serial.println(motorStatus);
    analogWrite(DC_LEFT1, PWM_SPEED_LEFT * (1 + (0 - x)/runMax));   //pwm调速-减速（值越大速度越慢）
    digitalWrite(DC_LEFT2, HIGH);
    analogWrite(DC_RIGHT1, PWM_SPEED_RIGHT);   //满速
    digitalWrite(DC_RIGHT2, HIGH);
  }
  else if (y <= -5 && x <= -5) { //左后
    motorStatus = "LeftBackward";
    digitalWrite(DC_LEFT1, HIGH);
    analogWrite(DC_LEFT2, PWM_SPEED_LEFT);    //满速
    digitalWrite(DC_RIGHT1, HIGH);
    analogWrite(DC_RIGHT2, PWM_SPEED_RIGHT * (1 + (0 - x)/runMax));  //pwm调速-减速（值越大速度越慢）
  }
  else if (y <= -5 && x >= 5) { //右后
    motorStatus = "RightBackward";
    Serial.println(motorStatus);
    digitalWrite(DC_LEFT1, HIGH);
    analogWrite(DC_LEFT2, PWM_SPEED_LEFT * (1 + x/runMax));     //pwm调速-减速（值越大速度越慢）
    digitalWrite(DC_RIGHT1, HIGH);
    analogWrite(DC_RIGHT2, PWM_SPEED_RIGHT);    //满速
  }
  else if (y >= 5) { //前
    motorStatus = "Forward";
    Serial.println(motorStatus);
    analogWrite(DC_LEFT1, PWM_SPEED_LEFT);   //满速
    digitalWrite(DC_LEFT2, HIGH);
    analogWrite(DC_RIGHT1, PWM_SPEED_RIGHT);   //满速
    digitalWrite(DC_RIGHT2, HIGH);
  }
  else if (y <= -5) { //后
    motorStatus = "Backward";
    Serial.println(motorStatus);
    digitalWrite(DC_LEFT1, HIGH);
    analogWrite(DC_LEFT2, PWM_SPEED_LEFT);
    digitalWrite(DC_RIGHT1, HIGH);
    analogWrite(DC_RIGHT2, PWM_SPEED_RIGHT);
  }
  else if (x <= -5) { //左
    motorStatus = "TurnLeft";
    Serial.println(motorStatus);
    digitalWrite(DC_LEFT1, HIGH);
    analogWrite(DC_LEFT2, PWM_SPEED_LEFT * 1.1);    //横向控制需要慢点
    analogWrite(DC_RIGHT1, PWM_SPEED_RIGHT * 1.1);
    digitalWrite(DC_RIGHT2, HIGH);
  }
  else if (x >= 5) { //右
    motorStatus = "TurnRight";
    Serial.println(motorStatus);
    analogWrite(DC_LEFT1, PWM_SPEED_LEFT * 1.1);   //横向控制需要慢点
    digitalWrite(DC_LEFT2, HIGH);
    digitalWrite(DC_RIGHT1, HIGH);
    analogWrite(DC_RIGHT2, PWM_SPEED_RIGHT * 1.1);
  }
  else if (y == 0 && x == 0) {  //停（注：esp的板子需要所有输出HIGH才是停）
    motorStatus = "Stop";
    Serial.println(motorStatus);
    digitalWrite(DC_LEFT1, HIGH);
    digitalWrite(DC_LEFT2, HIGH);
    digitalWrite(DC_RIGHT1, HIGH);
    digitalWrite(DC_RIGHT2, HIGH);
  }
}
