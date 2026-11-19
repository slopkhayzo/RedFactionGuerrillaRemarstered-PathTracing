# Third-party components

| Component | Version | Source | License | Integrity |
| --- | --- | --- | --- | --- |
| MinHook (`minhook/`) | v1.3.4 | https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip (copied from the PhysX project) | BSD-2-Clause (`minhook/LICENSE.txt`) | only `include/` and `src/` are kept |
| Ultimate ASI Loader, Win32 (`ultimate-asi-loader/dinput8.dll`) | v9.7.4 | https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v9.7.4/Ultimate-ASI-Loader.zip | MIT (`ultimate-asi-loader/LICENSE.txt`) | `dinput8.dll` SHA-256 `d5a059aa467a7a7127c8f6169f79fa63ff0f55986ee9eb2fd9a281bebf2aa2e6` (checked by `tools/run_game.ps1`) |
| NVIDIA DLSS SDK (`dlss/`) | v310.9.16 | https://github.com/NVIDIA/DLSS, tag `v310.9.16`, fetched by `tools/fetch_dlss.ps1`: the headers, the x64 static-CRT NGX library, the release runtime `nvngx_dlssd.dll` and the license | NVIDIA RTX SDKs License (`dlss/LICENSE.txt`) | `dlss/SHA256SUMS.txt` lists the files' hashes |

The loader's `dinput8.dll` is not meant for the repository: download the release archive above and put its
`dinput8.dll` into `ultimate-asi-loader/`. It loads `scripts\rfgrt.asi` when the game's own code starts and calls its
exported `InitializeASI`.

The DLSS SDK is not kept in the repository either: `tools/fetch_dlss.ps1` downloads it, and the build uses it when it
is there (DLSS Ray Reconstruction, `[GI] Denoiser=2`). The release archives include its runtime, `nvngx_dlssd.dll`, with
NVIDIA's license text.
