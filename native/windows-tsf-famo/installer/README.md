# 法墨输入法 Windows 事务安装器

机器级安装法墨自建的原生 TSF，与日用 Rime/Weasel 并存。安装、repair 和卸载都不写 `%AppData%\Rime`；法墨用户数据位于 `%LOCALAPPDATA%\Famo`。

## 交付模型

- 构建只接受一个 `FAMO_IDENTITY=Stable` 原生输出：`FamoTextService.dll`、`FamoRuntime.exe`、`FamoRimeEngine.dll`、`FamoProfileTool.exe` 和 `rime.dll`。
- 正式构建要求上述产品二进制签名有效。`-AllowUnsignedDevelopment` 只用于本地安装/repair/卸载验证。
- 每次安装或 repair 都提取到新的不可变目标：`{app}\versions\<version>-<manifest-prefix>-<transaction-id>`。
- `payload-manifest.txt` 固定版本、协议、架构、Stable identity、文件数、大小和 SHA-256。安装器在切换 TSF 注册前验证完整 manifest。
- 稳定身份使用产品 GUID 和 `runtime-v2/control-v2` endpoint；开发构建使用独立 GUID 和 `dev-runtime-v2/dev-control-v2` endpoint，不得混入稳定安装包。

## 事务顺序

1. 记录上一个活动目标、manifest、默认输入法和 profile 激活状态。
2. 提取新目标，完整验证 manifest 和每个文件的 SHA-256。
3. 停止旧 runtime，切离并反注册旧 profile，再注册新 profile。
4. 以原始用户身份准备 seed 事务；安装器把收据的完整 SHA-256 写入并 flush 到提权 journal 后，才按 CAS 规则应用用户文件。
5. 启动 `FamoRuntime.exe` 并通过 `--control deploy` 部署；验证 COM/profile/manifest 回读后才写入 `InstallState=Ready`，随后提交并清除已认证的 seed 收据。

任何阶段失败都按反向顺序停止新 runtime、反注册新 profile、恢复旧注册/激活/runtime，并以 CAS 回滚 seed：安装后出现的用户修改会被保留，不会被备份覆盖。所有延迟工作都使用 `famo-debt-v2|<transaction-id>|<kind>` 类型化债务；写入和删除均 flush 并精确回读，foreign、malformed 或同事务未知 kind 一律阻断。身份捕获和机器清理 helper 还会在创建目录前持久化 `famo-debt-v2|<transaction-id>|identity-helper:<nonce>` 或 `famo-debt-v2|<transaction-id>|machine-cleanup-helper:<nonce>` 的 `HelperCleanupDebt`；普通安装或卸载启动时先验证固定父目录、非 reparse 最终路径和两个允许的精确文件名，再逐个回收，不使用通配删除。普通安装启动会先恢复这些 helper 残留和终态债务，不会先创建新事务；新版安装器按三段版本号严格比较，只接管通过完整 journal 校验的更旧版 `Ready`/`RolledBack` 终态，不接管同版不同产物、未来版本或任何旧版非终态载荷。若终态目标已被部分删除，续删还必须同时匹配 journal 最终路径、NTFS 对象 ID，并验证残余树无 reparse；完成这些校验后删除仍被阻断时，交互安装才提示文件通常仍被系统占用并建议重启，其他恢复失败使用通用错误提示。用户、目标和恢复工件按顺序清完后，恢复任务和保留安装器才最后删除。若精确用户暂不可用、文件发生冲突或版本目录仍被占用，journal 会保留 cleanup/rollback debt 和精确 SID 恢复任务，在后续该用户登录时重试；债务清除前不删除恢复锚。对外终态只有 `Ready`、`RolledBack`、`PendingReboot` 和 `NotInstalled`。

setup 与 uninstall 从各自初始化入口的第一步起就持有同一个独立的全局事务互斥量，任何恢复或 journal 变更之前即拒绝并发进程。helper 回收会持有 `{app}`、`pending` 和精确 helper 目录的非共享删除句柄；精确文件和目录消失后，先对 `{app}` 所在卷执行 `FlushFileBuffers`，成功后才清除 `HelperCleanupDebt`。卷刷新失败会保留债务供下次恢复。

如果注册表指向的旧 `FamoTextService.dll` 仍被任一宿主进程加载，安装器不会形成新旧混用的 `Ready`：它记录旧 DLL 的路径、SHA-256 和文件版本，切离并反注册旧输入法，保持稳定 COM/profile 未注册，移除用户 TIP 和 runtime 自启，然后进入 `PendingReboot`。保留安装器及其完整 SHA-256 写入 copy-on-write v2 journal，并创建只对最初用户 SID 生效的 Task Scheduler 登录任务（`InteractiveToken`、`HighestAvailable`）；任务 XML 会在切离旧注册前完整读回。重启后只有旧 DLL 已卸载才注册新 profile，并在 manifest/COM/profile/runtime 全部回读成功后转为 `Ready`。继续失败时恢复为未注册的 `PendingReboot`，不会反复自启。

等待重启期间可用 `Test-FamoHealth.ps1` 和 `Test-FamoTsfRegistration.ps1` 验证安全终态。恢复安装器、hash、任务名和原用户 SID 只从 `ActiveTransactionId → Transactions\<id>\ActiveGeneration → gN` journal generation 读取。若决定放弃升级，以管理员身份运行该 generation 的 `ResumeInstaller`：

```powershell
& $resumeInstaller "/FamoRollback=$transactionId"
```

显式回滚会恢复先前的精确注册路径、上一层 rollback 指针、profile 激活状态、默认输入法和 runtime；等待重启期间直接卸载也会验证并清除精确恢复任务、禁用 profile 和暂存事务。

## 卸载语义

卸载器先由 `FamoProfileTool cleanup-user-for <journal-sid>` 借当前桌面 Explorer 的令牌（并核对其 SID）停止该用户 runtime、切走输入法并移除其 TIP/profile/HKCU COM override，再以管理员身份移除机器注册、Run 项和 `{app}\versions`。Inno Setup 的 `ExecAsOriginalUser` 不支持卸载阶段，因此卸载不依赖它。

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

## 自动更新发布

更新客户端固定读取 Windows 仓库的
`releases/latest/download/appcast.xml`；appcast 内的安装包 URL 固定到同版本不可变 tag，
并带 `windows-x64`、EdDSA 签名和
`/SILENT /SP- /NOICONS /NORESTART`。发现新版后仍由用户确认，安装事务与手动覆盖安装完全相同。

Windows EdDSA 私钥只放在发布机的密钥存储中，不入仓库。脚本会从私钥派生公钥，
并与客户端内置公钥逐字比对；不匹配时拒绝生成 appcast。发布前先设置正式私钥，
用同一密钥完成本地签名自检，再为正式安装包生成 appcast：

```powershell
$env:FAMO_UPDATE_PRIVATE_KEY = '<仓库外私钥路径>'
.\make-appcast-selftest.ps1
.\make-appcast.ps1 -AppVersion 1.5.5
```

若从 WSL 调用 Windows PowerShell，须用 Windows `cmd.exe` 作为宿主，避免 WSL
直接启动 `pwsh.exe` 时不等待 WinSparkle GUI 子系统工具：

```powershell
cmd.exe /d /c "set FAMO_UPDATE_PRIVATE_KEY=C:\path\to\key&& pwsh -NonInteractive -NoLogo -NoProfile -File .\native\windows-tsf-famo\installer\make-appcast-selftest.ps1"
```

把 `Famo-Setup-<version>.exe` 与 `appcast.xml` 一起上传到同一个 Windows Release；不得把
macOS appcast、安装包或 tag 混入本仓。脚本只生成本地资产，不创建或修改 GitHub Release。

本地自检只覆盖正式私钥与内置公钥配对、appcast 元数据、EdDSA 签名生成与验签；
它不访问真实 GitHub appcast，也不覆盖 WinSparkle 更新窗口和下载、篡改签名拒绝、
UAC 提权、应用优雅退出，或 Win10 / Win11 上从旧版本到新版本的事务安装与回滚。
发布前须在真实 Release 资产上另行完成这些人工/真机验证。

## 验证

- `Test-FamoHealth.ps1` 检查事务终态、Stable identity、完整 manifest、COM/profile、精确 runtime 路径和有界 control pipe；`PendingReboot` 还要求 runtime 缺席，并从 v2 journal 验证恢复安装器/hash 以及精确 SID 的 Task Scheduler XML。
- `Test-FamoTsfRegistration.ps1` 检查 HKCU COM、profile/category、激活状态、current-user TIP 和 stable/development 隔离；`PendingReboot` 要求 COM/profile 未注册、未激活且 TIP 不可见。
- `smoke-harness.ps1` 在当前电脑的一个提权 PowerShell 会话中执行一次安装/修复与健康检查，不创建 VM、证据包或应用矩阵。
- 本机手动复现范围见 `smoke_test.md`；跨版本发行认证须另行批准且不阻断开发。
