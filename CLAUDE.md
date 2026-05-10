# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test Commands

```sh
make              # Build ds4 CLI + ds4-server
make ds4          # Build CLI only
make ds4-server   # Build server only
make ds4_native   # Build CPU-only CLI (no Metal, for debug)
make test         # Build and run ds4_test --all
make clean        # Remove binaries and object files
```

Individual test flags (passed to `ds4_test`): `--metal-kernels`, `--logprob-vectors`, `--server`, `--long-context`, `--tool-call-quality`

Test environment: `DS4_TEST_MODEL` (default `ds4flash.gguf`), `DS4_TEST_LONG_PROMPT`, `DS4_TEST_VECTOR_FILE`

Test vectors in `tests/test-vectors/`, refreshed via `fetch_official_vectors.py`.

## Architecture

Inference engine for **DeepSeek V4 Flash only** — not a generic GGUF runner. Single C file (`ds4.c`, ~12k lines) with Objective-C Metal runtime (`ds4_metal.m`) and 19 Metal compute kernels (`metal/*.metal`).

**Engine layers (ds4.c):**
- GGUF loader — mmap-based, validates against a single fixed model shape
- DeepSeek BPE tokenizer with DSML chat template rendering
- CPU reference kernels (debug only — known macOS kernel VM bug with large mappings)
- Metal graph driver — schedules entire model as a compute graph; implements MLA (Multi-head Latent Attention) with compressed KV cache (ratio-4 indexer), Hyper-Connection with sinkhorn routing, MoE with 256 experts (6 active + 1 shared), FP8 KV cache
- Session layer — `ds4_session` owns one mutable timeline with `eval()`, `sample()`, `argmax()`, `rewind()`, token prefix matching for KV reuse
- Disk KV cache — serializes sessions to disk keyed by SHA1 of token IDs

**Frontends:**
- `ds4_cli.c` — interactive REPL (linenoise), `/think`, `/think-max`, `/nothink`, `/ctx`, `/read`, `/quit`
- `ds4_server.c` — HTTP server with OpenAI `/v1/chat/completions`, `/v1/completions` + Anthropic `/v1/messages`; SSE streaming, tool calling, thinking mode

**Model constants:** 43 layers, 4096 emb dim, 129280 vocab, 64 Q heads / 1 KV head, 512 head dim, 256 experts (6 active), SWA 128, HC factor 4.

## Key Rules

- **No C++** — strict C99 + Objective-C where Metal requires it
- No external dependencies beyond system libraries (`-lm -pthread -framework Foundation -framework Metal`)
- No permanent semantic flags — diagnostic switches are fine, but no permanent variant flags
- Avoid large CPU inference runs on macOS (kernel VM issue)
- Keep public APIs narrow — CLI/server code must not know tensor internals
- Prefer short Metal smoke tests for build verification
- Model loading must remain mmap-backed, no eager copying of the full GGUF
- Comments should explain *why* (shape choices, cache boundaries, memory policy), not *what*
