// Display pen names for project-maintained components. These are not claims
// about independent contributors; original legal notices stay in each package.
export const COMPONENT_AUTHORS = Object.freeze({
  'stock-watchlist':'cobi123','wooden-fish':'橘子酱','music-player':'晚风',
  'upcoming-todos':'土豆泥','computer-status':'手搓飞机',
  'two-key-pong':'弹珠汽水','bloomfrog_companion':'青团',
  'flappy-bird':'纸飞机','block-combo':'方糖','snake-turn':'小豆包',
  'tomato-clock':'番茄吐司','drink-reminder':'柠檬水','token-usage':'代码泡面',
  'fresh-fruit-slice':'西瓜籽',
  'cloud-hop':'小豆包','daily-almanac':'橘子酱','book-pocket':'橘子酱',
  'daily-news':'早起小鸟',
  'decision-capsule':'橘子酱','answer-book':'土豆泥','hope-train':'cobi123',
});
export function componentDisplayAuthor(work) {
  // Apply only to build-owned/local or server-verified maintained packages,
  // never rewrite the attribution of a third-party work with a similar name.
  const author=((work.builtin || work.official) && COMPONENT_AUTHORS[work.id])
    || work.author || (work.id?.startsWith('hachimi-') ? '哈基米' : '组件作者');
  return work.builtin||work.official ? String(author).replaceAll('官方','').trim()||'组件作者' : author;
}
