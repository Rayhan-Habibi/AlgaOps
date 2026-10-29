# AlgaOps gateway image (linux/arm64 for Raspberry Pi 4/5 or linux/amd64).
# Build from the repository root:
#   docker build -f deploy/docker/gateway.Dockerfile -t algaops-gateway .
# The C core is compiled and its CTest suite runs during the build.

FROM python:3.12-slim-bookworm AS core
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY algaops-assurance/ algaops-assurance/
RUN cmake -S algaops-assurance -B /build -DCMAKE_BUILD_TYPE=Release -DALGAOPS_WERROR=ON \
 && cmake --build /build -j"$(nproc)" \
 && ctest --test-dir /build --output-on-failure
COPY gateway/ gateway/
# Runtime part of the lock file (everything above "# Development and tests").
RUN sed -n '1,/^# Development and tests/p' gateway/requirements.lock > /requirements.runtime.txt \
 && pip wheel --no-cache-dir -w /wheels -r /requirements.runtime.txt \
 && pip wheel --no-cache-dir --no-deps -w /wheels ./gateway

FROM node:22-bookworm-slim AS dashboard
WORKDIR /src/dashboard
COPY dashboard/package.json dashboard/package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY dashboard/ ./
RUN npm run build

FROM python:3.12-slim-bookworm
RUN useradd --system --uid 10001 --home /var/lib/algaops algaops \
 && install -d -o algaops -g algaops -m 0750 /var/lib/algaops
COPY --from=core /wheels /tmp/wheels
RUN pip install --no-cache-dir --no-index --find-links /tmp/wheels algaops-gateway \
 && rm -rf /tmp/wheels
COPY --from=core /build/libalgaops.so /opt/algaops/lib/libalgaops.so
COPY --from=dashboard /src/dashboard/dist /opt/algaops/dashboard
COPY gateway/config/policy.default.toml /opt/algaops/config/policy.default.toml
ENV ALGAOPS_LIB=/opt/algaops/lib/libalgaops.so \
    ALGAOPS_POLICY=/opt/algaops/config/policy.default.toml \
    PYTHONUNBUFFERED=1
USER algaops
WORKDIR /var/lib/algaops
VOLUME ["/var/lib/algaops"]
EXPOSE 8080
ENTRYPOINT ["algaops-gateway"]
CMD ["--config", "/etc/algaops/gateway.toml", "run"]
