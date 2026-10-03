/**
 * One shared WebSocket to the gateway's /ws, reconnecting on its own.
 * Components subscribe with useLive(handler) and read connection state
 * with useLiveState().
 *
 * @module
 */

import { useEffect, useRef, useSyncExternalStore } from "react";

/** One message from the gateway's WebSocket (same shape as the gateway sends). */
export type LiveMessage =
  | { type: "reaction"; bookId: number; kind: "like" | "dislike"; at: string }
  | { type: "event"; bookId: number; event: string; payload: string; at: string }
  | { type: "status"; server: string; at: string }
  | { type: "broker"; connected: boolean; at: string };

/** What the status indicators show. */
export interface LiveState {
  /** The browser's WebSocket to the gateway. */
  socket: "connecting" | "open" | "closed";
  /** The gateway's MQTT connection; null until the gateway has said. */
  broker: boolean | null;
  /** `"online"` / `"offline"` from the server's status topic; null until known. */
  server: string | null;
}

const listeners = new Set<(m: LiveMessage) => void>();
const stateListeners = new Set<() => void>();
let state: LiveState = { socket: "connecting", broker: null, server: null };
let socket: WebSocket | null = null;
let retry = 500;

function setState(patch: Partial<LiveState>) {
  state = { ...state, ...patch };
  stateListeners.forEach((l) => l());
}

function connect() {
  const url = `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`;
  socket = new WebSocket(url);
  setState({ socket: "connecting" });
  socket.onopen = () => {
    retry = 500;
    setState({ socket: "open" });
  };
  socket.onmessage = (e) => {
    const message = JSON.parse(e.data) as LiveMessage;
    if (message.type === "broker") setState({ broker: message.connected });
    if (message.type === "status") setState({ server: message.server });
    listeners.forEach((l) => l(message));
  };
  socket.onclose = () => {
    setState({ socket: "closed", broker: null });
    setTimeout(connect, retry);
    retry = Math.min(retry * 2, 10000);
  };
}

function ensureConnected() {
  if (!socket) connect();
}

/**
 * React hook: calls `handler` with every live message while the component is
 * mounted. The latest `handler` is used, so it may close over fresh state.
 */
export function useLive(handler: (m: LiveMessage) => void): void {
  const ref = useRef(handler);
  ref.current = handler;
  useEffect(() => {
    ensureConnected();
    const listener = (m: LiveMessage) => ref.current(m);
    listeners.add(listener);
    return () => {
      listeners.delete(listener);
    };
  }, []);
}

/** React hook: the connection state, re-rendering when it changes. */
export function useLiveState(): LiveState {
  ensureConnected();
  return useSyncExternalStore(
    (l) => {
      stateListeners.add(l);
      return () => stateListeners.delete(l);
    },
    () => state,
  );
}
