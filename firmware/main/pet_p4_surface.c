#include "pet_p4_surface.h"
#include <string.h>
#include <math.h>
#include "pet_p4_app_icons.h"
static bool number(const cJSON *o,const char *k,int lo,int hi,int *out){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,k);if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||v->valuedouble<lo||v->valuedouble>hi||v->valuedouble!=(int)v->valuedouble)return false;*out=v->valueint;return true;}
static int choice(const cJSON *o,const char *key,const char *const *values,int count){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);if(!cJSON_IsString(v))return -1;for(int i=0;i<count;i++)if(!strcmp(v->valuestring,values[i]))return i;return -1;}
static bool keys(const cJSON *o,const char *const *allowed,int count){if(!cJSON_IsObject(o))return false;const cJSON *v; cJSON_ArrayForEach(v,o){bool found=false;for(int i=0;i<count;i++)if(!strcmp(v->string,allowed[i]))found=true;if(!found)return false;}return true;}
static bool var_name(const cJSON *o,const char *key,char out[32]){const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);if(!cJSON_IsString(v)||!v->valuestring[0]||strlen(v->valuestring)>31)return false;for(const char *p=v->valuestring;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'||*p=='.'))return false;strcpy(out,v->valuestring);return true;}
bool pet_surface_parse(const cJSON *json,pet_surface_t *out){
  static const char *const root_keys[]={"background","elements","images","engine","graphics","typography","fonts","interaction","game_view","data_view","scene_view","input"};
  static const char *const node_keys[]={"type","x","y","w","h","color","page","scale","radius","bind","text","image","rotate_ms","action","motion","opacity","opacity_end","font_size","line_height","align","valign","padding","gap","icon_size","weight","foreground","stroke","stroke_width","icon","gradient","key_for","tile","show","state_mask","host_mask"};
  static const char *const types[]={"panel","text","meter","ticks","list","transport","image","icon","button"};
  static const char *const binds[]={"title","subtitle","status","message","volume","index","count","level"};
  static const char *const app_binds[]={"title","eyebrow","headline","metricLabel","metricValue","metricUnit","badge","note","footer","progress"};
  memset(out,0,sizeof(*out));int v;
  const cJSON *engine=cJSON_GetObjectItemCaseSensitive(json,"engine");
  if(engine){if(!cJSON_IsString(engine)||strcmp(engine->valuestring,"p4-app-surface-v1"))return false;out->app=true;}
  const cJSON *input=cJSON_GetObjectItemCaseSensitive(json,"input");
  if(input){if(out->app||!cJSON_IsString(input)||strcmp(input->valuestring,"p4-media-touch-v1"))return false;out->media_touch=true;}
  if(cJSON_HasObjectItem(json,"graphics")){const cJSON *g=cJSON_GetObjectItemCaseSensitive(json,"graphics");if(!out->app||!cJSON_IsString(g)||strcmp(g->valuestring,"p4-app-graphics-v1"))return false;out->graphics=true;}
  const cJSON *ty=cJSON_GetObjectItemCaseSensitive(json,"typography");
  if(ty){if(!out->graphics||!cJSON_IsString(ty)||strcmp(ty->valuestring,"p4-app-type-v1"))return false;out->typography=true;}
  const cJSON *interaction=cJSON_GetObjectItemCaseSensitive(json,"interaction");
  if(interaction){if(!out->typography||!cJSON_IsString(interaction)||strcmp(interaction->valuestring,"p4-app-interaction-v1"))return false;out->interaction=true;}
  const cJSON *game=cJSON_GetObjectItemCaseSensitive(json,"game_view");
  if(game){
    static const char *const gkeys[]={"engine","x","y","w","h"};
    const cJSON *ge=cJSON_GetObjectItemCaseSensitive(game,"engine");
    if(!out->graphics||!keys(game,gkeys,5)||!cJSON_IsString(ge)||strcmp(ge->valuestring,"p4-game-surface-v1"))return false;
    if(!number(game,"x",0,639,&v))return false;out->game_x=v;
    if(!number(game,"y",0,479,&v))return false;out->game_y=v;
    if(!number(game,"w",64,640-out->game_x,&v))return false;out->game_w=v;
    if(!number(game,"h",64,480-out->game_y,&v))return false;out->game_h=v;out->game_view=true;
  }
  const cJSON *data=cJSON_GetObjectItemCaseSensitive(json,"data_view");
  if(data){
    static const char *const dk[]={"engine","x","y","w","h","layout","colors"};
    const cJSON *de=cJSON_GetObjectItemCaseSensitive(data,"engine"),*colors=cJSON_GetObjectItemCaseSensitive(data,"colors");
    const char *const layouts[]={"metrics","todos"};
    if(game||!out->graphics||!keys(data,dk,7)||!cJSON_IsString(de)||strcmp(de->valuestring,"p4-data-surface-v1"))return false;
    int layout=choice(data,"layout",layouts,2);if(layout<0)return false;out->data_view.todos=layout==1;
    if(!number(data,"x",0,639,&v))return false;out->data_view.x=v;
    if(!number(data,"y",0,479,&v))return false;out->data_view.y=v;
    if(!number(data,"w",320,640-out->data_view.x,&v))return false;out->data_view.w=v;
    if(!number(data,"h",300,480-out->data_view.y,&v))return false;out->data_view.h=v;
    if(!cJSON_IsArray(colors)||cJSON_GetArraySize(colors)!=5)return false;
    for(int i=0;i<5;i++){const cJSON *c=cJSON_GetArrayItem(colors,i);if(!cJSON_IsNumber(c)||!isfinite(c->valuedouble)||c->valuedouble<0||c->valuedouble>65535||c->valuedouble!=(int)c->valuedouble)return false;out->data_view.colors[i]=c->valueint;}
    out->data_view.enabled=true;
  }
  const cJSON *scene=cJSON_GetObjectItemCaseSensitive(json,"scene_view");
  if(scene){
    static const char *const sk[]={"engine","x","y","w","h","colors","sprites"};
    const cJSON *se=cJSON_GetObjectItemCaseSensitive(scene,"engine"),*colors=cJSON_GetObjectItemCaseSensitive(scene,"colors");
    if(game||data||!out->graphics||!keys(scene,sk,7)||!cJSON_IsString(se)||strcmp(se->valuestring,"p4-scene-surface-v1"))return false;
    if(!number(scene,"x",0,639,&v))return false;out->scene_view.x=v;
    if(!number(scene,"y",0,479,&v))return false;out->scene_view.y=v;
    if(!number(scene,"w",64,640-out->scene_view.x,&v))return false;out->scene_view.w=v;
    if(!number(scene,"h",64,480-out->scene_view.y,&v))return false;out->scene_view.h=v;
    if(!cJSON_IsArray(colors)||cJSON_GetArraySize(colors)!=5)return false;
    for(int i=0;i<5;i++){const cJSON *c=cJSON_GetArrayItem(colors,i);if(!cJSON_IsNumber(c)||!isfinite(c->valuedouble)||c->valuedouble<0||c->valuedouble>65535||c->valuedouble!=(int)c->valuedouble)return false;out->scene_view.colors[i]=c->valueint;}
    out->scene_view.enabled=true;
  }
  if(!keys(json,root_keys,out->media_touch?12:scene?11:data?10:game?9:out->interaction?8:out->typography?7:out->app?5:3)||!number(json,"background",0,65535,&v))return false;out->background=v;

  const cJSON *images=cJSON_GetObjectItemCaseSensitive(json,"images");
  if(images) {
    if(!cJSON_IsArray(images)||cJSON_GetArraySize(images)<1||cJSON_GetArraySize(images)>4)return false;
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
      if(!number(im,"frame_width",8,out->image_jpeg[i]?640:512,&v))return false;out->image_w[i]=v;
      if(!number(im,"frame_height",8,out->image_jpeg[i]?480:240,&v))return false;out->image_h[i]=v;
      if(out->image_jpeg[i]&&(i!=0||out->image_w[i]!=640||out->image_h[i]!=480))return false;
      if(!number(im,"frames",1,1,&v)||!number(im,"fps",1,1,&v))return false;
      if(!out->image_jpeg[i])pixels+=out->image_w[i]*out->image_h[i];if(pixels>98304)return false;
      strcpy(out->image_ids[i],id->valuestring);out->image_count++;
    }
  }
  if(scene){
    const cJSON *sprites=cJSON_GetObjectItemCaseSensitive(scene,"sprites");
    if(!cJSON_IsArray(sprites)||cJSON_GetArraySize(sprites)>4)return false;
    static const char *const spkeys[]={"entity","image","columns","fps"};
    for(int i=0;i<cJSON_GetArraySize(sprites);i++){
      const cJSON *sp=cJSON_GetArrayItem(sprites,i),*entity=cJSON_GetObjectItemCaseSensitive(sp,"entity"),*image=cJSON_GetObjectItemCaseSensitive(sp,"image");
      pet_surface_scene_sprite_t *dst=&out->scene_view.sprites[i];
      if(!keys(sp,spkeys,4)||!cJSON_IsString(entity)||!entity->valuestring[0]||strlen(entity->valuestring)>15||!cJSON_IsString(image))return false;
      for(const char *p=entity->valuestring;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'||*p=='.'))return false;
      for(int j=0;j<i;j++)if(!strcmp(out->scene_view.sprites[j].entity,entity->valuestring))return false;
      int at=-1;for(int j=0;j<out->image_count;j++)if(!strcmp(out->image_ids[j],image->valuestring))at=j;
      if(at<0||out->image_jpeg[at])return false;dst->image=at;
      if(!number(sp,"columns",1,8,&v)||out->image_w[at]%v||out->image_w[at]/v<8)return false;dst->columns=v;
      if(!number(sp,"fps",1,20,&v))return false;dst->fps=v;
      strcpy(dst->entity,entity->valuestring);out->scene_view.count++;
    }
  }
  const cJSON *fonts=cJSON_GetObjectItemCaseSensitive(json,"fonts");
  if(fonts){
    if(!out->typography||!cJSON_IsArray(fonts)||cJSON_GetArraySize(fonts)<1||cJSON_GetArraySize(fonts)>2)return false;
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
      if(image<0||out->image_jpeg[image]||out->image_w[image]>240||out->image_h[image]>240||out->image_w[image]%cw||out->image_h[image]%cheight)return false;
      for(int j=0;j<i;j++)if(out->fonts[j].image==image)return false;
      pet_surface_font_t *font=&out->fonts[i];font->image=image;font->cell_w=cw;font->cell_h=cheight;
      const unsigned char *p=(const unsigned char*)ch->valuestring;
      while(*p){
        if(font->count>=49)return false;
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
  if(!cJSON_IsArray(array)||count<1||count>((out->graphics||out->media_touch)?PET_SURFACE_NODES:20))return false;
  for(int i=0;i<count;i++){
    const cJSON *o=cJSON_GetArrayItem(array,i);pet_surface_node_t *n=&out->nodes[i];
    if(!keys(o,node_keys,out->graphics?35:out->app?17:out->media_touch?14:13))return false;
    int type=choice(o,"type",types,out->graphics?9:7);if(type<0)return false;n->type=type;
    if(out->app&&type!=0&&type!=1&&type!=2&&type!=6&&type!=7&&type!=8)return false;
    if(!number(o,"x",0,639,&v))return false;n->x=v;if(!number(o,"y",0,479,&v))return false;n->y=v;
    if(!number(o,"w",1,640-n->x,&v))return false;n->w=v;if(!number(o,"h",1,480-n->y,&v))return false;n->h=v;
    if(!number(o,"color",0,65535,&v))return false;n->color=v;
    n->image=-1;n->icon=-1;n->scale=1;n->page=out->app?4:2;n->bind=255;n->opacity=255;n->opacity_end=255;
    if(cJSON_HasObjectItem(o,"state_mask")){if(!out->graphics||!number(o,"state_mask",1,63,&v))return false;n->state_mask=v;}
    if(cJSON_HasObjectItem(o,"host_mask")){if(!out->graphics||!number(o,"host_mask",1,255,&v))return false;n->host_mask=v;}
    if(cJSON_HasObjectItem(o,"opacity")){if(!out->app||(type!=0&&type!=8)||!number(o,"opacity",0,255,&v))return false;n->opacity=v;}
    n->opacity_end=n->opacity;
    if(cJSON_HasObjectItem(o,"opacity_end")){if(!out->app||type!=0||!number(o,"opacity_end",0,255,&v))return false;n->opacity_end=v;}
    if(cJSON_HasObjectItem(o,"page")){if(!number(o,"page",0,out->app?4:2,&v))return false;n->page=v;}
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
    const cJSON *text=cJSON_GetObjectItemCaseSensitive(o,"text");
    if(text){if(!cJSON_IsString(text)||strlen(text->valuestring)>120)return false;for(const unsigned char *s=(void*)text->valuestring;*s;s++)if(*s<32||*s==127)return false;strcpy(n->text,text->valuestring);}
    if((type==1||type==8)&&((text!=NULL)==(n->bind!=255)||n->h<(out->graphics?n->font_size:32*n->scale)))return false;
    if(type==8&&(2*n->padding>=n->w||(n->icon>=0&&n->icon_size>n->h)))return false;
    if(type==2&&(out->app?n->bind!=9:(n->bind!=4&&n->bind!=7)))return false;
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
    if(type==6){
      const cJSON *im=cJSON_GetObjectItemCaseSensitive(o,"image");if(!cJSON_IsString(im))return false;
      for(int j=0;j<out->image_count;j++)if(!strcmp(im->valuestring,out->image_ids[j]))n->image=j;
      if(n->image<0)return false;
      if(out->image_jpeg[n->image]&&(i!=0||n->x||n->y||n->w!=640||n->h!=480||n->page!=4||action||motion||n->state_mask||n->host_mask||cJSON_HasObjectItem(o,"tile")||cJSON_HasObjectItem(o,"show")))return false;
      if(cJSON_HasObjectItem(o,"rotate_ms")){
        if(out->app)return false;
        if(!number(o,"rotate_ms",8000,60000,&v)||n->w!=n->h||n->w>320||out->image_w[n->image]!=out->image_h[n->image])return false;
        n->rotate_ms=v;
      }
    }else if(cJSON_HasObjectItem(o,"image")||cJSON_HasObjectItem(o,"rotate_ms"))return false;
    const cJSON *tile=cJSON_GetObjectItemCaseSensitive(o,"tile"),*show=cJSON_GetObjectItemCaseSensitive(o,"show");
    if(tile){
      static const char *const tk[]={"columns","rows","row","var","max_var"};
      if(!out->interaction||type!=6||!keys(tile,tk,5)||!var_name(tile,"var",n->tile_var))return false;
      if(!number(tile,"columns",1,8,&v))return false;n->tile_cols=v;
      if(!number(tile,"rows",1,8,&v))return false;n->tile_rows=v;
      if(!number(tile,"row",0,n->tile_rows-1,&v))return false;n->tile_row=v;
      if(out->image_w[n->image]%n->tile_cols||out->image_h[n->image]%n->tile_rows||out->image_w[n->image]/n->tile_cols<8||out->image_h[n->image]/n->tile_rows<8)return false;
      if(cJSON_HasObjectItem(tile,"max_var")&&!var_name(tile,"max_var",n->tile_max_var))return false;
    }
    if(show){
      static const char *const sk[]={"var","min","max"};
      if(!out->interaction||!keys(show,sk,3)||!var_name(show,"var",n->show_var)||!number(show,"min",-1000000000,1000000000,&v))return false;n->show_min=v;
      if(!number(show,"max",n->show_min,1000000000,&v))return false;n->show_max=v;
    }
  }
  out->count=count;return true;
}
