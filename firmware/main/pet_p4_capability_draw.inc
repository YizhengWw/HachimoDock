/* Private native renderer adapter for the r150 capability parent, included by
 * pet_p4_renderer.c right after the surface image buffers. Two independent,
 * default-off pieces: the keyframe background cache fed by the existing
 * priority-1 asset worker, and the decorative particle emitters. No new task,
 * priority, clock, heap in a frame, or package-visible pixel pointer. */
#if PET_P4_KEYFRAME_BG
static void keyframe_invalidate_frame_cache(void);
/* Worker-owned decode buffers (allocated once at renderer init, PSRAM, DMA
 * capable when the JPEG driver grants them). The render thread copies one
 * P4J1 body into `input` only while the worker is idle; the worker writes
 * `output` only inside its job; the render thread reads `output` only after
 * `ready` names the frame it is about to paint. */
static const pet_surface_t *g_app_text_surface; /* tentative; the label code defines it below */
static uint8_t *g_keyframe_input;static size_t g_keyframe_input_capacity;
static uint16_t *g_keyframe_output;static size_t g_keyframe_output_capacity;static bool g_keyframe_output_dma;
static uint8_t *g_keyframe_tjpgd_workbuf;
static SemaphoreHandle_t g_jpeg_engine_lock; /* serializes the one hardware JPEG engine between render thread and worker */
static struct {
  volatile int8_t frame;        /* requested frame, -1 idle (render thread sets, worker clears) */
  volatile bool working;        /* worker owns input/output */
  volatile int8_t ready;        /* frame decoded in output, -1 none */
  volatile uint32_t revision,ready_revision;
  volatile size_t size;
  uint8_t shown;                /* frame painted into the frame cache */
  uint64_t swapped_ms;
  uint32_t shown_revision;
} g_keyframe={.frame=-1,.ready=-1};
static uint32_t g_keyframe_swaps,g_keyframe_worker_decodes,g_keyframe_fallback_decodes;
static bool keyframe_working_locked(void){return g_keyframe.working;}
static void keyframe_init(void){
  if(!g_keyframe_input){
    jpeg_decode_memory_alloc_cfg_t cfg={.buffer_direction=JPEG_DEC_ALLOC_INPUT_BUFFER};
    g_keyframe_input=(uint8_t*)jpeg_alloc_decoder_mem(PET_IMAGE_MAX_BYTES,&cfg,&g_keyframe_input_capacity);
    if(!g_keyframe_input){g_keyframe_input=(uint8_t*)heap_caps_malloc(PET_IMAGE_MAX_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(g_keyframe_input)g_keyframe_input_capacity=PET_IMAGE_MAX_BYTES;}
  }
  if(!g_keyframe_output){
    size_t bytes=(size_t)PET_P4_ASSET_WIDTH*PET_P4_ASSET_HEIGHT*sizeof(uint16_t);
    jpeg_decode_memory_alloc_cfg_t cfg={.buffer_direction=JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    g_keyframe_output=(uint16_t*)jpeg_alloc_decoder_mem(bytes,&cfg,&g_keyframe_output_capacity);
    g_keyframe_output_dma=g_keyframe_output!=NULL;
    if(!g_keyframe_output){g_keyframe_output=(uint16_t*)heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(g_keyframe_output)g_keyframe_output_capacity=bytes;}
  }
  if(!g_keyframe_tjpgd_workbuf)g_keyframe_tjpgd_workbuf=(uint8_t*)heap_caps_malloc(PET_P4_TJPGD_WORK_BUFFER_BYTES,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  if(!g_jpeg_engine_lock)g_jpeg_engine_lock=xSemaphoreCreateMutex();
}
bool pet_p4_renderer_keyframe_ready(void){return g_keyframe_input&&g_keyframe_output&&g_keyframe_output_capacity>=640U*480U*2U&&g_jpeg_engine_lock;}
static void keyframe_engine_lock(void){if(g_jpeg_engine_lock)xSemaphoreTake(g_jpeg_engine_lock,portMAX_DELAY);}
static void keyframe_engine_unlock(void){if(g_jpeg_engine_lock)xSemaphoreGive(g_jpeg_engine_lock);}
/* Worker only: one claimed job, decode into the worker buffers, publish `ready`. */
static bool keyframe_worker(void){
  portENTER_CRITICAL(&g_asset_prepare_mux);
  int frame=g_keyframe.frame;bool claim=frame>=0&&!g_keyframe.working&&!g_h264_program_window;size_t size=g_keyframe.size;uint32_t revision=g_keyframe.revision;
  if(claim){
    g_keyframe.working=true;
    /* The next decode may overwrite output before failing. Retire its old
     * ready tag BEFORE touching that buffer; failure must not publish old-frame
     * authority over partially replaced pixels. */
    g_keyframe.ready=-1;g_keyframe.ready_revision=0;
  }
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  if(!claim)return false;
  int w=0,h=0,stride=640;bool ok=false;
  if(g_keyframe_input&&g_keyframe_output&&size>12&&size-12<=g_keyframe_input_capacity){
    esp_err_t err;
    if(g_jpeg_decoder&&g_keyframe_output_dma)ok=decode_jpeg_frame_hardware_into(g_keyframe_input,size-12,g_keyframe_output,g_keyframe_output_capacity,&w,&h,&stride,&err);
    if(!ok&&g_keyframe_tjpgd_workbuf){JRESULT fallback;ok=decode_jpeg_frame_tjpgd_into(g_keyframe_input,size-12,g_keyframe_output,g_keyframe_output_capacity/sizeof(uint16_t),g_keyframe_tjpgd_workbuf,&w,&h,&fallback);stride=w;}
    ok=ok&&w==640&&h==480;
    /* Normalize a padded hardware stride in place so the render copy is one memcpy per row. */
    if(ok&&stride!=640)for(int y=1;y<480;y++)memmove(g_keyframe_output+y*640,g_keyframe_output+y*stride,1280);
  }
  portENTER_CRITICAL(&g_asset_prepare_mux);
  g_keyframe.working=false;g_keyframe.frame=-1;
  if(ok){g_keyframe.ready=(int8_t)frame;g_keyframe.ready_revision=revision;}
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  if(ok)pet_render_counter_increment(&g_keyframe_worker_decodes);
  return true;
}
/* Pack item that holds frame `index` (0 = the declared background, k>=1 = appended frame k+1). */
static const pet_p4_miniapp_sprite_t *keyframe_item(const pet_surface_t *s,unsigned index){
  unsigned at=index?(unsigned)s->image_count+index-1:0;
  if(at>=g_surface_images.count)return NULL;
  const pet_p4_miniapp_sprite_t *im=&g_surface_images.items[at];
  return pet_image_jpeg(g_surface_images.pixels+im->data_offset,im->data_length)?im:NULL;
}
/* Render thread, once per APP frame before the cache key: pick the frame the
 * clock asks for, swap only when the worker already holds it (never more often
 * than every 500 ms), and hand the worker the next body while it is idle. */
static void keyframe_advance(const pet_surface_t *s,unsigned long long now_ms){
  if(s->keyframes<2||!pet_p4_renderer_keyframe_ready())return;
  if(g_keyframe.shown_revision!=g_surface_revision){g_keyframe.shown=0;g_keyframe.shown_revision=g_surface_revision;g_keyframe.swapped_ms=now_ms;}
  unsigned want=(unsigned)((now_ms*s->keyframe_fps/1000ULL)%s->keyframes);
  portENTER_CRITICAL(&g_asset_prepare_mux);
  int ready=g_keyframe.ready_revision==g_surface_revision?g_keyframe.ready:-1;bool idle=g_keyframe.frame<0&&!g_keyframe.working;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  if(want!=g_keyframe.shown){
    if(ready==(int)want&&now_ms-g_keyframe.swapped_ms>=500ULL){
      g_keyframe.shown=(uint8_t)want;g_keyframe.swapped_ms=now_ms;keyframe_invalidate_frame_cache();
      pet_render_counter_increment(&g_keyframe_swaps);return;
    }
    if(idle&&ready!=(int)want&&g_asset_prepare_task){
      const pet_p4_miniapp_sprite_t *im=keyframe_item(s,want);
      if(im&&im->data_length<=PET_IMAGE_MAX_BYTES&&im->data_length-12<=g_keyframe_input_capacity){
        memcpy(g_keyframe_input,g_surface_images.pixels+im->data_offset+12,im->data_length-12);
        portENTER_CRITICAL(&g_asset_prepare_mux);
        g_keyframe.size=im->data_length;g_keyframe.revision=g_surface_revision;g_keyframe.frame=(int8_t)want;
        portEXIT_CRITICAL(&g_asset_prepare_mux);
        xTaskNotifyGive(g_asset_prepare_task);
      }
    }
  }
}
/* Render thread inside the JPEG branch: paint the shown frame from the worker
 * cache. False means the caller decodes the returned item itself (frame 0 before
 * the worker ever ran, or the cache currently holds another frame). */
static bool keyframe_blit(const pet_surface_t *s,const pet_p4_miniapp_sprite_t **item){
  *item=keyframe_item(s,g_keyframe.shown);
  if(!*item)*item=&g_surface_images.items[0];
  if(s->keyframes<2||!pet_p4_renderer_keyframe_ready())return false;
  portENTER_CRITICAL(&g_asset_prepare_mux);
  bool hit=g_keyframe.ready==(int8_t)g_keyframe.shown&&g_keyframe.ready_revision==g_surface_revision&&!g_keyframe.working&&g_keyframe.frame<0;
  if(hit)g_keyframe.working=true; /* hold the buffer against a new request while copying */
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  if(!hit){pet_render_counter_increment(&g_keyframe_fallback_decodes);return false;}
  for(int y=0;y<480;y++)memcpy(g_framebuffer+y*640,g_keyframe_output+y*640,1280);
  portENTER_CRITICAL(&g_asset_prepare_mux);g_keyframe.working=false;portEXIT_CRITICAL(&g_asset_prepare_mux);
  return true;
}
#endif

#if PET_P4_PARTICLES
#include "pet_p4_particles.h"
static EXT_RAM_BSS_ATTR pet_particles_state_t g_particle_state[PET_SURFACE_PARTICLE_EMITTERS];
static EXT_RAM_BSS_ATTR pet_surface_particles_t g_particle_shape[PET_SURFACE_PARTICLE_EMITTERS];
static char g_particle_widget[PET_P4_MINIAPP_WIDGET_ID_MAX];
static uint8_t g_particle_shape_count;
static uint32_t g_particle_revision;
static portMUX_TYPE g_particle_action_mux=portMUX_INITIALIZER_UNLOCKED;
static char g_particle_actions[4][48];static uint32_t g_particle_action_revisions[4];static uint8_t g_particle_action_count;
/* Any task: remember a dispatched transition action for the next frame's bursts. */
void pet_p4_renderer_note_action(const char *action,uint32_t revision){
  if(!revision||!action||!action[0]||strlen(action)>47)return;
  portENTER_CRITICAL(&g_particle_action_mux);
  if(g_particle_action_count<4){strcpy(g_particle_actions[g_particle_action_count],action);g_particle_action_revisions[g_particle_action_count++]=revision;}
  portEXIT_CRITICAL(&g_particle_action_mux);
}
static pet_particles_config_t particle_config(const pet_surface_particles_t *e){
  return (pet_particles_config_t){.x=e->x,.y=e->y,.w=e->w,.h=e->h,.color=e->color,.life_ms=e->life_ms,.tick_ms=e->trigger_tick_ms,
    .spread=e->spread,.count=e->count,.budget=e->budget,.speed=e->speed,.shape=e->shape,.gravity=e->gravity};
}
static void particle_dot(void *context,int x,int y,int size,uint16_t color){
  const pet_surface_particles_t *e=context;int right=e->x+e->w,bottom=e->y+e->h;
  for(int dy=-(size/2);dy<=size/2;dy++)for(int dx=-(size/2);dx<=size/2;dx++){
    if(size==3&&dx&&dy)continue; /* spark: a plus sign, not a block */
    int px=x+dx,py=y+dy;
    if(px<e->x||px>=right||py<e->y||py>=bottom)continue;
    fill_rect(px,py,1,1,color);
  }
}
static bool particle_guard(const pet_surface_particles_t *e){
  if(!e->when_var[0])return true;
  return e->when_ready&&pet_p4_game_compare(e->when_current,e->when_op,e->when_value);
}
/* Overlay after the cached package layer: dots never enter the frame cache. */
static void draw_app_particles(const pet_surface_t *s,const char *widget_id,uint32_t revision,unsigned page,unsigned long long now_ms){
  /* Actions belong to the next APP frame even when it has no emitters. Do not
   * retain them until another component happens to declare the same trigger. */
  char actions[4][48];uint32_t revisions[4];uint8_t count;
  portENTER_CRITICAL(&g_particle_action_mux);
  count=g_particle_action_count;memcpy(actions,g_particle_actions,sizeof(actions));memcpy(revisions,g_particle_action_revisions,sizeof(revisions));g_particle_action_count=0;
  portEXIT_CRITICAL(&g_particle_action_mux);
  if(!s->particle_count){g_particle_shape_count=0;g_particle_revision=0;g_particle_widget[0]=0;return;}
  bool same=g_particle_revision==revision&&g_particle_shape_count==s->particle_count&&!strcmp(g_particle_widget,widget_id);
  for(unsigned i=0;same&&i<s->particle_count;i++){
    pet_surface_particles_t a=s->particles[i],b=g_particle_shape[i];a.when_current=b.when_current=0;a.when_ready=b.when_ready=false;
    same=!memcmp(&a,&b,sizeof(a));
  }
  if(!same){
    for(unsigned i=0;i<s->particle_count;i++){g_particle_shape[i]=s->particles[i];pet_particles_reset(&g_particle_state[i],0x9e3779b9U+i);}
    g_particle_revision=revision;g_particle_shape_count=s->particle_count;strncpy(g_particle_widget,widget_id,sizeof(g_particle_widget)-1);g_particle_widget[sizeof(g_particle_widget)-1]=0;
  }
  for(unsigned i=0;i<s->particle_count;i++){
    const pet_surface_particles_t *e=&s->particles[i];pet_particles_config_t c=particle_config(e);
    if(e->page!=4&&e->page!=page)continue;
    bool emitting=particle_guard(e);
    if(emitting&&e->trigger_action[0])for(unsigned k=0;k<count;k++)if(revisions[k]==revision&&!strcmp(actions[k],e->trigger_action))(void)pet_particles_burst(&c,&g_particle_state[i]);
    (void)pet_particles_step(&c,&g_particle_state[i],now_ms,emitting);
    (void)pet_particles_draw(&c,&g_particle_state[i],particle_dot,(void*)e);
  }
}
#define PET_PARTICLES_ACTIVE(s) ((s)->particle_count!=0)
#else
#define PET_PARTICLES_ACTIVE(s) ((void)(s),false)
#endif
