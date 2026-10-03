/**
 * API explorer built from the server's live OpenRPC document (rpc.discover):
 * every method with its parameters, result and errors, a "try it" form, and
 * the shared schemas.
 *
 * @module
 */

import { useEffect, useMemo, useState } from "react";
import { Search } from "../components/Icons";

type Schema = Record<string, any>;
interface Param { name: string; required: boolean; description?: string; schema: Schema }
interface Method {
  name: string;
  summary: string;
  description?: string;
  params: Param[];
  result: { name: string; schema: Schema };
  errors: Array<{ $ref: string }>;
  tags?: Array<{ $ref: string }>;
}
interface Doc {
  info: { title: string; version: string; description: string };
  methods: Method[];
  components: { schemas: Record<string, Schema>; errors: Record<string, { code: number; message: string; data?: Record<string, string> }> };
}

const refName = (ref: string) => ref.split("/").pop()!;

/**
 * A one-line type: "integer ≥ 1", "string (date)", "Book[]", "string | null".
 */
function typeOf(s: Schema): string {
  if (s.$ref) return refName(s.$ref);
  if (s.oneOf) return s.oneOf.map(typeOf).join(" | ");
  if ("const" in s) return JSON.stringify(s.const);
  if (s.enum) return s.enum.map((v: unknown) => JSON.stringify(v)).join(" | ");
  if (s.type === "array") return `${typeOf(s.items ?? {})}[]`;
  const bits: string[] = [];
  if (s.format) bits.push(s.format);
  if (s.minimum !== undefined && s.maximum !== undefined) bits.push(`${s.minimum}–${s.maximum}`);
  else if (s.minimum !== undefined) bits.push(`≥ ${s.minimum}`);
  if (s.maxLength !== undefined) bits.push(`≤ ${s.maxLength} χαρ.`);
  return `${s.type ?? "any"}${bits.length ? ` (${bits.join(", ")})` : ""}`;
}

function exampleValue(s: Schema, doc: Doc): unknown {
  if (s.$ref) return exampleValue(doc.components.schemas[refName(s.$ref)] ?? {}, doc);
  if (s.oneOf) return exampleValue(s.oneOf[0], doc);
  if (s.enum) return s.enum[0];
  if (s.type === "string") return s.format === "date" ? "2000-01-01" : s.format === "date-time" ? new Date().toISOString() : "";
  if (s.type === "integer" || s.type === "number") return s.minimum ?? 1;
  if (s.type === "boolean") return true;
  if (s.type === "array") return [exampleValue(s.items ?? {}, doc)];
  return {};
}

/**
 * Required params with placeholder values; optional ones are listed in the table.
 */
function exampleParams(m: Method, doc: Doc): string {
  const params: Record<string, unknown> = {};
  for (const p of m.params) if (p.required) params[p.name] = exampleValue(p.schema, doc);
  return JSON.stringify(params, null, 2);
}

function SchemaView({ schema, doc, onRef }: { schema: Schema; doc: Doc; onRef: (name: string) => void }) {
  const resolved = schema.$ref ? doc.components.schemas[refName(schema.$ref)] : schema;
  if (resolved?.type === "object" && resolved.properties)
    return (
      <table>
        <thead><tr><th>Πεδίο</th><th>Τύπος</th><th>Περιγραφή</th></tr></thead>
        <tbody>
          {Object.entries(resolved.properties as Record<string, Schema>).map(([name, p]) => {
            const ref = p.$ref ?? p.items?.$ref ?? p.oneOf?.find((x: Schema) => x.$ref)?.$ref;
            return (
              <tr key={name}>
                <td><code>{name}</code>{!(resolved.required ?? []).includes(name) && <span className="muted"> (προαιρετικό)</span>}</td>
                <td className="type">{ref ? <a href="#" onClick={(e) => (e.preventDefault(), onRef(refName(ref)))}>{typeOf(p)}</a> : typeOf(p)}</td>
                <td className="secondary">{p.description ?? ""}</td>
              </tr>
            );
          })}
        </tbody>
      </table>
    );
  return <code className="type">{typeOf(schema)}</code>;
}

/** The `/api` page: method list with search, method details, schemas. */
export function ApiPage() {
  const [doc, setDoc] = useState<Doc | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [filter, setFilter] = useState("");
  const [selected, setSelected] = useState<string>(() => location.hash.slice(1) || "books.search");
  const [schemaName, setSchemaName] = useState<string | null>(null);
  const [params, setParams] = useState("{}");
  const [response, setResponse] = useState<{ text: string; ms: number } | null>(null);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    fetch("/openrpc.json")
      .then((r) => (r.ok ? r.json() : Promise.reject(new Error(`HTTP ${r.status}`))))
      .then(setDoc)
      .catch((e) => setError(`Δεν φορτώθηκε το OpenRPC: ${e.message}`));
  }, []);

  const method = doc?.methods.find((m) => m.name === selected);
  useEffect(() => {
    if (doc && method) setParams(exampleParams(method, doc));
    setResponse(null);
    history.replaceState(null, "", `#${selected}`);
  }, [doc, selected]);

  const groups = useMemo(() => {
    const out = new Map<string, Method[]>();
    for (const m of doc?.methods ?? []) {
      if (filter && !m.name.includes(filter.toLowerCase()) && !m.summary.toLowerCase().includes(filter.toLowerCase())) continue;
      const tag = m.tags?.[0] ? refName(m.tags[0].$ref) : "other";
      out.set(tag, [...(out.get(tag) ?? []), m]);
    }
    return [...out.entries()].sort(([a], [b]) => a.localeCompare(b));
  }, [doc, filter]);

  const send = async () => {
    let parsed: unknown;
    try {
      parsed = JSON.parse(params || "{}");
    } catch (e) {
      setResponse({ text: `Τα params δεν είναι έγκυρο JSON: ${(e as Error).message}`, ms: 0 });
      return;
    }
    setBusy(true);
    const started = performance.now();
    try {
      const r = await fetch("/rpc", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ jsonrpc: "2.0", id: 1, method: selected, params: parsed }),
      });
      const text = await r.text();
      setResponse({ text: JSON.stringify(JSON.parse(text), null, 2), ms: performance.now() - started });
    } catch (e) {
      setResponse({ text: String(e), ms: performance.now() - started });
    } finally {
      setBusy(false);
    }
  };

  if (error) return <p className="error">{error}</p>;
  if (!doc) return <p className="muted">Φόρτωση του OpenRPC…</p>;

  return (
    <>
      <div className="page-head">
        <h1>{doc.info.title}</h1>
        <p>έκδοση {doc.info.version} · {doc.methods.length} μέθοδοι · JSON-RPC 2.0 πάνω από TCP</p>
        <span className="spacer" />
        <a href="/openrpc.json" target="_blank" rel="noreferrer">openrpc.json</a>
      </div>
      <div className="api">
        <nav className="card method-list">
          <span className="input-icon">
            <Search size={14} />
            <input placeholder="Αναζήτηση μεθόδου" value={filter} onChange={(e) => setFilter(e.target.value)} type="search" />
          </span>
          {groups.map(([tag, methods]) => (
            <div key={tag}>
              <div className="group">{tag}</div>
              {methods.map((m) => (
                <button key={m.name} className={m.name === selected ? "on" : ""} onClick={() => (setSelected(m.name), setSchemaName(null))}>
                  {m.name}
                </button>
              ))}
            </div>
          ))}
          <div className="group">schemas</div>
          {Object.keys(doc.components.schemas).map((name) => (
            <button key={name} className={schemaName === name ? "on" : ""} onClick={() => setSchemaName(name)}>{name}</button>
          ))}
        </nav>

        {schemaName ? (
          <section className="card method">
            <div className="row"><h2><code>{schemaName}</code></h2><span className="muted">schema</span></div>
            {doc.components.schemas[schemaName]?.description && <p className="secondary" style={{ margin: 0 }}>{doc.components.schemas[schemaName]!.description}</p>}
            <SchemaView schema={{ $ref: `#/components/schemas/${schemaName}` }} doc={doc} onRef={setSchemaName} />
          </section>
        ) : method ? (
          <section className="card method">
            <div>
              <h2><code>{method.name}</code></h2>
              <p style={{ margin: "6px 0 0" }}>{method.summary}</p>
              {method.description && <p className="secondary" style={{ margin: "6px 0 0", whiteSpace: "pre-line" }}>{method.description}</p>}
            </div>

            <div>
              <h3 style={{ marginBottom: 8 }}>Παράμετροι</h3>
              {method.params.length === 0 ? (
                <p className="muted" style={{ margin: 0 }}>Καμία.</p>
              ) : (
                <table>
                  <thead><tr><th>Όνομα</th><th>Τύπος</th><th></th><th>Περιγραφή</th></tr></thead>
                  <tbody>
                    {method.params.map((p) => (
                      <tr key={p.name}>
                        <td><code>{p.name}</code></td>
                        <td className="type">{typeOf(p.schema)}</td>
                        <td>{p.required ? <span className="badge req">υποχρεωτικό</span> : <span className="badge">προαιρετικό</span>}</td>
                        <td className="secondary">{p.description}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              )}
            </div>

            <div>
              <h3 style={{ marginBottom: 8 }}>Αποτέλεσμα <code className="muted">{method.result.name}</code></h3>
              <SchemaView schema={method.result.schema} doc={doc} onRef={setSchemaName} />
            </div>

            <div>
              <h3 style={{ marginBottom: 8 }}>Σφάλματα</h3>
              <table>
                <tbody>
                  {method.errors.map((e) => {
                    const def = doc.components.errors[refName(e.$ref)];
                    return (
                      <tr key={e.$ref}>
                        <td className="num" style={{ width: 80 }}><code>{def?.code}</code></td>
                        <td>{def?.message}</td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>

            <div style={{ display: "flex", flexDirection: "column", gap: 8 }}>
              <h3>Δοκίμασέ το</h3>
              <textarea className="code" value={params} onChange={(e) => setParams(e.target.value)} spellCheck={false} aria-label="params (JSON)" />
              <div className="row">
                <button className="primary" onClick={send} disabled={busy}>Αποστολή</button>
                <span className="muted" style={{ fontSize: 13 }}>
                  Στέλνεται πραγματικά στον server{/(create|update|delete|set)/i.test(method.name) ? " και αλλάζει δεδομένα" : ""}.
                </span>
                {response && response.ms > 0 && <span className="muted" style={{ marginLeft: "auto" }}>{response.ms.toFixed(0)} ms</span>}
              </div>
              {response && <pre className="response">{response.text}</pre>}
            </div>
          </section>
        ) : (
          <p className="muted">Διάλεξε μια μέθοδο.</p>
        )}
      </div>
    </>
  );
}
