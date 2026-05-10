FROM alpine:3.20 AS build

RUN apk add --no-cache build-base gzip

WORKDIR /src

COPY builder.c .
COPY server.c .

RUN mkdir -p /src/resources
COPY resources/references.json.gz /src/resources/references.json.gz

RUN gcc -O3 -flto -march=haswell -fomit-frame-pointer -fno-math-errno -DNDEBUG \
    -o builder builder.c -lm

RUN gcc -O3 -flto -march=haswell -mavx2 -mfma -mbmi2 -mpopcnt \
    -fomit-frame-pointer -fno-math-errno -DNDEBUG \
    -pthread -o rinha server.c -lm

ARG MAX_REFS=500000
RUN gzip -dc /src/resources/references.json.gz | MAX_REFS=${MAX_REFS} ./builder /src/index.bin

FROM alpine:3.20

WORKDIR /app

COPY --from=build /src/rinha /app/rinha
COPY --from=build /src/index.bin /app/index.bin

EXPOSE 8080

CMD ["/app/rinha", "/app/index.bin"]
