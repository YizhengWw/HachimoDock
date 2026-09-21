/** PC-only default-stock preview. Never writes a watchlist or invents quotes. */
export const DEFAULT_PREVIEW_STOCKS = [
  { symbol: "hk01810", name: "小米集团", code: "01810" },
  { symbol: "hk09988", name: "阿里巴巴", code: "09988" },
];

export function stockPreviewRows(status) {
  const quotes = Array.isArray(status?.quotes) ? status.quotes : [];
  return DEFAULT_PREVIEW_STOCKS.map(stock => {
    const quote = quotes.find(item => item.symbol === stock.symbol);
    const stale = Boolean(quote && (quote.stale || status?.error));
    const rawChange = String(quote?.changePercent ?? "").trim();
    const hasChange = rawChange !== "" && Number.isFinite(Number(rawChange));
    return { ...stock,
      price: quote?.price || "—",
      change: hasChange ? `${Number(rawChange) > 0 && !rawChange.startsWith("+") ? "+" : ""}${rawChange}%` : "—",
      tone: stale ? 0 : Math.sign(Number(quote?.tone) || 0),
      stale,
      available: Boolean(quote?.price),
      date: String(quote?.quoteTime || "").match(/^\d{4}-\d{2}-\d{2}/)?.[0] || "",
    };
  });
}
