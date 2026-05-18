/**
 * GLM (Zhipu AI) provider for GenDB.
 * Uses OpenAI-compatible API with a custom tool-use loop.
 *
 * GLM is a chat completion model, not a coding agent SDK, so we implement
 * the tool execution loop ourselves: send prompt → parse tool_calls →
 * execute tools locally → send results back → repeat until done.
 *
 * Requires: GLM_API_KEY or ZHIPUAI_API_KEY environment variable.
 * Uses the `openai` npm package with custom baseURL.
 */

import OpenAI from "openai";
import { readFile, writeFile, mkdir } from "fs/promises";
import { execSync } from "child_process";
import { existsSync } from "fs";
import { dirname, resolve } from "path";
import { defaults, getProviderConfig } from "../gendb.config.mjs";
import { formatDuration } from "../shared.mjs";

const GLM_BASE_URL = "https://open.bigmodel.cn/api/paas/v4";

function getClient() {
  const apiKey = process.env.GLM_API_KEY || process.env.ZHIPUAI_API_KEY;
  if (!apiKey) {
    throw new Error("GLM API key not set. Set GLM_API_KEY or ZHIPUAI_API_KEY environment variable.");
  }
  return new OpenAI({ apiKey, baseURL: GLM_BASE_URL });
}

const TOOL_DEFINITIONS = [
  {
    type: "function",
    function: {
      name: "Read",
      description: "Read a file from the filesystem. Returns file contents with line numbers.",
      parameters: {
        type: "object",
        properties: {
          file_path: { type: "string", description: "Absolute path to the file" },
          offset: { type: "integer", description: "Line number to start reading from (0-based)" },
          limit: { type: "integer", description: "Number of lines to read" },
        },
        required: ["file_path"],
      },
    },
  },
  {
    type: "function",
    function: {
      name: "Write",
      description: "Write content to a file (creates parent directories if needed).",
      parameters: {
        type: "object",
        properties: {
          file_path: { type: "string", description: "Absolute path to write to" },
          content: { type: "string", description: "Content to write" },
        },
        required: ["file_path", "content"],
      },
    },
  },
  {
    type: "function",
    function: {
      name: "Edit",
      description: "Replace exact text in a file. old_string must match exactly.",
      parameters: {
        type: "object",
        properties: {
          file_path: { type: "string", description: "Absolute path to the file" },
          old_string: { type: "string", description: "Exact text to find and replace" },
          new_string: { type: "string", description: "Replacement text" },
        },
        required: ["file_path", "old_string", "new_string"],
      },
    },
  },
  {
    type: "function",
    function: {
      name: "Bash",
      description: "Execute a bash command and return its output.",
      parameters: {
        type: "object",
        properties: {
          command: { type: "string", description: "The bash command to execute" },
          timeout: { type: "integer", description: "Timeout in milliseconds (default: 120000)" },
        },
        required: ["command"],
      },
    },
  },
  {
    type: "function",
    function: {
      name: "Grep",
      description: "Search for a pattern in files. Returns matching lines with file paths and line numbers.",
      parameters: {
        type: "object",
        properties: {
          pattern: { type: "string", description: "Search pattern (regex)" },
          path: { type: "string", description: "File or directory to search in" },
          include: { type: "string", description: "File pattern to include (e.g., '*.cpp')" },
        },
        required: ["pattern", "path"],
      },
    },
  },
  {
    type: "function",
    function: {
      name: "Glob",
      description: "Find files matching a glob pattern.",
      parameters: {
        type: "object",
        properties: {
          pattern: { type: "string", description: "Glob pattern (e.g., 'src/**/*.cpp')" },
          path: { type: "string", description: "Base directory to search from" },
        },
        required: ["pattern"],
      },
    },
  },
];

async function executeTool(name, args, cwd) {
  try {
    switch (name) {
      case "Read": {
        const content = await readFile(args.file_path, "utf-8");
        const lines = content.split("\n");
        const start = args.offset || 0;
        const end = args.limit ? start + args.limit : lines.length;
        const slice = lines.slice(start, end);
        return slice.map((l, i) => `${start + i + 1}\t${l}`).join("\n");
      }

      case "Write": {
        await mkdir(dirname(args.file_path), { recursive: true });
        await writeFile(args.file_path, args.content, "utf-8");
        return `File written: ${args.file_path} (${args.content.length} bytes)`;
      }

      case "Edit": {
        const fileContent = await readFile(args.file_path, "utf-8");
        if (!fileContent.includes(args.old_string)) {
          return `Error: old_string not found in ${args.file_path}`;
        }
        const count = fileContent.split(args.old_string).length - 1;
        if (count > 1) {
          return `Error: old_string found ${count} times in ${args.file_path}. Must be unique.`;
        }
        const newContent = fileContent.replace(args.old_string, args.new_string);
        await writeFile(args.file_path, newContent, "utf-8");
        return `File edited: ${args.file_path}`;
      }

      case "Bash": {
        const timeout = args.timeout || 120000;
        const output = execSync(args.command, {
          cwd,
          timeout,
          maxBuffer: 10 * 1024 * 1024,
          encoding: "utf-8",
          stdio: ["pipe", "pipe", "pipe"],
        });
        return output.slice(0, 50000);
      }

      case "Grep": {
        const includeFlag = args.include ? `--include="${args.include}"` : "";
        const cmd = `grep -rn ${includeFlag} "${args.pattern}" "${args.path}" 2>/dev/null | head -200`;
        const output = execSync(cmd, { cwd, encoding: "utf-8", timeout: 30000, stdio: ["pipe", "pipe", "pipe"] });
        return output || "(no matches)";
      }

      case "Glob": {
        const basePath = args.path || cwd;
        const cmd = `find "${basePath}" -path "${args.pattern}" 2>/dev/null | head -200`;
        const output = execSync(cmd, { cwd, encoding: "utf-8", timeout: 30000, stdio: ["pipe", "pipe", "pipe"] });
        return output || "(no matches)";
      }

      default:
        return `Unknown tool: ${name}`;
    }
  } catch (err) {
    return `Error executing ${name}: ${err.message}`;
  }
}

function filterTools(allowedTools) {
  if (!allowedTools || allowedTools.length === 0) return TOOL_DEFINITIONS;
  const allowed = new Set(allowedTools.filter(t => t !== "Skill"));
  return TOOL_DEFINITIONS.filter(t => allowed.has(t.function.name));
}

export async function runAgent(name, { systemPrompt, userPrompt, allowedTools, model, cwd, timeoutMs, configName, useSkills, domainSkillsPrompt, effortLevel: effortOverride, verbose = false }) {
  const effectivePrompt = (useSkills !== false && domainSkillsPrompt)
    ? systemPrompt + "\n\n" + domainSkillsPrompt
    : systemPrompt;

  const timeout = timeoutMs || defaults.agentTimeoutMs;
  const providerCfg = getProviderConfig("glm");
  const effectiveModel = model || providerCfg.model;
  const maxIter = providerCfg.maxToolIterations || 80;

  console.log(`\n[${"=".repeat(60)}]`);
  console.log(`[Orchestrator] Spawning agent: ${name} (provider: glm, model: ${effectiveModel}, timeout: ${formatDuration(timeout)})`);
  console.log(`[${"=".repeat(60)}]\n`);

  const startTime = Date.now();
  const client = getClient();
  const tools = filterTools(allowedTools);
  const workingDir = cwd || process.cwd();

  const messages = [
    { role: "system", content: effectivePrompt },
    { role: "user", content: userPrompt },
  ];

  let tokens = { input: 0, output: 0, cache_read: 0, cache_creation: 0 };
  let costUsd = 0;
  let resultText = "";
  let agentError = null;

  try {
    for (let iter = 0; iter < maxIter; iter++) {
      if (Date.now() - startTime > timeout) {
        agentError = `Agent "${name}" timed out after ${formatDuration(timeout)}`;
        break;
      }

      const requestParams = {
        model: effectiveModel,
        messages,
        max_tokens: 16384,
      };

      if (tools.length > 0) {
        requestParams.tools = tools;
        requestParams.tool_choice = "auto";
      }

      let response;
      try {
        response = await client.chat.completions.create(requestParams);
      } catch (apiErr) {
        if (apiErr.status === 429) {
          console.log(`[${name}] Rate limited, waiting 5s...`);
          await new Promise(r => setTimeout(r, 5000));
          continue;
        }
        throw apiErr;
      }

      if (response.usage) {
        tokens.input += response.usage.prompt_tokens || 0;
        tokens.output += response.usage.completion_tokens || 0;
      }

      const choice = response.choices[0];
      const msg = choice.message;

      messages.push(msg);

      if (verbose && msg.content) {
        console.log(`[${name}] ${msg.content.slice(0, 200)}`);
      }

      if (choice.finish_reason === "stop" || !msg.tool_calls || msg.tool_calls.length === 0) {
        resultText = msg.content || "";
        break;
      }

      for (const toolCall of msg.tool_calls) {
        const toolName = toolCall.function.name;
        let toolArgs;
        try {
          toolArgs = JSON.parse(toolCall.function.arguments);
        } catch {
          toolArgs = {};
        }

        if (verbose) {
          console.log(`[${name}] Tool: ${toolName}${toolArgs.command ? ': ' + String(toolArgs.command).slice(0, 120) : toolArgs.file_path ? ': ' + toolArgs.file_path : ''}`);
        }

        const toolResult = await executeTool(toolName, toolArgs, workingDir);

        messages.push({
          role: "tool",
          tool_call_id: toolCall.id,
          content: toolResult,
        });
      }
    }
  } catch (err) {
    agentError = `Agent "${name}" failed: ${err.message}`;
  }

  const durationMs = Date.now() - startTime;
  costUsd = estimateGlmCost(effectiveModel, tokens);

  if (agentError) {
    console.error(`\n[Orchestrator] Agent "${name}" failed (${formatDuration(durationMs)}, ${tokens.input + tokens.output} tokens, $${costUsd.toFixed(2)}): ${agentError}`);
    return { result: resultText, durationMs, tokens, costUsd, error: agentError, skillsUsed: {} };
  }

  console.log(`\n[Orchestrator] Agent "${name}" completed (${formatDuration(durationMs)}, ${tokens.input + tokens.output} tokens, $${costUsd.toFixed(2)})`);
  return { result: resultText, durationMs, tokens, costUsd, skillsUsed: {} };
}

function estimateGlmCost(model, tokens) {
  const GLM_PRICING = {
    "glm-5.1":     { input: 1.00, output: 4.00 },
    "glm-4-plus":  { input: 0.50, output: 2.00 },
    "glm-4":       { input: 0.10, output: 0.10 },
    "glm-4-long":  { input: 0.01, output: 0.01 },
  };
  const pricing = GLM_PRICING[model] || GLM_PRICING["glm-5.1"];
  const perM = 1_000_000;
  return (tokens.input * pricing.input) / perM
    + (tokens.output * pricing.output) / perM;
}
