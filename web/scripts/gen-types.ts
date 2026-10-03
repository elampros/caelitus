// Generates TypeScript types for the UI from the OpenRPC document that the
// C++ server produces (docs/openrpc.json):
//   - an interface per component schema (Book, Author, ...)
//   - `Methods`: for every method, its params and result types
// Run: npm run gen   (after `cmake --build <dir> --target openrpc`)

import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

type Schema = Record<string, any>;

const root = join(dirname(fileURLToPath(import.meta.url)), "../..");
const doc = JSON.parse(readFileSync(join(root, "docs/openrpc.json"), "utf8"));
const out = join(root, "web/ui/src/api/generated.ts");

const quote = (s: string) => JSON.stringify(s);
const key = (name: string) => (/^[A-Za-z_$][\w$]*$/.test(name) ? name : quote(name));
const comment = (text: string | undefined, indent = "") =>
  text ? `${indent}/** ${text.replace(/\*\//g, "* /").replace(/\n+/g, " ")} */\n` : "";

function type(s: Schema, indent = ""): string {
  if (s.$ref) return String(s.$ref).split("/").pop()!;
  if (s.oneOf) return s.oneOf.map((x: Schema) => type(x, indent)).join(" | ");
  if ("const" in s) return JSON.stringify(s.const);
  if (s.enum) return s.enum.map((v: unknown) => JSON.stringify(v)).join(" | ");
  switch (s.type) {
    case "string":
      return "string";
    case "integer":
    case "number":
      return "number";
    case "boolean":
      return "boolean";
    case "null":
      return "null";
    case "array": {
      const item = type(s.items ?? {}, indent);
      return /[|&]/.test(item) ? `(${item})[]` : `${item}[]`;
    }
    case "object": {
      if (!s.properties) return "Record<string, unknown>";
      const required = new Set<string>(s.required ?? []);
      const inner = indent + "  ";
      const fields = Object.entries(s.properties as Record<string, Schema>).map(
        ([name, p]) =>
          `${comment(p.description, inner)}${inner}${key(name)}${required.has(name) ? "" : "?"}: ${type(p, inner)};`,
      );
      return `{\n${fields.join("\n")}\n${indent}}`;
    }
    default:
      return "unknown";
  }
}

const lines: string[] = [
  `// Generated from docs/openrpc.json (${doc.info.title} ${doc.info.version}) by web/scripts/gen-types.ts.`,
  "// Do not edit: run `npm run gen` instead.",
  "",
];

for (const [name, schema] of Object.entries(doc.components.schemas as Record<string, Schema>)) {
  lines.push(comment(schema.description) + `export type ${name} = ${type(schema)};`, "");
}

lines.push("export interface Methods {");
for (const m of doc.methods as Schema[]) {
  const params = m.params.length
    ? `{\n${m.params
        .map(
          (p: Schema) =>
            `${comment(p.description, "      ")}      ${key(p.name)}${p.required ? "" : "?"}: ${type(p.schema, "      ")}${
              p.required ? "" : " | null"
            };`,
        )
        .join("\n")}\n    }`
    : "Record<string, never>";
  lines.push(
    comment(m.summary, "  ") + `  ${quote(m.name)}: {\n    params: ${params};\n    result: ${type(m.result.schema, "    ")};\n  };`,
  );
}
lines.push("}", "", "export type MethodName = keyof Methods;", "");

writeFileSync(out, lines.join("\n"));
console.log(`wrote ${out}: ${Object.keys(doc.components.schemas).length} schemas, ${doc.methods.length} methods`);
