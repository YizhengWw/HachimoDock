/* Included by game.c: bounded Q8 solver shared verbatim by device and WASM.
 * At most 8 bodies, 16 microsteps, two ordered pair passes. No heap/float/IO.
 * Max displacement per microstep is <=0.832 cell at max speed/tick, preventing
 * two one-cell AABBs from crossing without an overlap sample. */
static int32_t body_div(int32_t value,int32_t *remainder){
  int32_t sum=value+*remainder,whole=sum/1000;*remainder=sum-whole*1000;return whole;
}
static unsigned body_wall(pet_p4_game_entity_t *e,int width,int height){
  unsigned hit=0;
  int32_t maxx=(width-e->width)*256,maxy=(height-e->height)*256;
  if(e->x_q8<0){e->x_q8=0;if(e->body_vx_q8<0)e->body_vx_q8=-e->body_vx_q8*e->restitution/100;hit|=1u<<PET_P4_GAME_EDGE_LEFT;}
  if(e->x_q8>maxx){e->x_q8=maxx;if(e->body_vx_q8>0)e->body_vx_q8=-e->body_vx_q8*e->restitution/100;hit|=1u<<PET_P4_GAME_EDGE_RIGHT;}
  if(e->y_q8<0){e->y_q8=0;if(e->body_vy_q8<0)e->body_vy_q8=-e->body_vy_q8*e->restitution/100;hit|=1u<<PET_P4_GAME_EDGE_TOP;}
  if(e->y_q8>maxy){e->y_q8=maxy;if(e->body_vy_q8>0)e->body_vy_q8=-e->body_vy_q8*e->restitution/100;hit|=1u<<PET_P4_GAME_EDGE_BOTTOM;}
  return hit;
}
static bool body_pair(pet_p4_game_entity_t *a,pet_p4_game_entity_t *b){
  if(!a->body||!b->body||!a->active||!b->active||(a->body_static&&b->body_static))return false;
  int32_t ox=(a->x_q8+a->width*256<b->x_q8+b->width*256?a->x_q8+a->width*256:b->x_q8+b->width*256)-(a->x_q8>b->x_q8?a->x_q8:b->x_q8);
  int32_t oy=(a->y_q8+a->height*256<b->y_q8+b->height*256?a->y_q8+a->height*256:b->y_q8+b->height*256)-(a->y_q8>b->y_q8?a->y_q8:b->y_q8);
  if(ox<=0||oy<=0)return false;
  bool x=ox<=oy;
  int32_t *pa=x?&a->x_q8:&a->y_q8,*pb=x?&b->x_q8:&b->y_q8;
  int32_t *va=x?&a->body_vx_q8:&a->body_vy_q8,*vb=x?&b->body_vx_q8:&b->body_vy_q8;
  int sign=(*pa+(x?a->width:a->height)*128<=*pb+(x?b->width:b->height)*128)?1:-1;
  int overlap=x?ox:oy,ma=a->body_static?0:b->body_static?overlap:overlap/2;
  *pa-=sign*ma;*pb+=sign*(overlap-ma);
  int relative=((*va)-(*vb))*sign;
  if(relative>0){
    int bounce=a->restitution<b->restitution?a->restitution:b->restitution;
    int impulse=relative*(100+bounce)/100;
    int denom=a->body_static?b->mass:b->body_static?a->mass:a->mass+b->mass;
    if(!a->body_static)*va=bounded_clamp(*va-sign*impulse*(b->body_static?a->mass:b->mass)/denom,-16384,16384);
    if(!b->body_static)*vb=bounded_clamp(*vb+sign*impulse*(a->body_static?b->mass:a->mass)/denom,-16384,16384);
  }
  return true;
}
static bool bounded_bodies_step(pet_p4_game_engine_t *g,uint64_t now){
  bounded_process_trigger(g,PET_P4_GAME_TRIGGER_TICK,NULL,now);
  if(!g->running||g->game_over)return true;
  uint16_t contacts[PET_P4_GAME_MAX_ENTITIES]={0};uint8_t edges[PET_P4_GAME_MAX_ENTITIES]={0};
  for(unsigned sub=0;sub<16;sub++){
    unsigned dt=((sub+1)*g->tick_ms/16)-(sub*g->tick_ms/16);
    for(unsigned i=0;i<g->bounded.entity_count;i++){
      pet_p4_game_entity_t *e=&g->bounded_entities[i];if(!e->body||e->body_static||!e->active)continue;
      e->body_vy_q8=bounded_clamp(e->body_vy_q8+body_div(g->bounded.body_gravity*256*(int32_t)dt,&e->body_rg),-16384,16384);
      int32_t dx=body_div(e->body_vx_q8*(int32_t)dt,&e->body_rx);
      e->x_q8+=dx;e->y_q8+=body_div(e->body_vy_q8*(int32_t)dt,&e->body_ry);
      if(e->rolling){e->angle_q8=(e->angle_q8+dx*360/e->width)%92160;if(e->angle_q8<0)e->angle_q8+=92160;}
      edges[i]|=body_wall(e,g->width,g->height);
    }
    for(unsigned pass=0;pass<2;pass++)for(unsigned i=0;i<g->bounded.entity_count;i++)for(unsigned j=i+1;j<g->bounded.entity_count;j++){
      pet_p4_game_entity_t *a=&g->bounded_entities[i],*b=&g->bounded_entities[j];
      if(body_pair(a,b)){contacts[i]|=1u<<j;edges[i]|=body_wall(a,g->width,g->height);edges[j]|=body_wall(b,g->width,g->height);}
    }
  }
  for(unsigned i=0;i<g->bounded.entity_count;i++){
    pet_p4_game_entity_t *e=&g->bounded_entities[i];e->x=e->x_q8/256;e->y=e->y_q8/256;
  }
  /* Once per pair/edge per logical tick, after solving; no recursive event queue. */
  for(unsigned i=0;i<g->bounded.entity_count;i++){
    for(unsigned edge=1;edge<=4;edge++)if(edges[i]&(1u<<edge))bounded_execute_rules(g,PET_P4_GAME_TRIGGER_EDGE,NULL,i,-1,edge,now,NULL,NULL);
    for(unsigned j=i+1;j<g->bounded.entity_count;j++)if(contacts[i]&(1u<<j))bounded_execute_rules(g,PET_P4_GAME_TRIGGER_COLLISION,NULL,i,j,PET_P4_GAME_EDGE_NONE,now,NULL,NULL);
  }
  return true;
}
