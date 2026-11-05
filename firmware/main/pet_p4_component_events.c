/* p4-events-v1 portable core; see pet_p4_component_events.h. */
#include "pet_p4_component_events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
bool pet_cev_name_valid(const char *name) {
  if (!name || !name[0]) return false;
  size_t len = 0;
  while (len < PET_CEV_NAME_MAX && name[len]) len++;
  if (len >= PET_CEV_NAME_MAX) return false;
  for (size_t i = 0; i < len; i++) {
    char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
  }
  return true;
}
void pet_cev_bus_reset(pet_cev_bus_t *bus) { if (bus) memset(bus, 0, sizeof(*bus)); }
static pet_cev_component_t *bus_find(pet_cev_bus_t *bus, const char *widget_id) {
  if (!bus || !widget_id || !widget_id[0]) return NULL;
  for (unsigned i = 0; i < PET_CEV_COMPONENTS; i++) if (bus->items[i].used && !strcmp(bus->items[i].widget_id, widget_id)) return &bus->items[i];
  return NULL;
}
bool pet_cev_bus_subscribe(pet_cev_bus_t *bus, const char *widget_id, const char (*names)[PET_CEV_NAME_MAX], unsigned count) {
  if (!bus || !widget_id || !widget_id[0] || strlen(widget_id) >= PET_CEV_WIDGET_ID_MAX || count > PET_CEV_NAMES_PER_PACKAGE || (count && !names)) return false;
  for (unsigned i = 0; i < count; i++) if (!pet_cev_name_valid(names[i])) return false;
  pet_cev_component_t *c = bus_find(bus, widget_id);
  if (!c) {
    for (unsigned i = 0; i < PET_CEV_COMPONENTS && !c; i++) if (!bus->items[i].used) c = &bus->items[i];
    if (!c) return false;
    memset(c, 0, sizeof(*c));
    strcpy(c->widget_id, widget_id);
    c->used = true;
  }
  c->name_count = (uint8_t) count;
  memset(c->names, 0, sizeof(c->names));
  for (unsigned i = 0; i < count; i++) strcpy(c->names[i], names[i]);
  return true;
}
void pet_cev_bus_unsubscribe(pet_cev_bus_t *bus, const char *widget_id) {
  pet_cev_component_t *c = bus_find(bus, widget_id);
  if (c) memset(c, 0, sizeof(*c));
}
unsigned pet_cev_bus_emit(pet_cev_bus_t *bus, const char *name) {
  if (!bus || !pet_cev_name_valid(name)) return 0;
  unsigned delivered = 0;
  bus->emitted++;
  for (unsigned i = 0; i < PET_CEV_COMPONENTS; i++) {
    pet_cev_component_t *c = &bus->items[i];
    if (!c->used) continue;
    bool wants = false;
    for (unsigned n = 0; n < c->name_count; n++) if (!strcmp(c->names[n], name)) { wants = true; break; }
    if (!wants) continue;
    if (c->count >= PET_CEV_QUEUE) { c->dropped++; bus->dropped++; continue; }
    strcpy(c->queue[(c->head + c->count) % PET_CEV_QUEUE], name);
    c->count++; delivered++; bus->delivered++;
  }
  return delivered;
}
bool pet_cev_bus_pop(pet_cev_bus_t *bus, const char *widget_id, char out[PET_CEV_NAME_MAX]) {
  pet_cev_component_t *c = bus_find(bus, widget_id);
  if (!c || !c->count || !out) return false;
  strcpy(out, c->queue[c->head]);
  c->head = (uint8_t) ((c->head + 1) % PET_CEV_QUEUE);
  c->count--;
  return true;
}
unsigned pet_cev_bus_pending(const pet_cev_bus_t *bus, const char *widget_id) {
  const pet_cev_component_t *c = bus_find((pet_cev_bus_t *) bus, widget_id);
  return c ? c->count : 0;
}
uint32_t pet_cev_bus_dropped(const pet_cev_bus_t *bus, const char *widget_id) {
  const pet_cev_component_t *c = bus_find((pet_cev_bus_t *) bus, widget_id);
  return c ? c->dropped : 0;
}

/* ---- alarms --------------------------------------------------------------- */
void pet_cev_alarms_cancel(pet_cev_alarm_set_t *set, unsigned index) {
  if (!set || index >= set->count || index >= PET_CEV_ALARMS) return;
  pet_cev_alarm_t *a = &set->items[index]; a->armed = false; a->pending = false;
  a->cancelled=true;a->waiting_for_clock=false;a->utc_deadline=false;a->waiting_ms=0;set->clock_changes++;
  if (a->every_ms >= PET_CEV_ALARM_MIN_MS) a->last_var = INT32_MIN;
}
void pet_cev_alarms_start(pet_cev_alarm_set_t *set, uint64_t now_ms) {
  if (!set) return;
  for (unsigned i = 0; i < set->count && i < PET_CEV_ALARMS; i++) {
    pet_cev_alarm_t *a = &set->items[i];
    if (a->every_ms >= PET_CEV_ALARM_MIN_MS && !a->armed && a->last_var != INT32_MIN && now_ms <= UINT64_MAX - a->every_ms) { a->armed = true; a->deadline_ms = now_ms + a->every_ms; }
  }
}
void pet_cev_alarms_observe_var(pet_cev_alarm_set_t *set, unsigned index, int32_t value, uint64_t now_ms) {
  if (!set || index >= set->count || index >= PET_CEV_ALARMS) return;
  pet_cev_alarm_t *a = &set->items[index];
  if (a->at_var < 0 || value == a->last_var) return;
  /* Legacy saves omitted last_var. Baseline the first restored value without
   * replacing the remaining duration; explicit zero still cancels. */
  if (a->last_var == INT32_MIN && (a->armed || a->pending)) {
    a->last_var = value;
    if (value <= 0) { a->armed = false; a->pending = false; }
    return;
  }
  a->last_var = value;
  if (value <= 0) { a->armed = false; a->pending = false; return; }
  if ((uint32_t) value < PET_CEV_ALARM_MIN_MS || (uint32_t)value > 7u * 86400000u) return;
  if (now_ms > UINT64_MAX - (uint32_t)value) return;
  a->pending = false; a->armed = true; a->deadline_ms = now_ms + (uint64_t) (uint32_t) value;
}
unsigned pet_cev_alarms_tick(pet_cev_alarm_set_t *set, uint64_t now_ms) {
  unsigned due = 0;
  if (!set) return 0;
  for (unsigned i = 0; i < set->count && i < PET_CEV_ALARMS; i++) {
    pet_cev_alarm_t *a = &set->items[i];
    if (!a->armed || now_ms < a->deadline_ms) continue;
    a->pending = true; due |= 1u << i;
    if (a->every_ms >= PET_CEV_ALARM_MIN_MS) {
      uint64_t delta = a->every_ms - (now_ms - a->deadline_ms) % a->every_ms;
      if (now_ms > UINT64_MAX - delta) a->armed = false;
      else a->deadline_ms = now_ms + delta;
    }
    else a->armed = false;
  }
  return due;
}
unsigned pet_cev_alarms_take_pending(pet_cev_alarm_set_t *set) {
  unsigned mask = 0;
  if (!set) return 0;
  for (unsigned i = 0; i < set->count && i < PET_CEV_ALARMS; i++) if (set->items[i].pending && !set->items[i].waiting_for_clock) { mask |= 1u << i; set->items[i].pending = false; }
  return mask;
}
size_t pet_cev_alarms_serialize(const pet_cev_alarm_set_t *set, uint64_t now_ms, char *out, size_t cap) {
  if (!set || !out || cap < 2) return 0;
  size_t used = 0; out[0] = 0;
  for (unsigned i = 0; i < set->count && i < PET_CEV_ALARMS; i++) {
    const pet_cev_alarm_t *a = &set->items[i];
    if (!a->armed && !a->pending && a->at_var < 0 && a->last_var != INT32_MIN) continue;
    unsigned long long remaining = a->armed && a->deadline_ms > now_ms ? a->deadline_ms - now_ms : 0;
    bool pending = a->pending || (a->armed && a->deadline_ms <= now_ms);
    char observed[24] = {0};
    if (a->at_var >= 0) snprintf(observed, sizeof(observed), ":v%ld", (long)a->last_var);
    int n = snprintf(out + used, cap - used, "%s%s:%llu%s%s%s", used ? ";" : "", a->id, remaining, pending ? ":p" : "", observed, a->last_var == INT32_MIN && a->at_var < 0 ? ":c" : "");
    if (n < 0 || (size_t) n >= cap - used) { out[0] = 0; return 0; }
    used += (size_t) n;
  }
  return used;
}
bool pet_cev_alarms_restore(pet_cev_alarm_set_t *set, const char *text, uint64_t now_ms) {
  if (!set || !text) return false;
  if (!text[0]) return true;
  size_t length = 0;
  while (length < PET_CEV_SAVE_TEXT_MAX && text[length]) length++;
  if (length >= PET_CEV_SAVE_TEXT_MAX || text[0] == ';' || text[length - 1] == ';' || strstr(text, ";;")) return false;
  pet_cev_alarm_set_t restored = *set;
  char copy[PET_CEV_SAVE_TEXT_MAX];
  strcpy(copy, text);
  char ids[PET_CEV_ALARMS][PET_CEV_NAME_MAX] = {{0}}; unsigned seen = 0;
  char *next = copy;
  for (char *item = copy; item; item = next) {
    next = strchr(item, ';');
    if (next) *next++ = 0;
    char *colon = strchr(item, ':');
    if (!colon) return false;
    *colon = 0;
    char *end = NULL;
    if (colon[1] < '0' || colon[1] > '9') return false;
    errno = 0;
    unsigned long long remaining = strtoull(colon + 1, &end, 10);
    if (errno == ERANGE) return false;
    bool pending = false, cancelled = false, observed = false; int32_t last_var = 0;
    while (*end) {
      if (!strncmp(end, ":p", 2) && (end[2] == ':' || !end[2]) && !pending) { pending = true; end += 2; }
      else if (!strncmp(end, ":c", 2) && !end[2] && !cancelled) { cancelled = true; end += 2; }
      else if (!strncmp(end, ":v", 2) && !observed) {
        char *number = end + 2;
        if (!((*number >= '0' && *number <= '9') || (*number == '-' && number[1] >= '0' && number[1] <= '9'))) return false;
        errno = 0; long value = strtol(number, &end, 10);
        if (errno == ERANGE || value < INT32_MIN || value > INT32_MAX) return false;
        last_var = (int32_t)value; observed = true;
      } else return false;
    }
    if (cancelled && (pending || remaining || observed)) return false;
    if (!pet_cev_name_valid(item) || remaining > 7ULL * 86400000ULL || remaining > UINT64_MAX - now_ms) return false;
    if (seen >= PET_CEV_ALARMS) return false;
    for (unsigned i = 0; i < seen; i++) if (!strcmp(ids[i], item)) return false;
    strcpy(ids[seen++], item);
    for (unsigned i = 0; i < set->count && i < PET_CEV_ALARMS; i++) {
      pet_cev_alarm_t *a = &restored.items[i];
      if (strcmp(a->id, item)) continue;
      if ((observed && a->at_var < 0) || (cancelled && a->at_var >= 0)) return false;
      a->pending = pending; a->armed = false;
      if (observed) a->last_var = last_var;
      else if (a->at_var >= 0 && (remaining || pending)) a->last_var = INT32_MIN;
      if (cancelled) { a->last_var = INT32_MIN; continue; }
      if (remaining || a->every_ms >= PET_CEV_ALARM_MIN_MS) { a->armed = true; a->deadline_ms = now_ms + remaining; }
    }
  }
  *set = restored;
  return true;
}

/* UTC-backed persistence is separate from the legacy uptime API. */
static bool clock_valid(const pet_cev_clock_snapshot_t *c){return c&&c->valid&&c->calibration_generation;}
static void clock_arm(pet_cev_alarm_t *a,uint32_t delay,uint64_t now,const pet_cev_clock_snapshot_t *c){
  a->armed=true;a->pending=false;a->cancelled=false;a->waiting_ms=delay;
  a->utc_deadline=false;a->waiting_for_clock=true;
  if(clock_valid(c)&&c->utc_ms<=UINT64_MAX-delay){a->utc_deadline=true;a->utc_deadline_ms=c->utc_ms+delay;a->waiting_for_clock=false;}
  a->deadline_ms=now<=UINT64_MAX-delay?now+delay:UINT64_MAX;
}
void pet_cev_alarms_start_clock(pet_cev_alarm_set_t *set,uint64_t now,const pet_cev_clock_snapshot_t *c){
  if(!set)return;
  for(unsigned i=0;i<set->count&&i<PET_CEV_ALARMS;i++){
    pet_cev_alarm_t *a=&set->items[i];
    if(a->every_ms>=PET_CEV_ALARM_MIN_MS&&!a->armed&&!a->pending&&!a->cancelled&&a->last_var!=INT32_MIN){clock_arm(a,a->every_ms,now,c);set->clock_changes++;}
  }
}
void pet_cev_alarms_observe_var_clock(pet_cev_alarm_set_t *set,unsigned index,int32_t value,uint64_t now,const pet_cev_clock_snapshot_t *c){
  if(!set||index>=set->count||index>=PET_CEV_ALARMS)return;
  pet_cev_alarm_t *a=&set->items[index];if(a->at_var<0||a->last_var==value)return;
  if(a->last_var==INT32_MIN&&(a->armed||a->pending)){
    a->last_var=value;if(value<=0)pet_cev_alarms_cancel(set,index);set->clock_changes++;return;
  }
  a->last_var=value;set->clock_changes++;
  if(value<=0){pet_cev_alarms_cancel(set,index);return;}
  if((uint32_t)value<PET_CEV_ALARM_MIN_MS||(uint32_t)value>7U*86400000U)return;
  clock_arm(a,(uint32_t)value,now,c);
}
unsigned pet_cev_alarms_tick_clock(pet_cev_alarm_set_t *set,uint64_t now,const pet_cev_clock_snapshot_t *c){
  if(!set)return 0;
  unsigned due=0;
  for(unsigned i=0;i<set->count&&i<PET_CEV_ALARMS;i++){
    pet_cev_alarm_t *a=&set->items[i];
    if(a->cancelled||(!a->armed&&!a->pending))continue;
    if(!clock_valid(c)){if(!a->waiting_for_clock){a->waiting_for_clock=true;if(!a->utc_deadline)a->waiting_ms=a->deadline_ms>now?(uint32_t)(a->deadline_ms-now):0;set->clock_changes++;}continue;}
    bool was_waiting=a->waiting_for_clock;
    if(a->waiting_for_clock){a->waiting_for_clock=false;set->clock_changes++;}
    if(!a->armed)continue;
    if(!a->utc_deadline){
      if(!was_waiting)a->waiting_ms=a->deadline_ms>now?(uint32_t)(a->deadline_ms-now):0;
      if(c->utc_ms>UINT64_MAX-a->waiting_ms){a->waiting_for_clock=true;continue;}
      a->utc_deadline_ms=c->utc_ms+a->waiting_ms;a->utc_deadline=true;set->clock_changes++;
    }
    if(c->utc_ms<a->utc_deadline_ms)continue;
    a->pending=true;due|=1U<<i;set->clock_changes++;
    if(a->every_ms>=PET_CEV_ALARM_MIN_MS){
      uint64_t delta=a->every_ms-(c->utc_ms-a->utc_deadline_ms)%a->every_ms;
      if(c->utc_ms>UINT64_MAX-delta)a->armed=false;
      else{a->utc_deadline_ms=c->utc_ms+delta;a->deadline_ms=now<=UINT64_MAX-delta?now+delta:UINT64_MAX;}
    }else a->armed=false;
  }
  return due;
}
size_t pet_cev_alarms_serialize_clock(const pet_cev_alarm_set_t *set,uint64_t now,const pet_cev_clock_snapshot_t *c,char *out,size_t cap){
  if(!set||!out||cap<2)return 0;
  size_t used=0;
  out[0]=0;
  for(unsigned i=0;i<set->count&&i<PET_CEV_ALARMS;i++){
    const pet_cev_alarm_t *a=&set->items[i];
    if(!a->armed&&!a->pending&&!a->cancelled&&a->at_var<0&&a->last_var!=INT32_MIN)continue;
    bool cancel=a->cancelled||(a->at_var<0&&a->last_var==INT32_MIN&&!a->armed);
    bool utc=a->utc_deadline&&(a->armed||a->pending)&&!cancel;
    bool pending=!cancel&&(a->pending||(a->armed&&utc&&clock_valid(c)&&c->utc_ms>=a->utc_deadline_ms));
    uint64_t number=cancel?0:utc?a->utc_deadline_ms:a->armed?(a->waiting_for_clock?a->waiting_ms:a->deadline_ms>now?a->deadline_ms-now:0):0;
    char observed[24]={0};if(a->at_var>=0)snprintf(observed,sizeof observed,":v%ld",(long)a->last_var);
    const char *basis=utc?":u":a->waiting_for_clock&&!cancel?":w":"";
    int n=snprintf(out+used,cap-used,"%s%s:%llu%s%s%s%s",used?";":"",a->id,(unsigned long long)number,basis,pending?":p":"",observed,cancel?":c":"");
    if(n<0||(size_t)n>=cap-used){out[0]=0;return 0;}used+=(size_t)n;
  }return used;
}
/* Parse twice from immutable input. Validation completes before the first
 * output store; only a tiny one-record token is automatic, not an alarm set. */
bool pet_cev_alarms_restore_clock(pet_cev_alarm_set_t *set,const char *text,uint64_t now,const pet_cev_clock_snapshot_t *c){
  if(!set||!text)return false;
  size_t length=0;
  while(length<PET_CEV_SAVE_TEXT_MAX&&text[length])length++;
  if(length>=PET_CEV_SAVE_TEXT_MAX)return false;
  if(!length)return true;
  for(unsigned pass=0;pass<2;pass++){
    const char *p=text;char seen[PET_CEV_ALARMS][PET_CEV_NAME_MAX]={{0}};unsigned count=0;
    while(*p){
      char id[PET_CEV_NAME_MAX]={0};unsigned n=0;while(*p&&*p!=':'){if(n+1>=sizeof id)return false;id[n++]=*p++;}
      if(!pet_cev_name_valid(id)||*p++!=':'||*p<'0'||*p>'9'||count>=PET_CEV_ALARMS)return false;
      for(unsigned i=0;i<count;i++)if(!strcmp(seen[i],id))return false;
      strcpy(seen[count++],id);
      errno=0;char *end;unsigned long long number=strtoull(p,&end,10);if(errno==ERANGE)return false;p=end;
      bool utc=false,wait=false,pending=false,cancel=false,observed=false;int32_t last=0;
      while(*p==':'){
        p++;char flag=*p++;
        if(flag=='u'&&!utc&&!wait)utc=true;
        else if(flag=='w'&&!utc&&!wait)wait=true;
        else if(flag=='p'&&!pending)pending=true;
        else if(flag=='c'&&!cancel)cancel=true;
        else if(flag=='v'&&!observed){if(!((*p>='0'&&*p<='9')||(*p=='-'&&p[1]>='0'&&p[1]<='9')))return false;errno=0;long long v=strtoll(p,&end,10);if(errno==ERANGE||v<INT32_MIN||v>INT32_MAX)return false;last=(int32_t)v;observed=true;p=end;}
        else return false;
        if(*p&&*p!=':'&&*p!=';')return false;
      }
      if((*p&&*p!=';')||(cancel&&(number||pending))||(!utc&&number>7ULL*86400000ULL)||(utc&&(!number||number>UINT64_C(4103049599000)))||(!utc&&number>UINT64_MAX-now))return false;
      for(unsigned i=0;i<set->count&&i<PET_CEV_ALARMS;i++){
        pet_cev_alarm_t *a=&set->items[i];if(strcmp(id,a->id))continue;
        if(observed&&a->at_var<0)return false;
        if(pass){
          a->pending=pending;a->cancelled=cancel;a->utc_deadline=utc;a->utc_deadline_ms=utc?number:0;a->waiting_ms=utc?0:(uint32_t)number;
          a->armed=!cancel&&(number||a->every_ms>=PET_CEV_ALARM_MIN_MS)&&!(pending&&a->at_var>=0);
          a->waiting_for_clock=!cancel&&(utc||wait)&&(a->armed||pending);
          a->deadline_ms=!utc?now+number:UINT64_MAX;
          if(observed)a->last_var=last;else if(a->at_var>=0&&(number||pending))a->last_var=INT32_MIN;
          if(cancel&&a->at_var<0)a->last_var=INT32_MIN;
          if(!utc&&!wait)a->waiting_for_clock=false; /* legacy remaining format */
        }
      }
      if(*p==';'){p++;if(!*p)return false;}
    }
  }
  set->clock_changes++;(void)pet_cev_alarms_tick_clock(set,now,c);return true;
}
