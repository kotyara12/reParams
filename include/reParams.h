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
#include <time.h>
#include "sys/queue.h"
#include "project_config.h"
#include "rTypes.h"

typedef void (*param_change_callback_t) (); 

typedef struct paramsGroup_t {
  paramsGroup_t *parent;
  const char* friendly;
  char *group;
  char *topic;
  STAILQ_ENTRY(paramsGroup_t) next;
} paramsGroup_t;
typedef struct paramsGroup_t *paramsGroupHandle_t;

typedef struct paramsEntry_t {
  param_kind_t type_param;
  param_type_t type_value;
  param_change_callback_t on_change;
  paramsGroup_t *group;
  const char* friendly;
  const char* key;
  void *value;
  char *topic;
  #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
  char *confirm;
  #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
  int qos;
  STAILQ_ENTRY(paramsEntry_t) next;
} paramsEntry_t;
typedef struct paramsEntry_t *paramsEntryHandle_t;

#if CONFIG_SILENT_MODE_ENABLE
typedef void (*silent_mode_change_callback_t) (const bool silent_mode);
#endif // CONFIG_SILENT_MODE_ENABLE

#ifdef __cplusplus
extern "C" {
#endif

bool paramsInit();
void paramsFree();
paramsGroupHandle_t paramsRegisterGroup(const paramsGroup_t* parent_group, const char* name_group, const char* name_friendly);
paramsEntryHandle_t paramsRegisterValue(const param_kind_t type_param, const param_type_t type_value, param_change_callback_t callback_change,
  const paramsGroupHandle_t parent_group, 
  const char* name_key, const char* name_friendly, const int qos, 
  void * value);
paramsEntryHandle_t paramsRegisterCommonValue(const param_kind_t type_param, const param_type_t type_value, param_change_callback_t callback_change,
  const char* name_key, const char* name_friendly, const int qos, 
  void * value);

// MQTT
void paramsMqttSubscribesOpen();
void paramsMqttSubscribesClose();
void paramsMqttIncomingMessage(char *topic, uint8_t *payload, size_t len);

// Silent mode
#if CONFIG_SILENT_MODE_ENABLE
bool isSilentMode();
void silentModeSetCallback(silent_mode_change_callback_t cb);
void silentModeCheck(const struct tm timeinfo);
#endif // CONFIG_SILENT_MODE_ENABLE


#ifdef __cplusplus
}
#endif

#endif // __RE_PARAMS_H__