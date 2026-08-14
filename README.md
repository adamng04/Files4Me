# Files4Me

Files4Me is a portable, native Windows dual-pane file manager. It combines Total Commander-style keyboard operations with a compact File Explorer-style interface.

The interface includes a native File/Edit/View/Help navigation row, rounded borderless controls, a DPI-scaled Material Icons command bar, one icon-only light/dark toggle, and a dual-pane toggle. File columns use compact square headers. A File Explorer-style sidebar provides Home, known folders, Recycle Bin, drives, Network, Linux/WSL shortcuts, and persistent user-pinned folders. Each pane has a live current-location filter that matches names, extensions, and file types without blocking enumeration.

The native Settings page covers startup, appearance, file display, operations, navigation/refresh, sidebar shortcuts, and advanced shell behavior. Changes apply immediately and persist in the portable INI file.

File mutations run through a background operation center instead of blocking the interface. Up to three jobs may run when they touch independent volumes or network shares; jobs sharing a root remain ordered. The bottom drawer shows progress and recent history, supports safe item-boundary pause, cancellation, retry, and conflict policies, and records interrupted work for explicit recovery after restart.

## Requirements

- Windows 10 or Windows 11 x64
- Visual Studio 2026 Build Tools
- Desktop development with C++ workload
- Windows 10/11 SDK

## Build

Open `Files4Me.sln` in Visual Studio and build `Release | x64`, or run from a Visual Studio Developer PowerShell:

```powershell
msbuild Files4Me.sln /m /p:Configuration=Release /p:Platform=x64
```

Output: `dist/Files4Me.exe`

Release identity is defined once in `src/version.h`; the executable, Windows version resource, updater comparison, and installer build consume it.

Files4Me uses only Windows system APIs. It stores `Files4Me.ini` and the versioned `Files4Me.jobs` operation journal beside the executable when writable, otherwise under `%LocalAppData%\Files4Me`.

## Installer

Run `powershell -ExecutionPolicy Bypass -File installer/build-installer.ps1` with Inno Setup 6 installed. The installer performs a per-user installation under `%LocalAppData%\Programs\Files4Me`, creates a Start Menu shortcut, and registers reversible **Open in Files4Me** commands for folders, folder backgrounds, and drives. An optional installer task makes Files4Me the default handler when folders and drives are opened; uninstall restores the previous handler if Files4Me still owns it.

Installed builds keep preferences and the operation journal under `%LocalAppData%\Files4Me`, separate from program files.

## Updates

Installed builds can check their explicitly compiled update channel once per day or through **Help > Check for updates**. Alpha builds use `updates/alpha`; stable builds use `updates/stable`. Files4Me downloads only HTTPS resources from the official `adamng04/Files4Me` GitHub repository, verifies an RSA-signed manifest and the installer's SHA-256 digest, and asks before downloading or launching Setup. Portable builds only open the GitHub release page.

Release maintainers generate the selected channel's `manifest.ini` and detached signature with `tools/sign-update.ps1 -Channel alpha` or `-Channel stable`; the channel argument is mandatory to prevent accidental cross-channel publication. The signer opens the non-exportable RSA-3072 key named `Files4Me Update Manifest Signing v1` from the current user's Microsoft Software Key Storage Provider and requires Windows strong-key approval for every signature. It refuses keys with the wrong provider, size, export policy, UI policy, or pinned public-key fingerprint.

Before the first CNG signing operation, migrate the original legacy key with `tools/manage-update-key.ps1`. Migration creates and verifies two independently salted password-encrypted PKCS#8 backups on different volumes, using PBES2, AES-256-CBC, PBKDF2-HMAC-SHA256, and 600,000 iterations. The original key is never deleted automatically.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\manage-update-key.ps1 `
  -Action Migrate `
  -BackupPath 'E:\Files4Me\Files4Me-update-recovery-A.pem','F:\Files4Me\Files4Me-update-recovery-B.pem'

powershell -NoProfile -ExecutionPolicy Bypass -File tools\manage-update-key.ps1 -Action Audit
```

Use two offline removable volumes and store the backup passphrase separately in a password manager. Test backups with `-Action TestBackup`. On a replacement Windows account or PC, use `-Action Restore` with one backup. TPM storage is not assumed: test the specific Platform Crypto Provider first; if it cannot import the existing identity, ship a dual-trust updater release before rotating to a newly generated TPM key. Installers belong in GitHub Releases, not Git history.

## License

Files4Me is licensed under the [MIT License](LICENSE). Google Material Icons remain available under the Apache License 2.0; see `assets/MATERIAL-ICONS-LICENSE.txt`.

Google Material Icons are embedded in the executable for offline use and distributed under Apache License 2.0. The license copy is at `assets/MATERIAL-ICONS-LICENSE.txt`.

Selected files support native Windows OLE drag-and-drop to the other pane, folders, Explorer, the desktop, and compatible applications. Right-click uses the full Windows Shell context menu, including installed shell extensions.

Files4Me 1.0 is the first stable release. It uses the stable signed-update channel, while the same 1.0 installer may be advertised once on the alpha channel to migrate existing preview installations. The release artifact is `Files4Me-1.0-release-Setup.exe`.

Downloads uses an Explorer-style date timeline (Today, Yesterday, this week, and older groups). ZIP files open as read-only folders using Windows' built-in compressed-folder handler; files can be opened, copied, dragged out, or extracted without bundling an archive library. Checkbox space is reserved in Details view so names never overlap selection controls.

The archive smoke test in `tests/archive_smoke.cpp` verifies real Windows ZIP enumeration and extraction against a generated fixture.

## Keyboard shortcuts

| Key | Action |
|---|---|
| Tab | Switch active pane |
| Enter | Open selected item |
| Insert | Toggle selected item |
| F2 | Rename |
| F5 | Copy to other pane |
| F6 | Move to other pane |
| F7 | Create folder |
| F8 / Delete | Move to Recycle Bin |
| Shift+Delete | Permanently delete |
| Ctrl+T / Ctrl+W | New / close tab |
| Ctrl+L | Focus path |
| Ctrl+F | Focus current-pane filter |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut / paste through Windows clipboard |
| Alt+Left / Alt+Right / Alt+Up | Back / forward / parent |
