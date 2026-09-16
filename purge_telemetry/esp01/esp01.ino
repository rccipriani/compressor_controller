// ESP-01 supervisory gateway: no relay/sensor GPIO control.
#include <ESP8266WiFi.h>
#include <AsyncMqttClient.h>
#include <time.h>
#include <coredecls.h>
#include <errno.h>
#include "config.h"
#include "Protocol.h"

const char ESP_FIRMWARE[] = "1.2.0";
AsyncMqttClient mqtt;
Uptime espUptime;
LineReader<240> uartLine;
char onlineTopic[128], statusCommand[128], purgeCommand[128], durationCommand[128];
char clientId[32];
bool connecting = false, haveController = false, requestStatus = false;
uint32_t lastConnect = 0, lastWifi = 0, lastSeen = 0, lastPublish = 0;
uint32_t lastCommand = 0, publishStarted = 0;
uint32_t pendingPurge = 0, lastPurgeRequest = 0, purgeQueuedAt = 0;
char purgeResult[48] = "";
bool pendingIsDuration = false, resultIsDuration = false, lastRequestIsDuration = false;
uint32_t pendingDuration = 0, configuredDuration = 0, configSeen = 0;
bool haveConfig = false;
uint16_t inFlight = 0;
byte topicIndex = 0;
uint32_t unoUptime = 0, count = 0, duration = 0;
char state[12] = "UNKNOWN", firmware[20] = "", success[8] = "unknown";
char lastTime[24] = "";
bool timeValid = false;
bool ntpSynchronized = false, timeSendPending = false;
uint32_t lastNtpSync = 0, lastTimeSent = 0, lastUartByte = 0;
bool unoClockValid = false;
uint32_t unoSyncEpoch = 0, unoSyncAge = 0, clockSeen = 0, lastEpoch = 0;
long unoCorrection = 0;

bool validHistory(uint32_t epoch, const char* flag) {
  return (!strcmp(flag,"0") && epoch == 0) ||
         (!strcmp(flag,"1") && epoch >= 1704067200UL);
}
void updateHistory(uint32_t epoch, const char* flag) {
  lastEpoch = epoch; timeValid = !strcmp(flag,"1");
  lastTime[0] = 0;
  if (timeValid) {
    time_t instant = epoch;
    struct tm utc;
    gmtime_r(&instant, &utc);
    strftime(lastTime,sizeof lastTime,"%Y-%m-%dT%H:%M:%SZ",&utc);
  }
}

void receiveLine(char* line) {
  // Required v1 fields are ordered. Reject partial/extra fields atomically.
  char u[12], c[12], d[12], st[12], su[8], fw[20], e[12], v[3];
  int end = 0;
  uint32_t newUptime, newCount, newDuration, epoch;
  if (sscanf(line, "STAT uptime=%11s state=%11s count=%11s duration_ms=%11s success=%7s firmware=%19s epoch=%11s time_valid=%2s%n",
             u, st, c, d, su, fw, e, v, &end) == 8 && line[end] == 0 &&
      parseDecimal(u, newUptime) && parseDecimal(c, newCount) &&
      parseDecimal(d, newDuration) && parseDecimal(e, epoch) && validHistory(epoch,v) &&
      (!strcmp(st,"IDLE") || !strcmp(st,"STARTUP") || !strcmp(st,"PURGING")) &&
      (!strcmp(su,"unknown") || !strcmp(su,"true") || !strcmp(su,"false"))) {
    if (haveController && (newUptime < unoUptime || newCount < count)) {
      unoClockValid = false; haveConfig = false; pendingPurge = 0;
    }
    unoUptime = newUptime; count = newCount; duration = newDuration;
    strcpy(state, st); strcpy(success, su); strcpy(firmware, fw);
    updateHistory(epoch,v);
    haveController = true; lastSeen = millis();
    return;
  }
  end = 0;
  if (sscanf(line, "EVENT purge_complete count=%11s duration_ms=%11s success=%7s epoch=%11s time_valid=%2s%n",
             c, d, su, e, v, &end) == 5 && line[end] == 0 &&
      parseDecimal(c, newCount) && parseDecimal(d, newDuration) && newCount &&
      parseDecimal(e,epoch) && validHistory(epoch,v) &&
      (!strcmp(su,"unknown") || !strcmp(su,"true") || !strcmp(su,"false"))) {
    count = newCount; duration = newDuration; strcpy(success, su);
    updateHistory(epoch,v); // authoritative Uno history, no receipt-time substitute
    return;
  }
  char correction[13];
  end=0;
  uint32_t age;
  if (sscanf(line,"CLOCK valid=%2s sync_epoch=%11s sync_age=%11s correction=%12s%n",
             v,e,u,correction,&end)==4 && line[end]==0 &&
      parseDecimal(e,epoch) && parseDecimal(u,age) && validHistory(epoch,v)) {
    char* tail;
    errno=0;
    long delta=strtol(correction,&tail,10);
    const char* digits=correction[0]=='-'?correction+1:correction;
    uint32_t magnitude;
    if (!parseDecimal(digits,magnitude) || errno || *tail) return;
    unoClockValid=!strcmp(v,"1"); unoSyncEpoch=epoch; unoSyncAge=age;
    unoCorrection=delta; clockSeen=millis();
    return;
  }
  if (!strcmp(line,"GET TIME")) timeSendPending=true;
  end=0;
  if (sscanf(line,"CONFIG duration_ms=%11s%n",d,&end)==1 && line[end]==0 &&
      parseDecimal(d,newDuration) && validPurgeDuration(newDuration)) {
    configuredDuration=newDuration; configSeen=millis(); haveConfig=true;
  }
  char result[20], kind[9]; end=0;
  if (sscanf(line,"ACK %8s %11s %19s%n",kind,e,result,&end)==3 && line[end]==0 &&
      !strcmp(kind,lastRequestIsDuration ? "duration" : "purge") &&
      parseDecimal(e,epoch) && epoch==lastPurgeRequest && epoch &&
      (!strcmp(result,"accepted") || !strcmp(result,"startup") || !strcmp(result,"busy") ||
       !strcmp(result,"clock_invalid") || !strcmp(result,"replay") || !strcmp(result,"expired") ||
       !strcmp(result,"out_of_range"))) {
    snprintf(purgeResult,sizeof purgeResult,"%lu %s",(unsigned long)epoch,result);
    resultIsDuration=lastRequestIsDuration;
  }
}

void onMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties,
               size_t len, size_t index, size_t total) {
  if (properties.retain || index || len != total) return;
  uint32_t now=millis();
  if (!strcmp(topic,statusCommand)) {
    if (total==7 && !memcmp(payload,"request",7) && uint32_t(now-lastCommand)>=5000) {
      lastCommand=now; requestStatus=true;
    }
    return;
  }
  bool isDuration = !strcmp(topic,durationCommand);
  if ((!isDuration && strcmp(topic,purgeCommand)) || total>16 || total<10 || !ntpSynchronized ||
      !haveController || uint32_t(now-lastSeen)>=15000 ||
      (!isDuration && strcmp(state,"IDLE")) || pendingPurge ||
      (lastPurgeRequest && uint32_t(now-purgeQueuedAt)<5000)) return;
  char request[17]; memcpy(request,payload,total); request[total]=0;
  // Reject embedded NULs rather than treating a prefix as the entire command.
  if (strlen(request)!=total) return;
  uint32_t epoch, requestedDuration=0;
  int64_t current=time(nullptr);
  if (!parseRemoteRequest(request,isDuration,epoch,requestedDuration) || epoch<=lastPurgeRequest ||
      current-int64_t(epoch)>10 || int64_t(epoch)-current>2 ||
      (isDuration && !validPurgeDuration(requestedDuration))) return;
  pendingPurge=lastPurgeRequest=epoch; purgeQueuedAt=now;
  pendingIsDuration=lastRequestIsDuration=isDuration; pendingDuration=requestedDuration;

}

void publishNext() {
  if (!mqtt.connected() || inFlight || uint32_t(millis()-lastPublish) < 100) return;
  if (purgeResult[0]) {
    char topic[128]; snprintf(topic,sizeof topic,"%s/command/%s_result",MQTT_BASE,resultIsDuration ? "duration" : "purge");
    inFlight=mqtt.publish(topic,1,false,purgeResult);
    publishStarted=lastPublish=millis();
    if (inFlight) purgeResult[0]=0;
    return;
  }
  bool available = haveController && uint32_t(millis()-lastSeen) < 15000;
  const char* suffix = nullptr;
  const char* value = "";
  char number[24];
  switch (topicIndex) {
    case 0: suffix="status/online"; value="true"; break;
    case 1: suffix="status/uptime"; snprintf(number,sizeof number,"%lu",(unsigned long)espUptime.seconds); value=number; break;
    case 2: suffix="status/wifi_rssi"; snprintf(number,sizeof number,"%d",WiFi.RSSI()); value=number; break;
    case 3: suffix="status/firmware"; value=ESP_FIRMWARE; break;
    case 4: suffix="controller/available"; value=available?"true":"false"; break;
    case 5: suffix="controller/uptime"; if(available) {snprintf(number,sizeof number,"%lu",(unsigned long)unoUptime); value=number;} break;
    case 6: suffix="controller/firmware"; value=available?firmware:""; break;
    case 7: suffix="status/state"; value=available?state:"UNKNOWN"; break;
    case 8: suffix="purge/active"; value=available?(!strcmp(state,"IDLE")?"false":"true"):""; break;
    case 9: suffix="purge/count"; if(available) {snprintf(number,sizeof number,"%lu",(unsigned long)count); value=number;} break;
    case 10: suffix="purge/last_duration"; if(available && count) {snprintf(number,sizeof number,"%lu",(unsigned long)duration); value=number;} break;
    case 11: suffix="purge/last_success"; value=available && count && strcmp(success,"unknown")?success:""; break;
    case 12: suffix="purge/last_success_valid"; value=available && count && strcmp(success,"unknown")?"true":"false"; break;
    case 13: suffix="purge/last_time"; value=available && timeValid?lastTime:""; break;
    case 14: suffix="purge/last_time_valid"; value=available && timeValid?"true":"false"; break;
    case 15: suffix="motor/running_valid"; value="false"; break;
    case 16: suffix="motor/rpm_valid"; value="false"; break;
    case 17: suffix="tank/pressure_valid"; value="false"; break;
    case 18: suffix="motor/running"; break; // clear obsolete retained measurements
    case 19: suffix="motor/rpm"; break;
    case 20: suffix="tank/pressure"; break;
    case 21: suffix="command/purge_enabled"; value="true"; break;
    case 22: suffix="purge/last_epoch"; if(available && timeValid) {snprintf(number,sizeof number,"%lu",(unsigned long)lastEpoch); value=number;} break;
    case 23: suffix="time/valid"; value=available && unoClockValid && uint32_t(millis()-clockSeen)<15000?"true":"false"; break;
    case 24: suffix="time/last_sync"; if(available && unoClockValid) {snprintf(number,sizeof number,"%lu",(unsigned long)unoSyncEpoch); value=number;} break;
    case 25: suffix="time/sync_age"; if(available && unoClockValid) {snprintf(number,sizeof number,"%lu",(unsigned long)unoSyncAge); value=number;} break;
    case 26: suffix="time/last_correction"; if(available && unoClockValid) {snprintf(number,sizeof number,"%ld",unoCorrection); value=number;} break;
    case 27: suffix="time/esp_synchronized"; value=ntpSynchronized?"true":"false"; break;
    case 29: suffix="purge/duration_ms"; if(available && haveConfig && uint32_t(millis()-configSeen)<15000) {snprintf(number,sizeof number,"%lu",(unsigned long)configuredDuration); value=number;} break;
    case 28: suffix="time/ntp_sync_age"; if(ntpSynchronized) {snprintf(number,sizeof number,"%lu",(unsigned long)(espUptime.seconds-lastNtpSync)); value=number;} break;
  }
  char topic[128];
  snprintf(topic,sizeof topic,"%s/%s",MQTT_BASE,suffix);
  // QoS1: wait for PUBACK before queuing another publication; bounded memory.
  inFlight = mqtt.publish(topic,1,true,value);
  publishStarted = millis(); lastPublish = millis();
  if (inFlight) topicIndex = (topicIndex + 1) % 30;
}

void setup() {
  Serial.setRxBufferSize(512);
  Serial.begin(9600); // GPIO3 RX, GPIO1 TX; no debug text on this port
  snprintf(onlineTopic,sizeof onlineTopic,"%s/status/online",MQTT_BASE);
  snprintf(statusCommand,sizeof statusCommand,"%s/command/status",MQTT_BASE);
  snprintf(purgeCommand,sizeof purgeCommand,"%s/command/purge",MQTT_BASE);
  snprintf(durationCommand,sizeof durationCommand,"%s/command/duration",MQTT_BASE);
  snprintf(clientId,sizeof clientId,"purge-%06x",ESP.getChipId());
  static_assert(sizeof(MQTT_BASE) < 80, "MQTT base too long");
  mqtt.setServer(MQTT_HOST,MQTT_PORT);
  mqtt.setClientId(clientId);
  mqtt.setCleanSession(true);
  mqtt.setKeepAlive(15);
  mqtt.setWill(onlineTopic,1,true,"false");
  if (MQTT_USER[0]) mqtt.setCredentials(MQTT_USER,MQTT_PASSWORD);
  mqtt.onConnect([](bool) {
    connecting=false; inFlight=0; topicIndex=0;
    mqtt.subscribe(statusCommand,0);
    mqtt.subscribe(purgeCommand,0);
    mqtt.subscribe(durationCommand,0);
  });
  mqtt.onDisconnect([](AsyncMqttClientDisconnectReason) {
    connecting=false; inFlight=0; pendingPurge=0; lastConnect=millis();
  });
  mqtt.onPublish([](uint16_t id) { if(id==inFlight) inFlight=0; });
  mqtt.onMessage(onMessage);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  settimeofday_cb([]() {
    // No other code sets ESP system time; this callback confirms an SNTP update.
    if (time(nullptr)>=1704067200 && uint64_t(time(nullptr))<4102444800ULL) {
      ntpSynchronized=true; timeSendPending=true; lastNtpSync=espUptime.seconds;
    }
  });
  configTime(TIMEZONE,NTP_SERVER); // periodic asynchronous SNTP; no wait
}
void serviceSerial(uint32_t now) {
  for (byte budget=0; budget<64 && Serial.available(); ++budget) {
    lastUartByte=now;
    if (uartLine.push(Serial.read(),now)) receiveLine(uartLine.data);
  }
  // Uno inserts 100-ms interline windows. Wait for 20-ms silence, then
  // send a short reply, avoiding its SoftwareSerial half-duplex TX.
  if (uint32_t(now-lastUartByte)<20) return;
  if (ntpSynchronized && (timeSendPending || uint32_t(now-lastTimeSent)>=600000)) {
    time_t current=time(nullptr);
    if (current>=1704067200 && uint64_t(current)<4102444800ULL) {
      char message[24];
      int size=snprintf(message,sizeof message,"TIME %lu\n",(unsigned long)current);
      if (Serial.availableForWrite()>=size) {
        Serial.write((const uint8_t*)message,size);
        lastTimeSent=now; timeSendPending=false;
        return;
      }
    }
  }
  if (pendingPurge) {
    if (!mqtt.connected() || uint32_t(now-purgeQueuedAt)>1000) pendingPurge=0;
    else {
      char message[40];
      int size=pendingIsDuration
        ? snprintf(message,sizeof message,"CMD duration %lu %lu\n",(unsigned long)pendingPurge,(unsigned long)pendingDuration)
        : snprintf(message,sizeof message,"CMD purge %lu\n",(unsigned long)pendingPurge);
      if (Serial.availableForWrite()>=size) {
        Serial.write((const uint8_t*)message,size); pendingPurge=0;
        return;
      }
    }
  }
  if (requestStatus && Serial.availableForWrite()>=11) {
    Serial.write("CMD status\n"); requestStatus=false;
  }
}
void serviceNetwork(uint32_t now) {
  static bool wasConnected=false;
  bool connected=WiFi.status()==WL_CONNECTED;
  if (connected && !wasConnected) configTime(TIMEZONE,NTP_SERVER);
  wasConnected=connected;
  if (!connected) {
    if(uint32_t(now-lastWifi)>=30000) { lastWifi=now; WiFi.reconnect(); }
  } else if (!mqtt.connected() && !connecting && uint32_t(now-lastConnect)>=5000) {
    lastConnect=now; connecting=true; mqtt.connect();
  }
  if ((connecting && uint32_t(now-lastConnect)>15000) ||
      (inFlight && uint32_t(now-publishStarted)>15000)) {
    mqtt.disconnect(true); connecting=false; inFlight=0; lastConnect=now;
  }
}
void loop() {
  uint32_t now=millis();
  espUptime.tick(now);
  serviceSerial(now);
  serviceNetwork(now);
  publishNext();
  yield();
}
