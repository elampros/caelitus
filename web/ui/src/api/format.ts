export const number = new Intl.NumberFormat("el-GR");

export function year(date: string): string {
  return date.slice(0, 4).replace(/^0+/, "");
}

export function dateTime(iso: string): string {
  return new Date(iso).toLocaleString("el-GR", { dateStyle: "short", timeStyle: "medium" });
}

export function time(iso: string): string {
  return new Date(iso).toLocaleTimeString("el-GR");
}

export function stars(average: number | null): string {
  if (average === null) return "—";
  return `${average.toFixed(1)} ★`;
}

export const periodLabels: Record<string, string> = {
  today: "Σήμερα",
  yesterday: "Χθες",
  last7Days: "7 ημέρες",
  last30Days: "30 ημέρες",
  lastYear: "Έτος",
  allTime: "Πάντα",
};

export const languageLabels: Record<string, string> = { el: "Ελληνικά", en: "Αγγλικά" };

// Up to two initials for a book's placeholder cover ("The Name of the Rose" -> "NR").
const minorWords = new Set(["the", "a", "an", "of", "and", "to", "in", "on", "ο", "η", "το", "οι", "τα", "του", "της", "των", "και"]);
export function initials(title: string): string {
  const words = title.split(/[\s:,.;!?'’"-]+/).filter((w) => w && !minorWords.has(w.toLowerCase()));
  return (words.length ? words : [title]).slice(0, 2).map((w) => w[0]!.toUpperCase()).join("");
}

// Deep, muted cover colors (white text stays readable on all); chosen by
// category, so books of one category look related. Decorative only.
const coverColors = ["#2f5d8a", "#7a4b8c", "#2f7a6b", "#9a5a2e", "#4a5a8f", "#8a3f4f", "#4f7a3a", "#5b5f6b"];
export function coverColor(categoryId: number): string {
  return coverColors[(categoryId - 1) % coverColors.length] ?? coverColors[0]!;
}
