// One shared WebSocket to the gateway's /ws, reconnecting on its own.
// Components subscribe with useLive(handler) and read connection state
// with useLiveState().

import { useEffect, useRef, useSyncExternalStore } from "react";

export type LiveMessage =
  | { type: "reaction"; bookId: number; kind: "like" | "dislike"; at: string }
  | { type: "event"; bookId: number; event: string; payload: string; at: string }
  | { type: "status"; server: string; at: string }
  | { type: "broker"; connected: boolean; at: string };

export interface LiveState {
  socket: "connecting" | "open" | "closed";
  broker: boolean | null;
  server: string | null;  // "online" / "offline" from the server's last will
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
