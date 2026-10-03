# The web UI and its gateway.
#   docker build -f docker/web.Dockerfile -t caelitus-web .
#
# Builds the UI (types generated from docs/openrpc.json), then runs the
# gateway, which serves the UI and the /rpc, /react, /ws and /health endpoints.

FROM node:22-slim
WORKDIR /app/web
COPY web/package.json web/package-lock.json ./
COPY web/gateway/package.json gateway/
COPY web/ui/package.json ui/
RUN npm ci
COPY docs/openrpc.json /app/docs/openrpc.json
COPY db/sample/catalog.json /app/db/sample/catalog.json
COPY web/ ./
RUN npm run build
ENV NODE_ENV=production PORT=8080
EXPOSE 8080
CMD ["npm", "start"]
