/**
 * How values are shown in the UI: Greek number and date formats, period and
 * language names, and the placeholder covers (initials on a category color).
 *
 * @module
 */

/** Greek number format (`12.345`): `number.format(n)`. */
export const number = new Intl.NumberFormat("el-GR");

/** The year of an ISO date, without leading zeros (`"0965-01-01"` -> `"965"`). */
export function year(date: string): string {
  return date.slice(0, 4).replace(/^0+/, "");
}

/** Date and time of an ISO timestamp, Greek short format. */
export function dateTime(iso: string): string {
  return new Date(iso).toLocaleString("el-GR", { dateStyle: "short", timeStyle: "medium" });
}

/** Time of day of an ISO timestamp. */
export function time(iso: string): string {
  return new Date(iso).toLocaleTimeString("el-GR");
}

/** An average rating as `4.3 ★`, or `—` when the book has no reviews. */
export function stars(average: number | null): string {
  if (average === null) return "—";
  return `${average.toFixed(1)} ★`;
}

/** Greek names of the ranking periods of `reactions.top`. */
export const periodLabels: Record<string, string> = {
  today: "Σήμερα",
  yesterday: "Χθες",
  last7Days: "7 ημέρες",
  last30Days: "30 ημέρες",
  lastYear: "Έτος",
  allTime: "Πάντα",
};

/** Greek names of the languages the sample catalog uses. */
export const languageLabels: Record<string, string> = { el: "Ελληνικά", en: "Αγγλικά" };

// Articles and conjunctions skipped when picking initials.
const minorWords = new Set(["the", "a", "an", "of", "and", "to", "in", "on", "ο", "η", "το", "οι", "τα", "του", "της", "των", "και"]);

/** Up to two initials for a book's placeholder cover ("The Name of the Rose" -> "NR"). */
export function initials(title: string): string {
  const words = title.split(/[\s:,.;!?'’"-]+/).filter((w) => w && !minorWords.has(w.toLowerCase()));
  return (words.length ? words : [title]).slice(0, 2).map((w) => w[0]!.toUpperCase()).join("");
}

// Deep, muted colors: white text stays readable on all of them.
const coverColors = ["#2f5d8a", "#7a4b8c", "#2f7a6b", "#9a5a2e", "#4a5a8f", "#8a3f4f", "#4f7a3a", "#5b5f6b"];

/**
 * The placeholder cover's color, chosen by category so books of one category
 * look related. Decorative only.
 */
export function coverColor(categoryId: number): string {
  return coverColors[(categoryId - 1) % coverColors.length] ?? coverColors[0]!;
}
