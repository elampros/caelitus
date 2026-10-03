// Ranked books for a period: paired horizontal bars (likes, dislikes) per
// book, value labels at the bar ends, a per-row hover tooltip, and a table
// view of the same numbers.

import { useState } from "react";
import { number } from "../api/format";
import type { RankedBook } from "../api/generated";

export function TopBooks({ items, view, onSelect }: { items: RankedBook[]; view: "chart" | "table"; onSelect: (id: number) => void }) {
  const [hover, setHover] = useState<number | null>(null);
  const max = Math.max(1, ...items.map((b) => Math.max(b.likes, b.dislikes)));

  if (!items.length) return <p className="muted">Δεν υπάρχουν αντιδράσεις σε αυτή την περίοδο.</p>;

  if (view === "table")
    return (
      <table>
        <thead>
          <tr><th>#</th><th>Βιβλίο</th><th className="num">Likes</th><th className="num">Dislikes</th><th className="num">Σκορ</th></tr>
        </thead>
        <tbody>
          {items.map((b, i) => (
            <tr key={b.bookId} onClick={() => onSelect(b.bookId)} style={{ cursor: "pointer" }}>
              <td className="muted">{i + 1}</td>
              <td>{b.title}</td>
              <td className="num">{number.format(b.likes)}</td>
              <td className="num">{number.format(b.dislikes)}</td>
              <td className="num">{number.format(b.score)}</td>
            </tr>
          ))}
        </tbody>
      </table>
    );

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 4 }}>
      {items.map((b, i) => (
        <div
          key={b.bookId}
          onPointerEnter={() => setHover(b.bookId)}
          onPointerLeave={() => setHover(null)}
          onClick={() => onSelect(b.bookId)}
          tabIndex={0}
          onFocus={() => setHover(b.bookId)}
          onBlur={() => setHover(null)}
          style={{
            display: "grid",
            gridTemplateColumns: "22px minmax(120px, 34%) 1fr",
            alignItems: "center",
            gap: 10,
            padding: "4px 6px",
            borderRadius: 6,
            cursor: "pointer",
            position: "relative",
            background: hover === b.bookId ? "var(--surface-2)" : undefined,
          }}
        >
          <span className="muted" style={{ fontSize: 12, textAlign: "right" }}>{i + 1}</span>
          <span style={{ fontSize: 13, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }} title={b.title}>{b.title}</span>
          <div style={{ display: "flex", flexDirection: "column", gap: 2 }}>
            {(["likes", "dislikes"] as const).map((k) => (
              <div key={k} style={{ display: "flex", alignItems: "center", gap: 6 }}>
                <div
                  style={{
                    height: 8,
                    width: `${Math.max(b[k] ? 1.5 : 0, (b[k] / max) * 88)}%`,
                    background: k === "likes" ? "var(--series-like)" : "var(--series-dislike)",
                    borderRadius: "0 4px 4px 0",
                  }}
                />
                <span style={{ fontSize: 11, color: "var(--text-secondary)", fontVariantNumeric: "tabular-nums" }}>{number.format(b[k])}</span>
              </div>
            ))}
          </div>
          {hover === b.bookId && (
            <div className="tooltip" style={{ right: 8, top: -6 }}>
              <div className="t-row"><b>{number.format(b.score)}</b><span className="muted">σκορ</span></div>
              <div className="t-row"><span className="t-key" style={{ background: "var(--series-like)" }} /><b>{number.format(b.likes)}</b><span className="muted">likes</span></div>
              <div className="t-row"><span className="t-key" style={{ background: "var(--series-dislike)" }} /><b>{number.format(b.dislikes)}</b><span className="muted">dislikes</span></div>
            </div>
          )}
        </div>
      ))}
    </div>
  );
}
