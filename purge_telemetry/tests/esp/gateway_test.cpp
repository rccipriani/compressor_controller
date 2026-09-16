#include "../Arduino.h"
#include <string.h>
#include <assert.h>
#include <iostream>
uint32_t testMillis=1000;
int pinStates[20]={};
#include "../../esp01/esp01.ino"
void receive(const char* input) {
 char copy[300]; strcpy(copy,input); receiveLine(copy);
}
int main() {
 setup();
 assert(!ntpSynchronized);
 timeSendPending=true;
 serviceSerial(testMillis);
 assert(Serial.tx.empty()); // plausible host time alone must not authorize TIME
 receive("STAT uptime=123 state=IDLE count=2 duration_ms=5000 success=unknown firmware=1.3.0 epoch=1789088325 time_valid=1");
 assert(haveController && count==2 && timeValid && lastEpoch==1789088325);
 std::string stamp=lastTime;
 assert(!stamp.empty() && stamp.back()=='Z');
 receive("STAT uptime=124 state=IDLE count=999 duration_ms=5000 success=unknown firmware=1.3.0 epoch=0 time_valid=1");
 assert(count==2); // invalid history rejects whole snapshot
 receive("EVENT purge_complete count=3 duration_ms=5000 success=unknown epoch=0 time_valid=0");
 assert(count==3 && !timeValid && lastTime[0]==0);
 receive("EVENT purge_complete count=4294967296 duration_ms=5000 success=unknown epoch=0 time_valid=0");
 assert(count==3);
 receive("CLOCK valid=1 sync_epoch=1789088325 sync_age=600 correction=-8");
 assert(unoClockValid && unoSyncAge==600 && unoCorrection==-8);
 ntpCallback(); assert(ntpSynchronized);
 serviceSerial(testMillis); assert(Serial.tx.find("TIME ")==0);
 Serial.tx.clear(); receive("GET TIME"); serviceSerial(testMillis);
 assert(Serial.tx.find("TIME ")==0);
 AsyncMqttClientMessageProperties properties;
 testMillis=10000; onMessage(purgeCommand,(char*)"request",properties,7,0,7);
 assert(!requestStatus);
 testMillis=16000; properties.retain=true;
 onMessage(statusCommand,(char*)"request",properties,7,0,7); assert(!requestStatus);
 properties.retain=false;
 onMessage(statusCommand,(char*)"request",properties,7,0,7); assert(requestStatus);
 inFlight=0; publishNext(); assert(mqtt.publications==1);
 for(int i=0;i<10000;++i) {++testMillis;publishNext();}
 assert(mqtt.publications==1); // no queue growth while PUBACK is absent
 inFlight=0; topicIndex=4; testMillis=40000; publishNext();
 assert(mqtt.lastTopic.find("controller/available")!=std::string::npos && mqtt.lastPayload=="false");
 // Fresh non-retained timestamp maps to exactly one bounded UART command.
 lastSeen=testMillis; strcpy(state,"IDLE"); Serial.tx.clear();
 char id[11]; snprintf(id,sizeof id,"%lu",(unsigned long)time(nullptr));
 properties.retain=true;
 onMessage(purgeCommand,id,properties,10,0,10); assert(!pendingPurge);
 properties.retain=false;
 onMessage(purgeCommand,id,properties,5,0,10); assert(!pendingPurge);
 onMessage(purgeCommand,(char*)"1000000000",properties,10,0,10); assert(!pendingPurge);
 onMessage(purgeCommand,id,properties,10,0,10); assert(pendingPurge);
 serviceSerial(testMillis);
 assert(Serial.tx==std::string("CMD purge ")+id+"\n" && !pendingPurge);
 testMillis+=6000; lastSeen=testMillis;
 onMessage(purgeCommand,id,properties,10,0,10); assert(!pendingPurge);
 char ack[64]; snprintf(ack,sizeof ack,"ACK purge %s accepted",id); receive(ack);
 inFlight=0; publishNext();
 assert(mqtt.lastTopic.find("command/purge_result")!=std::string::npos);
 assert(mqtt.lastPayload==std::string(id)+" accepted");
 // Report only controller-confirmed settings, clearing stale retained values.
 receive("CONFIG duration_ms=7500"); assert(haveConfig && configuredDuration==7500);
 receive("CONFIG duration_ms=30001"); assert(configuredDuration==7500);
 receive("CONFIG duration_ms=1000 extra"); assert(configuredDuration==7500);
 inFlight=0; topicIndex=29; testMillis+=100; publishNext();
 assert(mqtt.lastPayload=="7500" && mqtt.lastTopic.find("purge/duration_ms")!=std::string::npos);
 testMillis+=16000; inFlight=0; topicIndex=29; publishNext(); assert(mqtt.lastPayload.empty());
 // A duration request may be made while purging, but must be fresh and bounded.
 lastSeen=testMillis; strcpy(state,"PURGING"); Serial.tx.clear();
 snprintf(id,sizeof id,"%lu",(unsigned long)time(nullptr));
 char command[32]; snprintf(command,sizeof command,"%s 12000",id);
 properties.retain=true;
 onMessage(durationCommand,command,properties,16,0,16); assert(!pendingPurge);
 properties.retain=false;
 onMessage(durationCommand,command,properties,8,0,16); assert(!pendingPurge);
 snprintf(command,sizeof command,"%s 30001",id);
 onMessage(durationCommand,command,properties,16,0,16); assert(!pendingPurge);
 snprintf(command,sizeof command,"%s 12000",id);
 onMessage(durationCommand,command,properties,16,0,16); assert(pendingPurge && pendingIsDuration);
 serviceSerial(testMillis);
 assert(Serial.tx==std::string("CMD duration ")+id+" 12000\n");
 assert(configuredDuration==7500); // sending is not confirmation
 snprintf(ack,sizeof ack,"ACK duration %s accepted",id); receive(ack);
 inFlight=0; testMillis+=100; publishNext();
 assert(mqtt.lastTopic.find("command/duration_result")!=std::string::npos);
 receive("CONFIG duration_ms=12000"); assert(configuredDuration==12000);
 testMillis+=6000; lastSeen=testMillis;
 onMessage(durationCommand,command,properties,16,0,16); assert(!pendingPurge);
 // An unsent command expires instead of waiting for later reconnection.
 snprintf(id,sizeof id,"%lu",(unsigned long)time(nullptr));
 snprintf(command,sizeof command,"%s 1000",id);
 onMessage(durationCommand,command,properties,15,0,15); assert(pendingPurge);
 Serial.tx.clear(); testMillis+=1001; serviceSerial(testMillis); assert(!pendingPurge);
 assert(Serial.tx.find("CMD duration")==std::string::npos);
 std::cout<<"PASS: gateway parsing, time sync, bounded publication, stale availability, remote freshness/retention/fragments/replay/UART/ack\n";
}
