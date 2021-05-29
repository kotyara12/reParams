#include "reParams.h"
#include <string.h>
#include "rStrings.h"
#include "rLog.h"
#include "reEsp32.h"
#include "reNvs.h"
#include "reMqtt.h"
#include "reLedSys.h"
#include "sys/queue.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#if CONFIG_MQTT_OTA_ENABLE
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#endif // CONFIG_MQTT_OTA_ENABLE
#if CONFIG_TELEGRAM_ENABLE
#include "reTgSend.h"
#endif // CONFIG_TELEGRAM_ENABLE

typedef struct paramsEntry_t {
  param_kind_t type_param;
  param_type_t type_value;
  param_change_callback_t on_change;
  const char* friendly;
  const char* group;
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

STAILQ_HEAD(paramsHead_t, paramsEntry_t);
typedef struct paramsHead_t *paramsHeadHandle_t;

static paramsHeadHandle_t paramsList = nullptr;
static SemaphoreHandle_t paramsLock = nullptr;

#define OPTIONS_LOCK() do {} while (xSemaphoreTake(paramsLock, portMAX_DELAY) != pdPASS)
#define OPTIONS_UNLOCK() xSemaphoreGive(paramsLock)

static const char* tagPARAMS = "PARAMS";

#if CONFIG_MQTT_PARAMS_WILDCARD
  char* _paramsTopic = nullptr;
#endif // CONFIG_MQTT_PARAMS_WILDCARD

#if CONFIG_SILENT_MODE_ENABLE

static uint32_t tsSilentMode = CONFIG_SILENT_MODE_INTERVAL;
static bool stateSilentMode = false;
silent_mode_change_callback_t cbSilentMode = nullptr;
static const char* tagSM = "SILENT MODE";

#endif // CONFIG_SILENT_MODE_ENABLE

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

    paramsList = new paramsHead_t;
    if (paramsList) {
      STAILQ_INIT(paramsList);
    }
    else {
      vSemaphoreDelete(paramsLock);
      rlog_e(tagPARAMS, "Parameters manager initialization error!");
      return false;
    }
  };
  
  #if CONFIG_MQTT_OTA_ENABLE
  paramsRegValue(OPT_KIND_OTA, OPT_TYPE_STRING, nullptr,
    CONFIG_MQTT_SYSTEM_TOPIC, CONFIG_MQTT_OTA_TOPIC, CONFIG_MQTT_OTA_NAME, 
    CONFIG_MQTT_OTA_QOS, nullptr);
  #endif // CONFIG_MQTT_OTA_ENABLE

  #if CONFIG_MQTT_COMMAND_ENABLE
  paramsRegValue(OPT_KIND_COMMAND, OPT_TYPE_STRING, nullptr,
    CONFIG_MQTT_SYSTEM_TOPIC, CONFIG_MQTT_COMMAND_TOPIC, CONFIG_MQTT_COMMAND_NAME, 
    CONFIG_MQTT_COMMAND_QOS, nullptr);
  #endif // CONFIG_MQTT_COMMAND_ENABLE

  #if CONFIG_SILENT_MODE_ENABLE  
  paramsRegValue(OPT_KIND_PARAMETER, OPT_TYPE_TIMESPAN, nullptr,
    CONFIG_MQTT_COMMON_TOPIC, CONFIG_SILENT_MODE_TOPIC, CONFIG_SILENT_MODE_NAME,
    CONFIG_MQTT_PARAMS_QOS, (void*)&tsSilentMode);
  #endif // CONFIG_SILENT_MODE_ENABLE

  return true;
}

void paramsFree()
{
  OPTIONS_LOCK();

  if (paramsList) {
    paramsEntryHandle_t item, tmp;
    STAILQ_FOREACH_SAFE(item, paramsList, next, tmp) {
      STAILQ_REMOVE(paramsList, item, paramsEntry_t, next);
      if (item->topic) {
        mqttUnsubscribe(item->topic);
        free(item->topic);
      };
      delete item;
    };
    delete paramsList;
  };

  OPTIONS_UNLOCK();
  
  vSemaphoreDelete(paramsLock);
}

#if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED

void paramsMqttPublishConfirm(paramsEntryHandle_t entry)
{
  if (entry->type_param == OPT_KIND_PARAMETER) {
    if (entry->value) {
      // Generating a topic for a publication
      if (!entry->confirm) {
        entry->confirm = mqttGetTopic(CONFIG_MQTT_CONFIRM_TOPIC, entry->group, entry->key);
      };
      // Publish the current values
      if (entry->confirm) {
        mqttPublish(entry->confirm, 
          value2string(entry->type_value, entry->value), 
          CONFIG_MQTT_CONFIRM_QOS, CONFIG_MQTT_CONFIRM_RETAINED, 
          true, false, true);
      } else {
        rlog_e(tagPARAMS, "Failed to generate confirmation topic!");
      };
    } else {
      rlog_w(tagPARAMS, "Call publication parameter of undetermined value!");
    };
  };
}

#endif  // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED

bool paramsMqttSubscribeTry(char * topic, const uint8_t qos)
{
  if (topic) {
    rlog_d(tagPARAMS, "Try subscribe to topic: %s", topic);
    if (mqttSubscribe(topic, qos)) {
      return true;
    } else {
      rlog_w(tagPARAMS, "Failed subscribe to topic [ %s ]!", topic);
      free(topic);
    };
  } else {
    rlog_e(tagPARAMS, "Failed subscribe to topic: topic is null!");
  };
  return false;
}

void paramsMqttSubscribeEntry(paramsEntryHandle_t entry)
{
  // Generating a topic for a subscription
  char * _topic = nullptr;
  if (entry->type_param == OPT_KIND_PARAMETER) {
    _topic = mqttGetTopic(CONFIG_MQTT_PARAMS_TOPIC, entry->group, entry->key);
  } else {
    _topic = mqttGetTopic(entry->group, entry->key, nullptr);
  };
  if (_topic) {
    #if CONFIG_MQTT_PARAMS_WILDCARD
      // Subscription using # is valid for parameters only
      if (entry->type_param == OPT_KIND_PARAMETER) {
        // Subscribe once
        if (!_paramsTopic) {
          char * _paramsTemp = mqttGetTopic(CONFIG_MQTT_PARAMS_TOPIC, "#", nullptr);
          if (paramsMqttSubscribeTry(_paramsTemp, CONFIG_MQTT_PARAMS_QOS)) {
            _paramsTopic = _paramsTemp;
            entry->topic = _topic;
          };
        } else {
          entry->topic = _topic;
        };
      } else {
        // We subscribe to management topics separately
        if (paramsMqttSubscribeTry(_topic, entry->qos)) {
          entry->topic = _topic;
        };
      };
    #else
      // We succeeded in subscribing, we save the topic to identify incoming messages
      if (paramsMqttSubscribeTry(_topic, entry->qos)) {
        entry->topic = _topic;
      };
    #endif // CONFIG_MQTT_PARAMS_WILDCARD
  };
}

void paramsRegValue(const param_kind_t type_param, const param_type_t type_value, param_change_callback_t callback_change,
  const char* name_group, const char* name_key, const char* name_friendly, const int qos, 
  void * value)
{
  if (!paramsList) {
    paramsInit();
  };

  OPTIONS_LOCK();

  if (paramsList) {
    paramsEntryHandle_t item = new paramsEntry_t;
    item->type_param = type_param;
    item->type_value = type_value;
    item->on_change = callback_change;
    item->friendly = name_friendly;
    item->group = name_group;
    item->key = name_key;
    item->topic = nullptr;
    #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
    item->confirm = nullptr;
    #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
    item->qos = qos;
    item->value = value;
    // Append item to list
    STAILQ_INSERT_TAIL(paramsList, item, next);
    // Read value from NVS storage
    if (item->type_param == OPT_KIND_PARAMETER) {
      nvsRead(item->group, item->key, item->type_value, item->value);
      if (item->on_change) item->on_change();

      char* str_value = value2string(item->type_value, item->value);
      if (item->group) {
        rlog_d(tagPARAMS, "Parameter \"%s.%s\": [%s] registered", item->group, item->key, str_value);
      } else {
        rlog_d(tagPARAMS, "Parameter \"%s\": [%s] registered", item->key, str_value);
      };
      free(str_value);
      // We send the current value to the confirmation topic
      #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      if (mqttIsConnected()) {
        paramsMqttPublishConfirm(item);
      };
      #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
    } else {
      rlog_d(tagPARAMS, "System handler \"%s\" registered", item->key);
    };
    // We try to subscribe if the connection to the server is already established
    paramsMqttSubscribeEntry(item);
  };
  
  OPTIONS_UNLOCK();
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------------- OTA ---------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_MQTT_OTA_ENABLE

extern const char ota_pem_start[] asm(CONFIG_MQTT_OTA_PEM_START);
extern const char ota_pem_end[]   asm(CONFIG_MQTT_OTA_PEM_END); 

static const char* tagOTA = "OTA";

void paramsStartOTA(char *topic, uint8_t *payload, size_t len)
{
  if (strlen((char*)payload) > 0) {
    rlog_i(tagOTA, "OTA firmware upgrade received from \"%s\"", (char*)payload);
    #if CONFIG_TELEGRAM_ENABLE
    tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_OTA, (char*)payload);
    #endif // CONFIG_TELEGRAM_ENABLE

    // Resetting the value
    mqttUnsubscribe(topic);
    mqttPublish(topic, nullptr, CONFIG_MQTT_OTA_QOS, CONFIG_MQTT_OTA_RETAINED, true, false, false);
    mqttSubscribe(topic, CONFIG_MQTT_OTA_QOS);

    msTaskDelay(CONFIG_MQTT_OTA_DELAY);

    esp_http_client_config_t cfgOTA;
    memset(&cfgOTA, 0, sizeof(cfgOTA));
    cfgOTA.use_global_ca_store = false;
    cfgOTA.cert_pem = (char*)ota_pem_start;
    cfgOTA.skip_cert_common_name_check = true;
    cfgOTA.url = (char*)payload;
    cfgOTA.is_async = false;

    uint8_t tryUpdate = 0;
    esp_err_t err = ESP_OK;
    ledSysOff(true);
    ledSysStateSet(SYSLED_OTA, true);
    do {
      tryUpdate++;
      rlog_i(tagOTA, "Start of firmware upgrade from \"%s\", attempt %d", (char*)payload, tryUpdate);
      err = esp_https_ota(&cfgOTA);
      if (err == ESP_OK) {
        rlog_i(tagOTA, "Firmware upgrade completed!");
      } else {
        rlog_e(tagOTA, "Firmware upgrade failed: %d!", err);
      };
    } while ((err != ESP_OK) && (tryUpdate < CONFIG_MQTT_OTA_ATTEMPTS));

    #if CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_OTA_NOTIFY
    if (err == ESP_OK) {
      tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_OTA_OK, err);
    } else {
      tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_OTA_FAILED, err);
    };
    #endif // CONFIG_MQTT_OTA_TG_NOTIFY

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

void paramsExecCmd(char *topic, uint8_t *payload, size_t len)
{
  rlog_i(tagPARAMS, "Command received: [ %s ]", (char*)payload);
  
  #if CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_COMMAND_NOTIFY
  tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_CMD, (char*)payload);
  #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_COMMAND_NOTIFY

  // Resetting the value
  mqttUnsubscribe(topic);
  mqttPublish(topic, nullptr, CONFIG_MQTT_COMMAND_QOS, CONFIG_MQTT_COMMAND_RETAINED, true, false, false);
  mqttSubscribe(topic, CONFIG_MQTT_COMMAND_QOS);

  // restart controller
  if (strcasecmp((char*)payload, CONFIG_MQTT_CMD_REBOOT) == 0) {
    rlog_i(tagOTA, "******************* Restart system! *******************");
    esp_restart();
  };
}

#endif // CONFIG_MQTT_COMMAND_ENABLE

void paramsSetValue(paramsEntryHandle_t entry, uint8_t *payload, size_t len)
{
  rlog_i(tagPARAMS, "Received parameter [ %s ] from topic \"%s\"", (char*)payload, entry->topic);
  
  // Convert the resulting value to the target format
  void *new_value = string2value(entry->type_value, (char*)payload);
  if (new_value) {
    // If the new value is different from what is already written in the variable...
    if (equal2value(entry->type_value, entry->value, new_value)) {
      rlog_i(tagPARAMS, "Received value does not differ from existing one, ignored");
      // We send the current value to the confirmation topic
      #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      paramsMqttPublishConfirm(entry);
      #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      // We send a notification to telegram
      #if CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
      tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_PARAM_EQUAL, 
        entry->friendly, entry->group, entry->key);
      #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
    } else {
      // We block context switching to other tasks to prevent reading the value while it is changing
      vTaskSuspendAll();
      // We write the new value to the variable
      setNewValue(entry->type_value, entry->value, new_value);
      if (entry->on_change) entry->on_change();      
      // Restoring the scheduler
      xTaskResumeAll();
      // We save the resulting value in the storage
      nvsWrite(entry->group, entry->key, entry->type_value, entry->value);
      // We send the current value to the confirmation topic
      #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      paramsMqttPublishConfirm(entry);
      #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      // We send a notification to telegram
      #if CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
      char* tg_value = value2string(entry->type_value, entry->value);
      tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_PARAM_CHANGE, 
        entry->friendly, entry->group, entry->key, tg_value);
      if (tg_value) free(tg_value);
      #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
    };
  } else {
    rlog_e(tagPARAMS, "Could not convert value [ %s ]!", (char*)payload);
    // We send a notification to telegram
    #if CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
    tgSend(true, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_PARAM_BAD, 
      entry->friendly, entry->group, entry->key, (char*)payload);
    #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_TELEGRAM_PARAM_CHANGE_NOTIFY
  };
  if (new_value) free(new_value);
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------------- MQTT --------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void paramsMqttIncomingMessage(char *topic, uint8_t *payload, size_t len)
{
  OPTIONS_LOCK();
  ledSysOn(true);  

  if (paramsList) {
    paramsEntryHandle_t item;
    STAILQ_FOREACH(item, paramsList, next) {
      if (strcasecmp(item->topic, topic) == 0) {
        switch (item->type_param) {
          case OPT_KIND_OTA:
            #if CONFIG_MQTT_OTA_ENABLE
            paramsStartOTA(topic, payload, len);
            #endif // CONFIG_MQTT_OTA_ENABLE
            break;
          
          case OPT_KIND_COMMAND:
            #if CONFIG_MQTT_COMMAND_ENABLE
            paramsExecCmd(topic, payload, len);
            #endif // CONFIG_MQTT_COMMAND_ENABLE
            break;

          default:
            paramsSetValue(item, payload, len);
            break;
        };
        ledSysOff(true);
        OPTIONS_UNLOCK();
        return;
      };
    };
  };

  rlog_w(tagPARAMS, "MQTT message from topic [ %s ] was not processed!", topic);
  ledSysOff(true);
  OPTIONS_UNLOCK();
}

void paramsMqttSubscribing()
{
  rlog_i(tagPARAMS, "Subscribing to parameter topics...");

  OPTIONS_LOCK();

  if (paramsList) {
    // Recovering subscriptions to topics for which there was no subscription earlier
    paramsEntryHandle_t item;
    STAILQ_FOREACH(item, paramsList, next) {
      if (item->topic == nullptr) {
        paramsMqttSubscribeEntry(item);
        vTaskDelay(CONFIG_MQTT_SUBSCRIBE_INTERVAL / portTICK_RATE_MS);
      };
    };
  };

  OPTIONS_UNLOCK();
}

#if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED

void paramsMqttConfirmations()
{
  rlog_i(tagPARAMS, "Publication of current settings...");

  OPTIONS_LOCK();

  if (paramsList) {
    paramsEntryHandle_t item;
    STAILQ_FOREACH(item, paramsList, next) {
      if ((item->type_param == OPT_KIND_PARAMETER) && (item->confirm == nullptr)) {
        paramsMqttPublishConfirm(item);
        vTaskDelay(0);
      };
    };
  };

  OPTIONS_UNLOCK();
}

#endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED

void paramsMqttSubscribesOpen()
{
  if (mqttIsConnected()) {
    #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
    paramsMqttConfirmations();
    #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED

    paramsMqttSubscribing();
  };
}

void paramsMqttSubscribesClose()
{
  rlog_i(tagPARAMS, "Resetting parameter topics...");

  OPTIONS_LOCK();

  // Free global params topic
  #if CONFIG_MQTT_PARAMS_WILDCARD
    if (_paramsTopic) {
      free(_paramsTopic);
      _paramsTopic = nullptr;
    };
  #endif // CONFIG_MQTT_PARAMS_WILDCARD

  if (paramsList) {
    // Delete all topics from the heap
    paramsEntryHandle_t item;
    STAILQ_FOREACH(item, paramsList, next) {
      #if CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      free(item->confirm);
      item->confirm = nullptr;
      #endif // CONFIG_MQTT_PARAMS_CONFIRM_ENABLED
      free(item->topic);
      item->topic = nullptr;
    };
  };

  OPTIONS_UNLOCK();
}

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Silent mode -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_SILENT_MODE_ENABLE

bool isSilentMode()
{
  return stateSilentMode;
}

void silentModeSetCallback(silent_mode_change_callback_t cb)
{
  cbSilentMode = cb;
}

void silentModeCheck(const struct tm timeinfo)
{
  if (tsSilentMode > 0) {
    uint16_t t1 = tsSilentMode / 10000;
    uint16_t t2 = tsSilentMode % 10000;
    int16_t  t0 = timeinfo.tm_hour * 100 + timeinfo.tm_min;
    bool newSilentMode = (t1 < t2) ? ((t0 >= t1) && (t0 < t2)) : !((t0 >= t2) && (t1 > t0));
    // If the regime has changed
    if (stateSilentMode != newSilentMode) {
      stateSilentMode = newSilentMode;
      // Switching the system LED (take care of the rest yourself)  
      ledSysSetEnabled(!stateSilentMode);
      // Calling the callback function
      if (cbSilentMode) {
        cbSilentMode(stateSilentMode);
      };
      // Sending alerts
      if (stateSilentMode) {
        rlog_i(tagSM, "Silent mode activated");
        #if CONFIG_SILENT_MODE_TG_NOTIFY
        tgSend(CONFIG_SILENT_MODE_TG_MSG_NOTIFY, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_SILENT_MODE_ON);
        #endif // CONFIG_SILENT_MODE_TG_NOTIFY
      } else {
        rlog_i(tagSM, "Silent mode disabled");
        #if CONFIG_SILENT_MODE_TG_NOTIFY
        tgSend(CONFIG_SILENT_MODE_TG_MSG_NOTIFY, CONFIG_TELEGRAM_DEVICE, CONFIG_MESSAGE_TG_SILENT_MODE_OFF);
        #endif // CONFIG_SILENT_MODE_TG_NOTIFY
      };
    };
  };
}

#endif // CONFIG_SILENT_MODE_ENABLE

