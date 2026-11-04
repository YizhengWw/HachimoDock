/* Representation guard before cJSON loses string lengths. Not a JSON syntax,
 * UTF-8, schema or authentication validator. Binary resource data never uses
 * this API. Decoded NUL cannot be represented faithfully by device C strings;
 * a literal escaped backslash followed by u0000 remains ordinary text.
 * Linear bounded-input scan, no allocation, callbacks, output or IO.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
static inline bool pet_json_source_no_nul_bytes(const void *data,size_t length){
  if(!data)return false;
  const uint8_t *p=data;
  for(size_t i=0;i<length;i++){
    if(!p[i])return false;
    if(p[i]=='\\'){
      if(++i==length)break; /* Syntax errors are the JSON parser's decision. */
      if(!p[i])return false;
      if(p[i]=='u'&&length-i>=5&&p[i+1]=='0'&&p[i+2]=='0'&&p[i+3]=='0'&&p[i+4]=='0')return false;
    }
  }
  return true;
}
static inline bool pet_json_source_no_nul(const char *text){
  return text&&pet_json_source_no_nul_bytes(text,strlen(text));
}
static inline bool pet_json_hex4(const uint8_t *p,uint32_t *out){
  uint32_t value=0;
  for(unsigned j=0;j<4;j++){
    unsigned c=p[j],n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;
    if(n==16)return false;
    value=(value<<4)|n;
  }
  *out=value;return true;
}
/* Device-bound source text additionally requires strict Unicode scalars and
 * legal JSON string escapes/control spelling. No replacement/normalization.
 * This is NOT a complete JSON grammar, duplicate-key, depth/schema or authority
 * check: the actual JSON parser and schema still run. Explicit byte spans are
 * mandatory at raw transport/file boundaries; binary data never enters here.
 */
#define PET_JSON_SOURCE_MAX_DEPTH 32U
static inline bool pet_json_source_text_limit_bytes(const void *data,size_t length,size_t max_depth){
  if(!data)return false;
  const uint8_t*p=data;bool quoted=false;size_t depth=0;
  for(size_t i=0;i<length;){
    unsigned c=p[i];
    if(c<128){
      if(!c)return false;
      if(!quoted){
        if(c=='"')quoted=true;
        else if(c<32&&c!='\t'&&c!='\r'&&c!='\n')return false;
        else if(max_depth&&(c=='{'||c=='[')){
          if(depth==max_depth)return false;
          depth++;
        }else if(max_depth&&(c=='}'||c==']')){
          if(!depth)return false;
          depth--;
        }
        i++;continue;
      }
      if(c<32)return false;
      if(c=='"'){quoted=false;i++;continue;}
      if(c!='\\'){i++;continue;}
      if(length-i<2)return false;
      unsigned escape=p[i+1];
      if(escape=='u'){
        uint32_t scalar;if(length-i<6||!pet_json_hex4(p+i+2,&scalar)||!scalar)return false;
        if(scalar>=0xd800&&scalar<=0xdbff){
          uint32_t low;
          if(length-i<12||p[i+6]!='\\'||p[i+7]!='u'||!pet_json_hex4(p+i+8,&low)||low<0xdc00||low>0xdfff)return false;
          i+=12;continue;
        }
        if(scalar>=0xdc00&&scalar<=0xdfff)return false;
        i+=6;continue;
      }
      if(escape!='"'&&escape!='\\'&&escape!='/'&&escape!='b'&&escape!='f'&&escape!='n'&&escape!='r'&&escape!='t')return false;
      i+=2;continue;
    }
    unsigned n;uint32_t scalar;
    if(c>=0xc2&&c<=0xdf){n=2;scalar=c&31U;}
    else if(c>=0xe0&&c<=0xef){n=3;scalar=c&15U;}
    else if(c>=0xf0&&c<=0xf4){n=4;scalar=c&7U;}
    else return false;
    if(n>length-i)return false;
    for(unsigned j=1;j<n;j++){
      if((p[i+j]&0xc0U)!=0x80U)return false;
      scalar=(scalar<<6)|(p[i+j]&63U);
    }
    if((n==2&&scalar<0x80)||(n==3&&scalar<0x800)||(n==4&&scalar<0x10000)
        ||scalar>0x10ffff||(scalar>=0xd800&&scalar<=0xdfff))return false;
    i+=n;
  }
  return !quoted&&(!max_depth||!depth);
}
static inline bool pet_json_source_text_bytes(const void *data,size_t length){
  return pet_json_source_text_limit_bytes(data,length,0);
}
static inline bool pet_json_source_text(const char *text){
  return text&&pet_json_source_text_bytes(text,strlen(text));
}
/* Device ingress caps open JSON object/array containers before recursive cJSON
 * calls. Quotes/escapes cannot disguise depth. Container types/grammar/schema
 * still belong to the actual parser. No stack enlargement, complete call-graph
 * or runtime high-water claim is implied. Binary data never uses this path. */
static inline bool pet_json_source_bounded_text_bytes(const void *data,size_t length){
  return pet_json_source_text_limit_bytes(data,length,PET_JSON_SOURCE_MAX_DEPTH);
}
static inline bool pet_json_source_bounded_text(const char *text){
  return text&&pet_json_source_bounded_text_bytes(text,strlen(text));
}
