// Public comment identity only: never use hardware IDs, tokens or creator attribution.
export const COMMENT_ID_KEY = 'pet-manager:community-comment-id:v1';
export const COMMENT_NAME_KEY = 'pet-manager:community-comment-name:v1';
let sessionCode;
export function commentStorage() {try{return window.localStorage;}catch{return null;}}
export function normalizeCommentName(value) {
  const name=String(value??'').trim();
  if(!name) throw new Error('请输入昵称');
  if([...name].length>40) throw new Error('昵称最多 40 个字符');
  if(/[\p{Cc}\u202a-\u202e\u2066-\u2069]/u.test(name)) throw new Error('昵称不能包含换行或控制字符');
  return name;
}
export function readCommentIdentity(storage,uuid=()=>crypto.randomUUID()) {
  let code,name='';
  try{code=storage.getItem(COMMENT_ID_KEY);name=storage.getItem(COMMENT_NAME_KEY)||'';}catch{/* session-only fallback */}
  if(!/^用户-[a-f0-9]{12}$/.test(code||'')) {
    code=sessionCode ||= `用户-${uuid().replaceAll('-','').slice(0,12).toLowerCase()}`;
    try{storage.setItem(COMMENT_ID_KEY,code);}catch{/* never expose device identity as fallback */}
  }
  try{name=normalizeCommentName(name);}catch{name='';}
  return {code,name,displayName:name||code};
}
export function saveCommentName(storage,value) {
  const name=normalizeCommentName(value);
  try{storage.setItem(COMMENT_NAME_KEY,name);}catch{throw new Error('昵称保存失败，请检查本机存储后重试');}
  return name;
}
