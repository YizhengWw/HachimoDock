// Packaging P4 resources must read the P4 literal, even when another board's
// conditional PROJECT_VER appears first in the shared firmware entry point.
export function p4FirmwareSourceVersion(cmake) {
  const versions = [...String(cmake).matchAll(/^\s*set\(\s*PROJECT_VER\s+"([^"]+)"\s*\)/gmi)]
    .map((match) => match[1])
    .filter((version) => /^\d+\.\d+\.\d+-p4(?:[-+][A-Za-z0-9.-]+)?$/.test(version));
  if (versions.length !== 1) {
    throw new Error("无法确定唯一的 P4 固件源码版本");
  }
  return versions[0];
}
