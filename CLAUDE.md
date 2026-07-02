# zstreamer Project Rules

## Working Style

Behavioral guidelines to reduce common LLM coding mistakes (adapted from
[andrej-karpathy-skills](https://github.com/multica-ai/andrej-karpathy-skills)).
They bias toward caution over speed — for trivial tasks, use judgment.

### Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

### Surgical Changes

**Touch only what you must. Clean up only your own mess.**

- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it — don't delete it.
- Remove imports/variables/functions that YOUR changes made unused; leave
  pre-existing dead code alone unless asked.

The test: every changed line should trace directly to the user's request.

### Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan where each step has a verify check.

## Architecture

Node type hierarchy — all extend `node.h` base config/data:
- **source** (`source.h`): active producer, start/stop lifecycle
- **sink** (`sink.h`): terminal consumer, unrefs buffers
- **filter** (`filter.h`): conditional routing via children + false-children
- **processor**: uses `node.h` directly, no extra header

Source drivers MUST use `zstreamer_source_common_init` (not `zstreamer_node_common_init`) — inits semaphores before thread creation.

Every driver must call `ZSTREAMER_*_DT_INST_PRE_DEFINE(inst)` BEFORE defining data/config structs.

## Naming

**Family_Action** everywhere: files, structs, functions, macros, DTS compatibles.
Examples: `numgen_src`, `count_sink`, `odd_filter`, `spi_src`, `zstreamer,numgen-src`.

## DTS

- Nodes live inside a `streaming-graph` container (no `#address-cells`/`reg`)
- Peripherals referenced via phandles; don't duplicate device config in zstnode DTS

## Tests

```sh
# Run all test suites via twister
west twister -T tests -p native_sim --inline-logs

# Or build/run a single suite manually
west build -b native_sim -d build/test-<name> tests/subsys/<name> -p auto
timeout 120s ./build/test-<name>/zephyr/zephyr.exe
```

## Design Rules

- Start/stop contract: first `start()` → 0, second → `-EALREADY`; same for `stop()`
- Thread stack size is fixed at `ZSTREAMER_THREAD_STACK_SIZE` (2048) in `node.h`. Do NOT increase it — if a driver needs extra memory, allocate it in the driver's data struct, not on the stack.
- Use public `net_buf` APIs only

## Environment

- Linux, no Docker. Board: `native_sim`
- No parallel `west build` against same Zephyr tree
- native_sim: simulated time advances only when ALL threads blocked — use `k_sleep`/`k_yield` in loops

## Commit Messages

Zephyr-style with area prefixes: `drivers: dac: fix value masking`, `subsys: node: add metadata`, `tests: dac: add multi-channel test`, `samples: sine2pwm: update overlay`, `ci: add clang-format`

## Issue Tracking

This project uses **bd (beads)** for ALL issue tracking. Do NOT use markdown TODOs, task lists, or other tracking methods.

- Check ready work: `bd ready --json`
- Create issues: `bd create "Title" --description="Details" -t feature -p 2 --json`
- Use stdin for descriptions with special characters: `echo 'text' | bd create "Title" --description=- --json`
- Claim work: `bd update <id> --claim --json`
- Close work: `bd close <id> --reason "Done" --json`
- Link discovered work: `--deps discovered-from:<parent-id>`
- Do NOT use `bd edit` (interactive editor) — use `bd update` with flags instead
- Always use `--json` flag for programmatic use
- Each issue gets its own branch and PR into `main`
