# Rinha C Solution — Exact i16 100k Packed

Versão experimental para comparar contra a solução bucket/int8.

Esta versão segue a estratégia do líder:

- vetor `i16` com escala 8192;
- scan exato dos vetores de referência;
- top-5 vizinhos reais por insertion sort;
- AVX2 com `_mm256_sub_epi16` e `_mm256_madd_epi16`;
- registros alinhados em 64 bytes;
- query alinhada em 32 bytes;
- HAProxy em `mode tcp`;
- HAProxy -> APIs via Unix Domain Socket;
- parser JSON manual;
- respostas HTTP pré-computadas.

## Diferença para a versão ultra-c

A versão `ultra-c` usa bucket aproximado `int8` e ficou muito rápida com:

```yaml
SEARCH_RADIUS=0
MIN_CANDIDATES=0
```

Esta versão `exact-i16-100k` seleciona até 100k referências de forma distribuída no arquivo e faz scan completo somente nesse subconjunto. A expectativa é:

- melhor acurácia;
- P99 possivelmente maior;
- comportamento mais próximo da solução líder.

## Uso

Coloque o dataset:

```bash
mkdir -p resources
cp /caminho/references.json.gz resources/references.json.gz
```

Suba:

```bash
docker compose down --remove-orphans
rm -rf sock
mkdir -p sock
docker compose build --no-cache --progress=plain
docker compose up -d
```

Valide:

```bash
curl -i http://localhost:9999/ready

curl -sS -D /tmp/headers.txt -o /tmp/body.txt \
  -X POST http://localhost:9999/fraud-score \
  -H 'Content-Type: application/json' \
  --data-binary @sample-request.json || true

cat /tmp/headers.txt
wc -c /tmp/body.txt
cat /tmp/body.txt
```

Bench com curl:

```bash
CONCURRENCY=16 DURATION=30 URL=http://localhost:9999/fraud-score ./bench.sh
```

Bench com wrk e 50 payloads:

```bash
mkdir -p payloads
curl -fsSL https://raw.githubusercontent.com/zanfranceschi/rinha-de-backend-2026/refs/heads/main/resources/example-payloads.json \
  -o example-payloads.json
jq -c '.[]' example-payloads.json | awk '{ print > "payloads/payload-" NR ".json" }'

wrk -t4 -c16 -d30s --latency --timeout 10s -s post_random.lua http://localhost:9999
```

## Compose oficial

O `docker-compose.official.yml` usa a divisão sugerida pelo líder:

- LB: 0.10 CPU / 30 MB
- API1: 0.45 CPU / 160 MB
- API2: 0.45 CPU / 160 MB


## Importante

A versão anterior `exact-i16` varria todas as referências carregadas. Se o seu `references.json.gz` tiver milhões de vetores, o P99 sobe para dezenas/centenas de ms.

Esta versão limita o índice em build time:

```dockerfile
ARG MAX_REFS=100000
```

Para testar outro tamanho:

```bash
docker compose build --no-cache --build-arg MAX_REFS=150000 --progress=plain
docker compose up -d --force-recreate
```

Veja quantas refs foram carregadas:

```bash
docker logs rinha-api1 --tail=20
docker logs rinha-api2 --tail=20
```


## O que muda nesta versão packed

A versão exact-i16-100k anterior usava um `Record` de 64 bytes:

- 32 bytes do vetor;
- 1 byte de label;
- padding até 64 bytes.

Isso dobrava o tráfego de memória no scan.

Esta versão separa:

- `vectors`: array contíguo de 32 bytes por vetor;
- `labels`: array separado de 1 byte por referência.

Na prática, o scan de 100k cai de aproximadamente 6.4 MB por request para aproximadamente 3.2 MB por request, e o label é lido separadamente.
