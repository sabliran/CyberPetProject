# Build stage: compile native node modules (better-sqlite3) in alpine
FROM node:20-alpine AS builder
RUN apk add --no-cache python3 make g++
WORKDIR /build
COPY backend/package.json ./
RUN npm install --production

# Runtime stage: lean image without build tools
FROM node:20-alpine
# python3 runs the USB sync bridge (usb-bridge.py) as a child process of the
# server, started/stopped from the dashboard UI.
RUN apk add --no-cache python3
WORKDIR /app
COPY backend/ ./
# Overwrite with correctly-compiled alpine binaries from build stage
COPY --from=builder /build/node_modules ./node_modules
COPY public/ ../public/
COPY usb-bridge.py ./usb-bridge.py

# data/ is where store.db lives — mount a volume here to persist across restarts
VOLUME ["/app/data"]

EXPOSE 8080

CMD ["node", "server.js"]
