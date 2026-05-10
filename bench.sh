#!/usr/bin/env bash
set -euo pipefail

URL="${URL:-http://localhost:9999/fraud-score}"
PAYLOAD_URL="${PAYLOAD_URL:-https://raw.githubusercontent.com/zanfranceschi/rinha-de-backend-2026/refs/heads/main/resources/example-payloads.json}"

DURATION="${DURATION:-30}"
CONCURRENCY="${CONCURRENCY:-16}"
CONNECT_TIMEOUT="${CONNECT_TIMEOUT:-2}"
MAX_TIME="${MAX_TIME:-10}"

TMP_DIR="$(mktemp -d)"
PAYLOADS_JSON="$TMP_DIR/example-payloads.json"
PAYLOADS_DIR="$TMP_DIR/payloads"
RESULTS="$TMP_DIR/results.txt"
ERRORS="$TMP_DIR/errors.txt"

cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Comando obrigatório não encontrado: $1"
    echo "Instale com: sudo apt install -y $1"
    exit 1
  fi
}

need_cmd curl
need_cmd jq
need_cmd awk
need_cmd sort
need_cmd date
need_cmd wc

mkdir -p "$PAYLOADS_DIR"

echo "Baixando payloads..."
curl -fsSL "$PAYLOAD_URL" -o "$PAYLOADS_JSON"

echo "Separando payloads..."
jq -c '.[]' "$PAYLOADS_JSON" | awk -v dir="$PAYLOADS_DIR" '{ print > dir "/payload-" NR ".json" }'

TOTAL_PAYLOADS="$(find "$PAYLOADS_DIR" -type f -name 'payload-*.json' | wc -l | tr -d ' ')"
if [ "$TOTAL_PAYLOADS" -eq 0 ]; then
  echo "Nenhum payload encontrado."
  exit 1
fi

READY_URL="${URL%/fraud-score}/ready"

echo "Payloads: $TOTAL_PAYLOADS URL: $URL Duration: ${DURATION}s Concurrency: $CONCURRENCY"
echo "Testando /ready..."
curl -fsS -o /dev/null -w "ready_http_code=%{http_code}\n" "$READY_URL" || true

worker() {
  local wid="$1"
  local out_file="$TMP_DIR/results-$wid.txt"
  local err_file="$TMP_DIR/errors-$wid.txt"
  local end_ts=$(( $(date +%s) + DURATION ))

  while [ "$(date +%s)" -lt "$end_ts" ]; do
    local n=$(( (RANDOM % TOTAL_PAYLOADS) + 1 ))
    local payload="$PAYLOADS_DIR/payload-${n}.json"

    local result
    if result="$(
      curl -sS \
        --connect-timeout "$CONNECT_TIMEOUT" \
        --max-time "$MAX_TIME" \
        -o /dev/null \
        -w "%{http_code} %{time_total}" \
        -X POST "$URL" \
        -H "Content-Type: application/json" \
        --data-binary "@$payload" \
        2>>"$err_file"
    )"; then
      echo "$result" >> "$out_file"
    else
      echo "000 0" >> "$out_file"
    fi
  done
}

START_NS="$(date +%s%N)"

for i in $(seq 1 "$CONCURRENCY"); do
  worker "$i" &
done

wait

cat "$TMP_DIR"/results-*.txt > "$RESULTS" 2>/dev/null || true
cat "$TMP_DIR"/errors-*.txt > "$ERRORS" 2>/dev/null || true

END_NS="$(date +%s%N)"

ELAPSED="$(awk "BEGIN { printf \"%.4f\", ($END_NS - $START_NS) / 1000000000 }")"
TOTAL_REQ="$(wc -l < "$RESULTS" | tr -d ' ')"
OK_REQ="$(awk '$1 == 200 { ok++ } END { print ok+0 }' "$RESULTS")"
ERR_REQ="$(awk '$1 != 200 { err++ } END { print err+0 }' "$RESULTS")"

RPS="$(awk "BEGIN { if ($ELAPSED > 0) printf \"%.2f\", $TOTAL_REQ / $ELAPSED; else print \"0.00\" }")"
OK_RPS="$(awk "BEGIN { if ($ELAPSED > 0) printf \"%.2f\", $OK_REQ / $ELAPSED; else print \"0.00\" }")"
AVG_LAT="$(awk '$1 == 200 { sum += $2; n++ } END { if (n > 0) printf "%.4f", sum/n; else print "0.0000" }' "$RESULTS")"

percentile() {
  local p="$1"
  awk '$1 == 200 { print $2 }' "$RESULTS" | sort -n | awk -v p="$p" '
    { a[NR]=$1 }
    END {
      if (NR == 0) {
        print "0.0000"
      } else {
        idx = int(NR * p)
        if (idx < 1) idx = 1
        if (idx > NR) idx = NR
        printf "%.4f", a[idx]
      }
    }'
}

P50="$(percentile 0.50)"
P90="$(percentile 0.90)"
P99="$(percentile 0.99)"

echo "Resultado"
echo "========="
echo "Tempo total:       ${ELAPSED}s"
echo "Requests totais:   $TOTAL_REQ"
echo "HTTP 200:          $OK_REQ"
echo "Erros:             $ERR_REQ"
echo "RPS total:         $RPS"
echo "RPS HTTP 200:      $OK_RPS"
echo "Latência média:    ${AVG_LAT}s"
echo "P50:               ${P50}s"
echo "P90:               ${P90}s"
echo "P99:               ${P99}s"

echo
echo "Status codes:"
awk '{ codes[$1]++ } END { for (c in codes) print c, codes[c] }' "$RESULTS" | sort

if [ -s "$ERRORS" ]; then
  echo
  echo "Erros curl mais comuns:"
  sort "$ERRORS" | uniq -c | sort -nr | head -10
fi
