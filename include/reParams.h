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
#include <stddef.h>
#include <time.h>
#include "sys/queue.h"
#include "project_config.h"
#include "def_consts.h"
#include "rTypes.h"
#include "rLog.h"
#include "rStrings.h"
// #include "reStates.h"
#include "reEsp32.h"
#include "reNvs.h"
#include "reEvents.h"
#include "reMqtt.h"
#if CONFIG_MQTT_OTA_ENABLE
#include "reOTA.h"
#endif // CONFIG_MQTT_OTA_ENABLE
#if CONFIG_TELEGRAM_ENABLE
#include "reTgSend.h"
#endif // CONFIG_TELEGRAM_ENABLE

typedef enum {
  PARAM_NVS_RESTORED = 0,
  PARAM_SET_INTERNAL,
  PARAM_SET_CHANGED
} param_change_mode_t;

typedef enum {
  PARAM_HANDLER_NONE = 0,
  PARAM_HANDLER_EVENT,
  PARAM_HANDLER_CALLBACK,
  PARAM_HANDLER_CLASS
} param_handler_type_t;

class param_handler_t {
  public:
    virtual ~param_handler_t() {};
    virtual void onChange(param_change_mode_t mode) = 0;
};

typedef struct paramsGroup_t {
  paramsGroup_t *parent;
  char *key;
  char *topic;
  char *friendly;
  STAILQ_ENTRY(paramsGroup_t) next;
} paramsGroup_t;
typedef struct paramsGroup_t *paramsGroupHandle_t;

typedef struct paramsEntry_t {
  param_kind_t type_param;
  param_type_t type_value;
  param_handler_type_t type_handler;
  void *handler;
  paramsGroup_t *group;
  uint32_t id;
  const char *friendly;
  const char *key;
  void *value;
  void *min_value;
  void *max_value;
  char *topic_subscribe;
  char *topic_publish;
  bool subscribed = false;
  bool locked = false;
  bool notify = true;
  int  qos;
  STAILQ_ENTRY(paramsEntry_t) next;
} paramsEntry_t;
typedef struct paramsEntry_t *paramsEntryHandle_t;

typedef void (*params_callback_t) (paramsEntryHandle_t item, param_change_mode_t mode, void* value);

#ifdef __cplusplus
extern "C" {
#endif

bool paramsInit();
void paramsFree();

paramsGroupHandle_t paramsRegisterGroup(paramsGroup_t* parent_group, const char* name_key, const char* name_topic, const char* name_friendly);

paramsEntryHandle_t paramsRegisterValueEx(const param_kind_t type_param, const param_type_t type_value, 
  param_handler_type_t handler_type, void* change_handler,
  paramsGroupHandle_t parent_group, 
  const char* name_key, const char* name_friendly, const int qos, 
  void * value);
#define paramsRegisterValue(type_param, type_value, change_handler, parent_group, name_key, name_friendly, qos, value) \
  paramsRegisterValueEx(type_param, type_value, PARAM_HANDLER_EVENT, change_handler, parent_group, name_key, name_friendly, qos, value)

paramsEntryHandle_t paramsRegisterCommonValueEx(const param_kind_t type_param, const param_type_t type_value, 
  param_handler_type_t handler_type, void* change_handler,
  const char* name_key, const char* name_friendly, const int qos, 
  void * value);
#define paramsRegisterCommonValue(type_param, type_value, change_handler, name_key, name_friendly, qos, value) \
  paramsRegisterCommonValueEx(type_param, type_value, PARAM_HANDLER_EVENT, change_handler, name_key, name_friendly, qos, value)

void paramsSetLimitsI8(paramsEntryHandle_t entry, int8_t min_value, int8_t max_value);
void paramsSetLimitsU8(paramsEntryHandle_t entry, uint8_t min_value, uint8_t max_value);
void paramsSetLimitsI16(paramsEntryHandle_t entry, int16_t min_value, int16_t max_value);
void paramsSetLimitsU16(paramsEntryHandle_t entry, uint16_t min_value, uint16_t max_value);
void paramsSetLimitsI32(paramsEntryHandle_t entry, int32_t min_value, int32_t max_value);
void paramsSetLimitsU32(paramsEntryHandle_t entry, uint32_t min_value, uint32_t max_value);
void paramsSetLimitsI64(paramsEntryHandle_t entry, int64_t min_value, int64_t max_value);
void paramsSetLimitsU64(paramsEntryHandle_t entry, uint64_t min_value, uint64_t max_value);
void paramsSetLimitsFloat(paramsEntryHandle_t entry, float min_value, float max_value);
void paramsSetLimitsDouble(paramsEntryHandle_t entry, double min_value, double max_value);

void paramsMqttSubscribe(paramsEntryHandle_t entry);
void paramsMqttUnsubscribe(paramsEntryHandle_t entry);
void paramsMqttPublish(paramsEntryHandle_t entry, bool publish_in_mqtt);

void paramsValueStore(paramsEntryHandle_t entry, const bool callHandler);
void paramsValueSet(paramsEntryHandle_t entry, char *new_value, bool publish_in_mqtt);

// Functions for working with the MQTT broker directly
// Note: usually they are not needed, they will be called automatically when the corresponding event is received
void paramsMqttSubscribesOpen(bool mqttPrimary, bool forcedResubscribe);
void paramsMqttSubscribesClose();
void paramsMqttIncomingMessage(char *topic, char *payload, size_t len);

// Register event handlers
bool paramsEventHandlerRegister();

#ifdef __cplusplus
}
#endif

#endif // __RE_PARAMS_H__