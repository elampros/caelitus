/**
 * Book titles for live messages (which carry only ids). Unknown ids are
 * collected for a moment and fetched in one JSON-RPC batch.
 *
 * @module
 */

import { useEffect, useReducer } from "react";
import { rpcBatch, RpcError } from "./client";

const titles = new Map<number, string>();
const waiting = new Set<number>();
const subscribers = new Set<() => void>();
let timer: ReturnType<typeof setTimeout> | null = null;

async function flush() {
  timer = null;
  const ids = [...waiting];
  waiting.clear();
  const books = await rpcBatch("books.get", ids.map((id) => ({ id }))).catch(() => []);
  books.forEach((b, i) => titles.set(ids[i]!, b instanceof RpcError || !b ? `#${ids[i]} (διαγραμμένο)` : b.title));
  subscribers.forEach((s) => s());
}

/** Records a title already at hand (e.g. from a search), saving a request later. */
export function rememberTitle(id: number, title: string) {
  titles.set(id, title);
}

/**
 * React hook returning `title(id)`: the book's title if known, else `#id` for
 * now; the component re-renders once the batch with the title arrives.
 */
export function useTitle(): (id: number) => string {
  const [, rerender] = useReducer((x: number) => x + 1, 0);
  useEffect(() => {
    subscribers.add(rerender);
    return () => {
      subscribers.delete(rerender);
    };
  }, []);
  return (id) => {
    const known = titles.get(id);
    if (known) return known;
    if (!waiting.has(id)) {
      waiting.add(id);
      timer ??= setTimeout(flush, 150);
    }
    return `#${id}`;
  };
}
