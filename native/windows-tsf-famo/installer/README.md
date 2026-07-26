# 法墨输入法 Windows 事务安装器

机器级安装法墨自建的原生 TSF，与日用 Rime/Weasel 并存。安装、repair 和卸载都不写 `%AppData%\Rime`；法墨用户数据位于 `%LOCALAPPDATA%\Famo`。

## 交付模型

- 构建只接受一个 `FAMO_IDENTITY=Stable` 原生输出：`FamoTextService.dll`、`FamoRuntime.exe`、`FamoRimeEngine.dll`、`FamoProfileTool.exe` 和 `rime.dll`。
- 正式构建要求上述产品二进制签名有效。`-AllowUnsignedDevelopment` 只用于本地安装/repair/卸载验证。
- 每次安装或 repair 都提取到新的不可变目标：`{app}\versions\<version>-<manifest-prefix>-<transaction-id>`。
- `payload-manifest.txt` 固定版本、协议、架构、Stable identity、文件数、大小和 SHA-256。安装器在切换 TSF 注册前验证完整 manifest。
- 稳定身份使用产品 GUID/endpoint；开发构建使用独立 GUID/endpoint，不得混入稳定安装包。

## 事务顺序

1. 记录上一个活动目标、manifest、默认输入法和 profile 激活状态。
2. 提取新目标，完整验证 manifest 和每个文件的 SHA-256。
3. 停止旧 runtime，切离并反注册旧 profile，再注册新 profile。
4. 以原始用户身份运行 `FamoSettings.exe --seed-only`，启动 `FamoRuntime.exe`，并通过 `FamoRuntime.exe --control deploy` 部署。
5. 验证 COM/profile/manifest 回读后才写入 `InstallState=Ready`。

任何阶段失败都按反向顺序停止新 runtime、反注册新 profile、恢复旧注册/激活/runtime，并删除未就绪目标。对外终态只有 `Ready`、`RolledBack`、`PendingReboot` 和 `NotInstalled`。

如果注册表指向的旧 `FamoTextService.dll` 仍被任一宿主进程加载，安装器不会形成新旧混用的 `Ready`：它记录旧 DLL 的路径、SHA-256 和文件版本，切离并反注册旧输入法，保持稳定 COM/profile 未注册，移除用户 TIP 和 runtime 自启，然后进入 `PendingReboot`。保留安装器通过 HKLM RunOnce 以同一 `TransactionId` 在重启后继续；只有旧 DLL 已卸载后才注册新 profile，并在 manifest/COM/profile/runtime 全部回读成功后转为 `Ready`。继续失败时恢复为未注册的 `PendingReboot`，不会反复自启。

等待重启期间可用 `Test-FamoHealth.ps1` 和 `Test-FamoTsfRegistration.ps1` 验证安全终态。若决定放弃升级，以管理员身份运行注册表 `ResumeInstaller` 指向的文件：

```powershell
& $resumeInstaller "/FamoRollback=$transactionId"
```

显式回滚会恢复先前的精确注册路径、profile 激活状态、默认输入法和 runtime；等待重启期间直接卸载也会清除 RunOnce、禁用 profile 和暂存事务。

## 卸载语义

卸载器先由 `FamoProfileTool cleanup-user` 借当前桌面 Explorer 的令牌停止该用户 runtime、切走输入法并移除其 TIP/profile/HKCU COM override，再以管理员身份移除机器注册、Run 项和 `{app}\versions`。Inno Setup 的 `ExecAsOriginalUser` 不支持卸载阶段，因此卸载不依赖它。

- 静默卸载保留 `%LOCALAPPDATA%\Famo`。
- 交互卸载会询问是否删除 `%LOCALAPPDATA%\Famo`，默认保留。
- 只删除 `{app}\versions`，不删除 `{app}` 根下与产品版本无关的用户备份。
- 若 Explorer、浏览器等通用 TSF 宿主仍映射旧 `FamoTextService.dll`，卸载会立即撤销注册，并把该 DLL 及其版本目录登记为重启删除；不会强杀这些宿主。重启前不允许留下其他文件，重启后 `{app}\versions` 必须消失。

## 构建

前置：Inno Setup 6、.NET 10 SDK、Stable x64 原生输出、自足的 `../famo-config/payload`。

```powershell
.\build-installer.ps1
.\build-installer.ps1 -NativeOutput <stable-build-dir>
.\build-installer.ps1 -AllowUnsignedDevelopment # 仅本地验证
```

产物为 `dist\Famo-Setup-<version>.exe`。构建脚本会输出 SHA-256；`staging\`、`dist\` 和安装器 exe 不入库。

## 验证

- `Test-FamoHealth.ps1` 检查事务终态、Stable identity、完整 manifest、COM/profile、精确 runtime 路径和有界 control pipe；`PendingReboot` 还要求 runtime 缺席且 RunOnce 与同一事务一致。
- `Test-FamoTsfRegistration.ps1` 检查 HKCU COM、profile/category、激活状态、current-user TIP 和 stable/development 隔离；`PendingReboot` 要求 COM/profile 未注册、未激活且 TIP 不可见。
- `smoke-harness.ps1` 在当前电脑的一个提权 PowerShell 会话中执行一次安装/修复与健康检查，不创建 VM、证据包或应用矩阵。
- 本机手动复现范围见 `smoke_test.md`；跨版本发行认证须另行批准且不阻断开发。
