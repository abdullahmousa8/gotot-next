# SAC / Application Control Unblock Procedure

Reference document for removing the Windows **Smart App Control (SAC)** /
**Application Control (Device Guard / WDAC)** block that prevented the
GOTOT-NEXT dev binaries (`godot.windows.editor.dev.x86_64*.exe`) from running.

## 1. Context

- Assets under milestone 008B were blocked with:
  - `An Application Control policy has blocked this file`
  - `'...godot.windows.editor.dev.x86_64.console.exe' was blocked by your organization's Device Guard policy.` (exit 4551)
  - CodeIntegrity event 3118 / (block events).
- Root cause: Smart App Control running in **Enforce** mode, state stored in:
  `HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy → VerifiedAndReputablePolicyState = 1`.
- The binaries are unsigned custom dev builds, so SAC (which relies on
  Microsoft's reputable-signing/reputation signals) classified them as
  untrusted.

## 2. Exact steps used to unblock (2026-09-21)

1. **Confirmed the policy state and shell privilege**:
   ```powershell
   Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy'
   # VerifiedAndReputablePolicyState : 1   (Enforce)
   # SAC_EnforcementReason           : 1
   ```
   The shell was **not** elevated (`is_admin=False`), so HKLM writes were impossible directly.

2. **Set the policy value to 0 (Off) with elevation** via a UAC-approved
   elevated PowerShell process:
   ```powershell
   Set-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' `
     -Name VerifiedAndReputablePolicyState -Type DWord -Value 0
   ```
   Triggered as: `Start-Process powershell -Verb RunAs -File disable_sac.ps1 -Wait`
   (user approved the UAC prompt and confirms it is their intended action).

3. **Verified the live registry flip** (value now `0`) — but the block did
   **not** clear immediately because SAC enforcement is loaded at **boot time**.

4. **Rebooted the machine** (`shutdown /r /t 5`). After reboot:
   - `VerifiedAndReputablePolicyState` = `0`
   - A probe run `godot.windows.editor.dev.x86_64.exe --version` executed and
     printed `4.8.dev.custom_build` (exit 0).

5. **Re-ran the full verification suite on the same rebuilt binary**:
   - Regression 001A–006 (gt_smoke) = PASS
   - 007A (gt_007) = PASS
   - 008A (gt_008) = PASS
   - 008B (gt_008b, cross-run DET identical) = PASS

## 3. Is the fix permanent or temporary?

- **Permanent for this machine/OS session.** SAC is now **Off**; it stays Off
  until something explicitly re-enables it (Settings UI, MDM/Intune/GPO push,
  or a new user profile policy). Microsoft's SAC **cannot be turned back on
  programmatically once disabled** from the Settings UI; a drive/OS reset would
  restore defaults.

## 4. Could the block come back on a new build?

- **No** — while SAC stays Off. The block was caused by SAC's reputation
  policy classifying the *unsigned custom dev exe*, not by anything about a
  specific file path or build.
- **Yes, if it is re-enabled** by an administrator/IT (`VerifiedAndReputablePolicyState`
  back to `1`), or if machine enrollment pushes an explicit WDAC policy that
  blocks unsigned binaries. In that case repeat section 2 (or have IT deploy an
  allow-rule / enterprise-signed binaries).

## 5. Notes / precautions

- Disabling SAC **lowers** one layer of app-reputation protection on this
  machine (MS Windows: "Smart App Control will not block untrusted apps").
  It does **not** disable Windows Defender. Only the Owner/administrator may
  authorize it.
- Keep the change documented here so future sessions know the binary is now
  expected to run without the block.
- If a future run is blocked again, re-check:
  1. `HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy\VerifiedAndReputablePolicyState`
  2. recent CodeIntegrity events (`Get-WinEvent -LogName 'Microsoft-Windows-CodeIntegrity/Operational'`)
  3. whether the exe was rebuilt and needs the same treatment (it should run if SAC is Off).

## 6. Enterprise Enrollment Caveat

If this machine is enrolled in MDM/Intune/GPO, an administrator may
re-enable SAC or push an explicit WDAC policy that blocks unsigned
binaries. In that case:

1. Contact IT to either:
   - Add an allow-rule for the build output path, or
   - Deploy enterprise-signed binaries.
2. Re-check:
   - `HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy\VerifiedAndReputablePolicyState`
   - `Get-WinEvent -LogName 'Microsoft-Windows-CodeIntegrity/Operational'`

## 7. Recovery Steps (if block returns)

1. Verify SAC state:
   ```powershell
   Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy'
   ```
2. If `VerifiedAndReputablePolicyState = 1`:
   - Repeat Section 2 (elevated PowerShell + `Set-ItemProperty` + Reboot).
   - Or contact IT for an allow-rule.
3. If a new build is blocked despite SAC = 0:
   - Check CodeIntegrity events for the specific policy.
   - Verify the binary is not being blocked by a WDAC policy.