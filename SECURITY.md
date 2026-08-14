# Security

Files4Me minimizes its attack surface by using Windows system APIs only. It does not embed a browser engine, load plugins, parse archives, execute scripts, or depend on third-party packages.

The Windows Shell context menu can load shell-extension DLLs installed by other software into the Files4Me process. Those extensions are outside Files4Me's trust boundary and can affect stability or security. Remove or update untrusted shell extensions at the operating-system level.

The release build enables ASLR, high-entropy ASLR, DEP/NX, stack cookies, SDL checks, Spectre mitigations, and Control Flow Guard. The process runs as the current user with an `asInvoker` manifest. File operations use `IFileOperation`, and permanent deletion requires explicit confirmation.

These measures reduce risk but cannot guarantee the absence of vulnerabilities or operating-system zero-days. Keep Windows updated and obtain builds from a trusted source. Report suspected vulnerabilities privately with reproduction steps, affected version, and impact.

## Update trust

The updater accepts only HTTPS resources from the official GitHub and GitHubusercontent hosts. Update metadata is capped at 16 KiB and must carry a valid RSA-3072/SHA-256 signature from the offline Files4Me release key. Installer downloads are size-bounded and must match the signed SHA-256 digest before they can be launched. Automatic network failures stay silent and never block startup.

## File-system boundaries

- Paths are handled as Unicode strings and support long-path-aware Windows APIs.
- Directory enumeration does not recursively traverse reparse points.
- Files open through registered Windows file associations; Files4Me does not construct command lines from filenames.
- Drag-and-drop accepts only filesystem paths exposed through `CF_HDROP`; other data formats are rejected.
- Settings contain only UI state and paths. State uses the executable directory when writable, with `%LocalAppData%\Files4Me` as fallback.
- The operation journal uses a bounded, versioned binary format and atomic replacement. Running jobs become interrupted jobs after restart and are never resumed silently.
- Concurrent jobs are limited to three and are serialized when their source or destination roots overlap.
