import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

// In development the gateway (port 8080) serves the API; Vite proxies to it.
const gateway = process.env.GATEWAY_URL ?? "http://127.0.0.1:8080";

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
