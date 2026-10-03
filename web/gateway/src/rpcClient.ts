// JSON-RPC client for the caelitus TCP server: every message is JSON
// terminated by a NUL byte, and each connection answers in request order.
// A small pool of persistent connections; replies are matched to requests
// FIFO per connection. A timed-out request closes its connection, since a
// late reply would otherwise be matched to the next request.

import net from "node:net";

export class RpcError extends Error {
  constructor(
    readonly code: number,
    message: string,
    readonly data?: unknown,
  ) {
    super(message);
  }
}

interface Pending {
  resolve: (reply: string) => void;
  reject: (error: Error) => void;
  timer: NodeJS.Timeout;
}

class Connection {
  private socket: net.Socket | null = null;
  private connecting: Promise<net.Socket> | null = null;
  private buffer: Buffer = Buffer.alloc(0);
  private readonly pending: Pending[] = [];

  constructor(
    private readonly host: string,
    private readonly port: number,
    private readonly timeoutMs: number,
  ) {}

  get load(): number {
    return this.pending.length + (this.socket ? 0 : 1);
  }

  // Sends one message; resolves with the reply text, or with null when no
  // reply is expected (notifications).
  async send(message: string, expectReply: boolean): Promise<string | null> {
    const socket = await this.connect();
    const frame = Buffer.concat([Buffer.from(message, "utf8"), Buffer.from([0])]);
    if (!expectReply) {
      socket.write(frame);
      return null;
    }
    return new Promise<string>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.fail(new Error(`no reply within ${this.timeoutMs} ms`));
      }, this.timeoutMs);
      this.pending.push({ resolve, reject, timer });
      socket.write(frame);
    });
  }

  private connect(): Promise<net.Socket> {
    if (this.socket) return Promise.resolve(this.socket);
    if (this.connecting) return this.connecting;
    this.connecting = new Promise<net.Socket>((resolve, reject) => {
      const socket = net.createConnection({ host: this.host, port: this.port });
      socket.setNoDelay(true);
      socket.once("connect", () => {
        this.socket = socket;
        this.connecting = null;
        resolve(socket);
      });
      socket.on("data", (chunk) => this.onData(chunk));
      socket.on("error", (err) => {
        if (!this.socket) {
          this.connecting = null;
          reject(new Error(`cannot reach caelitus at ${this.host}:${this.port}: ${err.message}`));
        }
        this.fail(err);
      });
      socket.on("close", () => this.fail(new Error("connection to caelitus closed")));
    });
    return this.connecting;
  }

  private onData(chunk: Buffer): void {
    this.buffer = this.buffer.length ? Buffer.concat([this.buffer, chunk]) : chunk;
    for (let end = this.buffer.indexOf(0); end !== -1; end = this.buffer.indexOf(0)) {
      const reply = this.buffer.subarray(0, end).toString("utf8");
      this.buffer = this.buffer.subarray(end + 1);
      const waiter = this.pending.shift();
      if (!waiter) continue;  // nothing waiting: should not happen
      clearTimeout(waiter.timer);
      waiter.resolve(reply);
    }
  }

  // Drops the connection and fails everything waiting on it.
  private fail(error: Error): void {
    const socket = this.socket;
    this.socket = null;
    this.buffer = Buffer.alloc(0);
    socket?.destroy();
    for (const waiter of this.pending.splice(0)) {
      clearTimeout(waiter.timer);
      waiter.reject(error);
    }
  }
}

// Whether a request (or batch) has at least one member that gets a reply.
function expectsReply(request: unknown): boolean {
  const members = Array.isArray(request) ? request : [request];
  return members.some((m) => typeof m !== "object" || m === null || "id" in m);
}

export class RpcClient {
  private readonly pool: Connection[];
  private nextId = 1;

  constructor(host: string, port: number, size = 4, timeoutMs = 15000) {
    this.pool = Array.from({ length: size }, () => new Connection(host, port, timeoutMs));
  }

  // Forwards an already parsed JSON-RPC request or batch; returns the raw
  // reply text, or null for notifications.
  async forward(request: unknown): Promise<string | null> {
    const connection = this.pool.reduce((a, b) => (b.load < a.load ? b : a));
    return connection.send(JSON.stringify(request), expectsReply(request));
  }

  // Calls one method; resolves with its result or rejects with RpcError.
  async call<R = unknown>(method: string, params: Record<string, unknown> = {}): Promise<R> {
    const reply = await this.forward({ jsonrpc: "2.0", id: this.nextId++, method, params });
    const parsed = JSON.parse(reply ?? "null");
    if (parsed?.error) throw new RpcError(parsed.error.code, parsed.error.message, parsed.error.data);
    return parsed.result as R;
  }
}
