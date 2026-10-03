/**
 * The web gateway. Browsers cannot open raw TCP sockets, so this process
 * speaks HTTP/WebSocket to them and TCP/MQTT to the rest of the system:
 *
 * ```text
 *   POST /rpc            JSON-RPC request or batch, forwarded unchanged to caelitus
 *   GET  /openrpc.json   the API description (rpc.discover)
 *   POST /react          {bookId, kind} -> MQTT like/dislike
 *   GET  /ws             WebSocket: catalog events, likes, server status
 *   GET  /health         gateway, caelitus and broker reachability
 *   GET  /*              the built UI (ui/dist), when present
 * ```
 *
 * @module
 */

import fastifyStatic from "@fastify/static";
import fastifyWebsocket from "@fastify/websocket";
import Fastify from "fastify";
import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { Live } from "./live.ts";
import { RpcClient, RpcError } from "./rpcClient.ts";

const env = (name: string, fallback: string) => process.env[name] ?? fallback;
const config = {
  port: Number(env("PORT", "8080")),
  rpcHost: env("CAELITUS_RPC_HOST", "127.0.0.1"),
  rpcPort: Number(env("CAELITUS_RPC_PORT", "9000")),
  mqttUrl: env("MQTT_URL", "mqtt://127.0.0.1:1883"),
  reactionPrefix: env("REACTION_TOPIC_PREFIX", "catalog/in/books"),
  eventPrefix: env("EVENT_TOPIC_PREFIX", "catalog/books"),
  statusTopic: env("STATUS_TOPIC", "caelitus/status"),
};

const rpc = new RpcClient(config.rpcHost, config.rpcPort);
const live = new Live({
  url: config.mqttUrl,
  reactionPrefix: config.reactionPrefix,
  eventPrefix: config.eventPrefix,
  statusTopic: config.statusTopic,
});

const app = Fastify({ logger: { level: env("LOG_LEVEL", "info") }, bodyLimit: 1024 * 1024 });
await app.register(fastifyWebsocket);

const parseError = (reason: string) =>
  JSON.stringify({ jsonrpc: "2.0", id: null, error: { code: -32700, message: "Parse error", data: { reason } } });

// Raw body: the gateway does not reinterpret JSON-RPC, it only checks that
// it is JSON (to know whether a reply is expected).
app.addContentTypeParser("application/json", { parseAs: "string" }, (_req, body, done) => done(null, body));

app.post("/rpc", async (request, reply) => {
  let parsed: unknown;
  try {
    parsed = JSON.parse(String(request.body ?? ""));
  } catch (e) {
    return reply.code(200).type("application/json").send(parseError((e as Error).message));
  }
  try {
    const answer = await rpc.forward(parsed);
    if (answer === null) return reply.code(204).send();  // notifications only
    return reply.type("application/json").send(answer);
  } catch (e) {
    request.log.warn({ err: e }, "caelitus unreachable");
    return reply.code(502).type("application/json").send(
      JSON.stringify({ jsonrpc: "2.0", id: null, error: { code: -32003, message: "Service temporarily unavailable; retry later" } }),
    );
  }
});

app.get("/openrpc.json", async (_request, reply) => {
  try {
    return reply.send(await rpc.call("rpc.discover"));
  } catch (e) {
    return reply.code(502).send({ error: (e as Error).message });
  }
});

app.post("/react", async (request, reply) => {
  let body: { bookId?: unknown; kind?: unknown };
  try {
    body = JSON.parse(String(request.body ?? "{}"));
  } catch {
    return reply.code(400).send({ error: "body must be JSON" });
  }
  const bookId = Number(body.bookId);
  if (!Number.isSafeInteger(bookId) || bookId <= 0 || (body.kind !== "like" && body.kind !== "dislike"))
    return reply.code(400).send({ error: "expected {bookId: positive integer, kind: \"like\" | \"dislike\"}" });
  if (!live.react(bookId, body.kind)) return reply.code(503).send({ error: "MQTT broker not connected" });
  return reply.code(202).send({ accepted: true });
});

app.get("/health", async () => {
  let server: string;
  try {
    await rpc.call("system.ping");
    server = "up";
  } catch (e) {
    server = e instanceof RpcError ? "error" : "down";
  }
  return { gateway: "up", server, broker: live.brokerConnected ? "up" : "down" };
});

app.get("/ws", { websocket: true }, (socket) => {
  const send = (m: unknown) => socket.readyState === socket.OPEN && socket.send(JSON.stringify(m));
  send({ type: "broker", connected: live.brokerConnected, at: new Date().toISOString() });
  if (live.status) send(live.status);
  const unsubscribe = live.subscribe(send);
  socket.on("close", unsubscribe);
});

/**
 * The built UI, if any (in development Vite serves it and proxies the API).
 */
const uiDist = join(dirname(fileURLToPath(import.meta.url)), "../../ui/dist");
if (existsSync(uiDist)) {
  await app.register(fastifyStatic, { root: uiDist });
  app.setNotFoundHandler((request, reply) =>
    request.method === "GET" ? reply.sendFile("index.html") : reply.code(404).send({ error: "not found" }),
  );
}

const shutdown = async () => {
  await app.close();
  await live.close();
  process.exit(0);
};
process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);

await app.listen({ port: config.port, host: "0.0.0.0" });
app.log.info(`caelitus at ${config.rpcHost}:${config.rpcPort}, broker ${config.mqttUrl}`);
