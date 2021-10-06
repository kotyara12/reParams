#include "reParams.h"
#include <string.h>
#include <time.h>
#include "rStrings.h"
#include "rLog.h"
#include "reStates.h"
#include "reEsp32.h"
#include "reNvs.h"
#include "reEvents.h"
#include "reMqtt.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "project_config.h"
#if CONFIG_MQTT_OTA_ENABLE
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#endif // CONFIG_MQTT_OTA_ENABLE
#if CONFIG_TELEGRAM_ENABLE
#include "reTgSend.h"
#endif // CONFIG_TELEGRAM_ENABLE

STAILQ_HEAD(paramsGroupHead_t, paramsGroup_t);
STAILQ_HEAD(paramsEntryHead_t, paramsEntry_t);
typedef struct paramsEntryHead_t *paramsEntryHeadHandle_t;
typedef struct paramsGroupHead_t *paramsGroupHeadHandle_t;

static paramsGroupHeadHandle_t paramsGroups = nullptr;
static paramsEntryHeadHandle_t paramsList = nullptr;
static SemaphoreHandle_t paramsLock = nullptr;

#define OPTIONS_LOCK() do {} while (xSemaphoreTake(paramsLock, portMAX_DELAY) != pdPASS)
#define OPTIONS_UNLOCK() xSemaphoreGive(paramsLock)

static const char* tagPARAMS = "PRMS";

static bool _paramsMqttPrimary = true;
#if CONFIG_MQTT_PARAMS_WILDCARD
static char* _paramsWildcardTopic = nullptr;
#endif // CONFIG_MQTT_PARAMS_WILDCARD

paramsGroupHandle_t _pgCommon = nullptr;

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------- Common functions ----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool paramsInit()
{
  nvsInit();

  if (!paramsList) {
    paramsLock = xSemaphoreCreateMutex();
    if (!paramsLock) {
      rlog_e(tagPARAMS, "Can't create parameters mutex!");
      return false;
    };

    paramsGroups = (paramsGroupHeadHandle_t)calloc(1, sizeof(paramsGroupHead_t));
    if (paramsGroups) {
      STAILQ_INIT(paramsGroups);
    }
    else {
      vSemaphoreDelete(paramsLock);
      rlog_e(tagPARAMS, "Parameters manager initialization error!");
      return false;
    }

    paramsList = (paramsEntryHeadHandle_t)calloc(1, sizeof(paramsEntryHead_t));
    if (paramsList) {
      STAILQ_INIT(paramsList);
    }
    else {
      vSemaphoreDelete(paramsLock);
      rlog_e(tagPARAMS, "Parameters manager initialization error!");
      return false;
    };
  };
  
  #if CONFIG_MQTT_OTA_ENABLE
  paramsRegisterValue(OPT_KIND_OTA, OPT_TYPE_STRING, 
    nullptr, 
    nullptr, CONFIG_MQTT_OTA_TOPIC, CONFIG_MQTT_OTA_NAME, 
    CONFIG_MQTT_OTA_QOS, nullptr);
  #endif // CONFIG_MQTT_OTA_ENABLE

  #if CONFIG_MQTT_COMMAND_ENABLE
  paramsRegisterValue(OPT_KIND_COMMAND, OPT_TYPE_STRING,
    nullptr, 
    nullptr, CONFIG_MQTT_COMMAND_TOPIC, CONFIG_MQTT_COMMAND_NAME, 
    CONFIG_MQTT_COMMAND_QOS, nullptr);
  #endif // CONFIG_MQTT_COMMAND_ENABLE

  return true;
}

void paramsFree()
{
  OPTIONS_LOCK();

  if (paramsList) {
    paramsEntryHandle_t itemL, tmpL;
    STAILQ_FOREACH_SAFE(itemL, paramsList, next, tmpL) {
      STAILQ_REMOVE(paramsList, itemL, paramsEntry_t, next);
      if ((itemL->topic_subscribe) && itemL->subscribed) {
        mqttUnsubscribe(itemL->topic_subscribe);
        free(itemL->topic_subscribe);
      };
      #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      if (itemL->topic_publish) {
        free(itemL->topic_publish);
      };
      #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      free(itemL);
    };
    free(paramsList);
  };

  if (paramsGroups) {
    paramsGroupHandle_t itemG, tmpG;
    STAILQ_FOREACH_SAFE(itemG, paramsGroups, next, tmpG) {
      STAILQ_REMOVE(paramsGroups, itemG, paramsGroup_t, next);
      if (itemG->parent) {
        if (itemG->key) free(itemG->key);
        if (itemG->topic) free(itemG->topic);
        if (itemG->friendly) free(itemG->friendly);
      };
      free(itemG);
    };
    free(paramsGroups);
  };

  OPTIONS_UNLOCK();
  
  vSemaphoreDelete(paramsLock);
}

// -----------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- MQTT topics ------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void paramsMqttTopicsFreeEntry(paramsEntryHandle_t entry)
{
  if ((entry->key) && ((entry->topic_subscribe) || (entry->topic_publish))) {
    if (entry->group) {
      rlog_d(tagPARAMS, "Topics for parameter \"%s.%s\" has been scrapped", entry->group->key, entry->key);
    } else {
      rlog_d(tagPARAMS, "Topics for parameter \"%s\" has been scrapped", entry->key);
    };
  };

  if (entry->topic_subscribe) {
    free(entry->topic_subscribe);
    entry->topic_subscribe = nullptr;
  };

  #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
    if (entry->topic_publish) {
      free(entry->topic_publish);
      entry->topic_publish = nullptr;
    };
  #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
}

void paramsMqttTopicsCreateEntry(paramsEntryHandle_t entry)
{
  if (entry->key) {
    if (entry->type_param == OPT_KIND_PARAMETER) {
      if ((entry->group) && (entry->group->topic)) {
        entry->topic_subscribe = mqttGetTopic(_paramsMqttPrimary, CONFIG_MQTT_ROOT_PARAMS_LOCAL, CONFIG_MQTT_ROOT_PARAMS_TOPIC, entry->group->topic, entry->key);
        if (entry->topic_subscribe) {
          rlog_d(tagPARAMS, "Generated subscription topic for parameter \"%s.%s\": [ %s ]", entry->group->key, entry->key, entry->topic_subscribe);
        };
      };
    } else {
      entry->topic_subscribe = mqttGetTopic(_paramsMqttPrimary, CONFIG_MQTT_ROOT_SYSTEM_LOCAL, CONFIG_MQTT_ROOT_SYSTEM_TOPIC, entry->key, nullptr);
      if (entry->topic_subscribe) {
        rlog_d(tagPARAMS, "Generated subscription topic for parameter \"%s\": [ %s ]", entry->key, entry->topic_subscribe);
      };
    };
    if (!entry->topic_subscribe) {
      rlog_e(tagPARAMS, "Failed to generate subscription topic!");
    };
  
    #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      if (entry->type_param == OPT_KIND_PARAMETER) {
        if ((entry->group) && (entry->group->topic)) {
          entry->topic_publish = mqttGetTopic(_paramsMqttPrimary, CONFIG_MQTT_ROOT_CONFIRM_LOCAL, CONFIG_MQTT_ROOT_CONFIRM_TOPIC, entry->group->topic, entry->key);
        };
        if (entry->topic_publish) {
          rlog_d(tagPARAMS, "Generated confirmation topic for parameter \"%s.%s\": [ %s ]", entry->group->key, entry->key, entry->topic_publish);
        } else {
          rlog_e(tagPARAMS, "Failed to generate confirmation topic!");
        };
      };
    #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------ MQTT internal funcions -----------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED

void _paramsMqttConfirmEntry(paramsEntryHandle_t entry)
{
  if ((entry->group) && (entry->type_param == OPT_KIND_PARAMETER)) {
    if (entry->value) {
      if ((!entry->topic_subscribe) || (!entry->topic_publish)) {
        paramsMqttTopicsFreeEntry(entry);
        paramsMqttTopicsCreateEntry(entry);
      };
      if (entry->topic_publish) {
        mqttPublish(entry->topic_publish, 
          value2string(entry->type_value, entry->value), 
          entry->qos, CONFIG_MQTT_CONFIRM_RETAINED, 
          true, false, true);
      };
    } else {
      rlog_w(tagPARAMS, "Call publication parameter of undetermined value!");
    };
  };
}

void paramsMqttConfirmEntry(paramsEntryHandle_t entry)
{
  if (mqttIsConnected()) {
    _paramsMqttConfirmEntry(entry);
  }
}

#endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED

void _paramsMqttPublishEntry(paramsEntryHandle_t entry)
{
  if (entry->group && (entry->type_param == OPT_KIND_PARAMETER)) {
    if (entry->value) {
      if (!entry->topic_subscribe) {
        paramsMqttTopicsFreeEntry(entry);
        paramsMqttTopicsCreateEntry(entry);
      };
      if (entry->topic_subscribe) {
        mqttUnsubscribe(entry->topic_subscribe);
        mqttPublish(entry->topic_subscribe, 
          value2string(entry->type_value, entry->value), 
          entry->qos, CONFIG_MQTT_PARAMS_RETAINED, 
          true, false, true);
        entry->subscribed = mqttSubscribe(entry->topic_subscribe, entry->qos);
      };
    } else {
      rlog_w(tagPARAMS, "Call publication parameter of undetermined value!");
    };
  };
}

void paramsMqttPublishEntry(paramsEntryHandle_t entry)
{
  if (mqttIsConnected()) {
    _paramsMqttPublishEntry(entry);
  }
}

bool _paramsMqttSubscribeEntry(paramsEntryHandle_t entry)
{
  if (!entry->topic_subscribe) {
    paramsMqttTopicsFreeEntry(entry);
    paramsMqttTopicsCreateEntry(entry);
  };
  if (entry->topic_subscribe) {
    return mqttSubscribe(entry->topic_subscribe, entry->qos);
  };
  return false;
};

bool paramsMqttSubscribeEntry(paramsEntryHandle_t entry)
{
  if (mqttIsConnected()) {
    return _paramsMqttSubscribeEntry(entry);
  };
  return false;
}

#if CONFIG_MQTT_PARAMS_WILDCARD

bool _paramsMqttSubscribeWildcard()
{
  if (_paramsWildcardTopic) free(_paramsWildcardTopic);
  _paramsWildcardTopic = mqttGetTopic(_paramsMqttPrimary, CONFIG_MQTT_ROOT_PARAMS_LOCAL, CONFIG_MQTT_ROOT_PARAMS_TOPIC, "#", nullptr);
  if (_paramsWildcardTopic) {
    rlog_d(tagPARAMS, "Generated subscription topic for all parameters: [ %s ]", _paramsWildcardTopic);
    return mqttSubscribe(_paramsWildcardTopic, CONFIG_MQTT_PARAMS_QOS);
  } else {
    rlog_e(tagPARAMS, "Failed to generate wildcard topic!");
  };
  return false;
}

void paramsMqttFreeWildcard()
{
  if (_paramsWildcardTopic) free(_paramsWildcardTopic);
  _paramsWildcardTopic = nullptr;
  rlog_d(tagPARAMS, "Topics for all parameters has been scrapped");
}

#endif // CONFIG_MQTT_PARAMS_WILDCARD  

void paramsMqttPublish(paramsEntryHandle_t entry, bool publish_in_mqtt)
{
  if (mqttIsConnected() && (entry->type_param == OPT_KIND_PARAMETER)) {
    #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      _paramsMqttConfirmEntry(entry);
    #else
      if (publish_in_mqtt) {
        _paramsMqttPublishEntry(entry);
      };
    #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
  };
}

bool _paramsMqttSubscribe(paramsEntryHandle_t entry)
{
  // Create new topics
  paramsMqttTopicsFreeEntry(entry);
  paramsMqttTopicsCreateEntry(entry);

  // Publish current value
  #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
    if (entry->type_param == OPT_KIND_PARAMETER) {
      _paramsMqttConfirmEntry(entry);
    };
  #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED

  // Subscribe to topic
  #if CONFIG_MQTT_PARAMS_WILDCARD
    if (entry->type_param == OPT_KIND_PARAMETER) {
      return (_paramsWildcardTopic) || _paramsMqttSubscribeWildcard();
    } else {
      return _paramsMqttSubscribeEntry(entry);
    };
  #else
    return _paramsMqttSubscribeEntry(entry);
  #endif // CONFIG_MQTT_PARAMS_WILDCARD
}

void paramsMqttSubscribe(paramsEntryHandle_t entry)
{
  entry->subscribed = mqttIsConnected() && _paramsMqttSubscribe(entry);
}

void _paramsMqttUnubscribe(paramsEntryHandle_t entry)
{
  if (entry->subscribed) {
    #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      if (entry->type_param == OPT_KIND_PARAMETER) {
        if (_paramsWildcardTopic) {
          mqttUnsubscribe(_paramsWildcardTopic);
          free(_paramsWildcardTopic);
          _paramsWildcardTopic = nullptr;
        };
      } else {
        mqttUnsubscribe(entry->topic_subscribe);
      };
    #else
      mqttUnsubscribe(entry->topic_subscribe);
    #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED 
  };
  entry->subscribed = false;
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------- Register parameters -------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

paramsGroupHandle_t paramsRegisterGroup(paramsGroup_t* parent_group, const char* name_key, const char* name_topic, const char* name_friendly)
{
  paramsGroupHandle_t item = nullptr;

  if (!paramsGroups) {
    paramsInit();
  };

  OPTIONS_LOCK();

  if (paramsGroups) {
    item = (paramsGroupHandle_t)calloc(1, sizeof(paramsGroup_t));
    if (item) {
      item->parent = parent_group;
      if (item->parent) {
        item->key = malloc_stringf(CONFIG_MESSAGE_TG_PARAM_GROUP_DELIMITER, item->parent->key, name_key);
        item->friendly = malloc_stringf(CONFIG_MESSAGE_TG_PARAM_FIENDLY_DELIMITER, item->parent->friendly, name_friendly);
        item->topic = mqttGetSubTopic(item->parent->topic, name_topic);
      } else {
        item->key = (char*)name_key;
        item->friendly = (char*)name_friendly;
        item->topic = (char*)name_topic;
      };
      if (strlen(item->key) > 15) {
        rlog_e(tagPARAMS, "The group key name [%s] is too long!", item->key);
      };
      STAILQ_INSERT_TAIL(paramsGroups, item, next);
    };
  };

  OPTIONS_UNLOCK();

  return item;
}

paramsEntryHandle_t paramsRegisterValue(const param_kind_t type_param, const param_type_t type_value, param_handler_t *change_handler,
  paramsGroupHandle_t parent_group, 
  const char* name_key, const char* name_friendly, const int qos, 
  void * value)
{
  paramsEntryHandle_t item = nullptr;

  if (!paramsList) {
    paramsInit();
  };

  OPTIONS_LOCK();

  if (paramsList) {
    item = (paramsEntryHandle_t)calloc(1, sizeof(paramsEntry_t));
    if (item) {
      if (value) {
        item->id = (uint32_t)value;
      } else {
        item->id = 0;
      };
      item->type_param = type_param;
      item->type_value = type_value;
      item->handler = change_handler;
      item->friendly = name_friendly;
      item->group = parent_group;
      item->key = name_key;
      item->subscribed = false;
      item->topic_subscribe = nullptr;
      #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      item->topic_publish = nullptr;
      #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      item->qos = qos;
      item->value = value;
      // Append item to list
      STAILQ_INSERT_TAIL(paramsList, item, next);
      // Read value from NVS storage
      if (item->type_param == OPT_KIND_PARAMETER) {
        void* prev_value = clone2value(item->type_value, item->value);
        if ((item->group) && (item->group->key)) {
          nvsRead(item->group->key, item->key, item->type_value, item->value);
        };
        if (prev_value) {
          if (!equal2value(item->type_value, prev_value, item->value)) {
            eventLoopPost(RE_PARAMS_EVENTS, RE_PARAMS_RESTORED, &item->id, sizeof(item->id), portMAX_DELAY);
            if (item->handler) item->handler->onChange(PARAM_NVS_RESTORED);
          };
          free(prev_value);
        };

        char* str_value = value2string(item->type_value, item->value);
        if ((item->group) && (item->group->key)) {
          rlog_d(tagPARAMS, "Parameter \"%s.%s\": [%s] registered", item->group->key, item->key, str_value);
        } else {
          rlog_d(tagPARAMS, "Parameter \"%s\": [%s] registered", item->key, str_value);
        };
        free(str_value);
      } else {
        rlog_d(tagPARAMS, "System handler \"%s\" registered", item->key);
      };
      // We try to subscribe if the connection to the server is already established
      paramsMqttSubscribe(item);
    };
  };
  
  OPTIONS_UNLOCK();

  return item;
}

paramsEntryHandle_t paramsRegisterCommonValue(const param_kind_t type_param, const param_type_t type_value, param_handler_t *change_handler,
  const char* name_key, const char* name_friendly, const int qos, 
  void * value)
{
  if (!_pgCommon) {
    _pgCommon = paramsRegisterGroup(nullptr, CONFIG_MQTT_COMMON_TOPIC, CONFIG_MQTT_COMMON_TOPIC, CONFIG_MQTT_COMMON_FIENDLY);
  };

  if (_pgCommon) {
    return paramsRegisterValue(type_param, type_value, 
      change_handler, 
      _pgCommon, name_key, name_friendly, qos, value);
  };

  return nullptr;
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------------- OTA ---------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_MQTT_OTA_ENABLE

extern const char ota_pem_start[] asm(CONFIG_MQTT_OTA_PEM_START);
extern const char ota_pem_end[]   asm(CONFIG_MQTT_OTA_PEM_END); 

static const char* tagOTA = "OTA";

void paramsStartOTA(char *topic, char *payload)
{
  if (strlen(payload) > 0) {
    rlog_i(tagOTA, "OTA firmware upgrade received from \"%s\"", payload);
    #if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_OTA
      tgSend(CONFIG_NOTIFY_TELEGRAM_ALERT_OTA, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_OTA, payload);
    #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_OTA

    // If the data is received from MQTT, remove the value from the topic
    if (topic) {
      mqttUnsubscribe(topic);
      mqttPublish(topic, nullptr, CONFIG_MQTT_OTA_QOS, CONFIG_MQTT_OTA_RETAINED, true, false, false);
      mqttSubscribe(topic, CONFIG_MQTT_OTA_QOS);
    };

    // Notify other tasks to suspend activities
    eventLoopPost(RE_SYSTEM_EVENTS, RE_SYS_OTA, payload, strlen(payload) + 1, portMAX_DELAY);

    // Wait a bit, so that all that is necessary has been completed
    msTaskDelay(CONFIG_MQTT_OTA_DELAY);

    esp_http_client_config_t cfgOTA;
    memset(&cfgOTA, 0, sizeof(cfgOTA));
    cfgOTA.use_global_ca_store = false;
    cfgOTA.cert_pem = (char*)ota_pem_start;
    cfgOTA.skip_cert_common_name_check = true;
    cfgOTA.url = payload;
    cfgOTA.is_async = false;

    uint8_t tryUpdate = 0;
    esp_err_t err = ESP_OK;
    ledSysOn(true);
    do {
      tryUpdate++;
      rlog_i(tagOTA, "Start of firmware upgrade from \"%s\", attempt %d", payload, tryUpdate);
      err = esp_https_ota(&cfgOTA);
      if (err == ESP_OK) {
        rlog_i(tagOTA, "Firmware upgrade completed!");
      } else {
        rlog_e(tagOTA, "Firmware upgrade failed: %d!", err);
      };
    } while ((err != ESP_OK) && (tryUpdate < CONFIG_MQTT_OTA_ATTEMPTS));

    #if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_OTA
    if (err == ESP_OK) {
      tgSend(CONFIG_NOTIFY_TELEGRAM_ALERT_OTA, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_OTA_OK, err);
    } else {
      tgSend(CONFIG_NOTIFY_TELEGRAM_ALERT_OTA, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_OTA_FAILED, err);
    };
    #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_OTA

    msTaskDelay(CONFIG_MQTT_OTA_DELAY);
    rlog_i(tagOTA, "******************* Restart system! *******************");
    esp_restart();
  };
}

#endif // CONFIG_MQTT_OTA_ENABLE

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------- Commands ------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_MQTT_COMMAND_ENABLE

void paramsExecCmd(char *topic, char *payload)
{
  rlog_i(tagPARAMS, "Command received: [ %s ]", payload);
  
  #if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_COMMAND
    tgSend(CONFIG_NOTIFY_TELEGRAM_ALERT_COMMAND, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_CMD, payload);
  #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_COMMAND

  // If the data is received from MQTT, remove the value from the topic
  if (topic) {
    mqttUnsubscribe(topic);
    mqttPublish(topic, nullptr, CONFIG_MQTT_COMMAND_QOS, CONFIG_MQTT_COMMAND_RETAINED, true, false, false);
    mqttSubscribe(topic, CONFIG_MQTT_COMMAND_QOS);
  };

  // Built-in command: reload controller
  if (strcasecmp(payload, CONFIG_MQTT_CMD_REBOOT) == 0) {
    rlog_i(tagOTA, "******************* Restart system! *******************");
    msTaskDelay(3000);
    esp_restart();
  } 
  // Custom commands
  else {
    // Send a command to the main loop for custom processing
    eventLoopPost(RE_SYSTEM_EVENTS, RE_SYS_COMMAND, payload, strlen(payload)+1, portMAX_DELAY);
  };
}

#endif // CONFIG_MQTT_COMMAND_ENABLE

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Store new value ---------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED

void paramsTelegramNotify(paramsEntryHandle_t entry, bool notify, const char* notify_template, char* value)
{
  if (value) {
    if ((entry->group) && (entry->group->friendly) && (entry->group->key)) {
      tgSend(notify, CONFIG_TELEGRAM_DEVICE, notify_template, entry->group->friendly, entry->friendly, entry->group->key, entry->key, value);
    } else {
      tgSend(notify, CONFIG_TELEGRAM_DEVICE, notify_template, "", entry->friendly, CONFIG_MQTT_COMMON_TOPIC, entry->key, value);
    };
  } else {
    if ((entry->group) && (entry->group->friendly) && (entry->group->key)) {
      tgSend(notify, CONFIG_TELEGRAM_DEVICE, notify_template, entry->group->friendly, entry->friendly, entry->group->key, entry->key, "");
    } else {
      tgSend(notify, CONFIG_TELEGRAM_DEVICE, notify_template, "", entry->friendly, CONFIG_MQTT_COMMON_TOPIC, entry->key, "");
    };
  };
}
#endif // CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY

void paramsValueStore(paramsEntryHandle_t entry, const bool callHandler)
{
  OPTIONS_LOCK();
  if ((entry) && (entry->type_param == OPT_KIND_PARAMETER)) {
    // Save the value in the storage
    if ((entry->group) && (entry->group->key)) {
      nvsWrite(entry->group->key, entry->key, entry->type_value, entry->value);
    };
    // Post event
    if (callHandler) eventLoopPost(RE_PARAMS_EVENTS, RE_PARAMS_INTERNAL, &entry->id, sizeof(entry->id), portMAX_DELAY);
    // Call change handler
    if ((callHandler) && (entry->handler)) entry->handler->onChange(PARAM_SET_INTERNAL);      
    // Publish the current value
    paramsMqttPublish(entry, true);
    // Send notification to telegram
    #if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED
      char* tg_value = value2string(entry->type_value, entry->value);
      if (tg_value) {
        paramsTelegramNotify(entry, CONFIG_NOTIFY_TELEGRAM_ALERT_PARAM_CHANGED, CONFIG_MESSAGE_TG_PARAM_CHANGE, tg_value);
        free(tg_value);
      };
    #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
  };
  ledSysActivity();
  OPTIONS_UNLOCK();
}

void _paramsValueSet(paramsEntryHandle_t entry, char *value, bool publish_in_mqtt)
{
  rlog_i(tagPARAMS, "Received new value [ %s ] for parameter \"%s.%s\"", value, entry->group->key, entry->key);
  
  // Convert the resulting value to the target format
  void *new_value = string2value(entry->type_value, value);
  if (new_value) {
    // If the new value is different from what is already written in the variable...
    if (equal2value(entry->type_value, entry->value, new_value)) {
      rlog_i(tagPARAMS, "Received value does not differ from existing one, ignored");
      // Post event
      eventLoopPost(RE_PARAMS_EVENTS, RE_PARAMS_EQUALS, &entry->id, sizeof(entry->id), portMAX_DELAY);
      // Publish the current value
      paramsMqttPublish(entry, publish_in_mqtt);
      // Send notification to telegram
      #if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED
        paramsTelegramNotify(entry, CONFIG_NOTIFY_TELEGRAM_ALERT_PARAM_CHANGED, CONFIG_MESSAGE_TG_PARAM_EQUAL, value);
      #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
    } else {
      // Block context switching to other tasks to prevent reading the value while it is changing
      vTaskSuspendAll();
      // Set the new value to the variable
      setNewValue(entry->type_value, entry->value, new_value);
      // Restoring the scheduler
      xTaskResumeAll();
      // Save the value in the storage
      if ((entry->group) && (entry->group->key)) {
        nvsWrite(entry->group->key, entry->key, entry->type_value, entry->value);
      };
      // Publish the current value
      paramsMqttPublish(entry, publish_in_mqtt);
      // Post event
      eventLoopPost(RE_PARAMS_EVENTS, RE_PARAMS_CHANGED, &entry->id, sizeof(entry->id), portMAX_DELAY);
      // Call change handler
      if (entry->handler) entry->handler->onChange(PARAM_SET_CHANGED);      
      // Send notification to telegram
      #if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED
        paramsTelegramNotify(entry, CONFIG_NOTIFY_TELEGRAM_ALERT_PARAM_CHANGED, CONFIG_MESSAGE_TG_PARAM_CHANGE, value);
      #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
    };
  } else {
    rlog_e(tagPARAMS, "Could not convert value [ %s ]!", value);
    // Send notification to telegram
    #if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED
      paramsTelegramNotify(entry, CONFIG_NOTIFY_TELEGRAM_ALERT_PARAM_CHANGED, CONFIG_MESSAGE_TG_PARAM_BAD, value);
    #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
  };
  if (new_value) free(new_value);
}

void paramsValueSet(paramsEntryHandle_t entry, char *new_value, bool publish_in_mqtt)
{
  OPTIONS_LOCK();
  if ((entry) && (entry->type_param == OPT_KIND_PARAMETER)) {
    _paramsValueSet(entry, new_value, publish_in_mqtt);
  };
  OPTIONS_UNLOCK();
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------ MQTT public functions ------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void paramsMqttIncomingMessage(char *topic, char *payload, size_t len)
{
  OPTIONS_LOCK();

  if (paramsList) {
    paramsEntryHandle_t item;
    STAILQ_FOREACH(item, paramsList, next) {
      if (strcasecmp(item->topic_subscribe, topic) == 0) {
        switch (item->type_param) {
          case OPT_KIND_OTA:
            #if CONFIG_MQTT_OTA_ENABLE
            if ((topic) && (payload) && (strcmp(payload, "") != 0)) {
              paramsStartOTA(topic, payload);
            };
            #endif // CONFIG_MQTT_OTA_ENABLE
            break;
          
          case OPT_KIND_COMMAND:
            #if CONFIG_MQTT_COMMAND_ENABLE
            if ((topic) && (payload) && (strcmp(payload, "") != 0)) {
              paramsExecCmd(topic, payload);
            };
            #endif // CONFIG_MQTT_COMMAND_ENABLE
            break;

          default:
            _paramsValueSet(item, payload, false);
            break;
        };
        OPTIONS_UNLOCK();
        return;
      };
    };
  };

  rlog_w(tagPARAMS, "MQTT message from topic [ %s ] was not processed!", topic);
  #if CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
    tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_MQTT_NOT_PROCESSED, topic, payload);
  #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY

  OPTIONS_UNLOCK();
}

void paramsMqttSubscribesOpen(bool mqttPrimary, bool forcedResubscribe)
{
  if (mqttIsConnected()) {
    rlog_i(tagPARAMS, "Subscribing to parameter topics...");

    OPTIONS_LOCK();
    ledSysOn(true);

    bool _resubscribe = forcedResubscribe || (_paramsMqttPrimary != mqttPrimary);
    _paramsMqttPrimary = mqttPrimary;

    if (paramsList) {
      paramsEntryHandle_t item;
      STAILQ_FOREACH(item, paramsList, next) {
        if (_resubscribe && !item->subscribed) {
          item->subscribed = _paramsMqttSubscribe(item);
        };
        vTaskDelay(1);
      };
    };

    ledSysOff(true);
    OPTIONS_UNLOCK();
  };
}

void paramsMqttSubscribesClose()
{
  rlog_i(tagPARAMS, "Resetting parameter topics...");

  OPTIONS_LOCK();
  ledSysOn(true);

  // If there is a connection to the broker, you should complete it correctly
  if (mqttIsConnected() && (paramsList)) {
    paramsEntryHandle_t item;
    STAILQ_FOREACH(item, paramsList, next) {
     _paramsMqttUnubscribe(item);
     vTaskDelay(1);
    };
  };

  // Free wildcard topic 
  #if CONFIG_MQTT_PARAMS_WILDCARD
    paramsMqttFreeWildcard();
  #endif // CONFIG_MQTT_PARAMS_WILDCARD

  // Free all topics
  if (paramsList) {
    paramsEntryHandle_t item;
    STAILQ_FOREACH(item, paramsList, next) {
      paramsMqttTopicsFreeEntry(item);
      item->subscribed = false;
    };
  };

  ledSysOff(true);
  OPTIONS_UNLOCK();
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Events handlers ---------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

/*
static void paramsWiFiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  // STA disconnected
  if ((event_id == RE_WIFI_STA_DISCONNECTED) || (event_id == RE_WIFI_STA_STOPPED)) {
    paramsMqttSubscribesClose();
  };
}
*/

static void paramsMqttEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  // MQTT connected
  if (event_id == RE_MQTT_CONNECTED) {
    re_mqtt_event_data_t* data = (re_mqtt_event_data_t*)event_data;
    paramsMqttSubscribesOpen(data->primary, true);
  } 
  // MQTT disconnected
  else if ((event_id == RE_MQTT_CONN_LOST) || (event_id == RE_MQTT_CONN_FAILED)) {
    paramsMqttSubscribesClose();
  }
  // MQTT incomng message
  else if (event_id == RE_MQTT_INCOMING_DATA) {
    re_mqtt_incoming_data_t* data = (re_mqtt_incoming_data_t*)event_data;
    // Process incomng message
    paramsMqttIncomingMessage(data->topic, data->data, data->data_len);
    // Since only string pointers are sent through the event dispatcher, you must manually delete the strings
    if (data->topic) free(data->topic);
    if (data->data) free(data->data);
  };
}

bool paramsEventHandlerRegister()
{
  return eventHandlerRegister(RE_MQTT_EVENTS, ESP_EVENT_ANY_ID, &paramsMqttEventHandler, nullptr);
}

