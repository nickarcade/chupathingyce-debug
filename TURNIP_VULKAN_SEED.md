# Mesa Turnip Vulkan Integration: Starter Package & Seed Prompt

This document provides a turnkey context package and seed prompt to branch into a clean new chat session focused entirely on the **Mesa Turnip Vulkan Driver and ChupathingyCE Vulkan Renderer** pipeline.

---

## 1. Quick Copy-Paste Seed Prompt for Your New Chat

Copy and paste the block below into your new conversation window:

```markdown
Continue the ChupathingyCE Mesa Turnip Vulkan driver integration on Samsung Galaxy S10 (Snapdragon 855 / Adreno 640).

### Context & Prior Art
Refer to the previous session: [Previous Turnip Session](conversation://277888e1-128c-4659-91f8-b4b07c51c2bf)
Report Reference: `/data/data/com.termux/files/home/.gemini/antigravity-cli/brain/277888e1-128c-4659-91f8-b4b07c51c2bf/chupathingyce_vulkan_turnip_report.md`

### Current Hardware & Architecture
- Device: Samsung Galaxy S10 (`SM-G973U`), Snapdragon 855 (`SM8150`), Adreno 640 GPU, Android 12 (One UI 4.1).
- Target Driver: Mesa 26.2.5 Turnip Vulkan driver (`libvulkan_freedreno.so`) compiled with KGSL backend (`-Dfreedreno-kmds=kgsl`).
- Hook Layer: `libadrenotools.so` runtime redirection into `/sdcard/Android/data/dev.horrible.chupathingyce/files/turnip-26.2.5-adreno6xx.zip`.
- Target Codebase Branch: `pr-152-test` in `~/chupathingyce/` (upstream PR #152: dual-guest Vulkan pipeline `assets/halo_guest_vk.elf`).
- Build Automation Notebook: `/sdcard/Download/chupathingyce_vulkan_turnip_pipeline.ipynb`.

Please confirm you have read the references, inspect the local git status on `pr-152-test`, and outline the next steps for our Turnip/Vulkan development.
```

---

## 2. Key Files, Paths, and Artifacts

| Component | Path | Description |
| :--- | :--- | :--- |
| **Turnip Colab Build Notebook** | `/sdcard/Download/chupathingyce_vulkan_turnip_pipeline.ipynb` | End-to-end notebook for building Mesa 26.2.5 Turnip and ChupathingyCE Vulkan debug APK |
| **Turnip Driver Archive** | `/data/data/com.termux/files/home/chupathingyce-apks/turnip-26.2.5-adreno6xx.zip` | AdrenoTools-compatible ZIP containing `libvulkan_freedreno.so` & `meta.json` |
| **Turnip On-Device Staging** | `/sdcard/Android/data/dev.horrible.chupathingyce/files/turnip-26.2.5-adreno6xx.zip` | Where the in-game driver loader unpacks and binds the custom driver |
| **Vulkan Git Branch** | `pr-152-test` in `~/chupathingyce/` | Contains upstream PR #152 ("Add Vulkan renderer to Android port") + runtime draw pipeline fixes |
| **Technical Report** | `file:///data/data/com.termux/files/home/.gemini/antigravity-cli/brain/277888e1-128c-4659-91f8-b4b07c51c2bf/chupathingyce_vulkan_turnip_report.md` | Full architecture documentation, mermaid diagrams, and logcat trace |
| **Reference Screenshot** | `file:///data/data/com.termux/files/home/.gemini/antigravity-cli/brain/277888e1-128c-4659-91f8-b4b07c51c2bf/halo_vk.png` | Screenshot of ChupathingyCE running on Turnip Adreno 640 Vulkan 1.3.354 |

---

## 3. Configuration & Activation Summary (`config.toml`)

To activate or toggle Vulkan and the custom Turnip driver on-device:

**Location**: `/sdcard/Android/data/dev.horrible.chupathingyce/files/config.toml`

```toml
[display]
renderer = "vulkan"                      # Options: "vulkan" or "opengl"
vk_driver = "turnip-26.2.5-adreno6xx.zip" # Options: "auto", specific zip name, or "none"
```

---

## 4. Verification & Diagnostics Command

Execute the following in Termux to verify driver hook and GPU initialization:

```bash
rish -c 'logcat -d' | rg '(adrenotools|turnip|freedreno|vulkan|renderer: Vulkan)'
```

Expected logcat confirmation:
```text
I chupathingyce: vk driver: turnip-26.2.5-adreno6xx.zip, Turnip Mesa 26.2.5 (KGSL), Mesa 26.2.5
I hook_impl: hook_android_dlopen_ext: loading custom driver: .../libvulkan_freedreno.so
I chupathingyce: renderer: Vulkan on turnip-26.2.5-adreno6xx.zip, Turnip Mesa 26.2.5 (KGSL), Turnip Adreno (TM) 640, Vulkan 1.3.354
```
