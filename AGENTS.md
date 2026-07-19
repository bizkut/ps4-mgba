# Agent Instructions for mGBA PS4 port

## Git Commit Style

- Do NOT add `Generated with [Devin]` or `Co-Authored-By: Devin` lines to commit messages.
- Keep commit messages focused on the "why" and "what", nothing else.

## MCP / Tooling Preferences

- Do NOT use the `lean-ctx` MCP server or its `ctx_*` tools. It is removed from Devin.
- Use the `serena` MCP server instead for code intelligence, symbol navigation, and project exploration.
  Serena is launched via `uvx --from git+https://github.com/oraios/serena serena start-mcp-server`.
- Prefer native `read`, `grep`, `glob`, and `exec` tools for file/shell operations unless Serena's
  semantic tools (find symbol, references, call graph) are clearly needed.

## Build / Verification

- PS4 packaging workflow lives in `.github/workflows/ps4.yml` and builds via the Docker image at
  `src/platform/ps4/Dockerfile`.
- The `psbc` tool is built with `make` (not cmake) — its `Makefile` + `config.mak` expect
  `OpenGNM/include` (passed as `GNM_INCLUDE`) and `Vulkan-Headers` as sibling directories.
  Do NOT use the legacy `freegnm` repo; the gitgud.io mirror is no longer accessible and the
  project has migrated to OpenGNM.
- After Dockerfile changes, the `PS4 release` GitHub Action is the source of truth for verification.
