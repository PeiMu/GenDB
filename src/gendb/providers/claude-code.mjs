/**
 * Claude Code CLI provider for GenDB.
 * Spawns `claude -p` subprocesses to act as each agent.
 * Uses the user's existing Claude Code subscription (OAuth) — no ANTHROPIC_API_KEY needed.
 */

import { spawn } from "child_process";
import { writeFile, unlink, mkdtemp } from "fs/promises";
import { tmpdir } from "os";
import { resolve, dirname } from "path";
import { fileURLToPath } from "url";
import { defaults, getProviderConfig } from "../gendb.config.mjs";
import { formatDuration } from "../shared.mjs";

const __filename = fileURLToPath(import.meta.url);
const REPO_ROOT = resolve(dirname(__filename), "../../..");

export async function runAgent(name, { systemPrompt, userPrompt, allowedTools, model, cwd, timeoutMs, configName, useSkills, domainSkillsPrompt, effortLevel: effortOverride, verbose = false }) {
  const effectivePrompt = (useSkills !== false && domainSkillsPrompt)
    ? systemPrompt + "\n\n" + domainSkillsPrompt
    : systemPrompt;

  const timeout = timeoutMs || defaults.agentTimeoutMs;
  const providerCfg = getProviderConfig("claude-code");
  const effortLevel = effortOverride || (configName && providerCfg.agentEffortLevels[configName]);

  console.log(`\n[${"=".repeat(60)}]`);
  console.log(`[Orchestrator] Spawning agent: ${name} (provider: claude-code, timeout: ${formatDuration(timeout)}${effortLevel ? `, effort: ${effortLevel}` : ''})`);
  console.log(`[${"=".repeat(60)}]\n`);

  const startTime = Date.now();
  let tmpSpFile = null;
  let tmpUpFile = null;

  try {
    const tmpDir = await mkdtemp(resolve(tmpdir(), "gendb-"));
    tmpSpFile = resolve(tmpDir, "system_prompt.txt");
    tmpUpFile = resolve(tmpDir, "user_prompt.txt");

    await writeFile(tmpSpFile, effectivePrompt, "utf-8");
    await writeFile(tmpUpFile, userPrompt, "utf-8");

    const effectiveModel = model || providerCfg.model;
    const effectiveTools = useSkills === false
      ? allowedTools.filter(t => t !== "Skill")
      : allowedTools;

    const args = [
      "-p",
      "--output-format", "json",
      "--dangerously-skip-permissions",
      "--model", effectiveModel,
      "--allowedTools", effectiveTools.join(","),
      "--add-dir", REPO_ROOT,
    ];

    if (effortLevel) {
      args.push("--effort", effortLevel);
    }

    const result = await spawnClaude(args, tmpSpFile, tmpUpFile, cwd || process.cwd(), timeout, verbose, name);

    const durationMs = Date.now() - startTime;

    if (result.error) {
      console.error(`\n[Orchestrator] Agent "${name}" failed (${formatDuration(durationMs)}): ${result.error}`);
      return { result: result.text || "", durationMs, tokens: result.tokens, costUsd: result.costUsd, error: result.error, skillsUsed: {} };
    }

    console.log(`\n[Orchestrator] Agent "${name}" completed (${formatDuration(durationMs)}, ${result.tokens.input + result.tokens.output} tokens, $${result.costUsd.toFixed(2)})`);
    return { result: result.text, durationMs, tokens: result.tokens, costUsd: result.costUsd, skillsUsed: {} };

  } finally {
    try { if (tmpSpFile) await unlink(tmpSpFile); } catch {}
    try { if (tmpUpFile) await unlink(tmpUpFile); } catch {}
  }
}

function spawnClaude(args, spFile, upFile, cwd, timeout, verbose, agentName) {
  return new Promise((resolve) => {
    const shellCmd = `cat "${upFile}" | claude ${args.map(a => `"${a}"`).join(" ")} --system-prompt "$(cat '${spFile}')"`;

    const proc = spawn("bash", ["-c", shellCmd], {
      cwd,
      env: { ...process.env },
      stdio: ["pipe", "pipe", "pipe"],
    });

    let stdout = "";
    let stderr = "";

    proc.stdout.on("data", (chunk) => {
      stdout += chunk.toString();
      if (verbose) {
        const lines = chunk.toString().split("\n").filter(l => l.trim());
        for (const line of lines) {
          console.log(`[${agentName}] ${line.slice(0, 200)}`);
        }
      }
    });

    proc.stderr.on("data", (chunk) => {
      stderr += chunk.toString();
    });

    let timedOut = false;
    const timer = setTimeout(() => {
      timedOut = true;
      console.error(`\n[Orchestrator] Agent "${agentName}" timed out after ${formatDuration(timeout)}, killing...`);
      proc.kill("SIGTERM");
      setTimeout(() => proc.kill("SIGKILL"), 5000);
    }, timeout);

    proc.on("close", (code) => {
      clearTimeout(timer);

      const tokens = { input: 0, output: 0, cache_read: 0, cache_creation: 0 };
      let costUsd = 0;
      let text = "";
      let error = null;

      if (timedOut) {
        error = `Agent "${agentName}" timed out after ${formatDuration(timeout)}`;
        resolve({ text, tokens, costUsd, error });
        return;
      }

      if (code !== 0 && !stdout.trim()) {
        error = `claude CLI exited with code ${code}: ${stderr.slice(0, 500)}`;
        resolve({ text, tokens, costUsd, error });
        return;
      }

      try {
        const output = JSON.parse(stdout.trim());
        text = output.result || "";
        costUsd = output.total_cost_usd || 0;
        if (output.usage) {
          tokens.input = output.usage.input_tokens || 0;
          tokens.output = output.usage.output_tokens || 0;
          tokens.cache_read = output.usage.cache_read_input_tokens || 0;
          tokens.cache_creation = output.usage.cache_creation_input_tokens || 0;
        }
        if (output.subtype !== "success") {
          error = output.errors?.join("; ") || output.subtype || "unknown error";
        }
      } catch (parseErr) {
        text = stdout;
        if (code !== 0) {
          error = `Failed to parse claude output: ${parseErr.message}`;
        }
      }

      resolve({ text, tokens, costUsd, error });
    });
  });
}
