cat > bench.sh <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

URL="${URL:-http://localhost:8080/fraud-score}"
PAYLOAD_URL="${PAYLOAD_URL:-https://raw.githubusercontent.com/zanfranceschi/rinha-de-backend-2026/refs/heads/main/resources/example-payloads.json}"

DURATION="${DURATION:-30}"
CONCURRENCY="${CONCURRENCY:-8}"
TMP_DIR="$(mktemp -d)"
PAYLOADS_JSON="$TMP_DIR/example-payloads.json"
PAYLOADS_DIR="$TMP_DIR/payloads"
RESULTS="$TMP_DIR/results.txt"

mkdir -p "$PAYLOADS_DIR"

cleanup() {
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Comando obrigatório não encontrado: $1"
    echo "Instale com algo como: sudo apt install -y $1"
    exit 1
  fi
}

need_cmd curl
need_cmd jq
need_cmd xargs
need_cmd awk
need_cmd date

echo "Baixando payloads..."
curl -fsSL "$PAYLOAD_URL" -o "$PAYLOADS_JSON"

echo "Separando payloads..."
jq -c '.[]' "$PAYLOADS_JSON" | awk -v dir="$PAYLOADS_DIR" '{ print > dir "/payload-" NR ".json" }'

TOTAL_PAYLOADS="$(find "$PAYLOADS_DIR" -type f -name 'payload-*.json' | wc -l | tr -d ' ')"

if [ "$TOTAL_PAYLOADS" -eq 0 ]; then
  echo "Nenhum payload encontrado."
  exit 1
fi

echo "Payloads encontrados: $TOTAL_PAYLOADS"
echo "URL: $URL"
echo "Duração: ${DURATION}s"
echo "Concorrência: $CONCURRENCY"
echo

echo "Testando /ready..."
READY_URL="${URL%/fraud-score}/ready"
curl -fsS -o /dev/null -w "ready_http_code=%{http_code}\n" "$READY_URL" || true
echo

export URL
export PAYLOADS_DIR

worker() {
  end=$(( $(date +%s) + DURATION ))

  while [ "$(date +%s)" -lt "$end" ]; do
    file_num=$(( (RANDOM % TOTAL_PAYLOADS) + 1 ))
    file="$PAYLOADS_DIR/payload-${file_num}.json"

    code="$(
      curl -sS \
        -o /dev/null \
        -w "%{http_code} %{time_total}" \
        -X POST "$URL" \
        -H "Content-Type: application/json" \
        --data-binary "@$file" \
        2>/dev/null || echo "000 0"
    )"

    echo "$code"
  done
}

export -f worker
export DURATION
export TOTAL_PAYLOADS

START_NS="$(date +%s%N)"

seq "$CONCURRENCY" | xargs -P "$CONCURRENCY" -I{} bash -c 'worker' > "$RESULTS"

END_NS="$(date +%s%N)"

ELAPSED="$(awk "BEGIN { print ($END_NS - $START_NS) / 1000000000 }")"
TOTAL_REQ="$(wc -l < "$RESULTS" | tr -d ' ')"
OK_REQ="$(awk '$1 == 200 { ok++ } END { print ok+0 }' "$RESULTS")"
ERR_REQ="$(awk '$1 != 200 { err++ } END { print err+0 }' "$RESULTS")"
RPS="$(awk "BEGIN { if ($ELAPSED > 0) printf \"%.2f\", $TOTAL_REQ / $ELAPSED; else print 0 }")"
OK_RPS="$(awk "BEGIN { if ($ELAPSED > 0) printf \"%.2f\", $OK_REQ / $ELAPSED; else print 0 }")"

AVG_LAT="$(awk '$1 == 200 { sum += $2; n++ } END { if (n > 0) printf "%.4f", sum/n; else print "0" }' "$RESULTS")"

P50="$(awk '$1 == 200 { print $2 }' "$RESULTS" | sort -n | awk '{ a[NR]=$1 } END { if (NR > 0) printf "%.4f", a[int(NR*0.50) ? int(NR*0.50) : 1]; else print "0" }')"
P90="$(awk '$1 == 200 { print $2 }' "$RESULTS" | sort -n | awk '{ a[NR]=$1 } END { if (NR > 0) printf "%.4f", a[int(NR*0.90) ? int(NR*0.90) : 1]; else print "0" }')"
P99="$(awk '$1 == 200 { print $2 }' "$RESULTS" | sort -n | awk '{ a[NR]=$1 } END { if (NR > 0) printf "%.4f", a[int(NR*0.99) ? int(NR*0.99) : 1]; else print "0" }')"

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
EOF

chmod +x bench.sh
