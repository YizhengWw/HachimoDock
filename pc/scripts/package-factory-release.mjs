/**
 * [Input] A verified ESP32-P4 factory build directory and reviewed distribution notices.
 * [Output] A source-free, no-key ZIP containing the complete factory image, binary segments, resource catalogs, and guided macOS/Windows flashing tools.
 * [Pos] Private firmware-build to public factory-flasher package boundary.
 * [Sync] If this file changes, update `scripts/.folder.md` and `firmware/BUILD.md`.
 */

import { createHash } from "node:crypto";
import {
  chmodSync,
  copyFileSync,
  cpSync,
  createReadStream,
  existsSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  statSync,
  writeFileSync,
} from "node:fs";
import { basename, dirname, join, relative, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const scriptDir = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(scriptDir, "../..");
const FACTORY_FORMAT = "pet-manager-p4-factory-v1";

function usage() {
  return `Usage:
  node scripts/package-factory-release.mjs \\
    --build-dir <esp32-p4-build-directory> \\
    --output <empty-directory> \\
    [--license <license-file>] \\
    [--notice <third-party-notices-file>]`;
}

function parseArgs(argv) {
  const result = {};
  for (let index = 0; index < argv.length; index += 1) {
    const token = argv[index];
    if (token === "--help" || token === "-h") result.help = true;
    else if (token.startsWith("--")) {
      const value = argv[index + 1];
      if (!value || value.startsWith("--")) throw new Error(`missing value for ${token}`);
      result[token.slice(2)] = value;
      index += 1;
    } else {
      throw new Error(`unexpected argument: ${token}`);
    }
  }
  return result;
}

function requireFile(path, label) {
  if (!existsSync(path) || !statSync(path).isFile() || statSync(path).size === 0) {
    throw new Error(`${label} is missing or empty: ${path}`);
  }
}

function sha256File(path) {
  return new Promise((resolveHash, reject) => {
    const digest = createHash("sha256");
    const stream = createReadStream(path);
    stream.on("data", (chunk) => digest.update(chunk));
    stream.on("error", reject);
    stream.on("end", () => resolveHash(digest.digest("hex")));
  });
}

function listFiles(root) {
  const files = [];
  function visit(directory) {
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
      const path = join(directory, entry.name);
      if (entry.isDirectory()) visit(path);
      else if (entry.isFile()) files.push(path);
    }
  }
  visit(root);
  return files.sort((left, right) => relative(root, left).localeCompare(relative(root, right)));
}

function sanitizeVersion(value) {
  return value.replace(/[^0-9A-Za-z._-]+/g, "-");
}

function macFlashScript(imageName) {
  return `#!/bin/zsh
set -euo pipefail

script_dir="\${0:A:h}"
package_root="\${script_dir:h}"
image="\${package_root}/firmware/${imageName}"
venv="\${package_root}/.esptool-venv"

if ! command -v python3 >/dev/null 2>&1; then
  echo "未找到 Python 3.10+。请先从 https://www.python.org/downloads/ 安装。"
  exit 1
fi

if [[ ! -x "\${venv}/bin/python" ]]; then
  echo "首次运行：正在创建本地 esptool 环境……"
  python3 -m venv "\${venv}"
fi
if ! "\${venv}/bin/python" -c 'import importlib.util, sys; sys.exit(0 if importlib.util.find_spec("esptool") and __import__("esptool").__version__ == "5.4.0" else 1)' >/dev/null 2>&1; then
  "\${venv}/bin/python" -m pip install "esptool==5.4.0"
fi

echo "检测到的候选串口："
setopt null_glob
ports=(/dev/cu.usbserial* /dev/cu.wchusbserial* /dev/cu.usbmodem*)
for candidate in \${ports[@]}; do
  echo "  \${candidate}"
done
read "port?请输入设备串口（例如 /dev/cu.wchusbserialXXXX）："
if [[ -z "\${port}" ]]; then
  echo "未输入串口，已取消。"
  exit 1
fi

echo "警告：将擦除设备全部设置、形象和组件，并写入完整出厂镜像。"
read "confirmation?输入 ERASE 继续："
if [[ "\${confirmation}" != "ERASE" ]]; then
  echo "确认不匹配，已取消。"
  exit 1
fi

"\${venv}/bin/python" "\${script_dir}/flash_checked.py" --port "\${port}" --baud 921600 --image "\${image}"
echo "烧录与校验完成。请松开 BOOT，并按一次 RESET/EN 或重新上电。"
`;
}

function windowsFlashScript(imageName) {
  return `param(
    [string]$Port = ""
)

$ErrorActionPreference = "Stop"
$PackageRoot = Split-Path -Parent $PSScriptRoot
$Image = Join-Path $PackageRoot "firmware\\${imageName}"
$Venv = Join-Path $PackageRoot ".esptool-venv"
$VenvPython = Join-Path $Venv "Scripts\\python.exe"

function Invoke-Python([string[]]$Arguments) {
    if (Get-Command py -ErrorAction SilentlyContinue) {
        & py -3 @Arguments
    } elseif (Get-Command python -ErrorAction SilentlyContinue) {
        & python @Arguments
    } else {
        throw "未找到 Python 3.10+。请先从 https://www.python.org/downloads/ 安装。"
    }
    if ($LASTEXITCODE -ne 0) { throw "Python 命令执行失败。" }
}

if (-not (Test-Path -LiteralPath $VenvPython)) {
    Write-Host "首次运行：正在创建本地 esptool 环境……"
    Invoke-Python @("-m", "venv", $Venv)
}
& $VenvPython -c 'import importlib.util, sys; sys.exit(0 if importlib.util.find_spec("esptool") and __import__("esptool").__version__ == "5.4.0" else 1)'
if ($LASTEXITCODE -ne 0) {
    & $VenvPython -m pip install "esptool==5.4.0"
    if ($LASTEXITCODE -ne 0) { throw "esptool 安装失败。" }
}

if (-not $Port.Trim()) {
    Write-Host "检测到的串口："
    Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
        Select-Object DeviceID, Name |
        Format-Table -AutoSize
    $Port = Read-Host "请输入设备串口（例如 COM5）"
}
if (-not $Port.Trim()) { throw "未输入串口，已取消。" }

Write-Warning "将擦除设备全部设置、形象和组件，并写入完整出厂镜像。"
$Confirmation = Read-Host "输入 ERASE 继续"
if ($Confirmation -cne "ERASE") { throw "确认不匹配，已取消。" }

& $VenvPython (Join-Path $PSScriptRoot "flash_checked.py") --port $Port --baud 921600 --image $Image
if ($LASTEXITCODE -ne 0) { throw "烧录未完成。请检查芯片版本和上方错误，勿强制忽略检查。" }
Write-Host "烧录与校验完成。请松开 BOOT，并按一次 RESET/EN 或重新上电。"
`;
}

const args = parseArgs(process.argv.slice(2));
if (args.help) {
  console.log(usage());
  process.exit(0);
}
if (!args["build-dir"] || !args.output) throw new Error(usage());

const buildDir = resolve(args["build-dir"]);
const outputRoot = resolve(args.output);
const license = resolve(args.license || join(repositoryRoot, "LICENSE"));
const notice = resolve(args.notice || join(repositoryRoot, "THIRD_PARTY_NOTICES.md"));
const manifestPath = join(buildDir, "pet-manager-p4-factory.json");
const factoryPath = join(buildDir, "pet-manager-p4-factory.bin");
const resourceTree = join(buildDir, "factory-spiffs");
for (const [path, label] of [
  [manifestPath, "factory manifest"],
  [factoryPath, "factory image"],
  [license, "distribution license"],
  [notice, "third-party notices"],
]) requireFile(path, label);
if (!existsSync(resourceTree) || !statSync(resourceTree).isDirectory()) {
  throw new Error(`expanded factory resource tree is missing: ${resourceTree}`);
}
if (existsSync(outputRoot) && readdirSync(outputRoot).length > 0) {
  throw new Error(`output directory must be empty: ${outputRoot}`);
}

const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
if (manifest.format !== FACTORY_FORMAT || manifest.completeInstall !== true) {
  throw new Error(`manifest is not a complete ${FACTORY_FORMAT} image`);
}
if (manifest.chip !== "esp32p4" || manifest.flashOffset !== "0x0") {
  throw new Error("factory package must target ESP32-P4 from flash offset 0x0");
}
if (manifest.bytes !== statSync(factoryPath).size || manifest.sha256 !== await sha256File(factoryPath)) {
  throw new Error("factory image does not match its manifest");
}
for (const segment of manifest.segments || []) {
  const path = join(buildDir, basename(segment.path));
  requireFile(path, `factory segment ${segment.path}`);
  if (segment.bytes !== statSync(path).size || segment.sha256 !== await sha256File(path)) {
    throw new Error(`factory segment does not match its manifest: ${segment.path}`);
  }
}

const firmwareVersion = sanitizeVersion(String(manifest.version));
const chipFamily = manifest.chipFamily;
const expectedBounds = chipFamily === "v1" ? [1, 199] : chipFamily === "v3" ? [300, 399] : null;
const factoryBytes = readFileSync(factoryPath);
if (!expectedBounds || manifest.chipRevisionMin !== expectedBounds[0]
  || manifest.chipRevisionMax !== expectedBounds[1]) throw new Error("factory chip revision is missing or unsupported");
for (const offset of [0x2000, 0x10000]) {
  if (factoryBytes[offset] !== 0xe9 || factoryBytes.readUInt16LE(offset + 12) !== 18
    || factoryBytes.readUInt16LE(offset + 15) !== expectedBounds[0]
    || factoryBytes.readUInt16LE(offset + 17) !== expectedBounds[1]) {
    throw new Error("factory bootloader/application chip bounds do not match manifest");
  }
}
const packageName = `HachimoDock-P4-${chipFamily}_${firmwareVersion}_Factory-Flasher`;
const packageRoot = join(outputRoot, packageName);
const firmwareDir = join(packageRoot, "firmware");
const segmentDir = join(firmwareDir, "segments");
const resourceDir = join(packageRoot, "resources");
const toolDir = join(packageRoot, "tools");
mkdirSync(segmentDir, { recursive: true });
mkdirSync(resourceDir, { recursive: true });
mkdirSync(toolDir, { recursive: true });
for (const name of ["flash_checked.py", "p4_image_compat.py"]) {
  copyFileSync(join(repositoryRoot, "firmware", "tools", name), join(toolDir, name));
}

const imageName = `HachimoDock-P4_${firmwareVersion}_factory.bin`;
const manifestName = `HachimoDock-P4_${firmwareVersion}_factory.json`;
copyFileSync(factoryPath, join(firmwareDir, imageName));
writeFileSync(
  join(firmwareDir, manifestName),
  `${JSON.stringify({ ...manifest, image: imageName }, null, 2)}\n`,
);
for (const segment of manifest.segments) {
  const source = join(buildDir, basename(segment.path));
  const offset = String(segment.offset).replace(/^0x/i, "0x");
  copyFileSync(source, join(segmentDir, `${offset}-${basename(segment.path)}`));
}
cpSync(resourceTree, join(resourceDir, "expanded-storage"), { recursive: true });
copyFileSync(license, join(packageRoot, "LICENSE"));
copyFileSync(notice, join(packageRoot, "THIRD_PARTY_NOTICES.md"));
cpSync(join(repositoryRoot, "licenses"), join(packageRoot, "licenses"), { recursive: true });

const macTool = join(toolDir, "flash-macOS.command");
const windowsTool = join(toolDir, "flash-Windows.ps1");
writeFileSync(macTool, macFlashScript(imageName));
writeFileSync(windowsTool, windowsFlashScript(imageName));
chmodSync(macTool, 0o755);

writeFileSync(
  join(resourceDir, "README-CN.md"),
  `# 内置资源说明\n\n- \`expanded-storage/\`：出厂 SPIFFS 的展开内容，含默认形象索引、${manifest.components.count} 个内置组件清单及受限 PNG sprite。\n- \`../firmware/segments/0x520000-default-terrier.spiffs.bin\`：完整存储分区镜像。\n- \`../firmware/segments/0xc00000-default-terrier.appearance0.bin\`：默认西高地 H.264 形象分区镜像。\n- 最终烧录请始终使用 \`../firmware/${imageName}\`，不要逐个写入资源文件。\n`,
);

const readme = `# HachimoDock ESP32-P4 完整出厂烧录包\n\n版本：${manifest.version}\n\n本包不只是应用固件，包含完整 factory.bin、Bootloader、分区表、OTA 元数据、默认西高地形象、${manifest.components.count} 个内置组件、校验清单，以及 Windows/macOS 引导烧录工具。\n\n## 推荐用法\n\n1. 关闭 Pet Manager。\n2. Waveshare ESP32-P4-WIFI6 的 USB 数据路径跳线保持断开，让 Type-C 连接 CH343 USB-UART。\n3. macOS 双击 \`tools/flash-macOS.command\`；Windows 右键 \`tools/flash-Windows.ps1\` 并选择“使用 PowerShell 运行”。\n4. 首次运行会在本包内创建隔离的 esptool 环境，需要 Python 3.10+ 和网络。\n5. 按提示选择串口并输入 \`ERASE\`。工具会擦除整片 Flash，再把完整镜像从 \`0x0\` 写入并校验。\n\n## 重要警告\n\n该操作会覆盖设备设置、形象和组件。日常升级请使用 Pet Manager 的“固件升级”；只有新设备或明确恢复出厂时才使用本包。\n\n如果连接失败，按住 BOOT，短按一次 RESET/EN，松开 BOOT 后重试。若 921600 不稳定，可将工具脚本中的波特率改为 460800 后重新完整烧录。\n\n## 内容\n\n- \`firmware/${imageName}\`：推荐烧录的完整镜像（地址 0x0）\n- \`firmware/${manifestName}\`：镜像内容、分区和 SHA-256 清单\n- \`firmware/segments/\`：可独立核验的各二进制段，不建议普通用户分别烧录\n- \`resources/\`：默认形象及组件资源清单/分区资料\n- \`tools/\`：Windows/macOS 烧录工具\n- \`SHA256SUMS\`：包内全部文件校验值\n`;
const toolGuide = `
## 工具版本与 v3 接线

- 运行烧录脚本：Python **3.10+**，首次运行需要联网；脚本自动安装并核对 **esptool 5.4.0**，旧版本会自动更新。
- 本烧录包不需要 PlatformIO 或 ESP-IDF。自行编译源码才需要 PlatformIO Core **6.1.19**；v1 使用平台 55.03.32 / ESP-IDF 5.5.1，v3 使用平台 55.03.38-1 / ESP-IDF 5.5.4。
- 官方工具与说明：[esptool 5.4.0](https://github.com/espressif/esptool/releases/tag/v5.4.0)。请使用包内检查脚本，不要使用 --force 或不明确支持此芯片版本的图形烧录工具。
- **v1 与 v3 烧录包不可互刷，具体版本可以咨询客服进行确认。** ESP32-P4-WIFI6-M 的 M 不代表芯片修订版本。
- 屏幕排线连接 **DSI**，不是摄像头 **CSI**；断电后再插拔排线。配套屏幕的 v3 固件使用 24 MHz 显示时序。
- 未接摇杆时不会触发方向动作；接好摇杆、保持居中后重启，重新完成启动校准。

## 脚本无法双击时

macOS：打开终端，输入 zsh 后加空格，把 tools/flash-macOS.command 拖入终端，再回车。

Windows：在解压目录打开 PowerShell，执行：

\`\`\`powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\\tools\\flash-Windows.ps1 -Port COM5
\`\`\`

请把 COM5 改为设备实际串口。此参数只影响本次 PowerShell 进程，不修改全局执行策略。请先确认包来源和 SHA256SUMS。

烧录工具会先读取芯片版本，并核对完整镜像中的 Bootloader 和应用范围；确认匹配后才擦除、写入和校验。全部完成后，松开 BOOT 并重启，检查宠物画面和客户端连接。烧录速率默认 921600，必要时改为 460800；设备正常运行的串口速率为 4M，两者不同。
`;
const revisionLabel = (revision) => `${Math.floor(revision / 100)}.${revision % 100}`;
writeFileSync(join(packageRoot, "README-FLASHING-CN.md"), readme.replace(
  `版本：${manifest.version}`,
  `版本：${manifest.version}\n\n适用芯片：ESP32-P4 ${chipFamily}（${revisionLabel(manifest.chipRevisionMin)}～${revisionLabel(manifest.chipRevisionMax)}）。Windows/macOS 共用本包；v1 与 v3 不可互刷。工具会在擦除前核对芯片版本。`,
).replaceAll("${manifest.components.count} 个内置组件", `${manifest.components.count} 个内置组件`) + toolGuide);

const contentManifest = {
  schemaVersion: 1,
  format: "hachimodock-p4-factory-flasher-v1",
  firmwareVersion: manifest.version,
  chipFamily,
  chipRevisionMin: manifest.chipRevisionMin,
  chipRevisionMax: manifest.chipRevisionMax,
  chip: manifest.chip,
  flashOffset: manifest.flashOffset,
  factoryImage: `firmware/${imageName}`,
  appearance: manifest.appearance,
  components: manifest.components,
  publicSourceIncluded: false,
  embeddedInternalCredentials: false,
};
writeFileSync(
  join(packageRoot, "package-manifest.json"),
  `${JSON.stringify(contentManifest, null, 2)}\n`,
);

const localSecretsPath = join(repositoryRoot, "pc", ".internal-build-secrets.json");
const secretNeedles = [];
if (existsSync(localSecretsPath)) {
  const secrets = JSON.parse(readFileSync(localSecretsPath, "utf8"));
  for (const field of ["asrApiKey", "contentApiKey"]) {
    const value = typeof secrets[field] === "string" ? secrets[field].trim() : "";
    if (value) secretNeedles.push(Buffer.from(value));
  }
}
for (const path of listFiles(packageRoot)) {
  const bytes = readFileSync(path);
  if (secretNeedles.some((needle) => bytes.includes(needle))) {
    throw new Error(`factory package contains a known internal credential: ${relative(packageRoot, path)}`);
  }
}

const checksumLines = [];
for (const path of listFiles(packageRoot)) {
  const file = relative(packageRoot, path);
  checksumLines.push(`${await sha256File(path)}  ${file}`);
}
writeFileSync(join(packageRoot, "SHA256SUMS"), `${checksumLines.join("\n")}\n`);

mkdirSync(outputRoot, { recursive: true });
const archive = join(outputRoot, `${packageName}.zip`);
const zip = spawnSync("zip", ["-X", "-q", "-r", archive, packageName], {
  cwd: outputRoot,
  encoding: "utf8",
});
if (zip.status !== 0) throw new Error(zip.stderr.trim() || "zip packaging failed");

console.log(`factory flasher directory: ${packageRoot}`);
console.log(`factory flasher archive: ${archive}`);
console.log(`factory flasher SHA-256: ${await sha256File(archive)}`);
