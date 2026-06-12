# [HachimoDock（哈基米机）](https://github.com/YizhengWw/HachimoDock) Desktop Packaging

## Current package targets
- macOS uses a universal Tauri DMG so one file can run on Apple Silicon and Intel Macs.
- Windows uses the native Windows Tauri build to create x64 installers. Build this on a Windows runner for the real install package.
- macOS can cross-compile a Windows x64 portable executable with `cargo-xwin`, but it is not the canonical Windows installer path.

## Local macOS DMG
```sh
cd HachimoDock/ref
rustup target add aarch64-apple-darwin x86_64-apple-darwin
npm ci
npm run build:mac
```

Output:
```text
src-tauri/target/universal-apple-darwin/release/bundle/dmg/*.dmg
```

## Native Windows installer
```sh
cd HachimoDock/ref
rustup target add x86_64-pc-windows-msvc
npm ci
npm run build:win
```

Output:
```text
src-tauri/target/x86_64-pc-windows-msvc/release/bundle/nsis/*.exe
src-tauri/target/x86_64-pc-windows-msvc/release/bundle/msi/*.msi
```

The repository-level desktop build workflow runs the macOS and Windows builds on matching GitHub Actions hosts and uploads the installers as artifacts.

## macOS cross-compiled Windows portable exe
```sh
cd HachimoDock/ref
cargo install cargo-xwin
brew install llvm@20
PATH="$(brew --prefix llvm@20)/bin:$PATH" npm run build:win:portable
```

Output:
```text
src-tauri/target/x86_64-pc-windows-msvc/release/*.exe
```

Use this portable executable for quick validation only. For general distribution, use the Windows runner installer artifact.
