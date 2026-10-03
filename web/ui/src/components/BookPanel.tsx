// One book: details, likes per period (live), reviews with a form to add one,
// and the switch that lets it accept likes/dislikes over MQTT.

import { useEffect, useRef, useState } from "react";
import { react, rpc, RpcError } from "../api/client";
import { dateTime, languageLabels, number, periodLabels, year } from "../api/format";
import type { Book, ReactionStats, ReviewPage } from "../api/generated";
import { useLive } from "../api/live";
import { rememberTitle } from "../api/titles";
import { Cover, Rating, Reactions } from "./Bits";
import { Close, Star, ThumbDown, ThumbUp } from "./Icons";

const periods = ["today", "yesterday", "last7Days", "last30Days", "lastYear", "allTime"] as const;

export function BookPanel({ id, onClose, onChanged }: { id: number; onClose: () => void; onChanged: () => void }) {
  const [book, setBook] = useState<Book | null>(null);
  const [stats, setStats] = useState<ReactionStats | null>(null);
  const [reviews, setReviews] = useState<ReviewPage | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [form, setForm] = useState({ reviewerName: "", rating: 5, body: "" });
  const [formError, setFormError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const statsTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);

  const load = () =>
    Promise.all([rpc("books.get", { id }), rpc("reactions.get", { bookId: id }), rpc("reviews.list", { bookId: id, pageSize: 50 })])
      .then(([b, s, r]) => {
        setBook(b);
        rememberTitle(b.id, b.title);
        setStats(s);
        setReviews(r);
        setError(null);
      })
      .catch((e) => setError(e instanceof RpcError ? e.detail : String(e)));

  useEffect(() => {
    setBook(null);
    load();
  }, [id]);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => e.key === "Escape" && onClose();
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose]);

  // Counts are written once a second on the server: refresh shortly after
  // likes for this book arrive.
  useLive((m) => {
    if ((m.type === "reaction" || m.type === "event") && m.bookId === id) {
      clearTimeout(statsTimer.current);
      statsTimer.current = setTimeout(() => rpc("reactions.get", { bookId: id }).then(setStats).catch(() => {}), 1300);
    }
  });

  const toggleReactions = async (enabled: boolean) => {
    setBook(await rpc("books.setReactionsEnabled", { id, enabled }));
    onChanged();
  };

  const submitReview = async (e: React.FormEvent) => {
    e.preventDefault();
    setBusy(true);
    try {
      await rpc("reviews.create", { bookId: id, ...form });
      setForm({ reviewerName: form.reviewerName, rating: 5, body: "" });
      setFormError(null);
      await load();
      onChanged();
    } catch (err) {
      setFormError(err instanceof RpcError ? err.detail : String(err));
    } finally {
      setBusy(false);
    }
  };

  return (
    <>
      <div className="panel-backdrop" onClick={onClose} />
      <aside className="panel" role="dialog" aria-modal="true" aria-label={book?.title ?? "Βιβλίο"}>
        {error && <p className="error">{error}</p>}
        {!book && !error && <div className="skeleton" style={{ height: 116 }} />}
        {book && (
          <>
            <div className="panel-hero">
              <Cover title={book.title} categoryId={book.category.id} large />
              <div>
                <div className="eyebrow">{book.category.name}</div>
                <h1>{book.title}</h1>
                <div className="by">{book.authors.map((a) => a.name).join(", ")} · {year(book.publishedOn)}</div>
                <div className="facts">
                  <Rating average={book.ratingAverage} count={book.ratingCount} />
                  <Reactions likes={book.likes} dislikes={book.dislikes} />
                </div>
              </div>
              <button className="ghost icon-only" onClick={onClose} aria-label="Κλείσιμο"><Close size={18} /></button>
            </div>

            <dl className="kv">
              <dt>Γλώσσα</dt>
              <dd>{languageLabels[book.language] ?? book.language}</dd>
              <dt>Tags</dt>
              <dd>{book.tags.length ? <span className="row" style={{ gap: 6 }}>{book.tags.map((t) => <span key={t} className="chip" style={{ cursor: "default" }}>{t}</span>)}</span> : "—"}</dd>
              {book.isbn && (<><dt>ISBN</dt><dd className="tabular">{book.isbn}</dd></>)}
              {book.pageCount && (<><dt>Σελίδες</dt><dd className="tabular">{book.pageCount}</dd></>)}
              <dt>Τελευταία αλλαγή</dt>
              <dd className="muted">{dateTime(book.updatedAt)} · έκδοση εγγραφής {book.version}</dd>
            </dl>
            {book.description && <p className="secondary" style={{ margin: 0 }}>{book.description}</p>}

            <section className="section">
              <div className="section-title">
                <h2>Likes και dislikes</h2>
                <label className="switch">
                  <input type="checkbox" checked={book.reactionsEnabled} onChange={(e) => toggleReactions(e.target.checked)} />
                  {book.reactionsEnabled ? "Δέχεται" : "Δεν δέχεται"}
                </label>
              </div>
              <div className="row">
                <button disabled={!book.reactionsEnabled} onClick={() => react(id, "like")}><span className="reaction like"><ThumbUp /></span>Like</button>
                <button disabled={!book.reactionsEnabled} onClick={() => react(id, "dislike")}><span className="reaction dislike"><ThumbDown /></span>Dislike</button>
                <span className="muted" style={{ fontSize: 12.5 }}>
                  {book.reactionsEnabled ? "Στέλνεται μέσω MQTT· μετράει σε ~1 δευτ." : "Ενεργοποίησε τον διακόπτη για να δέχεται likes."}
                </span>
              </div>
              {stats && (
                <div className="period-grid">
                  {periods.map((p) => (
                    <div key={p} className="period">
                      <div className="p-label">{periodLabels[p]}</div>
                      <div className="p-values">
                        <Reactions likes={stats.periods[p].likes} dislikes={stats.periods[p].dislikes} size={12} />
                      </div>
                    </div>
                  ))}
                </div>
              )}
            </section>

            <section className="section">
              <div className="section-title">
                <h2>Κριτικές {reviews && <span className="muted" style={{ fontWeight: 400 }}>({number.format(reviews.total)})</span>}</h2>
              </div>
              <form onSubmit={submitReview} className="card review-form">
                <div className="row">
                  <input required placeholder="Το όνομά σου" value={form.reviewerName} onChange={(e) => setForm({ ...form, reviewerName: e.target.value })} style={{ flex: 1 }} aria-label="Όνομα" />
                  <span className="rating-input" role="radiogroup" aria-label="Βαθμολογία">
                    {[1, 2, 3, 4, 5].map((r) => (
                      <button type="button" key={r} className={r <= form.rating ? "on" : ""} onClick={() => setForm({ ...form, rating: r })}
                        role="radio" aria-checked={form.rating === r} aria-label={`${r} αστέρια`}>
                        <Star size={20} />
                      </button>
                    ))}
                  </span>
                </div>
                <textarea required rows={3} placeholder="Τι σου άρεσε ή δεν σου άρεσε;" value={form.body} onChange={(e) => setForm({ ...form, body: e.target.value })} aria-label="Κριτική" />
                <div className="row">
                  {formError && <span className="error" style={{ flex: 1 }}>{formError}</span>}
                  <span className="spacer" />
                  <button className="primary" disabled={busy}>Δημοσίευση κριτικής</button>
                </div>
              </form>
              {reviews && reviews.total === 0 && <p className="muted" style={{ margin: 0 }}>Καμία κριτική ακόμα. Γράψε την πρώτη.</p>}
              <div>
                {reviews?.items.map((r) => (
                  <div key={r.id} className="review">
                    <div className="row">
                      <strong>{r.reviewerName}</strong>
                      <span className="stars" aria-label={`${r.rating} από 5`}>
                        {Array.from({ length: 5 }, (_, i) => (
                          <Star key={i} size={12} style={i < r.rating ? undefined : { fill: "none", color: "var(--border-strong)" }} />
                        ))}
                      </span>
                      <span className="spacer" />
                      <span className="muted" style={{ fontSize: 12 }}>{dateTime(r.createdAt)}</span>
                    </div>
                    {r.title && <div style={{ fontWeight: 600 }}>{r.title}</div>}
                    <div className="secondary">{r.body}</div>
                  </div>
                ))}
              </div>
              {reviews && reviews.total > reviews.items.length && (
                <span className="muted">…και {reviews.total - reviews.items.length} ακόμα</span>
              )}
            </section>
          </>
        )}
      </aside>
    </>
  );
}
