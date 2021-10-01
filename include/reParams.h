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

typedef enum {
  PARAM_NVS_RESTORED = 0,
  PARAM_SET_INTERNAL,
  PARAM_SET_CHANGED
} param_change_mode_t;

class param_handler_t {
  public:
    virtual ~param_handler_t() {};
    virtual void onChange(param_change_mode_t mode) = 0;
};

typedef struct paramsGroup_t {
  paramsGroup_t *parent;
  char *key;
  char *topic;
  char* friendly;
  STAILQ_ENTRY(paramsGroup_t) next;
} paramsGroup_t;
typedef struct paramsGroup_t *paramsGroupHandle_t;

typedef struct paramsEntry_t {
  param_kind_t type_param;
  param_type_t type_value;
  param_handler_t *handler;
  paramsGroup_t *group;
  uint32_t id;
  const char* friendly;
  const char* key;
  void *value;
  char *topic_subscribe;
  #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
  char *topic_publish;
  #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
  bool subscribed = false;
  int qos;
  STAILQ_ENTRY(paramsEntry_t) next;
} paramsEntry_t;
typedef struct paramsEntry_t *paramsEntryHandle_t;

#ifdef __cplusplus
extern "C" {
#endif

bool paramsInit();
void paramsFree();

paramsGroupHandle_t paramsRegisterGroup(paramsGroup_t* parent_group, const char* name_key, const char* name_topic, const char* name_friendly);
paramsEntryHandle_t paramsRegisterValue(const param_kind_t type_param, const param_type_t type_value, param_handler_t *change_handler,
  paramsGroupHandle_t parent_group, 
  const char* name_key, const char* name_friendly, const int qos, 
  void * value);
paramsEntryHandle_t paramsRegisterCommonValue(const param_kind_t type_param, const param_type_t type_value, param_handler_t *change_handler,
  const char* name_key, const char* name_friendly, const int qos, 
  void * value);

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