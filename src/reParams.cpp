#include "reParams.h"
#include <string.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

STAILQ_HEAD(paramsGroupHead_t, paramsGroup_t);
STAILQ_HEAD(paramsEntryHead_t, paramsEntry_t);
typedef struct paramsEntryHead_t *paramsEntryHeadHandle_t;
typedef struct paramsGroupHead_t *paramsGroupHeadHandle_t;

static paramsGroupHeadHandle_t paramsGroups = nullptr;
static paramsEntryHeadHandle_t paramsList = nullptr;
static SemaphoreHandle_t paramsLock = nullptr;

#define OPTIONS_LOCK() do {} while (xSemaphoreTake(paramsLock, portMAX_DELAY) != pdPASS)
#define OPTIONS_UNLOCK() xSemaphoreGive(paramsLock)

static const char* logTAG = "PRMS";

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
      rlog_e(logTAG, "Can't create parameters mutex!");
      return false;
    };

    paramsGroups = (paramsGroupHeadHandle_t)esp_malloc(sizeof(paramsGroupHead_t));
    if (paramsGroups) {
      STAILQ_INIT(paramsGroups);
    }
    else {
      vSemaphoreDelete(paramsLock);
      rlog_e(logTAG, "Parameters manager initialization error!");
      return false;
    }

    paramsList = (paramsEntryHeadHandle_t)esp_malloc(sizeof(paramsEntryHead_t));
    if (paramsList) {
      STAILQ_INIT(paramsList);
    }
    else {
      vSemaphoreDelete(paramsLock);
      rlog_e(logTAG, "Parameters manager initialization error!");
      return false;
    };
  };
  
  #if CONFIG_MQTT_OTA_ENABLE
  paramsRegisterValueEx(OPT_KIND_OTA, OPT_TYPE_STRING, 
    PARAM_HANDLER_NONE, nullptr, 
    nullptr, CONFIG_MQTT_OTA_TOPIC, CONFIG_MQTT_OTA_NAME, 
    CONFIG_MQTT_OTA_QOS, nullptr);
  #endif // CONFIG_MQTT_OTA_ENABLE

  #if CONFIG_MQTT_COMMAND_ENABLE
  paramsRegisterValueEx(OPT_KIND_COMMAND, OPT_TYPE_STRING,
    PARAM_HANDLER_NONE, nullptr, 
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
      rlog_d(logTAG, "Topic for parameter \"%s.%s\" has been scrapped", entry->group->key, entry->key);
    } else {
      rlog_d(logTAG, "Topic for parameter \"%s\" has been scrapped", entry->key);
    };
  };

  if (entry->topic_subscribe) {
    free(entry->topic_subscribe);
    entry->topic_subscribe = nullptr;
  };

  if (entry->topic_publish) {
    free(entry->topic_publish);
    entry->topic_publish = nullptr;
  };
}

void paramsMqttTopicsCreateEntry(paramsEntryHandle_t entry)
{
  if (entry->key) {
    // Parameters always start with the prefix "config", but some parameter groups can be local
    // %LOCATION% / %DEVICE% / CONFI[G|RM] / ...
    if ((entry->type_param == OPT_KIND_PARAMETER) || (entry->type_param == OPT_KIND_PARAMETER_ONLINE)) {
      if ((entry->group) && (entry->group->topic)) {
        entry->topic_subscribe = mqttGetTopicDevice(_paramsMqttPrimary, CONFIG_MQTT_ROOT_PARAMS_LOCAL, CONFIG_MQTT_ROOT_PARAMS_TOPIC, entry->group->topic, entry->key);
        if (entry->topic_subscribe) {
          rlog_d(logTAG, "Generated subscription topic for parameter \"%s.%s\": [ %s ]", entry->group->key, entry->key, entry->topic_subscribe);
        };
      } else {
        entry->topic_subscribe = mqttGetTopicDevice(_paramsMqttPrimary, CONFIG_MQTT_ROOT_PARAMS_LOCAL, CONFIG_MQTT_ROOT_PARAMS_TOPIC, entry->key, nullptr);
        if (entry->topic_subscribe) {
          rlog_d(logTAG, "Generated subscription topic for parameter \"%s\": [ %s ]", entry->key, entry->topic_subscribe);
        };
      };
      if (!entry->topic_subscribe) {
        rlog_e(logTAG, "Failed to generate subscription topic!");
      };
      // Confirmation topic: only for parameters, data and commands do not have confirmation topics
      #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
        // Parameters always start with the prefix "confirm", but some parameter groups can be local
        if ((entry->group) && (entry->group->topic)) {
          entry->topic_publish = mqttGetTopicDevice(_paramsMqttPrimary, CONFIG_MQTT_ROOT_PARAMS_LOCAL, CONFIG_MQTT_ROOT_CONFIRM_TOPIC, entry->group->topic, entry->key);
          if (entry->topic_publish) {
            rlog_d(logTAG, "Generated confirmation topic for parameter \"%s.%s\": [ %s ]", entry->group->key, entry->key, entry->topic_publish);
          };
        } else {
          entry->topic_publish = mqttGetTopicDevice(_paramsMqttPrimary, CONFIG_MQTT_ROOT_PARAMS_LOCAL, CONFIG_MQTT_ROOT_CONFIRM_TOPIC, entry->key, nullptr);
          if (entry->topic_publish) {
            rlog_d(logTAG, "Generated confirmation topic for parameter \"%s\": [ %s ]", entry->key, entry->topic_publish);
          };
        };
        if (!entry->topic_publish) {
          rlog_e(logTAG, "Failed to generate confirmation topic!");
        };
      #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
    } 

    // Parameters related to all devices in a given location do not contain the device name
    // %LOCATION% / CONFIG / ...
    else if (entry->type_param == OPT_KIND_PARAMETER_LOCATION) {
      if ((entry->group) && (entry->group->topic)) {
        entry->topic_subscribe = mqttGetTopicLocation(_paramsMqttPrimary, CONFIG_MQTT_ROOT_PARAMS_LOCAL, CONFIG_MQTT_ROOT_PARAMS_TOPIC, entry->group->topic, entry->key);
        if (entry->topic_subscribe) {
          rlog_d(logTAG, "Generated subscription topic for parameter \"%s.%s\": [ %s ]", entry->group->key, entry->key, entry->topic_subscribe);
        };
      } else {
        entry->topic_subscribe = mqttGetTopicLocation(_paramsMqttPrimary, CONFIG_MQTT_ROOT_PARAMS_LOCAL, CONFIG_MQTT_ROOT_PARAMS_TOPIC, entry->key, nullptr);
        if (entry->topic_subscribe) {
          rlog_d(logTAG, "Generated subscription topic for parameter \"%s\": [ %s ]", entry->key, entry->topic_subscribe);
        };
      };
      if (!entry->topic_subscribe) {
        rlog_e(logTAG, "Failed to generate subscription topic!");
      };
    }

    // Local data starting with the special prefix %LOCAL%
    // %LOCAL% / ... or %LOCAL% / CONFIG_MQTT_ROOT_LOCDATA_TOPIC / ...
    else if ((entry->type_param == OPT_KIND_LOCDATA_ONLINE) || (entry->type_param == OPT_KIND_LOCDATA_STORED)) {
      entry->topic_publish = nullptr;
      if ((entry->group) && (entry->group->topic)) {
        #ifdef CONFIG_MQTT_ROOT_LOCDATA_TOPIC
          entry->topic_subscribe = mqttGetTopicSpecial(_paramsMqttPrimary, CONFIG_MQTT_ROOT_LOCDATA_LOCAL, CONFIG_MQTT_ROOT_LOCDATA_TOPIC, entry->group->topic, entry->key);
        #else
          entry->topic_subscribe = mqttGetTopicLocation(_paramsMqttPrimary, CONFIG_MQTT_ROOT_LOCDATA_LOCAL, entry->group->topic, entry->key, nullptr);
        #endif // CONFIG_MQTT_ROOT_LOCDATA_TOPIC
        if (entry->topic_subscribe) {
          rlog_d(logTAG, "Generated subscription topic for data \"%s.%s\": [ %s ]", entry->group->key, entry->key, entry->topic_subscribe);
        };
      } else {
        #ifdef CONFIG_MQTT_ROOT_LOCDATA_TOPIC
          entry->topic_subscribe = mqttGetTopicSpecial(_paramsMqttPrimary, CONFIG_MQTT_ROOT_LOCDATA_LOCAL, CONFIG_MQTT_ROOT_LOCDATA_TOPIC, entry->key, nullptr);
        #else
          entry->topic_subscribe = mqttGetTopicLocation(_paramsMqttPrimary, CONFIG_MQTT_ROOT_LOCDATA_LOCAL, entry->key, nullptr, nullptr);
        #endif // CONFIG_MQTT_ROOT_LOCDATA_TOPIC
        if (entry->topic_subscribe) {
          rlog_d(logTAG, "Generated subscription topic for data \"%s\": [ %s ]", entry->key, entry->topic_subscribe);
        };
      };
      if (!entry->topic_subscribe) {
        rlog_e(logTAG, "Failed to generate subscription topic!");
      };
    } 

    // External data. Topic is always fixed
    else if ((entry->type_param == OPT_KIND_EXTDATA_ONLINE) || (entry->type_param == OPT_KIND_EXTDATA_STORED)) {
      entry->topic_publish = nullptr;
      if ((entry->group) && (entry->group->topic)) {
        entry->topic_subscribe = mqttGetSubTopic(entry->group->topic, entry->key);
      } else {
        entry->topic_subscribe = malloc_string(entry->key);
      };
      if (entry->topic_subscribe) {
        rlog_d(logTAG, "Generated subscription topic for data \"%s\": [ %s ]", entry->key, entry->topic_subscribe);
      } else {
        rlog_e(logTAG, "Failed to generate subscription topic!");
      };
    }

    // Signals. Topic is // %LOCATION% / %DEVICE% / ... without confirmations
    else if ((entry->type_param == OPT_KIND_SIGNAL) || (entry->type_param == OPT_KIND_SIGNAL_AUTOCLR)) {
      if ((entry->group) && (entry->group->topic)) {
        entry->topic_subscribe = mqttGetTopicDevice(_paramsMqttPrimary, CONFIG_MQTT_ROOT_PARAMS_LOCAL, entry->group->topic, entry->key, nullptr);
        if (entry->topic_subscribe) {
          rlog_d(logTAG, "Generated subscription topic for parameter \"%s.%s\": [ %s ]", entry->group->key, entry->key, entry->topic_subscribe);
        };
      } else {
        entry->topic_subscribe = mqttGetTopicDevice(_paramsMqttPrimary, CONFIG_MQTT_ROOT_PARAMS_LOCAL, entry->key, nullptr, nullptr);
        if (entry->topic_subscribe) {
          rlog_d(logTAG, "Generated subscription topic for parameter \"%s\": [ %s ]", entry->key, entry->topic_subscribe);
        };
      };
      if (!entry->topic_subscribe) {
        rlog_e(logTAG, "Failed to generate subscription topic!");
      };
    }

    // Commands have no groups, always start with prefix "system"
    else {
      entry->topic_publish = nullptr;
      entry->topic_subscribe = mqttGetTopicDevice(_paramsMqttPrimary, CONFIG_MQTT_ROOT_SYSTEM_LOCAL, CONFIG_MQTT_ROOT_SYSTEM_TOPIC, entry->key, nullptr);
      if (entry->topic_subscribe) {
        rlog_d(logTAG, "Generated subscription topic for system command \"%s\": [ %s ]", entry->key, entry->topic_subscribe);
      };
    };
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------ MQTT internal funcions -----------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED

void _paramsMqttConfirmEntry(paramsEntryHandle_t entry)
{
  // Parameters only
  if ((entry->type_param == OPT_KIND_PARAMETER) || (entry->type_param == OPT_KIND_PARAMETER_ONLINE)) {
    if (entry->value) {
      if ((!entry->topic_subscribe) || (!entry->topic_publish)) {
        paramsMqttTopicsFreeEntry(entry);
        paramsMqttTopicsCreateEntry(entry);
      };
      if (entry->topic_publish) {
        mqttPublish(entry->topic_publish, 
          value2string(entry->type_value, entry->value), 
          entry->qos, CONFIG_MQTT_CONFIRM_RETAINED, 
          false, true);
      };
    } else {
      rlog_w(logTAG, "Call publication parameter of undetermined value!");
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
  // Parameters
  if ((entry->type_param == OPT_KIND_PARAMETER) 
   || (entry->type_param == OPT_KIND_PARAMETER_ONLINE) 
   || (entry->type_param == OPT_KIND_PARAMETER_LOCATION)) 
  {
    if (entry->value) {
      if (!entry->topic_subscribe) {
        paramsMqttTopicsFreeEntry(entry);
        paramsMqttTopicsCreateEntry(entry);
      };
      if (entry->topic_subscribe) {
        // mqttUnsubscribe(entry->topic_subscribe);
        entry->locked = true;
        mqttPublish(entry->topic_subscribe, 
          value2string(entry->type_value, entry->value), 
          entry->qos, CONFIG_MQTT_PARAMS_RETAINED, 
          false, true);
        // entry->subscribed = mqttSubscribe(entry->topic_subscribe, entry->qos);
      };
    } else {
      rlog_w(logTAG, "Call publication parameter of undetermined value!");
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
  _paramsWildcardTopic = mqttGetTopicDevice(_paramsMqttPrimary, CONFIG_MQTT_ROOT_PARAMS_LOCAL, CONFIG_MQTT_ROOT_PARAMS_TOPIC, "#", nullptr);
  if (_paramsWildcardTopic) {
    rlog_d(logTAG, "Generated subscription topic for all parameters: [ %s ]", _paramsWildcardTopic);
    return mqttSubscribe(_paramsWildcardTopic, CONFIG_MQTT_PARAMS_QOS);
  } else {
    rlog_e(logTAG, "Failed to generate wildcard topic!");
  };
  return false;
}

void paramsMqttFreeWildcard()
{
  if (_paramsWildcardTopic) free(_paramsWildcardTopic);
  _paramsWildcardTopic = nullptr;
  rlog_d(logTAG, "Topics for all parameters has been scrapped");
}

#endif // CONFIG_MQTT_PARAMS_WILDCARD  

void paramsMqttPublish(paramsEntryHandle_t entry, bool publish_in_mqtt)
{
  if (mqttIsConnected()) {
    // Parameters
    if ((entry->type_param == OPT_KIND_PARAMETER) || (entry->type_param == OPT_KIND_PARAMETER_ONLINE)) {
      #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
        _paramsMqttConfirmEntry(entry);
      #else
        if (publish_in_mqtt) {
          _paramsMqttPublishEntry(entry);
        };
      #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
    } else if (entry->type_param == OPT_KIND_PARAMETER_LOCATION) {
      if (publish_in_mqtt) {
        _paramsMqttPublishEntry(entry);
      };
    };
  };
}

bool _paramsMqttSubscribe(paramsEntryHandle_t entry)
{
  // Create new topics
  paramsMqttTopicsFreeEntry(entry);
  paramsMqttTopicsCreateEntry(entry);

  // Publish current value
  if (entry->type_param == OPT_KIND_PARAMETER_LOCATION) {
    // 2022-08-03: Left location parameters only on "reception"
    // _paramsMqttPublishEntry(entry);
  } else {
    #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      if ((entry->type_param == OPT_KIND_PARAMETER) || (entry->type_param == OPT_KIND_PARAMETER_ONLINE)) {
        _paramsMqttConfirmEntry(entry);
      };
    #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
  };

  // Subscribe to topic
  #if CONFIG_MQTT_PARAMS_WILDCARD
    if ((entry->type_param == OPT_KIND_PARAMETER) || (entry->type_param == OPT_KIND_PARAMETER_ONLINE)) {
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
  // Everything except outgoing data
  if (entry->subscribed) {
    #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      if ((entry->type_param == OPT_KIND_PARAMETER) || (entry->type_param == OPT_KIND_PARAMETER_ONLINE)) {
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

void paramsMqttUnubscribe(paramsEntryHandle_t entry)
{
  if (mqttIsConnected()) {
    _paramsMqttUnubscribe(entry);
  };
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
    STAILQ_FOREACH(item, paramsGroups, next) {
      if (name_key) {
        if ((item->parent == parent_group) && (strcasecmp(item->key, name_key) == 0)) {
          return item;
        };
      } else {
        if ((item->parent == parent_group) && (item->key == nullptr)) {
          return item;
        };
      };
    };

    item = (paramsGroupHandle_t)esp_calloc(1, sizeof(paramsGroup_t));
    if (item) {
      item->parent = parent_group;
      if (item->parent) {
        if (item->parent->key) {
          item->key = malloc_stringf(CONFIG_MESSAGE_TG_PARAM_GROUP_DELIMITER, item->parent->key, name_key);
        } else {
          item->key = (char*)name_key;
        };
        if (item->parent->friendly) {
          item->friendly = malloc_stringf(CONFIG_MESSAGE_TG_PARAM_FIENDLY_DELIMITER, item->parent->friendly, name_friendly);
        } else {
          item->friendly = (char*)name_friendly;
        };
        if (item->parent->topic) {
          item->topic = mqttGetSubTopic(item->parent->topic, name_topic);
        } else {
          item->topic = (char*)name_topic;
        };
      } else {
        item->key = (char*)name_key;
        item->friendly = (char*)name_friendly;
        item->topic = (char*)name_topic;
      };
      if ((item->key) && (strlen(item->key) > 15)) {
        rlog_w(logTAG, "The group key name [%s] is too long!", item->key);
      };
      STAILQ_INSERT_TAIL(paramsGroups, item, next);
    };
  };

  OPTIONS_UNLOCK();

  return item;
}

paramsEntryHandle_t paramsRegisterValueEx(const param_kind_t type_param, const param_type_t type_value, 
  param_handler_type_t handler_type, void* change_handler,
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
    STAILQ_FOREACH(item, paramsList, next) {
      if ((item->group == parent_group) && (strcasecmp(item->key, name_key) == 0)) {
        return item;
      };
    };

    item = (paramsEntryHandle_t)esp_calloc(1, sizeof(paramsEntry_t));
    if (item) {
      if (value) {
        item->id = (uint32_t)value;
      } else {
        item->id = 0;
      };
      item->type_param = type_param;
      item->type_value = type_value;
      item->type_handler = handler_type;
      item->handler = change_handler;
      item->friendly = name_friendly;
      item->group = parent_group;
      item->key = name_key;
      item->notify = true;
      item->locked = false;
      item->subscribed = false;
      item->topic_subscribe = nullptr;
      #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      item->topic_publish = nullptr;
      #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      item->qos = qos;
      item->value = value;
      item->min_value = nullptr;
      item->max_value = nullptr;
      // Append item to list
      STAILQ_INSERT_TAIL(paramsList, item, next);
      // Read value from NVS storage
      if ((item->type_param == OPT_KIND_COMMAND) || (item->type_param == OPT_KIND_OTA)) {
        rlog_d(logTAG, "System handler \"%s\" registered", item->key);
      } else if ((item->type_param == OPT_KIND_SIGNAL) || (item->type_param == OPT_KIND_SIGNAL_AUTOCLR)) {
        rlog_d(logTAG, "Signal \"%s\" registered", item->key);
      } else {
        if ((item->type_param == OPT_KIND_PARAMETER) 
         || (item->type_param == OPT_KIND_PARAMETER_LOCATION) 
         || (item->type_param == OPT_KIND_LOCDATA_STORED)
         || (item->type_param == OPT_KIND_EXTDATA_STORED)) 
        {
          void* prev_value = clone2value(item->type_value, item->value);
          if ((item->group) && (item->group->key)) {
            nvsRead(item->group->key, item->key, item->type_value, item->value);
          };
          if (prev_value) {
            if (!equal2value(item->type_value, prev_value, item->value)) {
              if (item->type_handler > PARAM_HANDLER_NONE) {
                if (item->id > 0) {
                  eventLoopPost(RE_PARAMS_EVENTS, RE_PARAMS_RESTORED, &item->id, sizeof(item->id), portMAX_DELAY);
                };
                if ((item->type_handler = PARAM_HANDLER_CLASS) && (item->handler)) {
                  param_handler_t* hdr = (param_handler_t*)item->handler;
                  hdr->onChange(PARAM_NVS_RESTORED);
                } else if ((item->type_handler = PARAM_HANDLER_CALLBACK) && (item->handler)) {
                  params_callback_t cbf = (params_callback_t)item->handler;
                  cbf(item, PARAM_NVS_RESTORED, item->value);
                };
              };
            };
            free(prev_value);
          };
        };

        char* str_value = value2string(item->type_value, item->value);
        if (str_value) {
          if ((item->group) && (item->group->key)) {
            rlog_d(logTAG, "Parameter \"%s.%s\": [%s] registered", item->group->key, item->key, str_value);
          } else {
            rlog_d(logTAG, "Parameter \"%s\": [%s] registered", item->key, str_value);
          };
          free(str_value);
        };
      };
      // We try to subscribe if the connection to the server is already established
      paramsMqttSubscribe(item);
    };
  };
  
  OPTIONS_UNLOCK();

  return item;
}

paramsEntryHandle_t paramsRegisterCommonValueEx(const param_kind_t type_param, const param_type_t type_value, 
  param_handler_type_t handler_type, void* change_handler,
  const char* name_key, const char* name_friendly, const int qos, 
  void * value)
{
  if (!_pgCommon) {
    _pgCommon = paramsRegisterGroup(nullptr, CONFIG_MQTT_COMMON_TOPIC, CONFIG_MQTT_COMMON_TOPIC, CONFIG_MQTT_COMMON_FIENDLY);
  };

  if (_pgCommon) {
    return paramsRegisterValueEx(type_param, type_value, 
      handler_type, change_handler, 
      _pgCommon, name_key, name_friendly, qos, value);
  };

  return nullptr;
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------- Limits --------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void paramsSetLimitsI8(paramsEntryHandle_t entry, int8_t min_value, int8_t max_value)
{
  if (entry) {
    entry->min_value = (int8_t*)esp_calloc(1, sizeof(int8_t));
    if (entry->min_value) {
      *(int8_t*)entry->min_value = min_value;
    };
    entry->max_value = (int8_t*)esp_calloc(1, sizeof(int8_t));
    if (entry->max_value) {
      *(int8_t*)entry->max_value = max_value;
    };
  };
}

void paramsSetLimitsU8(paramsEntryHandle_t entry, uint8_t min_value, uint8_t max_value)
{
  if (entry) {
    entry->min_value = (uint8_t*)esp_calloc(1, sizeof(uint8_t));
    if (entry->min_value) {
      *(uint8_t*)entry->min_value = min_value;
    };
    entry->max_value = (uint8_t*)esp_calloc(1, sizeof(uint8_t));
    if (entry->max_value) {
      *(uint8_t*)entry->max_value = max_value;
    };
  };
}

void paramsSetLimitsI16(paramsEntryHandle_t entry, int16_t min_value, int16_t max_value)
{
  if (entry) {
    entry->min_value = (int16_t*)esp_calloc(1, sizeof(int16_t));
    if (entry->min_value) {
      *(int16_t*)entry->min_value = min_value;
    };
    entry->max_value = (int16_t*)esp_calloc(1, sizeof(int16_t));
    if (entry->max_value) {
      *(int16_t*)entry->max_value = max_value;
    };
  };
}

void paramsSetLimitsU16(paramsEntryHandle_t entry, uint16_t min_value, uint16_t max_value)
{
  if (entry) {
    entry->min_value = (uint16_t*)esp_calloc(1, sizeof(uint16_t));
    if (entry->min_value) {
      *(uint16_t*)entry->min_value = min_value;
    };
    entry->max_value = (uint16_t*)esp_calloc(1, sizeof(uint16_t));
    if (entry->max_value) {
      *(uint16_t*)entry->max_value = max_value;
    };
  };
}

void paramsSetLimitsI32(paramsEntryHandle_t entry, int32_t min_value, int32_t max_value)
{
  if (entry) {
    entry->min_value = (int32_t*)esp_calloc(1, sizeof(int32_t));
    if (entry->min_value) {
      *(int32_t*)entry->min_value = min_value;
    };
    entry->max_value = (int32_t*)esp_calloc(1, sizeof(int32_t));
    if (entry->max_value) {
      *(int32_t*)entry->max_value = max_value;
    };
  };
}

void paramsSetLimitsU32(paramsEntryHandle_t entry, uint32_t min_value, uint32_t max_value)
{
  if (entry) {
    entry->min_value = (uint32_t*)esp_calloc(1, sizeof(uint32_t));
    if (entry->min_value) {
      *(uint32_t*)entry->min_value = min_value;
    };
    entry->max_value = (uint32_t*)esp_calloc(1, sizeof(uint32_t));
    if (entry->max_value) {
      *(uint32_t*)entry->max_value = max_value;
    };
  };
}

void paramsSetLimitsI64(paramsEntryHandle_t entry, int64_t min_value, int64_t max_value)
{
  if (entry) {
    entry->min_value = (int64_t*)esp_calloc(1, sizeof(int64_t));
    if (entry->min_value) {
      *(int64_t*)entry->min_value = min_value;
    };
    entry->max_value = (int64_t*)esp_calloc(1, sizeof(int64_t));
    if (entry->max_value) {
      *(int64_t*)entry->max_value = max_value;
    };
  };
}

void paramsSetLimitsU64(paramsEntryHandle_t entry, uint64_t min_value, uint64_t max_value)
{
  if (entry) {
    entry->min_value = (uint64_t*)esp_calloc(1, sizeof(uint64_t));
    if (entry->min_value) {
      *(uint64_t*)entry->min_value = min_value;
    };
    entry->max_value = (uint64_t*)esp_calloc(1, sizeof(uint64_t));
    if (entry->max_value) {
      *(uint64_t*)entry->max_value = max_value;
    };
  };
}

void paramsSetLimitsFloat(paramsEntryHandle_t entry, float min_value, float max_value)
{
  if (entry) {
    entry->min_value = (float*)esp_calloc(1, sizeof(float));
    if (entry->min_value) {
      *(float*)entry->min_value = min_value;
    };
    entry->max_value = (float*)esp_calloc(1, sizeof(float));
    if (entry->max_value) {
      *(float*)entry->max_value = max_value;
    };
  };
}

void paramsSetLimitsDouble(paramsEntryHandle_t entry, double min_value, double max_value)
{
  if (entry) {
    entry->min_value = (double*)esp_calloc(1, sizeof(double));
    if (entry->min_value) {
      *(double*)entry->min_value = min_value;
    };
    entry->max_value = (double*)esp_calloc(1, sizeof(double));
    if (entry->max_value) {
      *(double*)entry->max_value = max_value;
    };
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------------- OTA ---------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_MQTT_OTA_ENABLE

static const char* tagOTA = "OTA";

void paramsStartOTA(char *topic, char *payload)
{
  if ((payload) && (strlen(payload) > 0)) {
    rlog_i(tagOTA, "OTA firmware upgrade received from \"%s\"", payload);

    // If the data is received from MQTT, remove the value from the topic
    if (topic) {
      mqttUnsubscribe(topic);
      vTaskDelay(1);
      mqttPublish(topic, nullptr, CONFIG_MQTT_OTA_QOS, CONFIG_MQTT_OTA_RETAINED, false, false);
      vTaskDelay(1);
      mqttSubscribe(topic, CONFIG_MQTT_OTA_QOS);
    };

    // Start OTA task
    otaStart(malloc_string(payload));
  };
}

#endif // CONFIG_MQTT_OTA_ENABLE

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------- Commands ------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_MQTT_COMMAND_ENABLE

void paramsExecCmd(char *topic, char *payload)
{
  if (payload && (strlen(payload) > 0)) {
    rlog_i(logTAG, "Command received: [ %s ]", payload);
    
    #if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_COMMAND
      tgSend(MK_MAIN, CONFIG_NOTIFY_TELEGRAM_COMMAND_PRIORITY, CONFIG_NOTIFY_TELEGRAM_ALERT_COMMAND, CONFIG_TELEGRAM_DEVICE, 
        CONFIG_MESSAGE_TG_CMD, payload);
    #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_COMMAND

    // If the data is received from MQTT, remove the value from the topic
    if (topic) {
      mqttUnsubscribe(topic);
      vTaskDelay(1);
      mqttPublish(topic, nullptr, CONFIG_MQTT_COMMAND_QOS, CONFIG_MQTT_COMMAND_RETAINED, false, false);
      vTaskDelay(1);
      mqttSubscribe(topic, CONFIG_MQTT_COMMAND_QOS);
    };

    // Built-in command: reload controller
    if (strcasecmp(payload, CONFIG_MQTT_CMD_REBOOT) == 0) {
      msTaskDelay(3000);
      espRestart(RR_COMMAND_RESET);
    } 
    // Custom commands
    else {
      // Send a command to the main loop for custom processing
      eventLoopPost(RE_SYSTEM_EVENTS, RE_SYS_COMMAND, payload, strlen(payload)+1, portMAX_DELAY);
    };
  };
}

#endif // CONFIG_MQTT_COMMAND_ENABLE

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------- Signals -------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void paramsProcessSignal(paramsEntryHandle_t item, char *payload)
{
  if (item->topic_subscribe && payload && (strlen(payload) > 0)) {
    rlog_i(logTAG, "Received signal [ %s ] in topic \"%s\"", payload, item->topic_subscribe);
    
    // Post event and call change handler
    if (item->type_handler > PARAM_HANDLER_NONE) {
      if (item->id > 0) {
        eventLoopPost(RE_PARAMS_EVENTS, RE_PARAMS_CHANGED, &item->id, sizeof(item->id), portMAX_DELAY);
      };
      if (item->handler) {
        if (item->type_handler == PARAM_HANDLER_CLASS) {
          param_handler_t* hdr = (param_handler_t*)item->handler;
          hdr->onChange(PARAM_SET_CHANGED);
        } else if (item->type_handler == PARAM_HANDLER_CALLBACK) {
          params_callback_t cbf = (params_callback_t)item->handler;
          cbf(item, PARAM_SET_CHANGED, payload);
        };
      };
    };

    // Clear topic
    if (item->type_param == OPT_KIND_SIGNAL_AUTOCLR) {
      mqttUnsubscribe(item->topic_subscribe);
      vTaskDelay(1);
      mqttPublish(item->topic_subscribe, nullptr, item->qos, false, false, false);
      vTaskDelay(1);
      mqttSubscribe(item->topic_subscribe, item->qos);
    };
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Store new value ---------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED

void paramsTelegramNotify(paramsEntryHandle_t entry, msg_priority_t priority, bool notify, const char* notify_template, char* value)
{
  if (value) {
    if ((entry->group) && (entry->group->friendly) && (entry->group->key)) {
      tgSendMsg(encMsgOptions(MK_PARAMS, notify, priority), CONFIG_TELEGRAM_DEVICE, 
        notify_template, entry->group->friendly, entry->friendly, entry->group->key, entry->key, value);
    } else {
      tgSendMsg(encMsgOptions(MK_PARAMS, notify, priority), CONFIG_TELEGRAM_DEVICE, 
        notify_template, "", entry->friendly, CONFIG_MQTT_COMMON_TOPIC, entry->key, value);
    };
  } else {
    if ((entry->group) && (entry->group->friendly) && (entry->group->key)) {
      tgSendMsg(encMsgOptions(MK_PARAMS, notify, priority), CONFIG_TELEGRAM_DEVICE, 
        notify_template, entry->group->friendly, entry->friendly, entry->group->key, entry->key, "");
    } else {
      tgSendMsg(encMsgOptions(MK_PARAMS, notify, priority), CONFIG_TELEGRAM_DEVICE, 
        notify_template, "", entry->friendly, CONFIG_MQTT_COMMON_TOPIC, entry->key, "");
    };
  };
}
#endif // CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY

void paramsValueStore(paramsEntryHandle_t entry, const bool callHandler)
{
  OPTIONS_LOCK();
  if (entry) {
    if ((entry->type_param != OPT_KIND_COMMAND) && (entry->type_param != OPT_KIND_OTA)
      && (entry->type_param != OPT_KIND_SIGNAL) && (entry->type_param != OPT_KIND_SIGNAL_AUTOCLR)) {
      // Save the value in the storage
      if (((entry->type_param == OPT_KIND_PARAMETER) 
        || (entry->type_param == OPT_KIND_PARAMETER_LOCATION) 
        || (entry->type_param == OPT_KIND_LOCDATA_STORED)
        || (entry->type_param == OPT_KIND_EXTDATA_STORED)) 
      && (entry->group) && (entry->group->key)) 
      {
        nvsWrite(entry->group->key, entry->key, entry->type_value, entry->value);
      };
      // Post event and call change handler
      if (callHandler) {
        if (entry->type_handler > PARAM_HANDLER_NONE) {
          if (entry->id > 0) {
            eventLoopPost(RE_PARAMS_EVENTS, RE_PARAMS_INTERNAL, &entry->id, sizeof(entry->id), portMAX_DELAY);
          };
          if ((entry->type_handler = PARAM_HANDLER_CLASS) && (entry->handler)) {
            param_handler_t* hdr = (param_handler_t*)entry->handler;
            hdr->onChange(PARAM_SET_INTERNAL);
          } else if ((entry->type_handler = PARAM_HANDLER_CALLBACK) && (entry->handler)) {
            params_callback_t cbf = (params_callback_t)entry->handler;
            cbf(entry, PARAM_SET_INTERNAL, entry->value);
          };
        };
      };
      // Publish the current value
      paramsMqttPublish(entry, true);
      // Send notification
      if (entry->notify && ((entry->type_param == OPT_KIND_PARAMETER) 
                         || (entry->type_param == OPT_KIND_PARAMETER_ONLINE) 
                         || (entry->type_param == OPT_KIND_PARAMETER_LOCATION))) {
        // Send notification to telegram
        #if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED
          char* tg_value = value2string(entry->type_value, entry->value);
          if (tg_value) {
            paramsTelegramNotify(entry, CONFIG_NOTIFY_TELEGRAM_PARAM_PRIORITY, CONFIG_NOTIFY_TELEGRAM_ALERT_PARAM_CHANGED, 
              CONFIG_MESSAGE_TG_PARAM_CHANGE, tg_value);
            free(tg_value);
          };
        #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED
      };
    };
  };
  #if CONFIG_SYSLED_MQTT_ACTIVITY
  ledSysActivity();
  #endif // CONFIG_SYSLED_MQTT_ACTIVITY
  OPTIONS_UNLOCK();
}

void _paramsValueSet(paramsEntryHandle_t entry, char *value, bool publish_in_mqtt)
{
  rlog_i(logTAG, "Received new value [ %s ] for parameter \"%s.%s\"", value, entry->group->key, entry->key);
  
  // Convert the resulting value to the target format
  void *new_value = string2value(entry->type_value, value);
  if (new_value) {
    // If the new value is different from what is already written in the variable...
    if (equal2value(entry->type_value, entry->value, new_value)) {
      rlog_i(logTAG, "Received value does not differ from existing one, ignored");
      // Post event
      if ((entry->type_handler > PARAM_HANDLER_NONE) && (entry->id > 0)) {
        eventLoopPost(RE_PARAMS_EVENTS, RE_PARAMS_EQUALS, &entry->id, sizeof(entry->id), portMAX_DELAY);
      };
      // Publish value
      paramsMqttPublish(entry, publish_in_mqtt);
      // Send notification
      if (entry->notify && ((entry->type_param == OPT_KIND_PARAMETER) || (entry->type_param == OPT_KIND_PARAMETER_ONLINE))) {
        // Send notification to telegram
        #if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED
          paramsTelegramNotify(entry, CONFIG_NOTIFY_TELEGRAM_PARAM_PRIORITY, CONFIG_NOTIFY_TELEGRAM_ALERT_PARAM_CHANGED, 
            CONFIG_MESSAGE_TG_PARAM_EQUAL, value);
        #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED
      };
    } else {
      // Check the new value and possibly correct it to be valid
      if (valueCheckLimits(entry->type_value, new_value, entry->min_value, entry->max_value)) {
        // Block context switching to other tasks to prevent reading the value while it is changing
        vTaskSuspendAll();
        // Set the new value to the variable
        setNewValue(entry->type_value, entry->value, new_value);
        // Restoring the scheduler
        xTaskResumeAll();
        // Save the value in the storage
        if (((entry->type_param == OPT_KIND_PARAMETER) 
          || (entry->type_param == OPT_KIND_PARAMETER_LOCATION) 
          || (entry->type_param == OPT_KIND_LOCDATA_STORED)
          || (entry->type_param == OPT_KIND_EXTDATA_STORED)) 
         && (entry->group) && (entry->group->key)) 
        {
          nvsWrite(entry->group->key, entry->key, entry->type_value, entry->value);
        };
        // Post event and call change handler
        if (entry->type_handler > PARAM_HANDLER_NONE) {
          if (entry->id > 0) {
            eventLoopPost(RE_PARAMS_EVENTS, RE_PARAMS_CHANGED, &entry->id, sizeof(entry->id), portMAX_DELAY);
          };
          if ((entry->type_handler = PARAM_HANDLER_CLASS) && (entry->handler)) {
            param_handler_t* hdr = (param_handler_t*)entry->handler;
            hdr->onChange(PARAM_SET_CHANGED);
          } else if ((entry->type_handler = PARAM_HANDLER_CALLBACK) && (entry->handler)) {
            params_callback_t cbf = (params_callback_t)entry->handler;
            cbf(entry, PARAM_SET_CHANGED, entry->value);
          };
        };
        // Only for parameters...
        paramsMqttPublish(entry, publish_in_mqtt);
        // Send notification
        if (entry->notify && ((entry->type_param == OPT_KIND_PARAMETER) 
                           || (entry->type_param == OPT_KIND_PARAMETER_ONLINE) 
                           || (entry->type_param == OPT_KIND_PARAMETER_LOCATION))) {
          // Send notification to telegram
          #if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED
            paramsTelegramNotify(entry, CONFIG_NOTIFY_TELEGRAM_PARAM_PRIORITY, CONFIG_NOTIFY_TELEGRAM_ALERT_PARAM_CHANGED, 
              CONFIG_MESSAGE_TG_PARAM_CHANGE, value);
          #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED
        };
      } else {
        rlog_w(logTAG, "Received value [ %s ] is out of range, ignored!", value);
        // Only for parameters...
        paramsMqttPublish(entry, publish_in_mqtt);
        // Send notification
        if (entry->notify && ((entry->type_param == OPT_KIND_PARAMETER) 
                           || (entry->type_param == OPT_KIND_PARAMETER_ONLINE) 
                           || (entry->type_param == OPT_KIND_PARAMETER_LOCATION))) {
          // Send notification to telegram
          #if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED
            paramsTelegramNotify(entry, CONFIG_NOTIFY_TELEGRAM_PARAM_PRIORITY, CONFIG_NOTIFY_TELEGRAM_ALERT_PARAM_CHANGED, 
              CONFIG_MESSAGE_TG_PARAM_INVALID, value);
          #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED
        };
      };
    };
  } else {
    rlog_e(logTAG, "Could not convert value [ %s ]!", value);
    // Send notification
    if ((entry->type_param == OPT_KIND_PARAMETER) 
     || (entry->type_param == OPT_KIND_PARAMETER_ONLINE) 
     || (entry->type_param == OPT_KIND_PARAMETER_LOCATION)) {
      // Send notification to telegram
      #if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED
        paramsTelegramNotify(entry, CONFIG_NOTIFY_TELEGRAM_PARAM_PRIORITY, CONFIG_NOTIFY_TELEGRAM_ALERT_PARAM_CHANGED, 
          CONFIG_MESSAGE_TG_PARAM_BAD, value);
      #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED
    };
  };
  if (new_value) free(new_value);
}

void paramsValueSet(paramsEntryHandle_t entry, char *new_value, bool publish_in_mqtt)
{
  OPTIONS_LOCK();
  if (entry) {
    if ((entry->type_param == OPT_KIND_PARAMETER) 
     || (entry->type_param == OPT_KIND_PARAMETER_ONLINE)
     || (entry->type_param == OPT_KIND_PARAMETER_LOCATION) 
     || (entry->type_param == OPT_KIND_LOCDATA_ONLINE) 
     || (entry->type_param == OPT_KIND_LOCDATA_STORED
     || (entry->type_param == OPT_KIND_EXTDATA_ONLINE) 
     || (entry->type_param == OPT_KIND_EXTDATA_STORED))) 
    {
      _paramsValueSet(entry, new_value, publish_in_mqtt);
    };
  };
  OPTIONS_UNLOCK();
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------ MQTT public functions ------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void paramsMqttIncomingMessage(char *topic, char *payload, size_t len)
{
  if ((topic) && (payload)) {
    OPTIONS_LOCK();

    if (paramsList) {
      paramsEntryHandle_t item;
      STAILQ_FOREACH(item, paramsList, next) {
        if (item->topic_subscribe == nullptr) {
          uint8_t tryCnt = 0;
          do {
            tryCnt++;
            paramsMqttTopicsCreateEntry(item);
            if (item->topic_subscribe == nullptr) vTaskDelay(10);
          } while ((item->topic_subscribe == nullptr) && (tryCnt < 255));
        };

        if (item->topic_subscribe != nullptr) {
          if (strcasecmp(item->topic_subscribe, topic) == 0) {
            if (item->locked) {
              item->locked = false;
              rlog_v(logTAG, "Incoming value for locked parameter, ignored");
            } else {
              switch (item->type_param) {
                case OPT_KIND_OTA:
                  #if CONFIG_MQTT_OTA_ENABLE
                  if (strcmp(payload, "") != 0) {
                    paramsStartOTA(topic, payload);
                  };
                  #endif // CONFIG_MQTT_OTA_ENABLE
                  break;
                
                case OPT_KIND_COMMAND:
                  #if CONFIG_MQTT_COMMAND_ENABLE
                  if (strcmp(payload, "") != 0) {
                    paramsExecCmd(topic, payload);
                  };
                  #endif // CONFIG_MQTT_COMMAND_ENABLE
                  break;

                case OPT_KIND_SIGNAL:
                case OPT_KIND_SIGNAL_AUTOCLR:
                  if (strcmp(payload, "") != 0) {
                    paramsProcessSignal(item, payload);
                  };
                  break;

                case OPT_KIND_PARAMETER:
                case OPT_KIND_PARAMETER_ONLINE:
                case OPT_KIND_PARAMETER_LOCATION:
                case OPT_KIND_LOCDATA_ONLINE:
                case OPT_KIND_LOCDATA_STORED:
                case OPT_KIND_EXTDATA_ONLINE:
                case OPT_KIND_EXTDATA_STORED:
                  _paramsValueSet(item, payload, false);
                  break;

                default:
                  break;
              };
            };

            OPTIONS_UNLOCK();
            return;
          };
        };
      };
    };

    rlog_w(logTAG, "MQTT message from topic [ %s ] was not processed!", topic);
    #if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED
      tgSend(MK_SERVICE, CONFIG_NOTIFY_TELEGRAM_PARAM_PRIORITY, CONFIG_NOTIFY_TELEGRAM_ALERT_PARAM_CHANGED, CONFIG_TELEGRAM_DEVICE, 
        CONFIG_MESSAGE_TG_MQTT_NOT_PROCESSED, topic, payload);
    #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_PARAM_CHANGED

    OPTIONS_UNLOCK();
  };
}

void paramsMqttSubscribesOpen(bool mqttPrimary, bool forcedResubscribe)
{
  if (mqttIsConnected()) {
    rlog_i(logTAG, "Subscribing to parameter topics...");

    OPTIONS_LOCK();
    #if CONFIG_SYSLED_MQTT_ACTIVITY
    ledSysOn(true);
    #endif // CONFIG_SYSLED_MQTT_ACTIVITY

    bool _failed = false;
    bool _resubscribe = forcedResubscribe || (_paramsMqttPrimary != mqttPrimary);
    _paramsMqttPrimary = mqttPrimary;

    if (paramsList) {
      paramsEntryHandle_t item;
      STAILQ_FOREACH(item, paramsList, next) {
        if (_resubscribe && !item->subscribed) {
          uint8_t i = 0;
          while (mqttIsConnected() && (i < 100) && (mqttGetOutboxSize() > 1024)) {
            if (i == 0) { 
              rlog_v(logTAG, "Waiting for previous data to be sent from outbox..."); 
            };
            vTaskDelay(10);
            i++;
          };
          if (mqttIsConnected()) {
            item->subscribed = _paramsMqttSubscribe(item);
          } else {
            rlog_d(logTAG, "Connection to MQTT broker was unexpectedly lost");
            _failed = true;
            break;
          };
        };
        vTaskDelay(1);
      };
    };

    #if CONFIG_SYSLED_MQTT_ACTIVITY
    ledSysOff(true);
    #endif // CONFIG_SYSLED_MQTT_ACTIVITY
    OPTIONS_UNLOCK();

    if (_failed) {
      paramsMqttSubscribesClose();
      mqttTaskRestart();
    };
  };
}

void paramsMqttSubscribesClose()
{
  rlog_i(logTAG, "Resetting parameter topics...");

  OPTIONS_LOCK();
  #if CONFIG_SYSLED_MQTT_ACTIVITY
  ledSysOn(true);
  #endif // CONFIG_SYSLED_MQTT_ACTIVITY

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

  #if CONFIG_SYSLED_MQTT_ACTIVITY
  ledSysOff(true);
  #endif // CONFIG_SYSLED_MQTT_ACTIVITY
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
    if (event_data) {
      re_mqtt_event_data_t* data = (re_mqtt_event_data_t*)event_data;
      paramsMqttSubscribesOpen(data->primary, true);
    };
  } 
  // MQTT disconnected
  else if ((event_id == RE_MQTT_CONN_LOST) || (event_id == RE_MQTT_CONN_FAILED)) {
    paramsMqttSubscribesClose();
  }
  // MQTT incomng message
  else if (event_id == RE_MQTT_INCOMING_DATA) {
    if (event_data) {
      re_mqtt_incoming_data_t* data = (re_mqtt_incoming_data_t*)event_data;
      // Process incomng message
      paramsMqttIncomingMessage(data->topic, data->data, data->data_len);
      // Since only string pointers are sent through the event dispatcher, you must manually delete the strings
      if (data->topic) free(data->topic);
      if (data->data) free(data->data);
    };
  };
}

bool paramsEventHandlerRegister()
{
  return eventHandlerRegister(RE_MQTT_EVENTS, ESP_EVENT_ANY_ID, &paramsMqttEventHandler, nullptr);
}

