#ifndef __AUTOPWM_H__
#define __AUTOPWM_H__

#include <Arduino.h>

void PWM_Init(int PWM_Channel, int PWM_IO);
void PWM_Control(int PWM_Channel, int DutyA);

#endif
