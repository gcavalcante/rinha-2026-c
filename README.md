# Rinha C Solution — Exact i16 Packed Keep-Alive

Versão experimental com HTTP keep-alive real, múltiplas requisições por conexão, vetores i16 packed, HAProxy TCP + Unix Domain Socket e `MAX_REFS` default 50000.

## Build local

```bash
mkdir -p resources
cp ../rinha-c-solution/resources/references.json.gz resources/references.json.gz

docker compose down --remove-orphans
rm -rf sock
mkdir -p sock

docker compose build --no-cache --build-arg MAX_REFS=50000 --progress=plain
docker compose up -d --force-recreate
```

## Teste com limites oficiais locais

```bash
docker compose --compatibility down --remove-orphans
rm -rf sock
mkdir -p sock

docker compose --compatibility build --no-cache --build-arg MAX_REFS=50000 --progress=plain
docker compose --compatibility up -d --force-recreate

docker logs rinha-api1 --tail=20
```

## Publicar imagem

```bash
docker buildx build \
  --platform linux/amd64 \
  --build-arg MAX_REFS=50000 \
  -t gcavalcante/rinha-2026-c-exact-i16:50000-keepalive \
  --push .
```
