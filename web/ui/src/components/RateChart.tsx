// Live line chart: likes and dislikes per bucket over a sliding window.
// Two series -> legend plus direct end labels; crosshair + tooltip listing
// both series at the hovered bucket.

import { useLayoutEffect, useRef, useState } from "react";

export interface RatePoint {
  at: number;  // bucket start, ms
  likes: number;
  dislikes: number;
  known: boolean;  // false before the page started listening: not zero, unknown
}

const HEIGHT = 220;
const PAD = { top: 12, right: 64, bottom: 24, left: 36 };

function niceMax(v: number): number {
  if (v <= 5) return 5;
  const step = 10 ** Math.floor(Math.log10(v));
  return Math.ceil(v / step) * step;
}

// The last bucket is still filling up; drawing it would show a fake drop.
export function RateChart({ points: all, bucketSeconds }: { points: RatePoint[]; bucketSeconds: number }) {
  const points = all.slice(0, -1);
  const ref = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(600);
  const [hover, setHover] = useState<number | null>(null);

  useLayoutEffect(() => {
    const el = ref.current!;
    const observer = new ResizeObserver(() => setWidth(el.clientWidth));
    observer.observe(el);
    setWidth(el.clientWidth);
    return () => observer.disconnect();
  }, []);

  const innerW = Math.max(100, width - PAD.left - PAD.right);
  const innerH = HEIGHT - PAD.top - PAD.bottom;
  const max = niceMax(Math.max(1, ...points.filter((p) => p.known).map((p) => Math.max(p.likes, p.dislikes))));
  const x = (i: number) => PAD.left + (points.length <= 1 ? 0 : (i / (points.length - 1)) * innerW);
  const y = (v: number) => PAD.top + innerH - (v / max) * innerH;
  const firstKnown = points.findIndex((p) => p.known);
  const path = (key: "likes" | "dislikes") =>
    firstKnown < 0
      ? ""
      : points
          .slice(firstKnown)
          .map((p, j) => `${j ? "L" : "M"}${x(firstKnown + j).toFixed(1)},${y(p[key]).toFixed(1)}`)
          .join("");
  const ticks = [0, max / 2, max];
  const last = firstKnown >= 0 ? points[points.length - 1] : undefined;
  const windowSeconds = all.length * bucketSeconds;

  const onMove = (e: React.PointerEvent) => {
    const rect = ref.current!.getBoundingClientRect();
    const px = e.clientX - rect.left - PAD.left;
    const i = Math.round((px / innerW) * (points.length - 1));
    setHover(i >= 0 && i < points.length ? i : null);
  };

  const h = hover !== null ? points[hover] : undefined;
  const secondsAgo = h ? Math.round((Date.now() - h.at) / 1000) : 0;

  return (
    <div className="chart" ref={ref} onPointerMove={onMove} onPointerLeave={() => setHover(null)}>
      <svg height={HEIGHT} role="img" aria-label={`Likes και dislikes ανά ${bucketSeconds} δευτερόλεπτα, τελευταία ${windowSeconds / 60} λεπτά`}>
        {ticks.map((t) => (
          <g key={t}>
            <line className="gridline" x1={PAD.left} x2={PAD.left + innerW} y1={y(t)} y2={y(t)} />
            <text className="axis" x={PAD.left - 8} y={y(t) + 4} textAnchor="end">{t}</text>
          </g>
        ))}
        <text className="axis" x={PAD.left} y={HEIGHT - 6}>−{Math.round(windowSeconds / 60)} λεπτά</text>
        <text className="axis" x={PAD.left + innerW} y={HEIGHT - 6} textAnchor="end">τώρα</text>
        <path d={path("dislikes")} fill="none" stroke="var(--series-dislike)" strokeWidth={2} strokeLinejoin="round" />
        <path d={path("likes")} fill="none" stroke="var(--series-like)" strokeWidth={2} strokeLinejoin="round" />
        {last && (
          <>
            <text x={PAD.left + innerW + 8} y={y(last.likes) + 4} fontSize={12} fill="var(--text-secondary)">Likes</text>
            <text x={PAD.left + innerW + 8} y={y(last.dislikes) + (Math.abs(y(last.likes) - y(last.dislikes)) < 14 ? 18 : 4)} fontSize={12} fill="var(--text-secondary)">Dislikes</text>
          </>
        )}
        {firstKnown > 0 && (
          <text className="axis" x={x(firstKnown) - 6} y={PAD.top + innerH - 6} textAnchor="end">άνοιγμα σελίδας</text>
        )}
        {h && h.known && hover !== null && (
          <>
            <line x1={x(hover)} x2={x(hover)} y1={PAD.top} y2={PAD.top + innerH} stroke="var(--text-muted)" strokeWidth={1} />
            <circle cx={x(hover)} cy={y(h.likes)} r={4} fill="var(--series-like)" stroke="var(--surface-1)" strokeWidth={2} />
            <circle cx={x(hover)} cy={y(h.dislikes)} r={4} fill="var(--series-dislike)" stroke="var(--surface-1)" strokeWidth={2} />
          </>
        )}
      </svg>
      {h && hover !== null && (
        <div className="tooltip" style={{ left: Math.min(x(hover) + 12, width - 170), top: 8 }}>
          <div className="muted" style={{ fontSize: 12, marginBottom: 4 }}>πριν {secondsAgo} δευτ.</div>
          {h.known ? (
            <>
              <div className="t-row"><span className="t-key" style={{ background: "var(--series-like)" }} /><b>{h.likes}</b><span className="muted">likes</span></div>
              <div className="t-row"><span className="t-key" style={{ background: "var(--series-dislike)" }} /><b>{h.dislikes}</b><span className="muted">dislikes</span></div>
            </>
          ) : (
            <div className="muted">χωρίς δεδομένα (πριν ανοίξει η σελίδα)</div>
          )}
        </div>
      )}
    </div>
  );
}
