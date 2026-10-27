#include "pet_p4_local_tools.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static int clamp(int n,int lo,int hi){return n<lo?lo:n>hi?hi:n;}
static int wrap(int n,int max){return (n%max+max)%max;}
static bool keys(const cJSON *j,const char *list){
  if(!cJSON_IsObject(j))return false;
  const cJSON *a; cJSON_ArrayForEach(a,j){char k[64];if(!a->string||strlen(a->string)>48)return false;
    snprintf(k,sizeof(k),"|%s|",a->string);if(!strstr(list,k))return false;
    for(const cJSON *b=a->next;b;b=b->next)if(b->string&&!strcmp(a->string,b->string))return false;
  }return true;
}
static const cJSON *get(const cJSON *j,const char *k){return cJSON_GetObjectItemCaseSensitive(j,k);}
static bool str(const cJSON *j,char *out,size_t cap){
  if(!cJSON_IsString(j)||!j->valuestring[0]||strlen(j->valuestring)>=cap)return false;
  for(const unsigned char *p=(const unsigned char*)j->valuestring;*p;p++)if(*p<32||*p==127)return false;
  snprintf(out,cap,"%s",j->valuestring);return true;
}
static bool number(const cJSON *j,int lo,int hi,int *out){
  if(!cJSON_IsNumber(j)||!isfinite(j->valuedouble)||j->valuedouble<lo||j->valuedouble>hi||j->valuedouble!=j->valueint)return false;
  *out=j->valueint;return true;
}
static bool safe_action(const char *s){if(!s[0]||strlen(s)>47)return false;for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='.'||*s=='_'||*s=='-'))return false;return true;}
bool pet_local_parse(const cJSON *j,pet_local_config_t *o){
  memset(o,0,sizeof(*o));
  if(!keys(j,"|engine||kind||groups||actions||reveal_ms||idle||busy||hint|"))return false;
  char engine[32],kind[16];
  if(!str(get(j,"engine"),engine,sizeof(engine))||strcmp(engine,PET_LOCAL_ENGINE)||!str(get(j,"kind"),kind,sizeof(kind)))return false;
  if(strcmp(kind,"deck")&&strcmp(kind,"countdown"))return false;
  o->countdown=!strcmp(kind,"countdown");int delay;
  if(!number(get(j,"reveal_ms"),300,2000,&delay))return false;o->reveal_ms=delay;
  if(!str(get(j,"idle"),o->idle,sizeof(o->idle))||!str(get(j,"busy"),o->busy,sizeof(o->busy))||!str(get(j,"hint"),o->hint,sizeof(o->hint)))return false;
  const cJSON *groups=get(j,"groups"),*g;
  if(!cJSON_IsArray(groups)||cJSON_GetArraySize(groups)<1||cJSON_GetArraySize(groups)>3)return false;
  cJSON_ArrayForEach(g,groups){
    pet_local_group_t *q=&o->groups[o->count++];
    if(!keys(g,o->countdown?"|title||kind|":"|title||items|")||!str(get(g,"title"),q->title,sizeof(q->title)))return false;
    if(o->countdown){
      char t[16];if(!str(get(g,"kind"),t,sizeof(t)))return false;
      q->kind=!strcmp(t,"weekdays")?1:!strcmp(t,"weekend")?2:!strcmp(t,"date")?3:0;
      if(!q->kind)return false;
    }else{
      const cJSON *items=get(g,"items"),*it;
      if(!cJSON_IsArray(items)||cJSON_GetArraySize(items)<2||cJSON_GetArraySize(items)>16)return false;
      cJSON_ArrayForEach(it,items){pet_local_item_t *p=&q->items[q->count++];
        if(!keys(it,"|title||note|")||!str(get(it,"title"),p->title,sizeof(p->title))||!str(get(it,"note"),p->note,sizeof(p->note)))return false;
        for(int i=0;i<q->count-1;i++)if(!strcmp(q->items[i].title,p->title))return false;
      }
    }
  }
  const cJSON *actions=get(j,"actions"),*a;
  if(!cJSON_IsObject(actions)||cJSON_GetArraySize(actions)<2||cJSON_GetArraySize(actions)>8)return false;
  static const char *const ops[]={"","draw","exclude","reset","close","previous","next","edit","cancel","up","down"};
  unsigned used=0;
  cJSON_ArrayForEach(a,actions){
    if(!safe_action(a->string)||!cJSON_IsString(a))return false;int op=0;
    for(int i=0;i<o->action_count;i++)if(!strcmp(o->actions[i],a->string))return false;
    for(int i=1;i<=10;i++)if(!strcmp(a->valuestring,ops[i]))op=i;
    if(!op||(used&(1u<<op))||(o->countdown?(op<5):(op>6)))return false;
    used|=1u<<op;strcpy(o->actions[o->action_count],a->string);o->operations[o->action_count++]=op;
  }
  unsigned required=o->countdown?((1u<<5)|(1u<<6)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<10)):(1u<<1);
  if((used&required)!=required||((used&(1u<<2))&&!(used&(1u<<3)))||(!o->countdown&&o->count>1&&(used&((1u<<5)|(1u<<6)))!=((1u<<5)|(1u<<6))))return false;
  o->enabled=true;return true;
}
void pet_local_init(pet_local_state_t *s){memset(s,0,sizeof(*s));s->pending=-1;s->last_second=-2;}
bool pet_local_enabled(const pet_local_config_t *c,const pet_local_state_t *s,const char *action){
  int op=0;for(int i=0;i<c->action_count;i++)if(!strcmp(action,c->actions[i]))op=c->operations[i];
  if(!op)return false;
  if(c->countdown)return op==PET_LOCAL_CANCEL||op==PET_LOCAL_UP||op==PET_LOCAL_DOWN?s->phase==3:true;
  if(s->phase==1)return op==PET_LOCAL_CLOSE;
  if(op==PET_LOCAL_EXCLUDE)return s->phase==2;
  return true;
}
/* Gregorian civil dates, UTC-day indices. No libc timezone or RTC assumptions. */
int32_t pet_local_day(int y,int m,int d){y-=m<=2;int era=y/400,yoe=y-era*400;int doy=(153*(m+(m>2?-3:9))+2)/5+d-1;return era*146097+yoe*365+yoe/4-yoe/100+doy-719468;}
void pet_local_date(int32_t z,int *y,int *m,int *d){z+=719468;int era=z/146097,doe=z-era*146097,yoe=(doe-doe/1460+doe/36524-doe/146096)/365;*y=yoe+era*400;int doy=doe-(365*yoe+yoe/4-yoe/100),mp=(5*doy+2)/153;*d=doy-(153*mp+2)/5+1;*m=mp+(mp<10?3:-9);*y+=*m<=2;}
static int month_days(int y,int m){static const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};return days[m-1]+(m==2&&y%4==0&&(y%100!=0||y%400==0));}
int64_t pet_local_target(uint8_t kind,int32_t day,int minute,int64_t now){
  if(now<0||minute<0||minute>1439)return -1;
  if(kind==3)return day>=pet_local_day(2020,1,1)&&day<=pet_local_day(2099,12,31)?(int64_t)day*86400+minute*60:-1;
  int32_t today=now/86400;
  if(kind==2){int dow=wrap(today+4,7);int32_t saturday=today+(dow==0?-1:6-dow);return (int64_t)saturday*86400+minute*60;}
  for(int i=0;i<8;i++){int32_t date=today+i;int dow=wrap(date+4,7);int64_t t=(int64_t)date*86400+minute*60;
    if((kind==1?dow>=1&&dow<=5:dow==6)&&(kind==1||t>=now))return t;
  }return -1;
}
static int selection(const pet_local_config_t *c,const int32_t v[7]){return wrap(v[0],c->count);}
static void repair(const pet_local_config_t *c,int32_t v[7]){
  v[0]=selection(c,v);
  for(int i=0;i<c->count;i++)if(c->countdown){v[1+i*2]=clamp(v[1+i*2],0,pet_local_day(2099,12,31));v[2+i*2]=clamp(v[2+i*2],0,1439);}
    else {v[1+i*2]&=(1<<c->groups[i].count)-1;v[2+i*2]=clamp(v[2+i*2],-1,c->groups[i].count-1);}
}
bool pet_local_action(const pet_local_config_t *c,pet_local_state_t *s,int32_t v[7],const char *action,uint64_t ms,int64_t now,uint32_t random){
  if(!c->enabled||!pet_local_enabled(c,s,action))return false;int op=0;for(int i=0;i<c->action_count;i++)if(!strcmp(action,c->actions[i]))op=c->operations[i];if(!op)return false;
  repair(c,v);int at=v[0],a=1+2*at,b=a+1;const pet_local_group_t *g=&c->groups[at];
  if(!c->countdown){
    if(s->phase==1&&op!=PET_LOCAL_CLOSE)return false;
    if(op==PET_LOCAL_PREVIOUS||op==PET_LOCAL_NEXT){v[0]=wrap(at+(op==PET_LOCAL_NEXT?1:-1),c->count);s->phase=0;return true;}
    if(op==PET_LOCAL_CLOSE){s->phase=0;s->pending=-1;return true;}
    if(op==PET_LOCAL_RESET){v[a]=0;s->phase=0;return true;}
    if(op==PET_LOCAL_EXCLUDE){if(s->phase!=2||v[b]<0)return false;v[a]|=1<<v[b];s->phase=0;}
    if(op==PET_LOCAL_DRAW||op==PET_LOCAL_EXCLUDE){
      int candidates[16],n=0;for(int i=0;i<g->count;i++)if(!(v[a]&(1<<i)))candidates[n++]=i;
      if(!n){s->phase=4;s->pending=-1;return true;}
      if(n>1)for(int i=0;i<n;i++)if(candidates[i]==v[b]){candidates[i]=candidates[--n];break;}
      /* Multiply-high maps the supplied full-width entropy to a bounded index. */
      s->pending=candidates[((uint64_t)random*(unsigned)n)>>32];s->reveal_at=ms+c->reveal_ms;s->phase=1;return true;
    }return false;
  }
  if(op==PET_LOCAL_CANCEL){if(s->phase!=3)return false;s->phase=0;return true;}
  if(op==PET_LOCAL_EDIT){
    if(s->phase==3){v[a]=s->draft_day;v[b]=s->draft_minute;s->phase=0;return true;}
    if(now<0){s->phase=5;return true;}
    s->draft_day=v[a]>=pet_local_day(2020,1,1)?v[a]:(int32_t)(now/86400+1);s->draft_minute=v[b];s->field=0;s->phase=3;return true;
  }
  if(op==PET_LOCAL_PREVIOUS||op==PET_LOCAL_NEXT){int step=op==PET_LOCAL_NEXT?1:-1;
    if(s->phase==3)s->field=wrap(s->field+step,g->kind==3?5:2);
    else {v[0]=wrap(at+step,c->count);s->phase=0;}return true;
  }
  if((op==PET_LOCAL_UP||op==PET_LOCAL_DOWN)&&s->phase==3){
    int step=op==PET_LOCAL_UP?1:-1;int f=s->field;
    if(g->kind!=3)f+=3;
    int y,m,d;pet_local_date(s->draft_day,&y,&m,&d);
    if(f==0)y=clamp(y+step,2020,2099);
    if(f==1)m=wrap(m-1+step,12)+1;
    if(f==2)d=wrap(d-1+step,month_days(y,m))+1;
    d=clamp(d,1,month_days(y,m));s->draft_day=pet_local_day(y,m,d);
    int hour=s->draft_minute/60,minute=s->draft_minute%60;
    if(f==3)hour=wrap(hour+step,24);if(f==4)minute=wrap(minute+step,60);
    s->draft_minute=hour*60+minute;return true;
  }return false;
}
bool pet_local_tick(const pet_local_config_t *c,pet_local_state_t *s,int32_t v[7],uint64_t ms,int64_t now){
  if(!c->enabled)return false;repair(c,v);
  if(!c->countdown&&s->phase==1&&ms>=s->reveal_at){v[2+2*v[0]]=s->pending;s->phase=2;s->pending=-1;return true;}
  if(c->countdown&&(now!=s->last_second||(s->phase==5&&now>=0))){s->last_second=now;if(s->phase==5&&now>=0)s->phase=0;return true;}
  if(!c->countdown&&(s->last_second==-2||(s->phase==1&&s->last_second!=(int64_t)(ms/50)))){s->last_second=ms/50;return true;}
  return false;
}
void pet_local_view(const pet_local_config_t *c,const pet_local_state_t *s,const int32_t v[7],uint64_t ms,int64_t now,pet_local_view_t *o){
  memset(o,0,sizeof(*o));if(!c->enabled)return;
  int at=selection(c,v),a=1+2*at,b=a+1;const pet_local_group_t *g=&c->groups[at];o->phase=s->phase;
  snprintf(o->eyebrow,sizeof(o->eyebrow),"%s",g->title);snprintf(o->badge,sizeof(o->badge),"%d/%d",at+1,c->count);
  snprintf(o->note,sizeof(o->note),"%s",c->hint);
  if(!c->countdown){
    snprintf(o->primary,sizeof(o->primary),"%s",s->phase==1?"正在揭晓":"抽取一个");
    snprintf(o->secondary,sizeof(o->secondary),"排除重抽");
    int remaining=0;for(int i=0;i<g->count;i++)if(!(v[a]&(1<<i)))remaining++;
    snprintf(o->metric,sizeof(o->metric),"%d",remaining);snprintf(o->label,sizeof(o->label),"可选内容");
    const pet_local_item_t *item=v[b]>=0&&v[b]<g->count?&g->items[v[b]]:NULL;
    const char *headline=s->phase==1?c->busy:s->phase==4?"选项都排除啦":s->phase==2&&item?item->title:c->idle;
    snprintf(o->headline,sizeof(o->headline),"%s",headline);
    if(s->phase==2&&item)snprintf(o->note,sizeof(o->note),"%s",item->note);
    if(s->phase==4)snprintf(o->note,sizeof(o->note),"点击恢复选项，或切换一个分类。");
    if(s->phase==1){int64_t left=(int64_t)s->reveal_at-(int64_t)ms;o->progress=clamp(100-(int)(left*100/c->reveal_ms),0,100);}
    else o->progress=s->phase==2?100:0;
    return;
  }
  snprintf(o->primary,sizeof(o->primary),"%s",s->phase==3?"保存设置":"设置时间");snprintf(o->secondary,sizeof(o->secondary),"取消");
  int32_t day=s->phase==3?s->draft_day:v[a];int minute=s->phase==3?s->draft_minute:v[b];
  if(s->phase==3){
    int y,m,d;pet_local_date(day,&y,&m,&d);int f=s->field+(g->kind==3?0:3);
    static const char *const fields[]={"年","月","日","小时","分钟"};
    snprintf(o->headline,sizeof(o->headline),"调整%s",fields[f]);
    int val=f==0?y:f==1?m:f==2?d:f==3?minute/60:minute%60;
    snprintf(o->metric,sizeof(o->metric),"%02d",val);
    if(g->kind==3)snprintf(o->note,sizeof(o->note),"%04d-%02d-%02d  %02d:%02d",y,m,d,minute/60,minute%60);
    else snprintf(o->note,sizeof(o->note),"目标时间  %02d:%02d",minute/60,minute%60);
    snprintf(o->label,sizeof(o->label),"左右选项，上下调整");return;
  }
  if(now<0){o->phase=5;snprintf(o->headline,sizeof(o->headline),"等待时间校准");snprintf(o->note,sizeof(o->note),"连接新版 Pet Manager 校准时间后可离线使用。");snprintf(o->metric,sizeof(o->metric),"—");return;}
  int64_t target=pet_local_target(g->kind,day,minute,now);
  if(target<0){snprintf(o->headline,sizeof(o->headline),"给期待定个日子");snprintf(o->metric,sizeof(o->metric),"—");snprintf(o->note,sizeof(o->note),"点设置时间，保存属于你的小期待。");return;}
  int64_t left=target-now;
  if(left<=0){o->phase=2;snprintf(o->headline,sizeof(o->headline),"到站啦，去享受吧");snprintf(o->metric,sizeof(o->metric),"已到站");o->progress=100;}
  else {snprintf(o->headline,sizeof(o->headline),"下一站，%s",g->title);snprintf(o->label,sizeof(o->label),"距离出发还有");
    if(left>=86400){snprintf(o->metric,sizeof(o->metric),"%lld",(long long)((left+86399)/86400));snprintf(o->unit,sizeof(o->unit),"天");}
    else snprintf(o->metric,sizeof(o->metric),"%02lld:%02lld:%02lld",(long long)(left/3600),(long long)(left/60%60),(long long)(left%60));
    o->progress=clamp(100-(int)(left>86400?100:left*100/86400),0,99);
  }
  int y,m,d;pet_local_date(target/86400,&y,&m,&d);
  snprintf(o->note,sizeof(o->note),"%04d-%02d-%02d  %02d:%02d",y,m,d,(int)(target%86400/3600),(int)(target%3600/60));
}
