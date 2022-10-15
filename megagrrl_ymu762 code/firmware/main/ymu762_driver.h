
#ifndef YMU762_DRIVER_H
#define YMU762_DRIVER_H

#include "esp_log.h"

void Driver_Ymu762Write(uint8_t Port,uint8_t data);
uint8_t Driver_Ymu762Read(uint8_t Port);
void Driver_Ymu762Init(void);

#endif