/**
 * Loads the sample catalog (db/sample/catalog.json) through the JSON-RPC API,
 * so every rule and validation applies. Refuses to run on a non-empty catalog.
 *
 * ```text
 *   npm run seed                       # caelitus at 127.0.0.1:9000
 *   CAELITUS_RPC_PORT=9123 npm run seed
 * ```
 *
 * To load the same data without a running server, use db/sample/catalog.sql.
 *
 * @module
 */

import { readFileSync } from "node:fs";
import { RpcClient, RpcError } from "../gateway/src/rpcClient.ts";

interface SampleCatalog {
  categories: { slug: string; name: string }[];
  books: { title: string; authors: string[]; year: number; category: string; language: string; tags: string[] }[];
  reviewers: string[];
  reviewTexts: Record<string, string[]>;
}

const { categories, books, reviewers, reviewTexts }: SampleCatalog = JSON.parse(
  readFileSync(new URL("../../db/sample/catalog.json", import.meta.url), "utf8"),
);

const rpc = new RpcClient(process.env.CAELITUS_RPC_HOST ?? "127.0.0.1", Number(process.env.CAELITUS_RPC_PORT ?? 9000), 1);

/**
 * Deterministic pseudo-random numbers, so every seed produces the same data.
 */
let state = 20261003;
const random = () => {
  state = (state * 1103515245 + 12345) % 2147483648;
  return state / 2147483648;
};
const pick = <T>(items: T[]): T => items[Math.floor(random() * items.length)]!;

async function main() {
  const existing = await rpc.call<{ total: number }>("books.search", { pageSize: 1 });
  if (existing.total > 0) {
    console.error(`The catalog already has ${existing.total} books; seeding only runs on an empty catalog.`);
    process.exit(1);
  }

  const categoryIds = new Map<string, number>();
  for (const c of categories) {
    const created = await rpc.call<{ id: number }>("categories.create", { name: c.name, slug: c.slug });
    categoryIds.set(c.slug, created.id);
  }

  const authorIds = new Map<string, number>();
  for (const name of new Set(books.flatMap((b) => b.authors))) {
    const created = await rpc.call<{ id: number }>("authors.create", { name });
    authorIds.set(name, created.id);
  }

  let reviews = 0;
  let enabled = 0;
  for (const { title, authors, year, category, language, tags } of books) {
    const reactionsEnabled = random() < 0.85;
    const book = await rpc.call<{ id: number }>("books.create", {
      title,
      publishedOn: `${String(year).padStart(4, "0")}-01-01`,
      language,
      categoryId: categoryIds.get(category),
      authorIds: authors.map((a) => authorIds.get(a)),
      tags,
      reactionsEnabled,
    });
    if (reactionsEnabled) enabled++;

    const count = Math.floor(random() * 5);  // 0-4 reviews
    for (let i = 0; i < count; i++) {
      const rating = Math.min(5, Math.max(1, Math.round(3.6 + (random() - 0.4) * 3)));
      await rpc.call("reviews.create", {
        bookId: book.id,
        reviewerName: pick(reviewers),
        rating,
        body: pick(reviewTexts[String(rating)]!),
      });
      reviews++;
    }
  }

  console.log(
    `Seeded ${categories.length} categories, ${authorIds.size} authors, ${books.length} books ` +
      `(${enabled} accept likes/dislikes) and ${reviews} sample reviews.`,
  );
}

main()
  .then(() => process.exit(0))
  .catch((e) => {
    console.error(e instanceof RpcError ? `${e.message} ${JSON.stringify(e.data)}` : e);
    process.exit(1);
  });
