/* D-only parser/effect hooks. Included after the production JSON helpers.
 * Queues belong to the current native runtime; no background grants or retry. */
#if PET_P4_NOTIFY
static pet_notify_limiter_t g_d_notify_limiter;
static bool miniapp_parse_notify(miniapp_runtime_t *r,const cJSON *root){
  const cJSON *n=cJSON_GetObjectItemCaseSensitive(root,"notify");
  if(!n)return true;
  static const char *const keys[]={"title","badge_var"};
  const cJSON *title=cJSON_GetObjectItemCaseSensitive(n,"title"),*badge=cJSON_GetObjectItemCaseSensitive(n,"badge_var");
  if(!object_keys_allowed(n,keys,2)||!cJSON_IsString(title)||!pet_notify_text_ok(title->valuestring,63,true))return false;
  r->notice_badge_var=-1;
  if(badge){int at=cJSON_IsString(badge)?find_var(r,badge->valuestring):-1;if(at<0||r->vars[at].type!=MINIAPP_VAR_INT)return false;r->notice_badge_var=(int8_t)at;}
  strcpy(r->notice.title,title->valuestring);r->notice_enabled=true;return true;
}
static bool miniapp_parse_notify_rule(const miniapp_runtime_t *r,const cJSON *rule,bool *out){
  const cJSON *n=cJSON_GetObjectItemCaseSensitive(rule,"notify");*out=n!=NULL;
  return !n||(cJSON_IsTrue(n)&&r->notice_enabled);
}
static void miniapp_queue_notice(miniapp_runtime_t *r,uint64_t now){
  if(!r->active||!g_input_visible||!r->notice_enabled||r->notice_pending||!pet_notify_admit(&g_d_notify_limiter,r->widget_id,now))return;
  strcpy(r->notice.component,r->widget_id);r->notice.has_badge=r->notice_badge_var>=0;
  if(r->notice.has_badge)r->notice.badge=r->vars[(unsigned)r->notice_badge_var].int_value;
  r->notice_pending=true;
}
#endif
#if PET_P4_DATA_ROWS
static void miniapp_queue_refresh(miniapp_runtime_t *r,uint64_t now){
  if(!r->active||!g_input_visible||!r->data_source[0]||!r->data_rows.refresh_action[0]||r->data_refresh_pending)return;
  if(r->data_refresh_used&&(now<r->data_refresh_last||now-r->data_refresh_last<5000))return;
  r->data_refresh_used=true;r->data_refresh_last=now;r->data_refresh_pending=true;
}
#endif
#if PET_P4_NOTIFY || PET_P4_DATA_ROWS
/* Caller holds runtime lock, so publication cannot cross component replacement.
 * Clear before output; failed transport is consumed, never replayed on reconnect. */
static void miniapp_d_drain_locked(bool visible){
#if PET_P4_NOTIFY
  if(g_runtime.notice_pending){
    g_runtime.notice_pending=false;
    if(visible&&g_runtime.active){
      const pet_notify_t *n=&g_runtime.notice;cJSON *p=cJSON_CreateObject();
      if(p){cJSON_AddStringToObject(p,"componentId",n->component);cJSON_AddStringToObject(p,"title",n->title);if(n->has_badge)cJSON_AddNumberToObject(p,"badge",n->badge);pet_app_host_emit("widget/notify",p);cJSON_Delete(p);}
    }
  }
#endif
#if PET_P4_DATA_ROWS
  if(g_runtime.data_refresh_pending){
    g_runtime.data_refresh_pending=false;
    if(visible&&g_runtime.active){
      cJSON *p=cJSON_CreateObject();if(p){cJSON_AddStringToObject(p,"componentId",g_runtime.widget_id);cJSON_AddStringToObject(p,"source",g_runtime.data_source);pet_app_host_emit("widget/data/refresh",p);cJSON_Delete(p);}
    }
  }
#endif
}
#endif
