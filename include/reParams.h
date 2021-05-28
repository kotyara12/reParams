/* 
   EN: Library for storing and managing parameters
   RU: Библиотека хранения и управления параметрами
   --------------------------
   (с) 2021 Разживин Александр | Razzhivin Alexander
   kotyara12@yandex.ru | https://kotyara12.ru | tg: @kotyara1971
*/

#ifndef __RE_PARAMS_H__
#define __RE_PARAMS_H__

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "rTypes.h"

typedef void (*param_change_callback_t) (); 

#ifdef __cplusplus
extern "C" {
#endif

bool paramsInit();
void paramsFree();
void paramsRegValue(const param_kind_t type_param, const param_type_t type_value, param_change_callback_t callback_change,
  const char* name_group, const char* name_key, const char* name_friendly, const int qos, 
  void * value);

void paramsMqttSubscribes();
void paramsMqttResetSubscribes();
void paramsMqttIncomingMessage(char *topic, uint8_t *payload, size_t len);

#ifdef __cplusplus
}
#endif

#endif // __RE_PARAMS_H__