#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pet_device_sessions.h"

static pet_device_sessions_t m;
static void receive(const char *topic, const char *json, uint64_t now) {
  cJSON *p = cJSON_Parse(json);
  assert(p);
  assert(pet_device_sessions_handle(&m,topic,p,now));
  cJSON_Delete(p);
}
int main(void) {
  receive("session/current","{\"agentId\":\"codex\",\"sessions\":["
    "{\"id\":\"a\",\"title\":\"会话甲\",\"state\":\"thinking\",\"transitionRevision\":1},"
    "{\"id\":\"b\",\"title\":\"会话乙\",\"state\":\"working\",\"transitionRevision\":1}]}",100);
  assert(m.count == 2 && !strcmp(pet_device_sessions_lifecycle(&m),"working"));
  // A frame timestamp can precede a message received later in the same loop.
  pet_device_sessions_tick(&m,99);
  assert(m.count == 2 && !strcmp(pet_device_sessions_lifecycle(&m),"working"));
  assert(pet_device_sessions_select(&m,-1) && m.selected == 1);
  receive("session/current","{\"agentId\":\"codex\",\"sessions\":["
    "{\"id\":\"b\",\"title\":\"会话乙\",\"state\":\"working\",\"transitionRevision\":1},"
    "{\"id\":\"a\",\"title\":\"会话甲\",\"state\":\"done\",\"transitionRevision\":2,\"terminalRemainingMs\":1000}]}",200);
  assert(!strcmp(m.sessions[0].id,"a") && !strcmp(m.sessions[m.selected].id,"b"));
  assert(m.sessions[0].terminal_until_ms == 1200);
  receive("session/current","{\"agentId\":\"codex\",\"sessions\":["
    "{\"id\":\"a\",\"title\":\"会话甲\",\"state\":\"done\",\"transitionRevision\":2,\"terminalRemainingMs\":1000},"
    "{\"id\":\"b\",\"title\":\"会话乙\",\"state\":\"working\",\"transitionRevision\":1}]}",300);
  assert(m.sessions[0].terminal_until_ms == 1200);
  receive("session/current","{\"agentId\":\"codex\",\"sessions\":["
    "{\"id\":\"a\",\"title\":\"会话甲\",\"state\":\"working\",\"transitionRevision\":1},"
    "{\"id\":\"b\",\"title\":\"会话乙\",\"state\":\"working\",\"transitionRevision\":1}]}",400);
  assert(!strcmp(m.sessions[0].state,"done"));
  pet_device_sessions_tick(&m,1200);
  assert(m.count == 1 && !strcmp(m.sessions[0].id,"b"));
  receive("state/claude-code","{\"state\":\"working\"}",1300);
  assert(m.count == 0 && !strcmp(m.agent,"claude-code"));
  receive("session/current","{\"agentId\":\"claude-code\",\"sessions\":["
    "{\"id\":\"history\",\"title\":\"历史任务\",\"state\":\"done\"}]}",1400);
  assert(m.count == 0);
  receive("session/current","{\"agentId\":\"claude-code\",\"sessions\":["
    "{\"id\":\"c\",\"title\":\"新任务\",\"state\":\"working\"},"
    "{\"id\":\"c\",\"title\":\"重复任务\",\"state\":\"working\"}]}",1500);
  assert(m.count == 1);
  cJSON *invalid = cJSON_Parse("{\"agentId\":\"codex\",\"sessions\":{}}");
  assert(!pet_device_sessions_handle(&m,"session/current",invalid,1600));
  assert(m.count == 1 && !strcmp(m.agent,"claude-code"));
  cJSON_Delete(invalid);
  pet_device_sessions_tick(&m,31500);
  assert(!m.count && !strcmp(pet_device_sessions_lifecycle(&m),"idle"));
  char utf8[5];
  pet_device_text_copy(utf8,sizeof(utf8),"中文");
  assert(!strcmp(utf8,"中"));
  receive("session/current","{\"agentId\":\"claude-code\",\"displayEnabled\":false,\"sessions\":["
    "{\"id\":\"c\",\"title\":\"新任务\",\"state\":\"working\"}]}",32000);
  assert(m.count == 0);
  receive("session/current","{\"agentId\":\"claude-code\",\"sessions\":["
    "{\"id\":\"x\",\"title\":\"任务一\",\"state\":\"working\"},"
    "{\"id\":\"y\",\"title\":\"任务二\",\"state\":\"working\"}]}",33000);
  assert(pet_device_sessions_select(&m,1));
  receive("session/current","{\"agentId\":\"claude-code\",\"sessionId\":\"auto\",\"sessions\":["
    "{\"id\":\"x\",\"title\":\"任务一\",\"state\":\"working\"},"
    "{\"id\":\"y\",\"title\":\"任务二\",\"state\":\"working\"}]}",34000);
  assert(!strcmp(pet_device_sessions_selected(&m)->id,"y"));
  receive("speech/text","{\"sessionId\":\"x\",\"displayTitle\":\"更新标题\","
    "\"displayContent\":\"需要确认\",\"status\":\"waiting_user\"}",35000);
  assert(!strcmp(m.sessions[0].content,"需要确认"));
  assert(!strcmp(m.sessions[0].title,"更新标题"));
  assert(!strcmp(pet_device_sessions_lifecycle(&m),"working"));
  receive("speech/text","{\"sessionId\":\"y\",\"displayContent\":\"任务已完成\",\"status\":\"done\"}",36000);
  assert(!strcmp(pet_device_sessions_lifecycle(&m),"done"));
  assert(m.sessions[1].terminal_until_ms == 96000);
  receive("speech/text","{\"sessionId\":\"y\",\"status\":\"done\"}",37000);
  assert(m.sessions[1].terminal_until_ms == 96000);
  memset(&m,0,sizeof(m));
  receive("state/codex","{\"state\":\"working\"}",1000);
  pet_device_sessions_tick(&m,999);
  assert(!strcmp(pet_device_sessions_lifecycle(&m),"working"));
  puts("StopWatch lifecycle, TTL, ordering, navigation, isolation and UTF-8 passed");
}
