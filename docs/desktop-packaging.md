# Pet Manager Desktop Packaging

## Current package targets
- macOS uses a universal Tauri DMG so one file can run on Apple Silicon and Intel Macs.
- Windows uses the native Windows Tauri build to create x64 installers. Build this on a Windows runner for the real install package.
- macOS can cross-compile a Windows x64 portable executable with `cargo-xwin`, but it is not the canonical Windows installer path.

## Local macOS DMG with stable Accessibility identity

> **Required:** local installation, Accessibility authorization, and repeated
> development builds must use `npm run build:mac:local`. Do not substitute
> `npm run build:mac`: without a Developer ID identity, that command produces a
> per-build CDHash requirement, so an enabled Accessibility switch can still
> refer to the previous build. The stable local workflow signs the app before
> creating the DMG and verifies the exact `com.petmanager.desktop` designated
> requirement in both the source app and the app mounted from the final DMG.

```sh
cd pet-manager/ref
rustup target add aarch64-apple-darwin x86_64-apple-darwin
git lfs install --local
git lfs pull
npm ci
npm run build:mac:local
```

The bundled Node runtime must be a relocatable distribution binary. Homebrew
may link `node` to libraries outside the app bundle, so the packaging preflight
rejects any macOS Node/FFmpeg binary that depends on paths other than
`/System/Library` or `/usr/lib`. After one successful preflight, later local
builds reuse that exact validated staged Node even if the shell PATH changes.
For the first clean build, when the active Node is not relocatable, point the
build at an official standalone Node binary:

```sh
PET_MANAGER_NODE_BIN="/absolute/path/to/node-v22/bin/node" npm run build:mac:local
```

The local signature is intentionally an ad-hoc development identity with a
stable designated requirement. It is for the current developer machine only
and must not be distributed. Public releases must use a stable Apple Developer
ID Application certificate, notarization, and stapling; that certificate
provides the stable TCC identity instead of the local ad-hoc requirement.

Output:
```text
src-tauri/target/universal-apple-darwin/release/bundle/dmg/*.dmg
```

## Native Windows installer
```sh
cd pet-manager/ref
rustup target add x86_64-pc-windows-msvc
npm ci
npm run build:win
```

Output:
```text
src-tauri/target/x86_64-pc-windows-msvc/release/bundle/nsis/*.exe
src-tauri/target/x86_64-pc-windows-msvc/release/bundle/msi/*.msi
```

The repository-level workflow `.github/workflows/pet-manager-desktop-build.yml` runs the macOS and Windows builds on matching GitHub Actions hosts and uploads the installers as artifacts.

## macOS cross-compiled Windows portable exe
```sh
cd pet-manager/ref
cargo install cargo-xwin
brew install llvm@20
PATH="$(brew --prefix llvm@20)/bin:$PATH" npm run build:win:portable
```

Output:
```text
src-tauri/target/x86_64-pc-windows-msvc/release/pet-manager-tauri.exe
```

Use this portable executable for quick validation only. For general distribution, use the Windows runner installer artifact.
