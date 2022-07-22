#include "pwm_control.h"
#include <Arduino.h>

// PWM的通道，共16个(0-15)，分为高低速两组，
// 高速通道(0-7): 80MHz时钟，低速通道(8-15): 1MHz时钟
// 0-15都可以设置，只要不重复即可，参考上面的列表
// 如果有定时器的使用，千万要避开!!!
/*
 * LEDC Chan to Group/Channel/Timer Mapping
** ledc: 0  => Group: 0, Channel: 0, Timer: 0
** ledc: 1  => Group: 0, Channel: 1, Timer: 0
** ledc: 2  => Group: 0, Channel: 2, Timer: 1
** ledc: 3  => Group: 0, Channel: 3, Timer: 1
** ledc: 4  => Group: 0, Channel: 4, Timer: 2
** ledc: 5  => Group: 0, Channel: 5, Timer: 2
** ledc: 6  => Group: 0, Channel: 6, Timer: 3
** ledc: 7  => Group: 0, Channel: 7, Timer: 3
** ledc: 8  => Group: 1, Channel: 0, Timer: 0
** ledc: 9  => Group: 1, Channel: 1, Timer: 0
** ledc: 10 => Group: 1, Channel: 2, Timer: 1
** ledc: 11 => Group: 1, Channel: 3, Timer: 1
** ledc: 12 => Group: 1, Channel: 4, Timer: 2
** ledc: 13 => Group: 1, Channel: 5, Timer: 2
** ledc: 14 => Group: 1, Channel: 6, Timer: 3
** ledc: 15 => Group: 1, Channel: 7, Timer: 3
*/


// PWM频率，直接设置即可
int Motor_freq_PWM = 1000;

// PWM分辨率，取值为 0-20 之间，这里填写为10，那么后面的ledcWrite
//  这个里面填写的pwm值就在 0 - 2的10次方 之间 也就是 0-1024
int Motor_resolution_PWM = 10;

//频道0-16，不要和其他timer冲突，绑定io
void PWM_Init(int PWM_Channel, int PWM_IO){
    //输出口
    pinMode(PWM_IO, OUTPUT);
    // 设置通道
    ledcSetup(PWM_Channel, Motor_freq_PWM, Motor_resolution_PWM); 
    //将 LEDC pwm 通道绑定到指定 GPIO 口上，以实现io口的pwm输出
    ledcAttachPin(PWM_IO, PWM_Channel);
}

//pwm的占空比
void PWM_Control(int PWM_Channel, int DutyA){
    if(DutyA > 1024){
        DutyA = 1024;
    }
    ledcWrite(PWM_Channel, DutyA);
}
