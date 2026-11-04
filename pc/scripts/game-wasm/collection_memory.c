/* Freestanding fixed C memory/text helpers. No allocator or host imports. */
#include <stddef.h>
void *memcpy(void *target,const void *source,size_t count){
  unsigned char *out=target;const unsigned char *in=source;
  for(size_t i=0;i<count;i++)out[i]=in[i];return target;
}
void *memset(void *target,int value,size_t count){
  unsigned char *out=target;for(size_t i=0;i<count;i++)out[i]=(unsigned char)value;return target;
}
int strcmp(const char *a,const char *b){
  for(;*a&&*a==*b;a++,b++){}return (unsigned char)*a-(unsigned char)*b;
}
size_t strlen(const char *s){size_t n=0;for(;s[n];n++){}return n;}
