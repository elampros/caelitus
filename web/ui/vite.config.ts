/**
 * Vite configuration of the UI: React, the dev server on port 5173, and a
 * proxy of the API paths (`/rpc`, `/react`, `/health`, `/openrpc.json`, `/ws`)
 * to the gateway, so the UI and the API share one origin in development too.
 *
 * @module
 */

import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

// In development the gateway (port 8080) serves the API; Vite proxies to it.
const gateway = process.env.GATEWAY_URL ?? "http://127.0.0.1:8080";

/** The configuration Vite reads (`npm run dev -w ui`, `npm run build -w ui`). */
export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      "/rpc": gateway,
      "/react": gateway,
      "/health": gateway,
      "/openrpc.json": gateway,
      "/ws": { target: gateway.replace(/^http/, "ws"), ws: true },
    },
  },
});
