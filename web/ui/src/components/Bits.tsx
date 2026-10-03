// Small presentational pieces shared by the pages.

import { coverColor, initials, number } from "../api/format";
import { Star, ThumbDown, ThumbUp } from "./Icons";

export function Cover({ title, categoryId, large = false }: { title: string; categoryId: number; large?: boolean }) {
  return (
    <div className={`cover ${large ? "large" : ""}`} style={{ background: coverColor(categoryId) }} aria-hidden="true">
      {initials(title)}
    </div>
  );
}

export function Rating({ average, count }: { average: number | null; count: number }) {
  return (
    <span className="stars" title={`${count} κριτικές`}>
      <Star size={13} />
      {average === null ? "—" : average.toFixed(1)}
      <span className="muted">({count})</span>
    </span>
  );
}

export function Reactions({ likes, dislikes, size = 13 }: { likes: number; dislikes: number; size?: number }) {
  return (
    <>
      <span className="reaction like" title="likes">
        <ThumbUp size={size} />
        {number.format(likes)}
      </span>
      <span className="reaction dislike" title="dislikes">
        <ThumbDown size={size} />
        {number.format(dislikes)}
      </span>
    </>
  );
}
