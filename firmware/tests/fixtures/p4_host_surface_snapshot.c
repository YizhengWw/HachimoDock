/* Synthetic, credential-free data. Actual resolver bodies are inserted by the
 * test adapter; this is not a replacement implementation of those getters. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PET_P4_TEXT_TABLE 1
#define PET_P4_APP_DIRTY_CACHE 1
#define PET_P4_BIG_IMAGE 1
#define PET_P4_CELLS_V1 1
#define MINIAPP_VAR_NAME_MAX 32
#define MINIAPP_STRING_MAX 127
#define PET_FORM_VARS 8
#include "pet_p4_surface.h"
#include "pet_p4_host_render.h"
/* ACTUAL_VARIABLE_DECLARATION */
typedef struct {
    bool active,app_host;
    pet_surface_t surface;
    miniapp_var_t vars[8];
    int var_count,text_count,current_state,current_page,button_count;
    char text_pool[4160];
    uint16_t text_offset[64];
    struct {char action[48],event[48];} buttons[8];
    int form;
} miniapp_runtime_t;
static miniapp_runtime_t g_runtime;
static int g_pressed_node=-1;
static int32_t g_pressed_scroll[PET_SURFACE_VIEWPORTS+1];
static uint64_t g_pressed_context;
static uint32_t g_viewport_epoch,g_image_revision=1;
/* Unused form/action feature boundaries must not silently succeed if reached.
 * Fixtures have no form/action bindings. Production resolver and getters are
 * never stubbed, and no live host state API is provided by this adapter. */
static bool runtime_action_enabled(const miniapp_runtime_t *r,const char *a){(void)r;(void)a;abort();}
static void form_values(const miniapp_runtime_t *r,int32_t *v){(void)r;(void)v;abort();}
static int pet_form_field_index(const void *f,const char *s){(void)f;(void)s;abort();}
static void pet_form_text(const void *f,const int32_t *v,int i,char *out){(void)f;(void)v;(void)i;(void)out;abort();}
/* ACTUAL_RESOLVERS */

static pet_host_render_t host(int selector,int x,int y){
    pet_host_render_t h={.current=true,.fresh=true,.count=3};
    const char *names[]={"selector","pan_x","pan_y"};int values[]={selector,x,y};
    for(unsigned i=0;i<3;i++){
        strcpy(h.variables[i].name,names[i]);h.variables[i].integer=true;
        h.variables[i].number=values[i];
        snprintf(h.variables[i].text,sizeof(h.variables[i].text),"%d",values[i]);
    }
    return h;
}
static void reset(void){
    memset(&g_runtime,0,sizeof(g_runtime));
    g_runtime.active=true;g_runtime.app_host=true;g_runtime.surface.app=true;
    const char *names[]={"selector","pan_x","pan_y"};
    g_runtime.var_count=3;
    for(int i=0;i<3;i++){
        strcpy(g_runtime.vars[i].name,names[i]);g_runtime.vars[i].type=MINIAPP_VAR_INT;
        g_runtime.vars[i].int_min=-100;g_runtime.vars[i].int_max=100;
    }
    g_runtime.vars[0].int_min=0;g_runtime.vars[0].int_max=2;
    const char table[]="zero\0one\0two";
    memcpy(g_runtime.text_pool,table,sizeof(table));g_runtime.text_count=3;
    g_runtime.text_offset[0]=0;g_runtime.text_offset[1]=5;g_runtime.text_offset[2]=9;
    g_runtime.surface.count=3;
    for(int i=0;i<3;i++)g_runtime.surface.nodes[i].big=-1;
    strcpy(g_runtime.surface.nodes[0].text_at,"selector");
    g_runtime.surface.nodes[1].big=0;
    strcpy(g_runtime.surface.nodes[1].big_x_var,"pan_x");
    strcpy(g_runtime.surface.nodes[1].big_y_var,"pan_y");
    strcpy(g_runtime.surface.nodes[2].var_bind,"selector");
}
static pet_surface_t resolve(const pet_host_render_t *h){
    pet_surface_t out;memset(&out,0xa5,sizeof(out));
    assert(app_surface_locked(&out,h));return out;
}
static void expect(const pet_surface_t *s,const char *text,int x,int y){
    if(strcmp(s->nodes[0].text,text)||s->nodes[1].big_x!=x||s->nodes[1].big_y!=y){
        fprintf(stderr,"expected text=%s pan=(%d,%d); actual text=%s pan=(%ld,%ld)\n",
                text,x,y,s->nodes[0].text,(long)s->nodes[1].big_x,(long)s->nodes[1].big_y);
        exit(3);
    }
}
int main(int argc,char **argv){
    assert(argc==2);int mode=atoi(argv[1]);reset();
    pet_host_render_t h=host(1,17,-9);
    switch(mode){
    case 0:{
        pet_host_render_t before=h;miniapp_runtime_t runtime=g_runtime;
        pet_surface_t out=resolve(&h);expect(&out,"one",17,-9);
        assert(out.nodes[2].bound_value==1);
        assert(!memcmp(&before,&h,sizeof(h)));assert(!memcmp(&runtime,&g_runtime,sizeof(runtime)));
        break;
    }
    case 1:{
        pet_host_render_t newer=host(2,40,12);
        pet_surface_t a=resolve(&h),b=resolve(&newer),again=resolve(&h);
        expect(&a,"one",17,-9);expect(&b,"two",40,12);expect(&again,"one",17,-9);
        assert(!memcmp(&a,&again,sizeof(a)));assert(g_runtime.vars[0].int_value==0);
        break;
    }
    case 2:{
        g_runtime.vars[1].int_max=60;g_runtime.vars[2].int_min=-40;
        h=host(999,777,-999);pet_surface_t out=resolve(&h);expect(&out,"two",60,-40);break;
    }
    case 3:{
        g_runtime.vars[0].int_min=-5;g_runtime.vars[0].int_max=9;
        h=host(-1,17,-9);pet_surface_t out=resolve(&h);expect(&out,"",17,-9);
        h.variables[0].number=3;out=resolve(&h);expect(&out,"",17,-9);break;
    }
    case 4:{
        g_runtime.vars[0].int_value=2;g_runtime.vars[1].int_value=9;g_runtime.vars[2].int_value=4;
        h.fresh=false;pet_surface_t out=resolve(&h);expect(&out,"zero",0,0);
        out=resolve(NULL);expect(&out,"zero",0,0);
        h=host(1,17,-9);h.count=0;out=resolve(&h);expect(&out,"zero",0,0);
        h=host(1,17,-9);for(int i=0;i<3;i++)h.variables[i].integer=false;
        out=resolve(&h);expect(&out,"zero",0,0);break;
    }
    case 5:{
        g_runtime.app_host=false;g_runtime.vars[0].int_value=2;
        g_runtime.vars[1].int_value=17;g_runtime.vars[2].int_value=-9;
        pet_surface_t out=resolve(NULL);expect(&out,"two",17,-9);
        out=resolve(&h);expect(&out,"two",17,-9);break;
    }
    case 6:{
        g_runtime.surface.nodes[1].big_x_var[0]=0;
        strcpy(g_runtime.surface.nodes[1].big_y_var,"not_declared");
        pet_surface_t out=resolve(&h);expect(&out,"one",0,0);
        /* Defensive resolver input; the parser separately rejects this text ref. */
        strcpy(g_runtime.surface.nodes[0].text_at,"not_declared");
        out=resolve(&h);expect(&out,"",0,0);break;
    }
    case 7:{
        pet_surface_t out,before;memset(&out,0x5a,sizeof(out));before=out;
        g_runtime.active=false;assert(!app_surface_locked(&out,&h));assert(!memcmp(&out,&before,sizeof(out)));
        g_runtime.active=true;assert(app_surface_locked(NULL,&h));
        g_runtime.surface.app=false;assert(!app_surface_locked(&out,&h));break;
    }
    case 8:{
        /* Host+cells is parser-forbidden; do not synthesize that capability. */
        g_runtime.app_host=false;g_runtime.surface.count=1;
        pet_surface_node_t *n=&g_runtime.surface.nodes[0];n->type=PET_SURFACE_NODE_CELLS;
        strcpy(n->cursor_x,"pan_x");strcpy(n->cursor_y,"pan_y");
        g_runtime.vars[1].int_value=-50;g_runtime.vars[2].int_value=400;
        pet_surface_t out=resolve(NULL);assert(out.nodes[0].cursor_col==-1&&out.nodes[0].cursor_row==255);
        n->cursor_x[0]=0;n->cursor_y[0]=0;out=resolve(NULL);
        assert(out.nodes[0].cursor_col==-1&&out.nodes[0].cursor_row==-1);break;
    }
    case 9:{
        const char table[]="初始\0更新";memcpy(g_runtime.text_pool,table,sizeof(table));
        g_runtime.text_count=2;g_runtime.text_offset[0]=0;g_runtime.text_offset[1]=7;
        pet_surface_t out=resolve(&h);expect(&out,"更新",17,-9);break;
    }
    case 10:{
        h=host(1,17,-9);h.variables[7]=h.variables[0];h.variables[7].number=2;
        strcpy(h.variables[0].name,"other");h.count=8;
        pet_surface_t out=resolve(&h);expect(&out,"two",17,-9);
        strcpy(h.variables[7].name,"also_other");h.count=9;
        out=resolve(&h);expect(&out,"zero",17,-9);break;
    }
    default:abort();
    }
    return 0;
}
