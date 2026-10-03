/**
 * Book search. Filters live in the URL, so a search can be bookmarked or shared.
 *
 * @module
 */

import { useEffect, useMemo, useState } from "react";
import { useSearchParams } from "react-router-dom";
import { rpc, RpcError } from "../api/client";
import { languageLabels, number, year } from "../api/format";
import type { Author, BookPage, Category, Methods, TagUsage } from "../api/generated";
import { useLive } from "../api/live";
import { Cover, Rating, Reactions } from "../components/Bits";
import { BookPanel } from "../components/BookPanel";
import { ChevronLeft, ChevronRight, Close, Filter, Search } from "../components/Icons";

const PAGE_SIZE = 24;
type SearchParams = Methods["books.search"]["params"];

const sorts: Array<[NonNullable<SearchParams["sort"]>, string]> = [
  ["publishedDesc", "Νεότερα πρώτα"],
  ["publishedAsc", "Παλαιότερα πρώτα"],
  ["titleAsc", "Τίτλος (Α-Ω)"],
  ["ratingDesc", "Βαθμολογία"],
  ["createdDesc", "Πρόσφατα προστεθέντα"],
];

function useLookups() {
  const [categories, setCategories] = useState<Category[]>([]);
  const [authors, setAuthors] = useState<Author[]>([]);
  const [tags, setTags] = useState<TagUsage[]>([]);
  useEffect(() => {
    rpc("categories.list").then(setCategories).catch(() => {});
    rpc("tags.list").then(setTags).catch(() => {});
    (async () => {
      const all: Author[] = [];
      for (let page = 1; ; page++) {
        const result = await rpc("authors.search", { page, pageSize: 100 });
        all.push(...result.items);
        if (page >= result.pageCount) break;
      }
      setAuthors(all);
    })().catch(() => {});
  }, []);
  return { categories, authors, tags };
}

/** The `/` page: filter sidebar, results grid with paging, and the book panel. */
export function BooksPage() {
  const [params, setParams] = useSearchParams();
  const { categories, authors, tags } = useLookups();
  const [result, setResult] = useState<BookPage | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  const [refresh, setRefresh] = useState(0);
  const [titleInput, setTitleInput] = useState(params.get("title") ?? "");

  const get = (key: string) => params.get(key) ?? "";
  const update = (changes: Record<string, string | null>) => {
    const next = new URLSearchParams(params);
    for (const [k, v] of Object.entries(changes)) (v ? next.set(k, v) : next.delete(k));
    if (!("page" in changes)) next.delete("page");
    setParams(next, { replace: true });
  };

  /**
   * Debounced title search.
   */
  useEffect(() => {
    const t = setTimeout(() => titleInput !== get("title") && update({ title: titleInput.trim() || null }), 300);
    return () => clearTimeout(t);
  }, [titleInput]);

  const selectedTags = useMemo(() => (params.get("tags") ?? "").split(",").filter(Boolean), [params]);
  const query: SearchParams = useMemo(() => {
    const q: SearchParams = { page: Number(get("page") || 1), pageSize: PAGE_SIZE, sort: (get("sort") || "publishedDesc") as SearchParams["sort"] };
    if (get("title")) q.title = get("title");
    if (get("category")) q.categoryId = Number(get("category"));
    if (get("author")) q.authorId = Number(get("author"));
    if (get("language")) q.language = get("language");
    if (get("from")) q.publishedFrom = `${get("from").padStart(4, "0")}-01-01`;
    if (get("to")) q.publishedTo = `${get("to").padStart(4, "0")}-12-31`;
    if (get("minRating")) q.minRating = Number(get("minRating"));
    if (selectedTags.length) {
      q.tags = selectedTags;
      q.tagMatch = (get("tagMatch") || "any") as SearchParams["tagMatch"];
    }
    return q;
  }, [params]);

  useEffect(() => {
    let current = true;
    setLoading(true);
    rpc("books.search", query)
      .then((r) => current && (setResult(r), setError(null)))
      .catch((e) => current && setError(e instanceof RpcError ? e.detail : String(e)))
      .finally(() => current && setLoading(false));
    return () => {
      current = false;
    };
  }, [query, refresh]);

  /**
   * Catalog changes elsewhere refresh the list.
   */
  useLive((m) => {
    if (m.type === "event" && m.event !== "reviews") setRefresh((r) => r + 1);
  });

  const selected = get("book") ? Number(get("book")) : null;
  const toggleTag = (name: string) => {
    const next = selectedTags.includes(name) ? selectedTags.filter((t) => t !== name) : [...selectedTags, name];
    update({ tags: next.join(",") || null });
  };

  const [tagQuery, setTagQuery] = useState("");
  const [showAllTags, setShowAllTags] = useState(false);
  const [filtersOpen, setFiltersOpen] = useState(false);
  const visibleTags = useMemo(() => {
    const q = tagQuery.trim().toLowerCase();
    const matching = tags.filter((t) => !q || t.name.includes(q));
    const byUse = [...matching].sort((a, b) => b.bookCount - a.bookCount || a.name.localeCompare(b.name));
    return showAllTags || q ? byUse : byUse.slice(0, 18);
  }, [tags, tagQuery, showAllTags]);

  // The filters in effect, as removable chips above the results.
  const active: Array<[string, () => void]> = [];
  if (get("title")) active.push([`«${get("title")}»`, () => (setTitleInput(""), update({ title: null }))]);
  if (get("category")) active.push([categories.find((c) => String(c.id) === get("category"))?.name ?? "κατηγορία", () => update({ category: null })]);
  if (get("author")) active.push([authors.find((a) => String(a.id) === get("author"))?.name ?? "συγγραφέας", () => update({ author: null })]);
  if (get("language")) active.push([languageLabels[get("language")] ?? get("language"), () => update({ language: null })]);
  if (get("from") || get("to")) active.push([`${get("from") || "…"}–${get("to") || "…"}`, () => update({ from: null, to: null })]);
  if (get("minRating")) active.push([`${get("minRating")}+ αστέρια`, () => update({ minRating: null })]);
  for (const t of selectedTags) active.push([`#${t}`, () => toggleTag(t)]);
  const clearAll = () => {
    setTitleInput("");
    setParams(new URLSearchParams(get("sort") ? { sort: get("sort") } : {}), { replace: true });
  };

  return (
    <div className="books-layout">
      <aside className={`card sidebar ${filtersOpen ? "" : "collapsed"}`} aria-label="Φίλτρα">
        <div className="sidebar-head">
          <h2><Filter size={15} />Φίλτρα</h2>
          {active.length > 0 && <button className="link" onClick={clearAll}>Καθαρισμός</button>}
          <button className="ghost filters-toggle" onClick={() => setFiltersOpen((o) => !o)} aria-expanded={filtersOpen}>
            {filtersOpen ? "Απόκρυψη" : `Εμφάνιση${active.length ? ` (${active.length})` : ""}`}
          </button>
        </div>
        <label className="field">
          Τίτλος
          <span className="input-icon">
            <Search size={15} />
            <input value={titleInput} onChange={(e) => setTitleInput(e.target.value)} placeholder="π.χ. dune" type="search" />
          </span>
        </label>
        <label className="field">
          Κατηγορία
          <select value={get("category")} onChange={(e) => update({ category: e.target.value || null })}>
            <option value="">Όλες</option>
            {categories.map((c) => (
              <option key={c.id} value={c.id}>{c.name}</option>
            ))}
          </select>
        </label>
        <label className="field">
          Συγγραφέας
          <select value={get("author")} onChange={(e) => update({ author: e.target.value || null })}>
            <option value="">Όλοι</option>
            {authors.map((a) => (
              <option key={a.id} value={a.id}>{a.name}</option>
            ))}
          </select>
        </label>
        <label className="field">
          Γλώσσα
          <select value={get("language")} onChange={(e) => update({ language: e.target.value || null })}>
            <option value="">Όλες</option>
            {Object.entries(languageLabels).map(([code, label]) => (
              <option key={code} value={code}>{label}</option>
            ))}
          </select>
        </label>
        <div className="field">
          <label className="field" htmlFor="year-from">Έτος πρώτης έκδοσης</label>
          <div className="year-range">
            <input id="year-from" aria-label="από έτος" inputMode="numeric" value={get("from")} onChange={(e) => update({ from: e.target.value.replace(/\D/g, "").slice(0, 4) || null })} placeholder="από" />
            <span className="muted">–</span>
            <input aria-label="έως έτος" inputMode="numeric" value={get("to")} onChange={(e) => update({ to: e.target.value.replace(/\D/g, "").slice(0, 4) || null })} placeholder="έως" />
          </div>
        </div>
        <label className="field">
          Ελάχιστη βαθμολογία
          <select value={get("minRating")} onChange={(e) => update({ minRating: e.target.value || null })}>
            <option value="">Οποιαδήποτε</option>
            {[4.5, 4, 3.5, 3].map((r) => (
              <option key={r} value={r}>{r}+ αστέρια</option>
            ))}
          </select>
        </label>
        {tags.length > 0 && (
          <div className="tag-picker">
            <div className="row" style={{ justifyContent: "space-between" }}>
              <span className="field" style={{ fontSize: 12.5, fontWeight: 500, color: "var(--text-secondary)" }}>Tags</span>
              {selectedTags.length > 1 && (
                <div className="segmented" role="group" aria-label="Συνδυασμός tags">
                  {(["any", "all"] as const).map((m) => (
                    <button key={m} className={(get("tagMatch") || "any") === m ? "on" : ""} onClick={() => update({ tagMatch: m === "any" ? null : m })}>
                      {m === "any" ? "οποιοδήποτε" : "όλα"}
                    </button>
                  ))}
                </div>
              )}
            </div>
            <span className="input-icon">
              <Search size={14} />
              <input value={tagQuery} onChange={(e) => setTagQuery(e.target.value)} placeholder={`Αναζήτηση σε ${tags.length} tags`} type="search" />
            </span>
            <div className="tag-list">
              {visibleTags.map((t) => (
                <span key={t.name} className={`chip ${selectedTags.includes(t.name) ? "on" : ""}`} onClick={() => toggleTag(t.name)} role="button" tabIndex={0}
                  onKeyDown={(e) => (e.key === "Enter" || e.key === " ") && (e.preventDefault(), toggleTag(t.name))} aria-pressed={selectedTags.includes(t.name)}>
                  {t.name} <span className="count">{t.bookCount}</span>
                </span>
              ))}
              {visibleTags.length === 0 && <span className="muted" style={{ fontSize: 12.5 }}>Κανένα tag δεν ταιριάζει</span>}
            </div>
            {!tagQuery && tags.length > 18 && (
              <button className="link" style={{ alignSelf: "flex-start", fontSize: 12.5 }} onClick={() => setShowAllTags((v) => !v)}>
                {showAllTags ? "Λιγότερα" : `Όλα τα tags (${tags.length})`}
              </button>
            )}
          </div>
        )}
      </aside>

      <section aria-label="Αποτελέσματα">
        <div className="toolbar">
          <h1>Βιβλία</h1>
          {result && <span className="count">{number.format(result.total)} {result.total === 1 ? "βιβλίο" : "βιβλία"}</span>}
          {loading && <span className="muted">φόρτωση…</span>}
          {error && <span className="error">{error}</span>}
          <span className="spacer" />
          <label className="row" style={{ gap: 8, fontSize: 13 }}>
            <span className="secondary">Ταξινόμηση</span>
            <select value={get("sort") || "publishedDesc"} onChange={(e) => update({ sort: e.target.value })}>
              {sorts.map(([value, label]) => (
                <option key={value} value={value}>{label}</option>
              ))}
            </select>
          </label>
        </div>
        {active.length > 0 && (
          <div className="active-filters">
            {active.map(([label, remove]) => (
              <span key={label} className="chip on" role="button" tabIndex={0} onClick={remove} onKeyDown={(e) => e.key === "Enter" && remove()} title="Αφαίρεση">
                {label} <Close size={12} />
              </span>
            ))}
          </div>
        )}

        <div className="grid" style={{ opacity: loading ? 0.6 : 1, transition: "opacity 0.15s" }}>
          {!result && !error && Array.from({ length: 9 }, (_, i) => <div key={i} className="card skeleton" style={{ height: 104 }} />)}
          {result?.items.map((b) => {
            const open = () => update({ book: String(b.id), page: get("page") || null });
            return (
              <article key={b.id} className="card book" onClick={open} tabIndex={0} onKeyDown={(e) => e.key === "Enter" && open()}>
                <Cover title={b.title} categoryId={b.category.id} />
                <span className="category">{b.category.name}</span>
                <h3>{b.title}</h3>
                <div className="meta">{b.authors.map((a) => a.name).join(", ")} · {year(b.publishedOn)}</div>
                <div className="stats">
                  <Rating average={b.ratingAverage} count={b.ratingCount} />
                  <Reactions likes={b.likes} dislikes={b.dislikes} />
                  {!b.reactionsEnabled && <span className="off" title="Δεν δέχεται likes">χωρίς likes</span>}
                </div>
              </article>
            );
          })}
        </div>
        {result && result.total === 0 && (
          <div className="card empty">
            <Search size={28} />
            <strong className="secondary">Κανένα βιβλίο με αυτά τα φίλτρα</strong>
            <button className="link" onClick={clearAll}>Καθαρισμός φίλτρων</button>
          </div>
        )}

        {result && result.pageCount > 1 && (
          <nav className="pager" aria-label="Σελίδες">
            <button disabled={result.page <= 1} onClick={() => update({ page: String(result.page - 1) })}><ChevronLeft />Προηγούμενη</button>
            <span className="secondary">Σελίδα {result.page} από {result.pageCount}</span>
            <button disabled={result.page >= result.pageCount} onClick={() => update({ page: String(result.page + 1) })}>Επόμενη<ChevronRight /></button>
          </nav>
        )}
      </section>

      {selected && (
        <BookPanel
          id={selected}
          onClose={() => update({ book: null, page: get("page") || null })}
          onChanged={() => setRefresh((r) => r + 1)}
        />
      )}
    </div>
  );
}
