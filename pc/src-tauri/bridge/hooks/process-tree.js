"use strict";

const { execSync } = require("child_process");
const path = require("path");

const DEFAULT_TERMINAL_NAMES = {
  win32: [
    "windowsterminal.exe",
    "cmd.exe",
    "powershell.exe",
    "pwsh.exe",
    "code.exe",
    "cursor.exe",
    "codebuddy.exe",
    "alacritty.exe",
    "wezterm-gui.exe",
    "mintty.exe",
    "conemu64.exe",
    "conemu.exe",
    "hyper.exe",
    "tabby.exe",
    "antigravity.exe",
    "warp.exe",
    "iterm.exe",
    "ghostty.exe",
  ],
  darwin: [
    "terminal",
    "iterm2",
    "alacritty",
    "wezterm-gui",
    "kitty",
    "hyper",
    "tabby",
    "warp",
    "ghostty",
  ],
  linux: [
    "gnome-terminal",
    "kgx",
    "konsole",
    "xfce4-terminal",
    "tilix",
    "alacritty",
    "wezterm",
    "wezterm-gui",
    "kitty",
    "ghostty",
    "xterm",
    "lxterminal",
    "terminator",
    "tabby",
    "hyper",
    "warp",
  ],
};

const DEFAULT_SYSTEM_BOUNDARY = {
  win32: ["explorer.exe", "services.exe", "winlogon.exe", "svchost.exe"],
  darwin: ["launchd", "init", "systemd"],
  linux: ["systemd", "init"],
};

const DEFAULT_EDITOR_MAP = {
  win32: {
    "code.exe": "code",
    "cursor.exe": "cursor",
    "codebuddy.exe": "codebuddy",
  },
  darwin: {
    code: "code",
    cursor: "cursor",
    codebuddy: "codebuddy",
  },
  linux: {
    code: "code",
    cursor: "cursor",
    "code-insiders": "code",
    codebuddy: "codebuddy",
  },
};

const DEFAULT_COMM_EDITOR_HINTS = [
  { needle: "codebuddy", editor: "codebuddy" },
  { needle: "visual studio code", editor: "code" },
  { needle: "cursor.app", editor: "cursor" },
];

let _hasWmic = null;
let _windowsProcessSnapshot = null;

function hasWmic() {
  if (_hasWmic !== null) return _hasWmic;
  try {
    execSync("where wmic", {
      stdio: "ignore",
      windowsHide: true,
      timeout: 1000,
    });
    _hasWmic = true;
  } catch {
    _hasWmic = false;
  }
  return _hasWmic;
}

function loadWindowsProcessSnapshot() {
  if (_windowsProcessSnapshot) return _windowsProcessSnapshot;
  const snapshot = new Map();
  try {
    const json = execSync(
      "powershell -NoProfile -Command \"Get-CimInstance Win32_Process | Select-Object ProcessId,ParentProcessId,Name,CommandLine | ConvertTo-Json -Compress\"",
      { encoding: "utf8", timeout: 3000, windowsHide: true, maxBuffer: 16 * 1024 * 1024 }
    ).trim();
    if (!json) {
      _windowsProcessSnapshot = snapshot;
      return snapshot;
    }
    let parsed = JSON.parse(json);
    if (!Array.isArray(parsed)) parsed = [parsed];
    for (const item of parsed) {
      const pid = Number(item && item.ProcessId);
      if (!Number.isFinite(pid)) continue;
      snapshot.set(pid, {
        pid,
        name: String((item && item.Name) || "").trim().toLowerCase(),
        parentPid: Number(item && item.ParentProcessId),
        commandLine: String((item && item.CommandLine) || ""),
      });
    }
  } catch {}
  _windowsProcessSnapshot = snapshot;
  return snapshot;
}

function normalizeNameSet(values) {
  return new Set(
    (Array.isArray(values) ? values : [])
      .map((value) => String(value || "").trim().toLowerCase())
      .filter(Boolean)
  );
}

function detectEditorFromCommPath(commPath, hints) {
  const lower = String(commPath || "").toLowerCase();
  for (const hint of hints) {
    if (hint && hint.needle && lower.includes(String(hint.needle).toLowerCase())) {
      return hint.editor || null;
    }
  }
  return null;
}

function readProcessInfo(pid, isWin) {
  if (isWin) {
    if (hasWmic()) {
      const out = execSync(
        `wmic process where "ProcessId=${pid}" get Name,ParentProcessId /format:csv`,
        { encoding: "utf8", timeout: 1500, windowsHide: true }
      );
      const lines = out.trim().split("\n").filter((line) => line.includes(","));
      if (!lines.length) return null;
      const parts = lines[lines.length - 1].split(",");
      const name = (parts[1] || "").trim().toLowerCase();
      const parentPid = parseInt(parts[2], 10);
      if (!name || Number.isNaN(parentPid)) return null;
      return { name, parentPid, commandPath: "" };
    }

    const snapshot = loadWindowsProcessSnapshot();
    const entry = snapshot.get(pid);
    if (!entry) return null;
    const name = entry.name;
    const parentPid = Number(entry.parentPid);
    if (!name || Number.isNaN(parentPid)) return null;
    return { name, parentPid, commandPath: "" };
  }

  const ppidOut = execSync(`ps -o ppid= -p ${pid}`, {
    encoding: "utf8",
    timeout: 1000,
  }).trim();
  const commOut = execSync(`ps -o comm= -p ${pid}`, {
    encoding: "utf8",
    timeout: 1000,
  }).trim();
  const name = path.basename(commOut).toLowerCase();
  const parentPid = parseInt(ppidOut, 10);
  if (!name || Number.isNaN(parentPid)) return null;
  return { name, parentPid, commandPath: commOut };
}

function readProcessCommandLine(pid, isWin) {
  if (isWin) {
    if (hasWmic()) {
      return execSync(
        `wmic process where "ProcessId=${pid}" get CommandLine /format:csv`,
        { encoding: "utf8", timeout: 500, windowsHide: true }
      );
    }
    const snapshot = loadWindowsProcessSnapshot();
    const entry = snapshot.get(pid);
    return entry ? String(entry.commandLine || "") : "";
  }
  return execSync(`ps -o command= -p ${pid}`, {
    encoding: "utf8",
    timeout: 500,
  });
}

function resolveStableProcessContext(options = {}) {
  const platform = options.platform || process.platform;
  const isWin = platform === "win32";
  const basePlatform = platform === "linux" ? "linux" : (isWin ? "win32" : "darwin");

  const terminalNames = normalizeNameSet(
    (options.terminalNames || DEFAULT_TERMINAL_NAMES[basePlatform] || []).concat(
      options.terminalNameExtras || []
    )
  );
  const systemBoundary = normalizeNameSet(
    (options.systemBoundary || DEFAULT_SYSTEM_BOUNDARY[basePlatform] || []).concat(
      options.systemBoundaryExtras || []
    )
  );
  const editorMap = {
    ...(DEFAULT_EDITOR_MAP[basePlatform] || {}),
    ...(options.editorMap || {}),
  };
  const commEditorHints = Array.isArray(options.commEditorHints)
    ? options.commEditorHints
    : DEFAULT_COMM_EDITOR_HINTS;

  const agentNames = normalizeNameSet(options.agentNames || []);
  const agentCommandMarkers = (options.agentCommandMarkers || [])
    .map((value) => String(value || "").toLowerCase())
    .filter(Boolean);
  const nodeNames = normalizeNameSet(
    options.nodeProcessNames || ["node.exe", "node"]
  );

  const maxDepth = Number.isInteger(options.maxDepth) ? options.maxDepth : 8;
  let pid = Number.isInteger(options.startPid) ? options.startPid : process.ppid;
  let lastGoodPid = pid;
  let terminalPid = null;
  let detectedEditor = null;
  let agentPid = null;
  let pidChain = [];

  for (let i = 0; i < maxDepth; i++) {
    let info;
    try {
      info = readProcessInfo(pid, isWin);
    } catch {
      break;
    }
    if (!info) break;

    const { name, parentPid, commandPath } = info;
    pidChain.push(pid);

    if (!detectedEditor) {
      detectedEditor = editorMap[name] || detectEditorFromCommPath(commandPath, commEditorHints);
    }

    if (!agentPid) {
      if (agentNames.has(name)) {
        agentPid = pid;
      } else if (agentCommandMarkers.length && nodeNames.has(name)) {
        try {
          const commandLine = String(readProcessCommandLine(pid, isWin)).toLowerCase();
          if (agentCommandMarkers.some((marker) => commandLine.includes(marker))) {
            agentPid = pid;
          }
        } catch {}
      }
    }

    if (systemBoundary.has(name)) break;
    if (terminalNames.has(name)) terminalPid = pid;
    lastGoodPid = pid;

    if (!parentPid || parentPid === pid || parentPid <= 1) break;
    pid = parentPid;
  }

  let isHeadless = false;
  const headlessPattern = options.headlessPattern;
  if (agentPid && headlessPattern instanceof RegExp) {
    try {
      const commandLine = String(readProcessCommandLine(agentPid, isWin));
      isHeadless = headlessPattern.test(commandLine);
    } catch {}
  }

  return {
    stablePid: terminalPid || lastGoodPid,
    editor: detectedEditor || null,
    agentPid: agentPid || null,
    pidChain,
    isHeadless,
  };
}

module.exports = {
  resolveStableProcessContext,
};
