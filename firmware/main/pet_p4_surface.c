#include "pet_p4_surface.h"
#include <string.h>
#include <math.h>
#include "pet_p4_app_icons.h"
#if PET_P4_DATA_ROWS
#include "pet_p4_data_rows.h"
#endif
#if PET_CANVAS_ENABLED
#include "pet_p4_canvas_json.h"
#endif
static bool number(const cJSON *o,const char *k,int lo,int hi,int *out){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,k);if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||v->valuedouble<lo||v->valuedouble>hi||v->valuedouble!=(int)v->valuedouble)return false;*out=v->valueint;return true;}
static int choice(const cJSON *o,const char *key,const char *const *values,int count){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);if(!cJSON_IsString(v))return -1;for(int i=0;i<count;i++)if(!strcmp(v->valuestring,values[i]))return i;return -1;}
static bool keys(const cJSON *o,const char *const *allowed,int count){if(!cJSON_IsObject(o))return false;const cJSON *v; cJSON_ArrayForEach(v,o){bool found=false;for(int i=0;i<count;i++)if(!strcmp(v->string,allowed[i]))found=true;if(!found)return false;}return true;}
#if PET_P4_CLIP||PET_P4_BIG_IMAGE||PET_SERVICE_BRIDGE_ENABLED
/* Track C keys are checked by membership, independent of the prefix-count tables above. */
static bool keys_ext(const cJSON *o,const char *const *allowed,int count,const char *const *extra,int extra_count,bool root){if(!cJSON_IsObject(o))return false;const cJSON *v;cJSON_ArrayForEach(v,o){bool found=false;for(int i=0;i<count;i++)if(!strcmp(v->string,allowed[i]))found=true;for(int i=0;i<extra_count;i++)if(!strcmp(v->string,extra[i]))found=true;
#if PET_SERVICE_BRIDGE_ENABLED
  /* Bridge keys are independent of the legacy prefix-length tables. Their
   * profile/type/value constraints are still enforced below. */
  if(root){if(!strcmp(v->string,"services"))found=true;}
  else if(!strcmp(v->string,"image_bind")||!strcmp(v->string,"value_param")||!strcmp(v->string,"value_min")||!strcmp(v->string,"value_max"))found=true;
#else
  (void)root;
#endif
  if(!found)return false;}return true;}
static const char *const ext_root_keys[]={"big_images"};
static const char *const ext_node_keys[]={"asset","big","offset_x_var","offset_y_var"};
#define PET_SURFACE_ROOT_KEYS(o,allowed,count) keys_ext(o,allowed,count,ext_root_keys,PET_P4_BIG_IMAGE?1:0,true)
#define PET_SURFACE_NODE_KEYS(o,allowed,count) keys_ext(o,allowed,count,ext_node_keys,(PET_P4_CLIP?1:0)+(PET_P4_BIG_IMAGE?3:0),false)
#else
#define PET_SURFACE_ROOT_KEYS(o,allowed,count) keys(o,allowed,count)
#define PET_SURFACE_NODE_KEYS(o,allowed,count) keys(o,allowed,count)
#endif
static bool var_name(const cJSON *o,const char *key,char out[32]){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);if(!cJSON_IsString(v)||!v->valuestring[0]||strlen(v->valuestring)>31)return false;for(const char *p=v->valuestring;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'||*p=='.'))return false;strcpy(out,v->valuestring);return true;}
static bool parse_viewport(const cJSON *vp,pet_surface_viewport_t *a,bool named){
  static const char*const vk[]={"x","y","w","h","content_height","offset_var","engine"};
  static const char*const nk[]={"x","y","w","h","content_height","offset_var","id"};
  int v;char id[32];
  if(!keys(vp,named?nk:vk,7))return false;
  if(named){if(!var_name(vp,"id",id)||strlen(id)>15)return false;strcpy(a->id,id);}
  else if(strcmp(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(vp,"engine"))?cJSON_GetObjectItemCaseSensitive(vp,"engine")->valuestring:"","p4-app-viewport-v1"))return false;
  if(!number(vp,"x",0,639,&v))return false;
  a->x=v;
  if(!number(vp,"y",0,479,&v))return false;
  a->y=v;
  if(!number(vp,"w",64,640-a->x,&v))return false;
  a->w=v;
  if(!number(vp,"h",64,480-a->y,&v))return false;
  a->h=v;
  if(!number(vp,"content_height",a->h,2048,&v)||!var_name(vp,"offset_var",a->offset_var))return false;
  a->content_height=v;
  a->enabled=true;
  return true;
}
/* The two-argument entry keeps the v1 node budget; the bounded runtime passes
 * the negotiated p4-quota-v2 ceiling through pet_surface_parse_capped. */
bool pet_surface_parse(const cJSON *json,pet_surface_t *out){return pet_surface_parse_capped(json,out,PET_SURFACE_V1_NODES);}
bool pet_surface_parse_capped(const cJSON *json,pet_surface_t *out,unsigned max_nodes){return pet_surface_parse_profile(json,out,max_nodes,false);}
#if PET_P4_PARTICLES
/* One emitter object; every field bounded, the var guard only names a variable. */
static bool parse_particles(const cJSON *e,pet_surface_particles_t *p,bool bindings){
  static const char*const pk[]={"x","y","w","h","count","life_ms","speed","spread","gravity","color","shape","trigger","budget","when","page"};
  static const char*const shapes[]={"dot","spark"};
  static const char*const ops[]={"lte","lt","gte","gt","eq","ne"};
  static const char*const tk_action[]={"action"},*const tk_tick[]={"tick"},*const wk[]={"var","op","value"};
  int v;memset(p,0,sizeof(*p));
  if(!keys(e,pk,15)||!number(e,"x",0,639,&v))return false;p->x=v;
  if(!number(e,"y",0,479,&v))return false;p->y=v;
  if(!number(e,"w",1,640-p->x,&v))return false;p->w=v;
  if(!number(e,"h",1,480-p->y,&v))return false;p->h=v;
  if(!number(e,"count",1,PET_SURFACE_PARTICLE_DOTS,&v))return false;p->count=v;
  if(!number(e,"life_ms",100,4000,&v))return false;p->life_ms=v;
  if(!number(e,"speed",1,240,&v))return false;p->speed=v;
  if(!number(e,"spread",0,360,&v))return false;p->spread=v;
  if(!number(e,"gravity",-240,240,&v))return false;p->gravity=v;
  if(!number(e,"color",0,65535,&v))return false;p->color=v;
  if(!number(e,"budget",1,p->count,&v))return false;p->budget=v;
  v=choice(e,"shape",shapes,2);if(v<0)return false;p->shape=v;
  p->page=4;if(cJSON_HasObjectItem(e,"page")){if(!number(e,"page",0,4,&v))return false;p->page=v;}
  const cJSON *t=cJSON_GetObjectItemCaseSensitive(e,"trigger"),*action=cJSON_GetObjectItemCaseSensitive(t,"action");
  if(action){
    if(!keys(t,tk_action,1)||!cJSON_IsString(action)||!action->valuestring[0]||strlen(action->valuestring)>47)return false;
    for(const char *q=action->valuestring;*q;q++)if(!((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||(*q>='0'&&*q<='9')||*q=='_'||*q=='-'||*q=='.'))return false;
    strcpy(p->trigger_action,action->valuestring);
  }else{if(!keys(t,tk_tick,1)||!number(t,"tick",50,2000,&v))return false;p->trigger_tick_ms=v;}
  const cJSON *w=cJSON_GetObjectItemCaseSensitive(e,"when");
  if(w){
    if(!bindings||!keys(w,wk,3)||!var_name(w,"var",p->when_var)||!number(w,"value",-1000000000,1000000000,&v))return false;p->when_value=v;
    v=choice(w,"op",ops,6);if(v<0)return false;p->when_op=v;
  }
  return true;
}
#endif
bool pet_surface_parse_profile(const cJSON *json,pet_surface_t *out,unsigned max_nodes,bool graphics_v2){
  if(max_nodes>PET_SURFACE_NODES)max_nodes=PET_SURFACE_NODES;
#if !PET_P4_GRAPHICS_V2
  graphics_v2=false;
#endif
  /* p4-graphics-v2 ceilings apply only to a package that declared the profile. */
  const unsigned max_images=graphics_v2?PET_SURFACE_IMAGES:PET_SURFACE_V1_IMAGES,max_pixels=graphics_v2?PET_SURFACE_IMAGE_PIXELS:PET_SURFACE_V1_IMAGE_PIXELS;
  const unsigned max_fonts=graphics_v2?PET_SURFACE_FONTS:PET_SURFACE_V1_FONTS,max_chars=graphics_v2?PET_SURFACE_FONT_CHARS:PET_SURFACE_V1_FONT_CHARS,max_font_w=graphics_v2?512:240;
  static const char *const root_keys[]={"bindings","background","elements","images","engine","graphics","typography","fonts","interaction","game_view","data_view","scene_view","input","viewport","viewports","canvas","collection_view","shell","tabs","texts","particles","services"};
  static const char *const node_keys[]={"type","x","y","w","h","color","page","scale","radius","bind","text","image","rotate_ms","action","motion","opacity","opacity_end","font_size","line_height","align","valign","padding","gap","icon_size","weight","foreground","stroke","stroke_width","icon","gradient","key_for","tile","show","state_mask","host_mask","var_bind","in_view","view","field_bind","text_at","columns","cursor","page_var","count_var","text_id","row_bind","caption","image_bind","value_param","value_min","value_max"};
  static const char *const types[]={"panel","text","meter","ticks","list","transport","image","icon","button","cells","longtext","figure"};
  static const char *const binds[]={"title","subtitle","status","message","volume","index","count","level"};
  static const char *const app_binds[]={"title","eyebrow","headline","metricLabel","metricValue","metricUnit","badge","note","footer","progress"};
  memset(out,0,sizeof(*out));int v;
#if PET_P4_GRAPHICS_V2
  out->graphics_v2=graphics_v2;
#endif
  const cJSON *engine=cJSON_GetObjectItemCaseSensitive(json,"engine");
  if(engine){if(!cJSON_IsString(engine)||strcmp(engine->valuestring,"p4-app-surface-v1"))return false;out->app=true;}
  const cJSON *input=cJSON_GetObjectItemCaseSensitive(json,"input");
  if(input){if(out->app||!cJSON_IsString(input)||strcmp(input->valuestring,"p4-media-touch-v1"))return false;out->media_touch=true;}
  if(cJSON_HasObjectItem(json,"graphics")){const cJSON *g=cJSON_GetObjectItemCaseSensitive(json,"graphics");if(!out->app||!cJSON_IsString(g)||strcmp(g->valuestring,"p4-app-graphics-v1"))return false;out->graphics=true;}
  if(cJSON_HasObjectItem(json,"bindings")){
    const cJSON *b=cJSON_GetObjectItemCaseSensitive(json,"bindings");
    if(!out->graphics||!cJSON_IsString(b)||strcmp(b->valuestring,"p4-variable-bindings-v1"))return false;
    out->variable_bindings=true;
  }
  if(cJSON_HasObjectItem(json,"services")){
#if PET_SERVICE_BRIDGE_ENABLED
    const cJSON*v=cJSON_GetObjectItemCaseSensitive(json,"services");if(!out->app||!out->graphics||!out->variable_bindings||!cJSON_IsString(v)||strcmp(v->valuestring,"p4-service-bridge-v1"))return false;out->service_bridge=true;
#else
    return false;
#endif
  }
  const cJSON *vp=cJSON_GetObjectItemCaseSensitive(json,"viewport");
  const cJSON *collection=cJSON_GetObjectItemCaseSensitive(json,"collection_view");
#if PET_HOST_COLLECTION_ENABLED
  if(collection&&(!out->graphics||!pet_host_collection_parse_config(collection,&out->collection)))return false;
#else
  if(collection)return false;
#endif
  const cJSON *canvas=cJSON_GetObjectItemCaseSensitive(json,"canvas");
#if PET_CANVAS_ENABLED
  if(canvas&&(!out->variable_bindings||!pet_canvas_parse(canvas,&out->canvas)))return false;
#else
  if(canvas)return false;
#endif
  const cJSON *multi=cJSON_GetObjectItemCaseSensitive(json,"viewports");
  if((vp||multi)&&(!out->variable_bindings||cJSON_HasObjectItem(json,"game_view")||cJSON_HasObjectItem(json,"scene_view")||cJSON_HasObjectItem(json,"data_view")))return false;
  if(vp){
    if(multi||!parse_viewport(vp,&out->viewport,false))return false;
  }
  if(multi){
    static const char*const mk[]={"engine","items"};
    const cJSON *items=cJSON_GetObjectItemCaseSensitive(multi,"items");
    if(!keys(multi,mk,2)||strcmp(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(multi,"engine"))?cJSON_GetObjectItemCaseSensitive(multi,"engine")->valuestring:"","p4-app-viewports-v1")||!cJSON_IsArray(items)||cJSON_GetArraySize(items)<1||cJSON_GetArraySize(items)>PET_SURFACE_VIEWPORTS)return false;
    for(int i=0;i<cJSON_GetArraySize(items);i++){
      pet_surface_viewport_t*a=&out->viewports[i];if(!parse_viewport(cJSON_GetArrayItem(items,i),a,true))return false;
      for(int j=0;j<i;j++){const pet_surface_viewport_t*b=&out->viewports[j];if(!strcmp(a->id,b->id)||!strcmp(a->offset_var,b->offset_var)||(a->x<b->x+b->w&&a->x+a->w>b->x&&a->y<b->y+b->h&&a->y+a->h>b->y))return false;}
      out->viewport_count++;
    }
  }
  const cJSON *ty=cJSON_GetObjectItemCaseSensitive(json,"typography");
  if(ty){if(!out->graphics||!cJSON_IsString(ty)||strcmp(ty->valuestring,"p4-app-type-v1"))return false;out->typography=true;}
  const cJSON *interaction=cJSON_GetObjectItemCaseSensitive(json,"interaction");
  if(interaction){if(!out->typography||!cJSON_IsString(interaction)||strcmp(interaction->valuestring,"p4-app-interaction-v1"))return false;out->interaction=true;}
  const cJSON *game=cJSON_GetObjectItemCaseSensitive(json,"game_view");
  if(game){
    static const char *const gkeys[]={"engine","x","y","w","h"};
    const cJSON *ge=cJSON_GetObjectItemCaseSensitive(game,"engine");
    if(!out->graphics||!keys(game,gkeys,5)||!cJSON_IsString(ge)||strcmp(ge->valuestring,"p4-game-surface-v1"))return false;
    if(!number(game,"x",0,639,&v))return false;
    out->game_x=v;
    if(!number(game,"y",0,479,&v))return false;
    out->game_y=v;
    if(!number(game,"w",64,640-out->game_x,&v))return false;
    out->game_w=v;
    if(!number(game,"h",64,480-out->game_y,&v))return false;
    out->game_h=v;
    out->game_view=true;
  }
  const cJSON *data=cJSON_GetObjectItemCaseSensitive(json,"data_view");
  if(data){
    static const char *const dk[]={"engine","x","y","w","h","layout","colors"};
    const cJSON *de=cJSON_GetObjectItemCaseSensitive(data,"engine"),*colors=cJSON_GetObjectItemCaseSensitive(data,"colors");
    const char *const layouts[]={"metrics","todos","rows"};
    if(game||!out->graphics||!keys(data,dk,7)||!cJSON_IsString(de)||strcmp(de->valuestring,"p4-data-surface-v1"))return false;
    int layout=choice(data,"layout",layouts,PET_P4_DATA_ROWS?3:2);if(layout<0)return false;out->data_view.todos=layout==1;
#if PET_P4_DATA_ROWS
    out->data_view.rows=layout==2;
#endif
    if(!number(data,"x",0,639,&v))return false;
    out->data_view.x=v;
    if(!number(data,"y",0,479,&v))return false;
    out->data_view.y=v;
    if(!number(data,"w",320,640-out->data_view.x,&v))return false;
    out->data_view.w=v;
    if(!number(data,"h",300,480-out->data_view.y,&v))return false;
    out->data_view.h=v;
    if(!cJSON_IsArray(colors)||cJSON_GetArraySize(colors)!=5)return false;
    for(int i=0;i<5;i++){const cJSON *c=cJSON_GetArrayItem(colors,i);if(!cJSON_IsNumber(c)||!isfinite(c->valuedouble)||c->valuedouble<0||c->valuedouble>65535||c->valuedouble!=(int)c->valuedouble)return false;out->data_view.colors[i]=c->valueint;}
    out->data_view.enabled=true;
  }
  const cJSON *scene=cJSON_GetObjectItemCaseSensitive(json,"scene_view");
  if(scene){
    static const char *const sk[]={"engine","x","y","w","h","colors","sprites"};
    const cJSON *se=cJSON_GetObjectItemCaseSensitive(scene,"engine"),*colors=cJSON_GetObjectItemCaseSensitive(scene,"colors");
    if(game||data||!out->graphics||!keys(scene,sk,7)||!cJSON_IsString(se)||strcmp(se->valuestring,"p4-scene-surface-v1"))return false;
    if(!number(scene,"x",0,639,&v))return false;
    out->scene_view.x=v;
    if(!number(scene,"y",0,479,&v))return false;
    out->scene_view.y=v;
    if(!number(scene,"w",64,640-out->scene_view.x,&v))return false;
    out->scene_view.w=v;
    if(!number(scene,"h",64,480-out->scene_view.y,&v))return false;
    out->scene_view.h=v;
    if(!cJSON_IsArray(colors)||cJSON_GetArraySize(colors)!=5)return false;
    for(int i=0;i<5;i++){const cJSON *c=cJSON_GetArrayItem(colors,i);if(!cJSON_IsNumber(c)||!isfinite(c->valuedouble)||c->valuedouble<0||c->valuedouble>65535||c->valuedouble!=(int)c->valuedouble)return false;out->scene_view.colors[i]=c->valueint;}
    out->scene_view.enabled=true;
  }
  int root_count=collection?17:canvas?16:out->viewport_count?15:out->viewport.enabled?14:1+(out->media_touch?12:scene?11:data?10:game?9:out->interaction?8:out->typography?7:out->app?5:3);
  const cJSON *shell=cJSON_GetObjectItemCaseSensitive(json,"shell"),*tabs=cJSON_GetObjectItemCaseSensitive(json,"tabs"),*texts=cJSON_GetObjectItemCaseSensitive(json,"texts");
#if PET_P4_APP_SHELL
  if(shell){if(canvas||!out->graphics||!cJSON_IsString(shell)||strcmp(shell->valuestring,"p4-app-shell-v1"))return false;out->shell=true;root_count=20;}
  if(tabs){
    static const char *const tk[]={"items","var","x","y","w","h","colors"};
    const cJSON *items=cJSON_GetObjectItemCaseSensitive(tabs,"items"),*colors=cJSON_GetObjectItemCaseSensitive(tabs,"colors");
    pet_shell_tabs_t *t=&out->tabs;
    if(!out->shell||!out->variable_bindings||!keys(tabs,tk,7)||!cJSON_IsArray(items)||cJSON_GetArraySize(items)<PET_SHELL_TABS_MIN||cJSON_GetArraySize(items)>PET_SHELL_TABS_MAX||!var_name(tabs,"var",t->var))return false;
    if(!number(tabs,"x",0,639,&v))return false;t->x=v;if(!number(tabs,"y",0,479,&v))return false;t->y=v;
    if(!number(tabs,"w",64*cJSON_GetArraySize(items),640-t->x,&v))return false;t->w=v;if(!number(tabs,"h",32,96,&v)||t->y+v>480)return false;t->h=v;
    if(!cJSON_IsArray(colors)||cJSON_GetArraySize(colors)!=4)return false;
    for(int i=0;i<4;i++){const cJSON *c=cJSON_GetArrayItem(colors,i);if(!cJSON_IsNumber(c)||!isfinite(c->valuedouble)||c->valuedouble<0||c->valuedouble>65535||c->valuedouble!=(int)c->valuedouble)return false;t->colors[i]=c->valueint;}
    t->count=cJSON_GetArraySize(items);
    for(int i=0;i<t->count;i++){
      static const char *const ik[]={"label","icon","page"};
      const cJSON *item=cJSON_GetArrayItem(items,i),*label=cJSON_GetObjectItemCaseSensitive(item,"label"),*icon=cJSON_GetObjectItemCaseSensitive(item,"icon");
      pet_shell_tab_t *tab=&t->items[i];tab->icon=-1;
      if(!keys(item,ik,3)||!cJSON_IsString(label)||!label->valuestring[0]||strlen(label->valuestring)>24)return false;
      for(const unsigned char *c=(const unsigned char*)label->valuestring;*c;c++)if(*c<32||*c==127)return false;
      strcpy(tab->label,label->valuestring);
      if(icon){if(!cJSON_IsString(icon))return false;for(int k=0;k<PET_APP_ICON_COUNT;k++)if(!strcmp(icon->valuestring,pet_app_icons[k].name))tab->icon=k;if(tab->icon<0)return false;}
      if(!number(item,"page",0,PET_SHELL_PAGES-1,&v))return false;tab->page=v;
    }
    t->enabled=true;if(!pet_shell_tabs_valid(t))return false;root_count=20;
  }
#else
  if(shell||tabs)return false;
#endif
#if PET_P4_LONGTEXT
  if(texts){
    static const char *const xk[]={"id","text"};
    if(!out->variable_bindings||!cJSON_IsArray(texts)||cJSON_GetArraySize(texts)<1||cJSON_GetArraySize(texts)>2)return false;
    for(int i=0;i<cJSON_GetArraySize(texts);i++){
      const cJSON *x=cJSON_GetArrayItem(texts,i),*id=cJSON_GetObjectItemCaseSensitive(x,"id"),*body=cJSON_GetObjectItemCaseSensitive(x,"text");
      pet_surface_longtext_t *lt=&out->texts[i];
      if(!keys(x,xk,2)||!cJSON_IsString(id)||!id->valuestring[0]||strlen(id->valuestring)>15||!cJSON_IsString(body)||!body->valuestring[0]||!pet_text_flow_valid(body->valuestring,PET_TEXT_FLOW_MAX_BYTES))return false;
      for(const char *c=id->valuestring;*c;c++)if(!((*c>='a'&&*c<='z')||(*c>='0'&&*c<='9')||*c=='-'||*c=='_'))return false;
      for(int j=0;j<i;j++)if(!strcmp(out->texts[j].id,id->valuestring))return false;
      strcpy(lt->id,id->valuestring);strcpy(lt->text,body->valuestring);out->text_count++;
    }
    root_count=20;
  }
#else
  if(texts)return false;
#endif
  const cJSON *particles=cJSON_GetObjectItemCaseSensitive(json,"particles");
#if PET_P4_PARTICLES
  if(particles){
    if(!out->graphics||!cJSON_IsArray(particles)||cJSON_GetArraySize(particles)<1||cJSON_GetArraySize(particles)>PET_SURFACE_PARTICLE_EMITTERS)return false;
    for(int i=0;i<cJSON_GetArraySize(particles);i++){if(!parse_particles(cJSON_GetArrayItem(particles,i),&out->particles[i],out->variable_bindings))return false;out->particle_count++;}
    root_count=21;
  }
#else
  if(particles)return false;
#endif
  if(!PET_SURFACE_ROOT_KEYS(json,root_keys,root_count)||!number(json,"background",0,65535,&v))return false;
  out->background=v;

  const cJSON *images=cJSON_GetObjectItemCaseSensitive(json,"images");
  if(images) {
    if(!cJSON_IsArray(images)||cJSON_GetArraySize(images)<1||cJSON_GetArraySize(images)>(int)max_images)return false;
    static const char *const ik[]={"id","asset","frame_width","frame_height","frames","fps","encoding"};
    unsigned pixels=0;
    for(int i=0;i<cJSON_GetArraySize(images);i++){
      const cJSON *im=cJSON_GetArrayItem(images,i),*id=cJSON_GetObjectItemCaseSensitive(im,"id"),*asset=cJSON_GetObjectItemCaseSensitive(im,"asset");
      if(!keys(im,ik,out->graphics?7:6)||!cJSON_IsString(id)||!strlen(id->valuestring)||strlen(id->valuestring)>15||!cJSON_IsString(asset))return false;
      const cJSON *enc=cJSON_GetObjectItemCaseSensitive(im,"encoding");
      if(enc){if(!out->graphics||!cJSON_IsString(enc)||strcmp(enc->valuestring,"jpeg"))return false;out->image_jpeg[i]=true;}
      for(const char *c=id->valuestring;*c;c++)if(!((*c>='a'&&*c<='z')||(*c>='0'&&*c<='9')||*c=='-'||*c=='_'))return false;
      for(int j=0;j<i;j++)if(!strcmp(out->image_ids[j],id->valuestring))return false;
      const char *a=asset->valuestring;size_t len=strlen(a);
      if(len<12||strncmp(a,"assets/",7)||strcmp(a+len-4,".png"))return false;
      for(size_t k=7;k<len;k++)if(!((a[k]>='a'&&a[k]<='z')||(a[k]>='A'&&a[k]<='Z')||(a[k]>='0'&&a[k]<='9')||a[k]=='-'||a[k]=='_'||a[k]=='.'))return false;
      if(strstr(a,".."))return false;
      if(!number(im,"frame_width",8,out->image_jpeg[i]?640:512,&v))return false;
      out->image_w[i]=v;
      if(!number(im,"frame_height",8,out->image_jpeg[i]?480:240,&v))return false;
      out->image_h[i]=v;
      if(out->image_jpeg[i]&&(i!=0||out->image_w[i]!=640||out->image_h[i]!=480))return false;
#if PET_P4_KEYFRAME_BG
      /* p4-keyframe-bg-v1: only the JPEG background may carry 2..4 frames at 1..2 fps. */
      if(out->image_jpeg[i]&&graphics_v2){
        int fps;if(!number(im,"frames",1,PET_SURFACE_KEYFRAMES,&v)||!number(im,"fps",1,v>1?2:1,&fps))return false;
        if(v>1&&strlen(id->valuestring)>12)return false; /* "<id>.k" frame ids stay within 15 chars */
        out->keyframes=(uint8_t)v;out->keyframe_fps=(uint8_t)fps;
      }else
#endif
      if(!number(im,"frames",1,1,&v)||!number(im,"fps",1,1,&v))return false;
      if(!out->image_jpeg[i])pixels+=out->image_w[i]*out->image_h[i];
      if(pixels>max_pixels)return false;
      strcpy(out->image_ids[i],id->valuestring);out->image_count++;
    }
  }
#if PET_P4_BIG_IMAGE
  const cJSON *bigs=cJSON_GetObjectItemCaseSensitive(json,"big_images");
  if(bigs){
    static const char *const bk[]={"id","asset","width","height"};
    if(!out->variable_bindings||!cJSON_IsArray(bigs)||cJSON_GetArraySize(bigs)<1||cJSON_GetArraySize(bigs)>2)return false;
    for(int i=0;i<cJSON_GetArraySize(bigs);i++){
      const cJSON *b=cJSON_GetArrayItem(bigs,i),*id=cJSON_GetObjectItemCaseSensitive(b,"id");
      if(!keys(b,bk,4)||!cJSON_IsString(id)||!strlen(id->valuestring)||strlen(id->valuestring)>15)return false;
      for(const char *c=id->valuestring;*c;c++)if(!((*c>='a'&&*c<='z')||(*c>='0'&&*c<='9')||*c=='-'||*c=='_'))return false;
      for(int j=0;j<i;j++)if(!strcmp(out->big_ids[j],id->valuestring))return false;
      if(!number(b,"width",241,1280,&v))return false;out->big_w[i]=v;if(!number(b,"height",1,960,&v))return false;out->big_h[i]=v;
      strcpy(out->big_ids[i],id->valuestring);out->big_count++;
    }
  }
#endif
  if(scene){
    const cJSON *sprites=cJSON_GetObjectItemCaseSensitive(scene,"sprites");
    if(!cJSON_IsArray(sprites)||cJSON_GetArraySize(sprites)>(graphics_v2?PET_SURFACE_SCENE_SPRITES:PET_SURFACE_V1_SCENE_SPRITES))return false;
    static const char *const spkeys[]={"entity","image","columns","fps"};
    for(int i=0;i<cJSON_GetArraySize(sprites);i++){
      const cJSON *sp=cJSON_GetArrayItem(sprites,i),*entity=cJSON_GetObjectItemCaseSensitive(sp,"entity"),*image=cJSON_GetObjectItemCaseSensitive(sp,"image");
      pet_surface_scene_sprite_t *dst=&out->scene_view.sprites[i];
      if(!keys(sp,spkeys,4)||!cJSON_IsString(entity)||!entity->valuestring[0]||strlen(entity->valuestring)>15||!cJSON_IsString(image))return false;
      for(const char *p=entity->valuestring;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'||*p=='.'))return false;
      for(int j=0;j<i;j++)if(!strcmp(out->scene_view.sprites[j].entity,entity->valuestring))return false;
      int at=-1;for(int j=0;j<out->image_count;j++)if(!strcmp(out->image_ids[j],image->valuestring))at=j;
      if(at<0||out->image_jpeg[at])return false;
      dst->image=at;
      if(!number(sp,"columns",1,8,&v)||out->image_w[at]%v||out->image_w[at]/v<8)return false;
      dst->columns=v;
      if(!number(sp,"fps",1,20,&v))return false;
      dst->fps=v;
      strcpy(dst->entity,entity->valuestring);out->scene_view.count++;
    }
  }
  const cJSON *fonts=cJSON_GetObjectItemCaseSensitive(json,"fonts");
  if(fonts){
    if(!out->typography||!cJSON_IsArray(fonts)||cJSON_GetArraySize(fonts)<1||cJSON_GetArraySize(fonts)>(int)max_fonts)return false;
    static const char *const fk[]={"image","chars","metrics","cell"};
    for(int i=0;i<cJSON_GetArraySize(fonts);i++){
      const cJSON *f=cJSON_GetArrayItem(fonts,i),*im=cJSON_GetObjectItemCaseSensitive(f,"image"),*ch=cJSON_GetObjectItemCaseSensitive(f,"chars"),*m=cJSON_GetObjectItemCaseSensitive(f,"metrics");
      if(!keys(f,fk,4)||!cJSON_IsString(im)||!cJSON_IsString(ch)||!cJSON_IsArray(m))return false;
      int cw=32,cheight=32;const cJSON *cell=cJSON_GetObjectItemCaseSensitive(f,"cell");
      if(cell){
        if(!cJSON_IsArray(cell)||cJSON_GetArraySize(cell)!=2)return false;
        const cJSON *x=cJSON_GetArrayItem(cell,0),*y=cJSON_GetArrayItem(cell,1);
        if(!cJSON_IsNumber(x)||!cJSON_IsNumber(y)||x->valuedouble!=x->valueint||y->valuedouble!=y->valueint)return false;
        cw=x->valueint;cheight=y->valueint;
        if((cw!=16&&cw!=32&&cw!=48&&cw!=64)||(cheight!=32&&cheight!=48&&cheight!=64)||cw>cheight)return false;
      }
      int image=-1;for(int j=0;j<out->image_count;j++)if(!strcmp(im->valuestring,out->image_ids[j]))image=j;
      if(image<0||out->image_jpeg[image]||out->image_w[image]>(int)max_font_w||out->image_h[image]>240||out->image_w[image]%cw||out->image_h[image]%cheight)return false;
      for(int j=0;j<i;j++)if(out->fonts[j].image==image)return false;
      pet_surface_font_t *font=&out->fonts[i];font->image=image;font->cell_w=cw;font->cell_h=cheight;
      const unsigned char *p=(const unsigned char*)ch->valuestring;
      while(*p){
        if(font->count>=max_chars)return false;
        uint32_t cp=*p++;int more=0;
        if(cp>=0xc2&&cp<=0xdf){cp&=31;more=1;}
        else if(cp>=0xe0&&cp<=0xef){cp&=15;more=2;}
        else if(cp>=128)return false;
        for(int j=0;j<more;j++){if((*p&0xc0)!=0x80)return false;cp=(cp<<6)|(*p++&63);}
        if(cp<32||(cp>=127&&cp<160)||(more==1&&cp<128)||(more==2&&cp<2048)||(cp>=0xd800&&cp<=0xdfff))return false;
        for(int a=0;a<=i;a++)for(int b=0;b<out->fonts[a].count;b++)if(out->fonts[a].chars[b]==cp)return false;
        font->chars[font->count++]=cp;
      }
      if(!font->count||font->count>(out->image_w[image]/cw)*(out->image_h[image]/cheight)||cJSON_GetArraySize(m)!=font->count)return false;
      for(int j=0;j<font->count;j++){
        const cJSON *a=cJSON_GetArrayItem(m,j);if(!cJSON_IsArray(a)||cJSON_GetArraySize(a)!=3)return false;
        for(int k=0;k<3;k++){const cJSON *n=cJSON_GetArrayItem(a,k);if(!cJSON_IsNumber(n)||!isfinite(n->valuedouble)||n->valuedouble<(k==0?4:0)||n->valuedouble>32||n->valuedouble!=(int)n->valuedouble)return false;font->metrics[j][k]=n->valueint;}
        if(font->metrics[j][1]>font->metrics[j][2])return false;
      }
      out->font_count++;
    }
  }
  const cJSON *array=cJSON_GetObjectItemCaseSensitive(json,"elements");int count=cJSON_GetArraySize(array);
  if(!cJSON_IsArray(array)||count<1||count>((out->graphics||out->media_touch)?(int)max_nodes:20))return false;
  for(int i=0;i<count;i++){
    const cJSON *o=cJSON_GetArrayItem(array,i);pet_surface_node_t *n=&out->nodes[i];
    if(!PET_SURFACE_NODE_KEYS(o,node_keys,out->variable_bindings?((PET_P4_LONGTEXT||PET_P4_DATA_ROWS)?47:42):out->graphics?35:out->app?17:out->media_touch?14:13))return false;
    int type=choice(o,"type",types,out->graphics?(PET_P4_LONGTEXT?12:PET_P4_CELLS_V1?10:9):7);
#if PET_P4_CLIP
    if(type<0&&out->graphics&&out->variable_bindings){const cJSON *tp=cJSON_GetObjectItemCaseSensitive(o,"type");if(cJSON_IsString(tp)&&!strcmp(tp->valuestring,"clip"))type=PET_SURFACE_NODE_CLIP;}
#endif
    if(type<0)return false;
    n->type=type;
    if(type==9&&!PET_P4_CELLS_V1)return false;
    if(out->app&&!PET_SURFACE_EXT_TYPE(type)&&type!=0&&type!=1&&type!=2&&type!=6&&type!=7&&type!=8&&!(PET_P4_CELLS_V1&&type==PET_SURFACE_NODE_CELLS)&&type!=10&&type!=11)return false;
    const cJSON*inside=cJSON_GetObjectItemCaseSensitive(o,"in_view");if(inside){if(!out->viewport.enabled||!cJSON_IsTrue(inside))return false;n->in_view=true;if(cJSON_HasObjectItem(o,"motion"))return false;}
    const cJSON*view=cJSON_GetObjectItemCaseSensitive(o,"view");
    if(view){
      if(inside||!out->viewport_count||!cJSON_IsString(view)||cJSON_HasObjectItem(o,"motion"))return false;
      int at=-1;for(int j=0;j<out->viewport_count;j++)if(!strcmp(out->viewports[j].id,view->valuestring))at=j;
      if(at<0)return false;
      n->view_index=at;
      n->in_view=true;
    }
    const pet_surface_viewport_t *nv=pet_surface_node_viewport(out,n);
    int width=nv?nv->w:640,height=nv?nv->content_height:480;
    if(!number(o,"x",0,width-1,&v))return false;
    n->x=v;
    if(!number(o,"y",0,height-1,&v))return false;
    n->y=v;
    if(!number(o,"w",1,width-n->x,&v))return false;
    n->w=v;
    int mh=height-n->y;
    if(mh>480)mh=480;
    if(!number(o,"h",1,mh,&v))return false;
    n->h=v;
    if(!number(o,"color",0,65535,&v))return false;
    n->color=v;
#if PET_P4_CLIP
    if(type==PET_SURFACE_NODE_CLIP){
      const cJSON *asset=cJSON_GetObjectItemCaseSensitive(o,"asset");
      if(!cJSON_IsString(asset)||strlen(asset->valuestring)>120||strncmp(asset->valuestring,"assets/",7)||!((n->w==160&&n->h==120)||(n->w==320&&n->h==240))
        ||cJSON_HasObjectItem(o,"image")||cJSON_HasObjectItem(o,"action")||cJSON_HasObjectItem(o,"motion")||cJSON_HasObjectItem(o,"tile")||cJSON_HasObjectItem(o,"big"))return false;
      strcpy(n->text,asset->valuestring);
    }else if(cJSON_HasObjectItem(o,"asset"))return false;
#endif
    n->image=-1;n->icon=-1;n->scale=1;n->page=out->app?4:2;n->bind=255;n->opacity=255;n->opacity_end=255;
#if PET_P4_APP_SHELL
    if(out->shell)n->page=PET_SHELL_PAGE_SHARED;
#endif
#if PET_P4_LONGTEXT
    n->text_id=-1;
#endif
    if(cJSON_HasObjectItem(o,"state_mask")){if(!out->graphics||!number(o,"state_mask",1,PET_P4_QUOTA_V3?65535:63,&v))return false;n->state_mask=v;}
    if(cJSON_HasObjectItem(o,"host_mask")){if(!out->graphics||!number(o,"host_mask",1,255,&v))return false;n->host_mask=v;}
    if(cJSON_HasObjectItem(o,"opacity")){if(!out->app||(type!=0&&type!=8)||!number(o,"opacity",0,255,&v))return false;n->opacity=v;}
    n->opacity_end=n->opacity;
    if(cJSON_HasObjectItem(o,"opacity_end")){if(!out->app||type!=0||!number(o,"opacity_end",0,255,&v))return false;n->opacity_end=v;}
    if(cJSON_HasObjectItem(o,"page")){
#if PET_P4_APP_SHELL
      if(out->shell){if(!number(o,"page",0,PET_SHELL_PAGES-1,&v))return false;n->page=PET_SURFACE_SHELL_PAGE_BASE+v;}else
#endif
      {if(!number(o,"page",0,out->app?4:2,&v))return false;n->page=v;}
    }
    if(cJSON_HasObjectItem(o,"scale")){if(!number(o,"scale",1,3,&v))return false;n->scale=v;}
    if(cJSON_HasObjectItem(o,"radius")){if(!number(o,"radius",0,24,&v)||v*2>n->w||v*2>n->h)return false;n->radius=v;}
    n->foreground=n->color;n->font_size=16;n->line_height=20;n->icon_size=20;n->gap=9;
    n->align=type==8?1:0;n->valign=type==8?1:0;n->padding=type==8?16:0;
    const char *const ak[]={"left","center","right"},*const vk[]={"top","center","bottom"};
    if(cJSON_HasObjectItem(o,"align")){v=choice(o,"align",ak,3);if(v<0)return false;n->align=v;}
    if(cJSON_HasObjectItem(o,"valign")){v=choice(o,"valign",vk,3);if(v<0)return false;n->valign=v;}
    #define OPT_NUM(key,field,lo,hi) if(cJSON_HasObjectItem(o,key)){if(!number(o,key,lo,hi,&v))return false;n->field=v;}
    OPT_NUM("font_size",font_size,10,96);OPT_NUM("line_height",line_height,10,120);
    OPT_NUM("padding",padding,0,32);OPT_NUM("gap",gap,0,24);OPT_NUM("icon_size",icon_size,8,48);
    OPT_NUM("foreground",foreground,0,65535);OPT_NUM("stroke",stroke,0,65535);OPT_NUM("stroke_width",stroke_width,0,2);
    if(cJSON_HasObjectItem(o,"weight")){if(!number(o,"weight",400,600,&v)||(v!=400&&v!=600))return false;n->weight=v==600;}
    #undef OPT_NUM
    if(out->graphics&&!cJSON_HasObjectItem(o,"line_height"))n->line_height=(n->font_size*5+3)/4;
    if(n->line_height<n->font_size)return false;
    if(cJSON_HasObjectItem(o,"icon")){const cJSON *ic=cJSON_GetObjectItemCaseSensitive(o,"icon");if(!cJSON_IsString(ic)||(type!=7&&type!=8))return false;for(int k=0;k<PET_APP_ICON_COUNT;k++)if(!strcmp(ic->valuestring,pet_app_icons[k].name))n->icon=k;if(n->icon<0)return false;}
    if(type==7&&(n->icon<0||n->w>48||n->h>48))return false;
    if(cJSON_HasObjectItem(o,"gradient")){static const char *const gs[]={"horizontal","vertical"};v=choice(o,"gradient",gs,2);if(type!=0||v<0)return false;n->vertical=v==1;}
    if(cJSON_HasObjectItem(o,"bind")){int b=choice(o,"bind",out->app?app_binds:binds,out->app?10:8);if(b<0)return false;n->bind=b;}
    if(cJSON_HasObjectItem(o,"image_bind")||cJSON_HasObjectItem(o,"value_param")||cJSON_HasObjectItem(o,"value_min")||cJSON_HasObjectItem(o,"value_max")){
#if PET_SERVICE_BRIDGE_ENABLED
      if(!out->service_bridge)return false;
      const cJSON*binding=cJSON_GetObjectItemCaseSensitive(o,"image_bind"),*parameter=cJSON_GetObjectItemCaseSensitive(o,"value_param");
      if(binding){
        if(type!=6||!cJSON_IsString(binding)||!binding->valuestring[0]||strlen(binding->valuestring)>15||cJSON_HasObjectItem(o,"tile")||cJSON_HasObjectItem(o,"motion")||cJSON_HasObjectItem(o,"rotate_ms"))return false;
        for(const char*c=binding->valuestring;*c;c++)if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||*c=='.'||*c=='_'||*c=='-'))return false;strcpy(n->image_bind,binding->valuestring);
      }
      if(parameter){
        if(type!=2||!var_name(o,"value_param",n->value_param)||!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(o,"action"))||!cJSON_GetObjectItemCaseSensitive(o,"action")->valuestring[0])return false;
        n->value_min=0;n->value_max=100;
        if(cJSON_HasObjectItem(o,"value_min")){if(!number(o,"value_min",-1000000000,1000000000,&v))return false;n->value_min=v;}
        if(cJSON_HasObjectItem(o,"value_max")){if(!number(o,"value_max",-1000000000,1000000000,&v))return false;n->value_max=v;}
        if(n->value_min>=n->value_max)return false;
      }else if(cJSON_HasObjectItem(o,"value_min")||cJSON_HasObjectItem(o,"value_max"))return false;
#else
      return false;
#endif
    }
    if(cJSON_HasObjectItem(o,"var_bind")){
      if(!out->variable_bindings||(type!=1&&type!=2&&type!=8)||!var_name(o,"var_bind",n->var_bind)
        ||cJSON_HasObjectItem(o,"bind")||cJSON_HasObjectItem(o,"text")||cJSON_HasObjectItem(o,"key_for"))return false;
    }
    if(cJSON_HasObjectItem(o,"text_at")){
#if PET_P4_TEXT_TABLE
      if(!out->variable_bindings||(type!=1&&type!=8)||!var_name(o,"text_at",n->text_at)
        ||cJSON_HasObjectItem(o,"bind")||cJSON_HasObjectItem(o,"text")||cJSON_HasObjectItem(o,"var_bind")||cJSON_HasObjectItem(o,"field_bind")||cJSON_HasObjectItem(o,"key_for"))return false;
#else
      return false;
#endif
    }
    if(cJSON_HasObjectItem(o,"field_bind")){
      char id[32];
      if(!out->variable_bindings||(type!=1&&type!=8)||!var_name(o,"field_bind",id)||strlen(id)>15
        ||cJSON_HasObjectItem(o,"bind")||cJSON_HasObjectItem(o,"text")||cJSON_HasObjectItem(o,"var_bind")||cJSON_HasObjectItem(o,"key_for"))return false;
      strcpy(n->field_bind,id);
    }
    const cJSON *row_bind=cJSON_GetObjectItemCaseSensitive(o,"row_bind");
#if PET_P4_DATA_ROWS
    if(row_bind){
      int field=cJSON_IsString(row_bind)?pet_data_field_index(row_bind->valuestring):-1;
      if(!out->variable_bindings||(type!=1&&type!=8)||field<0||field==PET_DATA_FIELD_TONE||cJSON_HasObjectItem(o,"bind")||cJSON_HasObjectItem(o,"text")||cJSON_HasObjectItem(o,"var_bind")||cJSON_HasObjectItem(o,"field_bind")||cJSON_HasObjectItem(o,"key_for"))return false;
      n->row_bind=(uint8_t)(field+1);
    }
#else
    if(row_bind)return false;
#endif
    const cJSON *caption=cJSON_GetObjectItemCaseSensitive(o,"caption");
    bool longtext_keys=cJSON_HasObjectItem(o,"text_id")||cJSON_HasObjectItem(o,"page_var")||cJSON_HasObjectItem(o,"count_var");
    if((longtext_keys&&type!=10)||(caption&&type!=11))return false;
#if PET_P4_LONGTEXT
    if(type==10){
      /* longtext: id of a surface text, int page var, optional int count var; expanded into line nodes below. */
      const cJSON *tid=cJSON_GetObjectItemCaseSensitive(o,"text_id");char page_var[32],count_var[32]={0};
      if(!out->variable_bindings||!cJSON_IsString(tid)||!var_name(o,"page_var",page_var)||(cJSON_HasObjectItem(o,"count_var")&&!var_name(o,"count_var",count_var)))return false;
      if(caption||cJSON_HasObjectItem(o,"bind")||cJSON_HasObjectItem(o,"var_bind")||cJSON_HasObjectItem(o,"field_bind")||cJSON_HasObjectItem(o,"key_for")||cJSON_HasObjectItem(o,"action")||cJSON_HasObjectItem(o,"text")||n->in_view)return false;
      int id=-1;for(int j=0;j<out->text_count;j++)if(!strcmp(out->texts[j].id,tid->valuestring))id=j;
      if(id<0||out->texts[id].used||n->h<n->line_height||n->w<n->font_size||n->w>40*n->font_size||n->h/n->line_height>24)return false;
      pet_surface_longtext_t *lt=&out->texts[id];lt->used=true;lt->w=n->w;lt->font_size=n->font_size;lt->line_height=n->line_height;lt->lines_per_page=(uint8_t)pet_text_flow_per_page(n->h,n->line_height);
      strcpy(lt->page_var,page_var);strcpy(lt->count_var,count_var);n->text_id=(int8_t)id;n->text_line=lt->lines_per_page;
    }else if(type==11){
      /* figure: palette image above a one-line caption; expanded into image + text nodes below. */
      if(!out->variable_bindings||!cJSON_IsString(caption)||strlen(caption->valuestring)>120||cJSON_HasObjectItem(o,"text")||cJSON_HasObjectItem(o,"bind")||cJSON_HasObjectItem(o,"var_bind")||cJSON_HasObjectItem(o,"field_bind")||cJSON_HasObjectItem(o,"key_for")||cJSON_HasObjectItem(o,"action")||cJSON_HasObjectItem(o,"tile")||cJSON_HasObjectItem(o,"show")||cJSON_HasObjectItem(o,"motion")||n->in_view)return false;
      for(const unsigned char *c=(const unsigned char*)caption->valuestring;*c;c++)if(*c<32||*c==127)return false;
      if(n->h<n->line_height+8)return false;strcpy(n->text,caption->valuestring);
      const cJSON *im=cJSON_GetObjectItemCaseSensitive(o,"image");if(!cJSON_IsString(im))return false;
      for(int j=0;j<out->image_count;j++)if(!strcmp(im->valuestring,out->image_ids[j]))n->image=j;
      if(n->image<0||out->image_jpeg[n->image])return false;
    }
#else
    if(type==10||type==11||caption)return false;
#endif
    const cJSON *text=cJSON_GetObjectItemCaseSensitive(o,"text");
    if(text){if(!cJSON_IsString(text)||strlen(text->valuestring)>120)return false;for(const unsigned char *s=(void*)text->valuestring;*s;s++)if(*s<32||*s==127)return false;strcpy(n->text,text->valuestring);}
    int sources=(text!=NULL)+(n->bind!=255)+(n->var_bind[0]!=0)+(n->field_bind[0]!=0);
#if PET_P4_DATA_ROWS
    sources+=n->row_bind!=0;
#endif
#if PET_P4_TEXT_TABLE
    sources+=n->text_at[0]!=0;
#endif
    if((type==1||type==8)&&(sources!=1||n->h<(out->graphics?n->font_size:32*n->scale)))return false;
    if(type==8&&(2*n->padding>=n->w||(n->icon>=0&&n->icon_size>n->h)))return false;
    if(type==2&&(out->app?(n->bind!=9&&!n->var_bind[0]):(n->bind!=4&&n->bind!=7)))return false;
    const cJSON *action=cJSON_GetObjectItemCaseSensitive(o,"action");
    const cJSON *hint=cJSON_GetObjectItemCaseSensitive(o,"key_for");
    if(hint){
      if(!out->typography||type!=8||action||n->bind!=255||!cJSON_IsString(hint)||!strlen(hint->valuestring)||strlen(hint->valuestring)>47)return false;
      for(const char *p=hint->valuestring;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'||*p=='.'))return false;
      strcpy(n->key_for,hint->valuestring);
    }
    if(action){
      if((!out->app&&!out->media_touch)||!cJSON_IsString(action)||!strlen(action->valuestring)||strlen(action->valuestring)>47)return false;
      if(out->media_touch){
        const char *const actions[]={"media.toggle","media.queue","media.previous","media.next","media.up","media.down","media.select"};
        if(choice(o,"action",actions,7)<0)return false;
      }
      for(const char *p=action->valuestring;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'||*p=='.'))return false;
      strcpy(n->action,action->valuestring);
    }
    const cJSON *motion=cJSON_GetObjectItemCaseSensitive(o,"motion");
    if(motion){
      if(!out->app||type!=6||!cJSON_IsString(motion)||strcmp(motion->valuestring,"float")||n->y<4||n->y+n->h>476)return false;
      n->floating=true;
    }

    if(type==4&&n->h<48)return false;
#if PET_P4_BIG_IMAGE
    n->big=-1;
    if(cJSON_HasObjectItem(o,"big")){
      const cJSON *b=cJSON_GetObjectItemCaseSensitive(o,"big");
      if(type!=6||!out->variable_bindings||!cJSON_IsString(b)||cJSON_HasObjectItem(o,"image")||cJSON_HasObjectItem(o,"tile")||cJSON_HasObjectItem(o,"rotate_ms")||cJSON_HasObjectItem(o,"motion")||cJSON_HasObjectItem(o,"action")||n->in_view)return false;
      for(int j=0;j<out->big_count;j++)if(!strcmp(b->valuestring,out->big_ids[j]))n->big=j;
      if(n->big<0||!var_name(o,"offset_x_var",n->big_x_var)||!var_name(o,"offset_y_var",n->big_y_var)||n->w>out->big_w[n->big]||n->h>out->big_h[n->big])return false;
    }else if(cJSON_HasObjectItem(o,"offset_x_var")||cJSON_HasObjectItem(o,"offset_y_var"))return false;
    if(n->big>=0){ /* pannable tiles: no palette image slot */ }else
#endif
    if(type==6){
      const cJSON *im=cJSON_GetObjectItemCaseSensitive(o,"image");if(!cJSON_IsString(im))return false;
      for(int j=0;j<out->image_count;j++)if(!strcmp(im->valuestring,out->image_ids[j]))n->image=j;
      if(n->image<0)return false;
      if(cJSON_HasObjectItem(o,"columns")||cJSON_HasObjectItem(o,"cursor"))return false;
      if(out->image_jpeg[n->image]&&(n->in_view||i!=0||n->x||n->y||n->w!=640||n->h!=480||pet_surface_node_page_index(n)>=0||action||motion||n->state_mask||n->host_mask||cJSON_HasObjectItem(o,"tile")||cJSON_HasObjectItem(o,"show")))return false;
      if(cJSON_HasObjectItem(o,"rotate_ms")){
        if(out->app)return false;
        if(!number(o,"rotate_ms",8000,60000,&v)||n->w!=n->h||n->w>320||out->image_w[n->image]!=out->image_h[n->image])return false;
        n->rotate_ms=v;
      }
    }
#if PET_P4_CELLS_V1
    else if(type==PET_SURFACE_NODE_CELLS){
      /* Optional atlas: frame index = cell value (frame 0 is never drawn), one row of `columns` frames. */
      const cJSON *im=cJSON_GetObjectItemCaseSensitive(o,"image"),*cursor=cJSON_GetObjectItemCaseSensitive(o,"cursor");
      if(!out->variable_bindings||cJSON_HasObjectItem(o,"rotate_ms")||cJSON_HasObjectItem(o,"tile")||action||cJSON_HasObjectItem(o,"motion"))return false;
      if(im){
        if(!cJSON_IsString(im))return false;
        for(int j=0;j<out->image_count;j++)if(!strcmp(im->valuestring,out->image_ids[j]))n->image=j;
        if(n->image<0||out->image_jpeg[n->image]||!number(o,"columns",2,16,&v)||out->image_w[n->image]%v||out->image_w[n->image]/v<4)return false;
        n->tile_cols=v;n->tile_rows=1;
      }else if(cJSON_HasObjectItem(o,"columns"))return false;
      n->cursor_col=n->cursor_row=-1;
      if(cursor){
        static const char *const ck[]={"x_var","y_var","color"};
        if(!keys(cursor,ck,3)||!var_name(cursor,"x_var",n->cursor_x)||!var_name(cursor,"y_var",n->cursor_y)||!number(cursor,"color",0,65535,&v))return false;
        n->cursor_color=v;
      }
    }
#endif
    else if(type!=11&&(cJSON_HasObjectItem(o,"image")||cJSON_HasObjectItem(o,"rotate_ms")||cJSON_HasObjectItem(o,"columns")||cJSON_HasObjectItem(o,"cursor")))return false;
    else if(type==11&&(cJSON_HasObjectItem(o,"rotate_ms")||cJSON_HasObjectItem(o,"columns")||cJSON_HasObjectItem(o,"cursor")))return false;
    const cJSON *tile=cJSON_GetObjectItemCaseSensitive(o,"tile"),*show=cJSON_GetObjectItemCaseSensitive(o,"show");
    if(tile){
      static const char *const tk[]={"columns","rows","row","var","max_var"};
      if(!out->interaction||type!=6||!keys(tile,tk,5)||!var_name(tile,"var",n->tile_var))return false;
      if(!number(tile,"columns",1,8,&v))return false;
      n->tile_cols=v;
      if(!number(tile,"rows",1,8,&v))return false;
      n->tile_rows=v;
      if(!number(tile,"row",0,n->tile_rows-1,&v))return false;
      n->tile_row=v;
      if(out->image_w[n->image]%n->tile_cols||out->image_h[n->image]%n->tile_rows||out->image_w[n->image]/n->tile_cols<8||out->image_h[n->image]/n->tile_rows<8)return false;
      if(cJSON_HasObjectItem(tile,"max_var")&&!var_name(tile,"max_var",n->tile_max_var))return false;
    }
    if(show){
      static const char *const sk[]={"var","min","max"};
      if(!out->interaction||!keys(show,sk,3)||!var_name(show,"var",n->show_var)||!number(show,"min",-1000000000,1000000000,&v))return false;
      n->show_min=v;
      if(!number(show,"max",n->show_min,1000000000,&v))return false;
      n->show_max=v;
    }
  }
  out->count=count;
#if PET_P4_LONGTEXT
  {
    /* Expand longtext/figure nodes in place, back to front, so each line/caption is a standard node. */
    unsigned total=0;
    for(int i=0;i<count;i++){const pet_surface_node_t *n=&out->nodes[i];total+=n->type==10?n->text_line:n->type==11?2:1;}
    if(total>max_nodes)return false;
    unsigned dest=total;
    for(int i=count-1;i>=0;i--){
      pet_surface_node_t src=out->nodes[i];
      if(src.type==10){
        dest-=src.text_line;
        for(unsigned l=0;l<src.text_line;l++){
          pet_surface_node_t *n=&out->nodes[dest+l];*n=src;n->type=1;n->text_line=(uint8_t)l;n->y=(int16_t)(src.y+(int)l*src.line_height);n->h=src.line_height;n->text[0]=0;
        }
      }else if(src.type==11){
        dest-=2;
        pet_surface_node_t *im=&out->nodes[dest],*cap=&out->nodes[dest+1];
        *im=src;im->type=6;im->h=(uint16_t)(src.h-src.line_height);im->text[0]=0;
        *cap=src;cap->type=1;cap->image=-1;cap->y=(int16_t)(src.y+src.h-src.line_height);cap->h=src.line_height;cap->align=1;
      }else{dest-=1;out->nodes[dest]=src;}
    }
    out->count=(uint8_t)total;count=(int)total;
  }
#endif
#if PET_P4_APP_SHELL
  if(out->tabs.enabled){
    const pet_shell_tabs_t *t=&out->tabs;
    if(game||scene||out->media_touch||count+t->count>(int)max_nodes)return false;
    for(int i=0;i<count;i++){const pet_surface_node_t *n=&out->nodes[i];
      if(n->action[0]&&!n->in_view&&t->x<n->x+n->w&&t->x+t->w>n->x&&t->y<n->y+n->h&&t->y+t->h>n->y)return false;
    }
    for(unsigned i=0;i<t->count;i++){
      pet_surface_node_t *n=&out->nodes[out->count++];memset(n,0,sizeof(*n));
      pet_shell_tab_box(t,i,&n->x,&n->w);n->y=t->y;n->h=t->h;n->type=8;n->page=PET_SHELL_PAGE_SHARED;n->bind=255;n->image=-1;n->icon=t->items[i].icon;
      n->scale=1;n->opacity=n->opacity_end=255;n->font_size=14;n->line_height=18;n->icon_size=18;n->gap=6;n->padding=4;n->align=1;n->valign=1;
      n->color=t->colors[0];n->foreground=t->colors[2];n->tab_slot=(uint8_t)(i+1);strcpy(n->text,t->items[i].label);
#if PET_P4_LONGTEXT
      n->text_id=-1;
#endif
#if PET_P4_BIG_IMAGE
      n->big=-1;
#endif
    }
    count=out->count;
  }
#endif
#if PET_HOST_COLLECTION_ENABLED
  if(collection){
    const pet_host_collection_config_t *c=&out->collection;
    int height=c->visible*c->row_height+(c->visible-1)*c->gap;
    if(game||data||scene||out->media_touch||count+3*c->visible>(int)max_nodes)return false;
    for(int i=0;i<(out->viewport.enabled?1:out->viewport_count);i++){
      const pet_surface_viewport_t *v=out->viewport.enabled?&out->viewport:&out->viewports[i];
      if(c->x<v->x+v->w&&c->x+c->w>v->x&&c->y<v->y+v->h&&c->y+height>v->y)return false;
    }
    for(int i=0;i<count;i++){const pet_surface_node_t *n=&out->nodes[i];
      if(n->action[0]&&!n->in_view&&c->x<n->x+n->w&&c->x+c->w>n->x&&c->y<n->y+n->h&&c->y+height>n->y)return false;
    }
    for(unsigned slot=0;slot<c->visible;slot++)for(unsigned field=0;field<3;field++){
      pet_surface_node_t *n=&out->nodes[out->count++];memset(n,0,sizeof(*n));
#if PET_P4_LONGTEXT
      /* Collection labels are not lines of longtext block zero. */
      n->text_id=-1;
#endif
#if PET_P4_BIG_IMAGE
      n->big=-1;
#endif
      n->collection_slot=slot+1;n->collection_field=field;n->x=c->x;n->y=c->y+slot*(c->row_height+c->gap);
      n->w=c->w;n->h=c->row_height;n->type=field?1:8;n->page=c->page;n->bind=255;n->image=n->icon=-1;
      n->scale=1;n->opacity=n->opacity_end=255;n->font_size=field==2?12:16;n->line_height=field==2?16:20;
      if(!field){n->radius=12;n->color=c->colors[0];n->foreground=c->colors[2];n->stroke=c->colors[4];n->stroke_width=1;strcpy(n->action,c->actions[slot]);}
      else {n->x+=12;n->y+=field==1?6:28;n->w-=24;n->h=field==1?20:16;n->weight=field==1;n->color=c->colors[field==1?2:3];}
    }
  }
#endif
  return true;
}

#if PET_HOST_COLLECTION_ENABLED
void pet_surface_resolve_collection(pet_surface_t *s,const pet_host_collection_snapshot_t *snapshot,bool available){
  if(!s||!s->collection.enabled)return;
  available=available&&pet_host_collection_matches(&s->collection,snapshot);
  for(unsigned i=0;i<s->count;i++){
    pet_surface_node_t *n=&s->nodes[i];if(!n->collection_slot)continue;
    unsigned slot=n->collection_slot-1;
    const pet_host_collection_row_t *r=available&&slot<snapshot->visible?&snapshot->rows[slot]:NULL;
    bool placeholder=slot==0&&n->collection_field==1&&(!available||!snapshot->count);
    n->hidden|=(!r||!r->present)&&!placeholder;
    if(!n->collection_field)n->disabled|=!r||!r->present;
    if(!n->collection_field){n->color=s->collection.colors[r&&r->selected?1:0];n->stroke=s->collection.colors[r&&r->selected?5:4];}
    else {const char *text=placeholder?(available?"暂无条目":"等待电脑数据"):r&&r->present?(n->collection_field==1?r->label:r->detail):"";strcpy(n->text,text);}
  }
}
#endif

#if PET_P4_APP_SHELL
void pet_surface_resolve_tabs(pet_surface_t *s,int32_t active){
  if(!s||!s->tabs.enabled)return;
  unsigned at=pet_shell_tab_index(&s->tabs,active);
  for(unsigned i=0;i<s->count;i++){
    pet_surface_node_t *n=&s->nodes[i];if(!n->tab_slot)continue;
    bool on=n->tab_slot-1==at;n->color=s->tabs.colors[on?1:0];n->foreground=s->tabs.colors[on?3:2];n->weight=on;
  }
}
#endif
#if PET_P4_LONGTEXT
unsigned pet_surface_resolve_longtext(pet_surface_t *s,int id,int32_t page_value){
  if(!s||id<0||id>=s->text_count||!s->texts[id].used)return 1;
  const pet_surface_longtext_t *lt=&s->texts[id];
  pet_text_line_t lines[PET_TEXT_FLOW_MAX_LINES];bool truncated;
  unsigned n=pet_text_flow_lines(lt->text,lt->w,lt->font_size,lines,PET_TEXT_FLOW_MAX_LINES,&truncated);
  unsigned per=lt->lines_per_page?lt->lines_per_page:1,pages=pet_text_flow_pages(n,per),page=pet_text_flow_page_index(page_value,pages);
  for(unsigned i=0;i<s->count;i++){
    pet_surface_node_t *node=&s->nodes[i];if(node->text_id!=id||node->type!=1)continue;
    unsigned at=page*per+node->text_line;
    if(at<n)pet_text_flow_copy(lt->text,&lines[at],node->text,sizeof(node->text));else node->text[0]=0;
  }
  return pages;
}
#endif
#if PET_P4_DATA_ROWS
void pet_surface_resolve_rows(pet_surface_t *s,const pet_p4_data_row_t *selected){
  if(!s)return;
  for(unsigned i=0;i<s->count;i++){
    pet_surface_node_t *n=&s->nodes[i];if(!n->row_bind)continue;
    char scratch[5];const char *text=selected?pet_data_row_field(selected,n->row_bind-1,scratch):"";
    strncpy(n->text,text,sizeof(n->text)-1);n->text[sizeof(n->text)-1]=0;
  }
}
#endif

const pet_surface_viewport_t *pet_surface_node_viewport(const pet_surface_t*s,const pet_surface_node_t*n){
  if(!s||!n||!n->in_view)return NULL;
  if(s->viewport_count)return n->view_index<s->viewport_count?&s->viewports[n->view_index]:NULL;
  return s->viewport.enabled?&s->viewport:NULL;
}
bool pet_surface_project(const pet_surface_viewport_t*v,const pet_surface_node_t*n,int32_t offset,pet_surface_node_t*out){
  if(!n||!out)return false;
  *out=*n;
  if(!n->in_view)return true;
  if(!v||!v->enabled)return false;
  offset=pet_surface_clamp_offset(v,offset);
  out->x=n->x+v->x;out->y=n->y+v->y-offset;
  return out->x<(int)v->x+v->w&&out->x+out->w>v->x&&out->y<(int)v->y+v->h&&out->y+out->h>v->y;
}
int32_t pet_surface_clamp_offset(const pet_surface_viewport_t*v,int32_t offset){
  if(!v||!v->enabled)return 0;
  int32_t max=v->content_height-v->h;if(offset<0)offset=0;if(offset>max)offset=max;
  return offset;
}
bool pet_surface_hit(const pet_surface_viewport_t*v,const pet_surface_node_t*n,int32_t offset,int x,int y){
  pet_surface_node_t projected;
  if(!pet_surface_project(v,n,offset,&projected))return false;
  if(n->in_view&&(x<v->x||x>=v->x+v->w||y<v->y||y>=v->y+v->h))return false;
  return x>=projected.x&&x<projected.x+projected.w&&y>=projected.y&&y<projected.y+projected.h;
}
