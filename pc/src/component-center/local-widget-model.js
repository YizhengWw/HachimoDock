export const LOCAL_SOURCES = new Set(["computer.status", "todos.upcoming"]);
export function localDateTime(iso) {
  if (!iso) return "";
  const date = new Date(iso);
  if (!Number.isFinite(date.getTime())) return "";
  const pad = n => String(n).padStart(2, "0");
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}T${pad(date.getHours())}:${pad(date.getMinutes())}`;
}
export function deadlineIso(local) {
  if (!local) return "";
  const date = new Date(local);
  if (!Number.isFinite(date.getTime())) throw new Error("请填写有效的截止日期和时间");
  return date.toISOString();
}
export function todoDateLabel(iso) {
  const local = localDateTime(iso);
  return local ? local.slice(5).replace("T", " ") : "未设置截止时间";
}
export function todoHeaderDate(date) {
  return /^\d{4}-\d{2}-\d{2}$/.test(date || "") ? date.slice(5) : "等待数据";
}
export function todoMutation(operation, item, fields = {}) {
  return { operation, ...(item ? { id: item.id, revision: item.revision } : {}), ...fields };
}
