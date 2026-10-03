/**
 * Live dashboard: system status, reaction rate, catalog event feed and the
 * most liked/disliked books for a chosen period.
 *
 * @module
 */

import { useEffect, useRef, useState } from "react";
import { rpc } from "../api/client";
import { number, periodLabels, time } from "../api/format";
import type { Methods, RankedBook } from "../api/generated";
import { type LiveMessage, useLive, useLiveState } from "../api/live";
import { useTitle } from "../api/titles";
import { BookPanel } from "../components/BookPanel";
import { RateChart, type RatePoint } from "../components/RateChart";
import { Reactions } from "../components/Bits";
import { Alert, BookIcon, Pulse, Server } from "../components/Icons";
import { TopBooks } from "../components/TopBooks";

const BUCKET_SECONDS = 5;
const BUCKETS = 60;  // 5 minutes
type Period = Methods["reactions.top"]["params"]["period"];
type Order = NonNullable<Methods["reactions.top"]["params"]["order"]>;

const bucketOf = (ms: number) => Math.floor(ms / (BUCKET_SECONDS * 1000)) * BUCKET_SECONDS * 1000;

/**
 * `openedAt`: the bucket in which this page started listening. It and the
 * buckets before it are unknown (not zero): the first one was only partly observed.
 */
function emptyBuckets(now: number, openedAt: number): RatePoint[] {
  const start = bucketOf(now);
  return Array.from({ length: BUCKETS }, (_, i) => {
    const at = start - (BUCKETS - 1 - i) * BUCKET_SECONDS * 1000;
    return { at, likes: 0, dislikes: 0, known: at > openedAt };
  });
}

const eventLabels: Record<string, string> = {
  created: "νέο βιβλίο",
  updated: "αλλαγή βιβλίου",
  deleted: "διαγραφή βιβλίου",
  reviews: "νέα κριτική",
};

/** The `/live` page. */
export function DashboardPage() {
  const live = useLiveState();
  const title = useTitle();
  const [openedAt] = useState(() => bucketOf(Date.now()));
  const [points, setPoints] = useState<RatePoint[]>(() => emptyBuckets(Date.now(), openedAt));
  const [session, setSession] = useState({ likes: 0, dislikes: 0 });
  const [feed, setFeed] = useState<LiveMessage[]>([]);
  const [period, setPeriod] = useState<Period>("today");
  const [order, setOrder] = useState<Order>("mostLiked");
  const [view, setView] = useState<"chart" | "table">("chart");
  const [top, setTop] = useState<RankedBook[]>([]);
  const [topUnavailable, setTopUnavailable] = useState(false);
  const [catalogSize, setCatalogSize] = useState<number | null>(null);
  const [selected, setSelected] = useState<number | null>(null);
  const dirty = useRef(true);

  /**
   * Slide the window every bucket.
   */
  useEffect(() => {
    const t = setInterval(() => {
      setPoints((prev) => {
        const fresh = emptyBuckets(Date.now(), openedAt);
        const byAt = new Map(prev.map((p) => [p.at, p]));
        return fresh.map((p) => byAt.get(p.at) ?? p);
      });
    }, 1000);
    return () => clearInterval(t);
  }, []);

  useLive((m) => {
    if (m.type === "reaction") {
      dirty.current = true;
      setSession((s) => ({ ...s, [m.kind === "like" ? "likes" : "dislikes"]: s[m.kind === "like" ? "likes" : "dislikes"] + 1 }));
      const at = bucketOf(Date.parse(m.at));
      setPoints((prev) =>
        prev.map((p) => (p.at === at ? { ...p, [m.kind === "like" ? "likes" : "dislikes"]: p[m.kind === "like" ? "likes" : "dislikes"] + 1 } : p)),
      );
    } else {
      if (m.type === "event") {
        dirty.current = true;
        if (m.event === "created" || m.event === "deleted") rpc("books.search", { pageSize: 1 }).then((r) => setCatalogSize(r.total)).catch(() => {});
      }
      setFeed((f) => [m, ...f].slice(0, 60));
    }
  });

  useEffect(() => {
    rpc("books.search", { pageSize: 1 }).then((r) => setCatalogSize(r.total)).catch(() => {});
  }, []);

  /**
   * Rankings: reload when the filter changes, and every 2 s while reactions arrive.
   */
  useEffect(() => {
    let current = true;
    const load = () => {
      dirty.current = false;
      rpc("reactions.top", { period, order, limit: 10 })
        .then((r) => current && (setTop(r.items), setTopUnavailable(false)))
        .catch(() => current && setTopUnavailable(true));  // keep the last list we had
    };
    load();
    const t = setInterval(() => dirty.current && load(), 2000);
    return () => {
      current = false;
      clearInterval(t);
    };
  }, [period, order]);

  const lastMinute = points.slice(-60 / BUCKET_SECONDS).reduce((s, p) => s + p.likes + p.dislikes, 0);
  const serverOk = live.server === "online";

  return (
    <>
      <div className="page-head">
        <h1>Live</h1>
        <p>Likes, dislikes και αλλαγές του καταλόγου, τη στιγμή που συμβαίνουν.</p>
      </div>

      {live.server === "offline" && (
        <div className="banner warning" role="status">
          <Alert size={18} />
          <div>
            <strong>Ο server είναι εκτός λειτουργίας</strong>
            Τα likes που φαίνονται στο γράφημα στάλθηκαν στο MQTT αλλά <b>δεν καταγράφονται</b> μέχρι να ξαναξεκινήσει.
            Η κατάταξη ενημερώνεται ξανά μόλις επιστρέψει.
          </div>
        </div>
      )}

      <div className="tiles">
        <div className="card tile">
          <div className="label"><Server />Server</div>
          <div className="value status">
            <span className={`dot ${serverOk ? "good" : live.server ? "bad" : "wait"}`} style={{ width: 10, height: 10 }} aria-hidden="true" />
            {serverOk ? "Online" : live.server === "offline" ? "Offline" : "Άγνωστο"}
          </div>
          <div className="sub">MQTT broker: {live.broker ? "συνδεδεμένος" : "εκτός"}</div>
        </div>
        <div className="card tile">
          <div className="label"><Pulse />Αντιδράσεις, τελευταίο λεπτό</div>
          <div className="value">{number.format(lastMinute)}</div>
          <div className="sub">≈ {(lastMinute / 60).toFixed(1)} ανά δευτερόλεπτο</div>
        </div>
        <div className="card tile">
          <div className="label">Από το άνοιγμα της σελίδας</div>
          <div className="value" style={{ fontSize: 22, display: "flex", gap: 18 }}>
            <Reactions likes={session.likes} dislikes={session.dislikes} size={18} />
          </div>
          <div className="sub">likes και dislikes που στάλθηκαν</div>
        </div>
        <div className="card tile">
          <div className="label"><BookIcon />Βιβλία στον κατάλογο</div>
          <div className="value">{catalogSize === null ? "—" : number.format(catalogSize)}</div>
        </div>
      </div>

      <div className="dash">
        <section className="card chart-card">
          <header>
            <h2>Likes και dislikes ανά {BUCKET_SECONDS} δευτ.</h2>
            <div className="legend">
              <span><span className="key" style={{ background: "var(--series-like)" }} />Likes</span>
              <span><span className="key" style={{ background: "var(--series-dislike)" }} />Dislikes</span>
            </div>
          </header>
          <RateChart points={points} bucketSeconds={BUCKET_SECONDS} />
          {lastMinute === 0 && (
            <p className="muted" style={{ margin: 0, fontSize: 12.5 }}>
              Ησυχία. Πάτα like σε ένα βιβλίο, ή τρέξε <code>npm run simulate</code> στο <code>web/</code>. Το γράφημα
              γεμίζει ένα σημείο κάθε {BUCKET_SECONDS} δευτερόλεπτα από τη στιγμή που άνοιξε η σελίδα.
            </p>
          )}
        </section>

        <section className="card chart-card">
          <header><h2>Συμβάντα καταλόγου</h2></header>
          {feed.length === 0 && (
            <div className="empty">
              <Pulse size={26} />
              Εδώ εμφανίζονται νέα βιβλία, αλλαγές και κριτικές μόλις γίνουν.
            </div>
          )}
          <ul className="feed">
            {feed.map((m, i) => (
              <li key={i}>
                <time>{time(m.at)}</time>
                {m.type === "event" && (
                  <span>
                    <span className="secondary">{eventLabels[m.event] ?? m.event}:</span>{" "}
                    <a href="#" onClick={(e) => (e.preventDefault(), setSelected(m.bookId))}>{title(m.bookId)}</a>
                    {m.event === "reviews" && m.payload && <span className="muted"> · {m.payload.replace(/^(\d)/, "$1★")}</span>}
                  </span>
                )}
                {m.type === "status" && <span>Server: <strong>{m.server}</strong></span>}
                {m.type === "broker" && <span>MQTT broker {m.connected ? "συνδέθηκε" : "αποσυνδέθηκε"}</span>}
              </li>
            ))}
          </ul>
        </section>
      </div>

      <div className="section-bar">
        <h2>Κατάταξη</h2>
        <div className="segmented" role="group" aria-label="Περίοδος">
          {(Object.keys(periodLabels) as Period[]).map((p) => (
            <button key={p} className={period === p ? "on" : ""} onClick={() => setPeriod(p)}>{periodLabels[p]}</button>
          ))}
        </div>
        <div className="segmented" role="group" aria-label="Σειρά">
          <button className={order === "mostLiked" ? "on" : ""} onClick={() => setOrder("mostLiked")}>Πιο αγαπημένα</button>
          <button className={order === "mostDisliked" ? "on" : ""} onClick={() => setOrder("mostDisliked")}>Λιγότερο αγαπημένα</button>
        </div>
        <span className="spacer" />
        <div className="segmented" role="group" aria-label="Προβολή">
          <button className={view === "chart" ? "on" : ""} onClick={() => setView("chart")}>Γράφημα</button>
          <button className={view === "table" ? "on" : ""} onClick={() => setView("table")}>Πίνακας</button>
        </div>
      </div>
      <section className="card chart-card">
        <header>
          <h2>Top 10 · {periodLabels[period]}</h2>
          <div className="legend">
            <span><span className="key" style={{ background: "var(--series-like)", height: 8 }} />Likes</span>
            <span><span className="key" style={{ background: "var(--series-dislike)", height: 8 }} />Dislikes</span>
          </div>
          <span className="muted" style={{ fontSize: 12 }}>κατάταξη με likes − dislikes</span>
        </header>
        {topUnavailable && top.length === 0 ? (
          <div className="empty">
            <Alert size={24} />
            Η κατάταξη δεν είναι διαθέσιμη: ο server δεν απαντά.
          </div>
        ) : (
          <TopBooks items={top} view={view} onSelect={setSelected} />
        )}
        {topUnavailable && top.length > 0 && <p className="muted" style={{ margin: 0, fontSize: 12.5 }}>Τελευταία διαθέσιμη κατάταξη· ο server δεν απαντά αυτή τη στιγμή.</p>}
      </section>

      {selected && <BookPanel id={selected} onClose={() => setSelected(null)} onChanged={() => (dirty.current = true)} />}
    </>
  );
}
