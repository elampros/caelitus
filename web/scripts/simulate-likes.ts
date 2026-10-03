// Publishes random likes/dislikes over MQTT, like many readers' devices
// would, so the live dashboard has something to show. A few books are far
// more popular than the rest (Zipf-like), and some are mostly disliked.
//
//   npm run simulate                   # 20 reactions/s
//   RATE=100 npm run simulate

import mqtt from "mqtt";
import { RpcClient } from "../gateway/src/rpcClient.ts";

const rate = Number(process.env.RATE ?? 20);
const prefix = process.env.REACTION_TOPIC_PREFIX ?? "catalog/in/books";
const rpc = new RpcClient(process.env.CAELITUS_RPC_HOST ?? "127.0.0.1", Number(process.env.CAELITUS_RPC_PORT ?? 9000), 1);

interface Summary { id: number; reactionsEnabled: boolean }
interface BookPage { items: Summary[]; pageCount: number }

async function enabledBooks(): Promise<number[]> {
  const ids: number[] = [];
  for (let page = 1; ; page++) {
    const result = await rpc.call<BookPage>("books.search", { page, pageSize: 100, sort: "titleAsc" });
    ids.push(...result.items.filter((b) => b.reactionsEnabled).map((b) => b.id));
    if (page >= result.pageCount) return ids;
  }
}

const ids = await enabledBooks();
if (!ids.length) {
  console.error("No books accept likes yet (seed the catalog, or enable reactions on some books).");
  process.exit(1);
}
// Shuffle once so popularity is not tied to the alphabet.
for (let i = ids.length - 1; i > 0; i--) {
  const j = Math.floor(Math.random() * (i + 1));
  [ids[i], ids[j]] = [ids[j]!, ids[i]!];
}
const weights = ids.map((_, i) => 1 / (i + 1));
const total = weights.reduce((a, b) => a + b, 0);
const pickBook = () => {
  let r = Math.random() * total;
  for (let i = 0; i < ids.length; i++) if ((r -= weights[i]!) <= 0) return { id: ids[i]!, rank: i };
  return { id: ids[0]!, rank: 0 };
};

const client = mqtt.connect(process.env.MQTT_URL ?? "mqtt://127.0.0.1:1883", { clientId: `caelitus-simulator-${process.pid}` });
client.on("connect", () => {
  console.log(`Sending ~${rate} reactions/s for ${ids.length} books to ${prefix}/<id>/like|dislike (Ctrl+C to stop)`);
  let sent = 0;
  setInterval(() => {
    const { id, rank } = pickBook();
    // Every 7th book in the popularity order is mostly disliked.
    const dislikeChance = rank % 7 === 3 ? 0.75 : 0.15;
    client.publish(`${prefix}/${id}/${Math.random() < dislikeChance ? "dislike" : "like"}`, "");
    if (++sent % (rate * 10) === 0) console.log(`${sent} reactions sent`);
  }, 1000 / rate);
});
client.on("error", (e) => console.error("MQTT:", e.message));
process.on("SIGINT", () => client.end(false, {}, () => process.exit(0)));
