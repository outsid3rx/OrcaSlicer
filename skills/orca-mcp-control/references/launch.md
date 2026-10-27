# Launch an enabled Orca window

Use this when MCP finds no enabled windows or a required native capability is disabled. Verify the installed app path before launching it.

On macOS, this enables local agent control and print transmission for the new process:

```sh
ORCA_AGENT_CONTROL=1 \
  /Applications/OrcaSlicer.app/Contents/MacOS/OrcaSlicer
```

Ordinary Dock/Finder launch does not set these variables. The environment of an already-running process cannot be changed by setting variables in another terminal. Do not close or restart a user's unsaved project automatically. Explain the need to save/reopen, or launch a separate enabled instance when appropriate. After launching, rediscover windows and identify the intended project by its full UUID pair; do not assume the newest or focused window is the target.

In current builds, agent control enables native transmission and device control. Older builds may additionally require `ORCA_AGENT_PRINTING=1` in the same launch environment; inspect `printers.list` / `transmission_enabled` and avoid restarting an unsaved project. The skill's per-job approval rule still applies.

File imports and exports additionally need native allowed roots. `ORCA_AGENT_INPUT_ROOTS` and `ORCA_AGENT_OUTPUT_ROOTS` take **JSON arrays of absolute directory paths**, not colon-separated paths. For example, if the user chose `/absolute/path/printing-project` as the project directory:

```sh
ORCA_AGENT_CONTROL=1 \
ORCA_AGENT_INPUT_ROOTS='["/absolute/path/printing-project"]' \
ORCA_AGENT_OUTPUT_ROOTS='["/absolute/path/printing-project"]' \
  /Applications/OrcaSlicer.app/Contents/MacOS/OrcaSlicer
```

Use the user's actual directories, and prefer narrower import/export folders when available. Native file commands enforce these roots independently of MCP; do not bypass a rejection with another upload or file path. Never dump the whole process environment while troubleshooting: it may contain credentials.

On Linux and Windows, set the same environment variables for the process that starts the installed Orca executable, using that platform's shell syntax and absolute native paths. Discover the executable location instead of assuming the macOS layout.
