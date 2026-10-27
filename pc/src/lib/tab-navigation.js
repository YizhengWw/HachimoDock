// Automatically activated tabs: arrows/Home/End focus and select; Tab leaves the strip.
export function navigateTabs(event, ids, current, select) {
  if (!ids.length) return;
  let index = ids.indexOf(current);
  if (event.key === "ArrowRight") index = (index + 1) % ids.length;
  else if (event.key === "ArrowLeft") index = (index - 1 + ids.length) % ids.length;
  else if (event.key === "Home") index = 0;
  else if (event.key === "End") index = ids.length - 1;
  else return;
  event.preventDefault();
  select(ids[index]);
  event.currentTarget.closest('[role="tablist"]')?.querySelectorAll('[role="tab"]')[index]?.focus();
}
