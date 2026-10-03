// Typed JSON-RPC client over the gateway's POST /rpc. Method names, params
// and results come from types generated from the server's OpenRPC document.

import type { MethodName, Methods } from "./generated";

export class RpcError extends Error {
  constructor(
    readonly code: number,
    message: string,
    readonly data?: { code?: string; field?: string; reason?: string; [k: string]: unknown },
  ) {
    super(message);
  }
  // A message fit for the user: validation details when there are some.
  get detail(): string {
    return this.data?.reason ?? this.message;
  }
}

type ParamsArg<M extends MethodName> = Methods[M]["params"] extends Record<string, never>
  ? [params?: Record<string, never>]
  : [params: Methods[M]["params"]];

let nextId = 1;

async function post(body: unknown): Promise<any> {
  const response = await fetch("/rpc", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });
  if (response.status === 204) return null;
  const text = await response.text();
  try {
    return JSON.parse(text);
  } catch {
    throw new RpcError(-32003, `Ο gateway απάντησε ${response.status}`);
  }
}

export async function rpc<M extends MethodName>(method: M, ...[params]: ParamsArg<M>): Promise<Methods[M]["result"]> {
  const reply = await post({ jsonrpc: "2.0", id: nextId++, method, params: params ?? {} });
  if (reply?.error) throw new RpcError(reply.error.code, reply.error.message, reply.error.data);
  return reply.result;
}

// Several calls in one round trip (a JSON-RPC batch). Results keep the
// order of the calls; failed calls come back as RpcError values.
export async function rpcBatch<M extends MethodName>(
  method: M,
  paramsList: Methods[M]["params"][],
): Promise<Array<Methods[M]["result"] | RpcError>> {
  if (!paramsList.length) return [];
  const first = nextId;
  nextId += paramsList.length;
  const replies: any[] = await post(paramsList.map((params, i) => ({ jsonrpc: "2.0", id: first + i, method, params })));
  const byId = new Map(replies.map((r) => [r.id, r]));
  return paramsList.map((_, i) => {
    const r = byId.get(first + i);
    return r?.error ? new RpcError(r.error.code, r.error.message, r.error.data) : r?.result;
  });
}

// A like/dislike, sent through the gateway to MQTT like a device would.
export async function react(bookId: number, kind: "like" | "dislike"): Promise<void> {
  const response = await fetch("/react", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ bookId, kind }),
  });
  if (!response.ok) throw new Error((await response.json()).error ?? `HTTP ${response.status}`);
}
