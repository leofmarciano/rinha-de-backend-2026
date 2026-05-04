# Leader Study: thiagorigonatti/rinha-2026

Audit source: https://github.com/thiagorigonatti/rinha-2026

Audited commit: `7ae309fbfe0e0ccd25fc01dfcb008b2b5f8522b3`

License: `AGPL-3.0`. This project must not copy source code from that repository. The allowed use
here is clean-room engineering: observe public behavior and architecture, then implement equivalent
ideas independently in this codebase.

## What The Leader Does Well

- Runtime is C with no web framework in the hot path.
- HAProxy talks to each API through Unix domain sockets and uses `http-reuse always`.
- Requests use fixed-size buffers and pre-rendered JSON responses for the six possible scores.
- The index stores 14-dimensional vectors as fixed-point `int16` using scale `10000`.
- Search keeps a deterministic top-5 ordered by distance and original dataset id.
- Vectors are scanned in SoA layout, with an AVX2 path on the target Haswell CPU.
- IVF clusters include per-list bounding boxes for repair after the initial probe.

## Attack Surface

- The public compose uses `IVF_NPROBE=1` with `IVF_CLUSTERS=256`.
- For 3,000,000 vectors this creates lists averaging about 11,719 vectors.
- A larger `nlist` can reduce candidates per probe while improving coarse assignment quality.
- Detection errors matter more than shaving latency after p99 is already under the scoring knee.

## Decisions For This Repo

- Keep UDS + HAProxy and fixed response bodies.
- Replace the old `f16` index with clean-room `ivfi16`: `int16` vectors, SoA layout, labels, original ids, centroids and bbox.
- Support `nlist` variants through the builder: `4096`, `8192`, `16384`.
- Default to `K=8192`, `BASE_NPROBE=24`, `AMBIG_NPROBE=48`, `BBOX_MODE=ambiguous-only`.
- Keep exact fallback available but disabled by default until offline fallback rate is proven below `0.3%`.
- Keep `io_uring` out of scope until index/search quality beats the previous baseline.
