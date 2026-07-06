FROM node:20-alpine

WORKDIR /app

COPY backend/package.json ./
RUN npm install --production

COPY backend/ ./
COPY public/ ../public/

# data/ is where store.json lives - mount a volume here to persist across container restarts
VOLUME ["/app/data"]

EXPOSE 8080

CMD ["node", "server.js"]
