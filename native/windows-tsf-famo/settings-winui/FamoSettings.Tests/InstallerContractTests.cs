using Xunit;

namespace Famo.Settings.Tests;

public sealed class InstallerContractTests
{
    [Fact]
    public void Installer_CapturesTheExactOriginalUserThroughAnAuthenticatedOneShotPipe()
    {
        string tool = RepoText("native/windows-tsf-famo/text-service/tools/dev_profile_main.cpp");
        string iss = InstallerText("famo-setup.iss");

        foreach (string contract in new[]
        {
            "capture-original-user", "prove-current-token",
            "CreateNamedPipeW", "PIPE_REJECT_REMOTE_CLIENTS",
            "ImpersonateNamedPipeClient", "OpenThreadToken",
            "TokenLinkedToken", "CheckTokenMembership",
            "ConvertSidToStringSidW", "CREATE_NEW",
        })
        {
            Assert.Contains(contract, tool);
        }

        Assert.Contains("CoCreateGuid", iss);
        Assert.Contains("CaptureOriginalUserIdentity", iss);
        Assert.Contains("OriginalUserSid", iss);
        Assert.Contains("OriginalUserSession", iss);
        Assert.Contains("OriginalUserResumeCapable", iss);
        Assert.Contains(
            "CleanupExactHelperAndDebt(TransactionId", iss);
        Assert.Contains(
            "'identity-' + Nonce + '.txt'", iss);
        Assert.DoesNotContain("{username}", iss, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Installer_JournalCommitsCompleteVersionTwoRecordsThroughOneStateMachine()
    {
        string iss = InstallerText("famo-setup.iss");

        Assert.Contains("JournalVersion = '2'", iss);
        Assert.Contains("function JournalDigest", iss);
        Assert.Contains("procedure TransitionTransactionPhase", iss);
        Assert.Contains("function LoadTransactionJournal", iss);
        foreach (string phase in new[]
        {
            "Prepared", "PayloadVerified", "ResumeArmed", "DetachIntent",
            "PendingReboot", "ActivateIntent", "MachineRegistered",
            "UserStateIntent", "VerifyIntent", "RollbackIntent", "Ready",
            "RolledBack",
        })
        {
            Assert.Contains($"Phase{phase} = '{phase}'", iss);
        }

        int transition = Position(iss, "procedure TransitionTransactionPhase");
        string transitionBody = iss[transition..Position(iss, "function LoadTransactionJournal", transition)];
        int writer = Position(iss, "procedure WriteJournalGeneration");
        string writerBody = iss[writer..Position(iss, "function ReadJournalValue", writer)];
        int digest = Position(writerBody, "'Digest'");
        int phaseCommit = Position(writerBody, "'Phase'", digest);
        Assert.True(digest < phaseCommit);
        int writeGeneration = Position(transitionBody, "WriteJournalGeneration");
        int readback = Position(transitionBody, "ValidateJournalGeneration", writeGeneration);
        int semanticReadback = Position(
            transitionBody, "ValidateCurrentJournalArtifact", readback);
        int pointerCommit = Position(
            transitionBody, "'ActiveGeneration'", semanticReadback);
        Assert.True(
            writeGeneration < readback &&
            readback < semanticReadback &&
            semanticReadback < pointerCommit);
        Assert.Contains(
            "ReadJournalGeneration(GenerationKey, Journal)", transitionBody);
        Assert.Contains("NextGeneration := JournalGeneration", transitionBody);
        Assert.Contains("NextGeneration := NextGeneration + 1", transitionBody);
        Assert.Contains("RegFlushKey", iss);
        Assert.DoesNotContain("RegWriteStringValue(HKLM64, JournalKey", iss[..transition]);
        Assert.Contains("JournalManifestHash", transitionBody);
        Assert.Contains("OriginalUserSid", transitionBody);
        Assert.Contains("JournalPendingFinalTarget", transitionBody);
        Assert.Contains("JournalPreviousFinalTarget", transitionBody);
        Assert.Contains("JournalAllowDowngrade", transitionBody);
    }

    [Fact]
    public void Installer_SeparatesHistoricalJournalStructureFromCurrentRecoveryArtifact()
    {
        string iss = InstallerText("famo-setup.iss");
        int structural = Position(iss, "function ValidateJournalSemantics");
        int current = Position(iss, "function ValidateCurrentJournalArtifact", structural);
        string structuralBody = iss[structural..current];
        string currentBody = iss[current..Position(iss, "function InspectJournalGenerations", current)];

        Assert.Contains("Journal.Version + '-'", structuralBody);
        Assert.Contains("Copy(Journal.ManifestHash, 1, 12)", structuralBody);
        Assert.Contains("ValidateJournalSemantics(Journal, ExpectedId)", currentBody);
        Assert.Contains("CompareText(Journal.Version, '{#AppVersion}')", currentBody);
        Assert.Contains("CompareText(Journal.ManifestHash, '{#ManifestHash}')", currentBody);

        int scan = Position(iss, "function FindRecoverableTransaction");
        string scanBody = iss[scan..Position(iss, "procedure CleanupAllValidatedRecoveryArtifacts", scan)];
        Assert.Contains(
            "ValidateRecoverableJournalArtifact(Journal, Id)", scanBody);
        Assert.Contains("ValidateCurrentJournalArtifact(Journal, Id)", scanBody);
        Assert.Contains("unfinished transaction requires its retained installer", scanBody);
    }

    [Fact]
    public void Installer_FailsClosedOnMalformedGenerationNamesAndCompleteRecords()
    {
        string iss = InstallerText("famo-setup.iss");
        int declaration = Position(iss, "function InspectJournalGenerations");
        int inspect = Position(iss, "function InspectJournalGenerations", declaration + 1);
        string body = iss[inspect..Position(iss, "function AdoptCompleteOrphanGeneration", inspect)];

        Assert.Contains("(Names[I] <> 'g' + IntToStr(Generation))", body);
        Assert.Contains("RegQueryStringValue(HKLM64, Key, 'Phase'", body);
        Assert.Contains("not ReadJournalGeneration(Key, Candidate)", body);
        Assert.Contains("not ValidateJournalSemantics(Candidate, Id)", body);
        Assert.Contains("unreachable crash remnant", body);
    }

    [Fact]
    public void Installer_ArmsAndReadsBackOneExactSidRecoveryTaskBeforeDetach()
    {
        string iss = InstallerText("famo-setup.iss");
        int schedule = Position(iss, "procedure ScheduleRecoveryTask");
        string scheduleBody = iss[schedule..Position(iss, "procedure DeleteRecoveryTask", schedule)];

        foreach (string value in new[]
        {
            "<LogonTrigger>", "<UserId>' + OriginalUserSid + '</UserId>",
            "<LogonType>InteractiveToken</LogonType>",
            "<RunLevel>HighestAvailable</RunLevel>",
            "' /Create /TN '", "' /Query /TN '", "ExecAndCaptureOutput",
        })
        {
            Assert.Contains(value, scheduleBody);
        }
        Assert.Contains("function ExpectedRecoveryArguments", iss);
        Assert.Contains("'/FamoRecover=' + Id", iss);
        Assert.Contains("' /FamoManifest=' + JournalManifestHash", iss);
        Assert.Contains("' /FamoVersion=' + JournalAppVersion", iss);
        Assert.Contains("ValidateRecoveryTaskXml", scheduleBody);
        Assert.Contains(
            "SaveStringsToUTF16LEFile(TaskXml, Lines)",
            scheduleBody);
        Assert.DoesNotContain(
            "SaveStringsToUTF8File",
            scheduleBody);
        Assert.Contains(
            "'<?xml version=\"1.0\" encoding=\"UTF-16\"?>'",
            scheduleBody);
        Assert.Contains("TransitionTransactionPhase(PhaseResumeArmed)", scheduleBody);
        int ensureFolder = Position(scheduleBody, "EnsureRecoveryTaskFolderByCom");
        int proveFresh = Position(
            scheduleBody, "RecoveryTaskExistsByCom(TaskName, TaskExists)",
            ensureFolder);
        int claimTask = Position(scheduleBody, "JournalTaskName := TaskName", proveFresh);
        int persistIntent = Position(
            scheduleBody, "TransitionTransactionPhase(PhaseResumeArmed)",
            claimTask);
        int createTask = Position(scheduleBody, "' /Create /TN '", persistIntent);
        int comReadback = Position(
            scheduleBody, "RecoveryTaskExistsByCom(TaskName, TaskExists)",
            createTask);
        int xmlReadback = Position(scheduleBody, "' /Query /TN '", comReadback);
        int validateXml = Position(scheduleBody, "ValidateRecoveryTaskXml", xmlReadback);
        Assert.True(
            ensureFolder < proveFresh &&
            proveFresh < claimTask &&
            claimTask < persistIntent &&
            persistIntent < createTask &&
            createTask < comReadback &&
            comReadback < xmlReadback &&
            xmlReadback < validateXml);
        Assert.DoesNotContain("' /XML ' + AddQuotes(TaskXml) + ' /F'", scheduleBody);

        int folder = Position(iss, "function EnsureRecoveryTaskFolderByCom");
        string folderBody = iss[folder..Position(iss, "function ValidateRecoveryArtifactPath", folder)];
        Assert.Contains("RootFolder.CreateFolder('Famo', ExpectedSddl)", folderBody);
        Assert.Contains("RecoveryTaskFolderSecurityDescriptor", folderBody);
        Assert.Contains("Folder.GetSecurityDescriptor(", folderBody);
        Assert.Contains("(Tasks.Count = 0) and (Subfolders.Count = 0)", folderBody);
        Assert.Contains(
            "'D:PAI(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;' +",
            iss);
        Assert.Contains("OriginalUserSid + ')'", iss);
        int utf16Writer = Position(
            iss, "function SaveStringsToUTF16LEFile");
        string utf16WriterBody = iss[
            utf16Writer..Position(
                iss, "procedure ScheduleRecoveryTask", utf16Writer)];
        Assert.Contains("#$FEFF", utf16WriterBody);
        Assert.Contains("TFileStream.Create(FileName, fmCreate)", utf16WriterBody);
        Assert.Contains("Length(Text) * 2", utf16WriterBody);

        int taskValidator = Position(iss, "function ValidateRecoveryTaskXml");
        string taskValidatorBody = iss[taskValidator..Position(iss, "function ExpectedRecoveryDirectory", taskValidator)];
        foreach (string exact in new[]
        {
            "<SecurityDescriptor>", "<Principal id=\"OriginalUser\">",
            "<Actions Context=\"OriginalUser\">", "<Enabled>true</Enabled>",
            "<IdleTrigger>", "<CalendarTrigger>", "<SessionStateChangeTrigger>",
            "<SendEmail>", "<ShowMessage>",
        })
        {
            Assert.Contains(exact, taskValidatorBody);
        }
        Assert.Contains("ExtractUniqueXmlElement(Xml, '<LogonTrigger>'", taskValidatorBody);
        Assert.Contains("ExtractUniqueXmlElement(Xml, '<Settings>'", taskValidatorBody);
        Assert.Contains(
            "ExtractUniqueXmlElement(Xml, '<Principal id=\"OriginalUser\">'",
            taskValidatorBody);
        Assert.Contains("XmlEscape(OriginalUserAccount)", taskValidatorBody);
        Assert.Contains(
            "(CountText(TriggerElement, '<Enabled>') <= 1)",
            taskValidatorBody);
        Assert.Contains(
            "(CountText(SettingsElement, '<Enabled>') <= 1)",
            taskValidatorBody);
        Assert.Contains("(CountText(Xml, '<Enabled>false</Enabled>') = 0)", taskValidatorBody);
        Assert.DoesNotContain(
            "(CountText(Xml, '<Enabled>true</Enabled>') = 2)",
            taskValidatorBody);
        Assert.DoesNotContain("RunOnceKey", iss);
        Assert.DoesNotContain("FamoResumePending", iss);

        int enterPending = Position(iss, "procedure EnterPendingReboot");
        string pendingBody = iss[enterPending..Position(iss, "procedure StartRuntimeAsOriginalUser", enterPending)];
        int arm = Position(pendingBody, "ScheduleRecoveryTask");
        int detach = Position(pendingBody, "TransitionTransactionPhase(PhaseDetachIntent)", arm);
        int switchAway = Position(pendingBody, "'switch-away'", detach);
        Assert.True(arm < detach && detach < switchAway);
        Assert.Contains("if not OriginalUserResumeCapable then", pendingBody);
    }

    [Fact]
    public void Installer_SplitsMachineRegistrationFromExactUserEnablement()
    {
        string iss = InstallerText("famo-setup.iss");
        string tool = RepoText("native/windows-tsf-famo/text-service/tools/dev_profile_main.cpp");
        string registration = RepoText("native/windows-tsf-famo/text-service/src/registration.cpp");
        string exports = RepoText("native/windows-tsf-famo/text-service/src/FamoTextService.def");

        Assert.Contains("DllRegisterMachine", exports);
        Assert.Contains("HRESULT RegisterMachineProfile()", registration);
        Assert.Contains("RegisterTsfProfile(false)", registration);
        Assert.Contains("if (enable_current_user)", registration);
        Assert.Contains("register-machine", tool);
        Assert.Contains("check-machine", tool);

        int registerTarget = Position(iss, "function RegisterTarget");
        string registerBody = iss[registerTarget..Position(iss, "function UnregisterTarget", registerTarget)];
        Assert.Contains("'register-machine-at '", registerBody);
        Assert.Contains("'check-machine'", registerBody);
        Assert.DoesNotContain("'register-machine'", registerBody);

        int unregisterPrevious = Position(
            iss, "function UnregisterPreviousRegistration");
        int registerPrevious = Position(
            iss, "function RegisterPreviousRegistration", unregisterPrevious);
        string unregisterPreviousBody = iss[unregisterPrevious..registerPrevious];
        string registerPreviousBody = iss[registerPrevious..Position(
            iss, "function DetectLoadedPreviousHost", registerPrevious)];
        Assert.Contains(
            "'unregister-machine-at ' + AddQuotes(PreviousHost)",
            unregisterPreviousBody);
        Assert.Contains(
            "Result := RunRegSvr32(PreviousHost, True)",
            unregisterPreviousBody);
        Assert.Contains(
            "'register-machine-at ' + AddQuotes(PreviousHost)",
            registerPreviousBody);
        Assert.Contains(
            "Result := RunRegSvr32(PreviousHost, False)",
            registerPreviousBody);

        int userState = Position(iss, "procedure InstallUserState");
        string userBody = iss[userState..Position(iss, "procedure RollbackTransaction", userState)];
        int prepare = Position(userBody, "--prepare-seed-transaction");
        int clear = Position(
            userBody, "'clear-user-com-shadow ' + OriginalUserSid", prepare);
        int enable = Position(userBody, "'enable', True", clear);
        int apply = Position(userBody, "--apply-seed-transaction", enable);
        Assert.True(prepare < clear && clear < enable && enable < apply);
    }

    [Fact]
    public void Installer_RejectsDowngradesUnlessTheFreshJournalRecordsConsent()
    {
        string iss = InstallerText("famo-setup.iss");
        int policy = Position(iss, "procedure CheckDowngradePolicy");
        string policyBody = iss[policy..Position(iss, "procedure SwitchRegistration", policy)];

        Assert.Contains(
            "StrToVersion(JournalAppVersion + '.0', NewVersion)",
            policyBody);
        Assert.Contains(
            "StrToVersion(PreviousVersion + '.0', PreviousPackedVersion)",
            policyBody);
        Assert.DoesNotContain("GetPackedVersion", policyBody);
        Assert.Contains("ComparePackedVersion(NewVersion, PreviousPackedVersion) < 0", policyBody);
        Assert.Contains("if not JournalAllowDowngrade then", policyBody);
        Assert.Contains("{param:FamoAllowDowngrade|}", iss);

        int postInstall = Position(iss, "if CurStep = ssPostInstall");
        string body = iss[postInstall..Position(iss, "function NeedRestart", postInstall)];
        int verified = Position(body, "TransitionTransactionPhase(PhasePayloadVerified)");
        int check = Position(body, "CheckDowngradePolicy", verified);
        int intent = Position(body, "SwitchRegistration", check);
        Assert.True(verified < check && check < intent);
    }

    [Fact]
    public void Installer_AcceptsObservedActiveButDisabledProfileSnapshots()
    {
        string iss = InstallerText("famo-setup.iss");
        string health = RepoText(
            "native/windows-tsf-famo/weasel-fork/tests/Test-FamoHealth.ps1");
        int semantics = Position(iss, "function ValidateJournalSemantics");
        string semanticsBody = iss[
            semantics..Position(
                iss, "function ValidateCurrentJournalArtifact", semantics)];
        int capture = Position(iss, "procedure CapturePreviousUserState");
        string captureBody = iss[
            capture..Position(iss, "function ReadPreparedSeedReceiptHash", capture)];

        Assert.Contains("PreviousProfileActive = '1'", semanticsBody);
        Assert.Contains("PreviousProfileEnabled = '1'", semanticsBody);
        Assert.DoesNotContain(
            "(Journal.PreviousProfileActive <> '1')",
            semanticsBody);
        Assert.Contains("'is-active'", captureBody);
        Assert.Contains("'is-enabled'", captureBody);
        Assert.DoesNotContain(
            "[string]$record.PreviousProfileActive -eq '1' -and",
            health);
    }

    [Fact]
    public void Installer_CapturesNoPreviousUserStateWhenThereIsNoPreviousInstall()
    {
        string iss = InstallerText("famo-setup.iss");
        int capture = Position(iss, "procedure CapturePreviousUserState");
        string captureBody = iss[
            capture..Position(iss, "function ReadPreparedSeedReceiptHash", capture)];
        int semantics = Position(iss, "function ValidateJournalSemantics");
        string semanticsBody = iss[
            semantics..Position(
                iss, "function ValidateCurrentJournalArtifact", semantics)];

        // A first install must short-circuit before the probes: an empty
        // PreviousTarget has no user state, and the probes run through a
        // broker that cannot distinguish "no" from "could not tell".
        int guard = Position(captureBody, "if PreviousTarget = '' then");
        Assert.True(guard < Position(captureBody, "'is-active'"));
        Assert.True(guard < Position(captureBody, "'is-enabled'"));
        Assert.True(guard < Position(captureBody, "'--is-input-tip'"));
        foreach (string reset in new[]
        {
            "PreviousProfileActive := False;",
            "PreviousProfileEnabled := False;",
            "PreviousInputTipPresent := False;",
        })
        {
            Assert.True(guard < Position(captureBody, reset));
        }

        // The journal predicate stays an enforced invariant rather than being
        // relaxed to tolerate the flags this bug used to record.
        Assert.Contains(
            "((Journal.PreviousTarget <> '') or", semanticsBody);
        Assert.Contains(
            "((Journal.PreviousProfileActive = '0') and", semanticsBody);
        Assert.Contains(
            "(Journal.PreviousProfileEnabled = '0') and", semanticsBody);
        Assert.Contains(
            "(Journal.PreviousInputTipPresent = '0')))", semanticsBody);
    }

    [Fact]
    public void Installer_AcceptsAProtectedSeedReceiptFile()
    {
        string iss = InstallerText("famo-setup.iss");
        int guard = Position(iss, "function ValidateProtectedFile");
        string guardBody = iss[
            guard..Position(iss, "procedure RequireFixedProtectedInstallRoot", guard)];
        int receipt = Position(iss, "function ReadPreparedSeedReceiptHash");
        string receiptBody = iss[
            receipt..Position(iss, "procedure InstallUserState", receipt)];
        string compactGuard = string.Concat(
            guardBody.Where(character => !char.IsWhiteSpace(character)));

        Assert.Contains(
            "(ChildAttributesand(FileAttributeDirectoryorFileAttributeReparsePoint))<>0",
            compactGuard);
        Assert.Contains(
            "PathSame(ExtractFileDir(ChildFinalPath), ParentFinalPath)",
            guardBody);
        Assert.Contains(
            "ValidateProtectedFile(TransactionDirectory, 'receipt.json')",
            receiptBody);
        Assert.DoesNotContain(
            "ValidateProtectedChild(TransactionDirectory, 'receipt.json')",
            receiptBody);
    }

    [Fact]
    public void Installer_RollbackRestoresProfileOnlyAfterMutationIntent()
    {
        string iss = InstallerText("famo-setup.iss");
        int rollback = Position(iss, "procedure RollbackTransaction");
        string rollbackBody = iss[
            rollback..Position(
                iss, "function RetryRolledBackCleanupDebt", rollback)];

        int derive = Position(
            rollbackBody, "HadProfileMutationIntent :=");
        int rollbackIntent = Position(
            rollbackBody,
            "TransitionTransactionPhase(PhaseRollbackIntent)",
            derive);
        int profileGate = Position(
            rollbackBody, "if HadProfileMutationIntent then", rollbackIntent);
        int profileRestore = Position(
            rollbackBody, "EnableCommand := 'enable'", profileGate);
        int runtimeRestore = Position(
            rollbackBody, "RunBoundDesktopExitCode(PreviousServer", profileRestore);

        Assert.True(
            derive < rollbackIntent &&
            rollbackIntent < profileGate &&
            profileGate < profileRestore &&
            profileRestore < runtimeRestore);
        Assert.Contains("JournalPhase = PhaseDetachIntent", rollbackBody);
        Assert.Contains("JournalPhase = PhasePendingReboot", rollbackBody);
        Assert.Contains("JournalPhase = PhaseActivateIntent", rollbackBody);
        Assert.Contains("JournalPhase = PhaseMachineRegistered", rollbackBody);
    }

    [Fact]
    public void Installer_DoesNotArmUserRollbackWhenSeedPrepareProducedNoReceipt()
    {
        string iss = InstallerText("famo-setup.iss");
        int rollback = Position(iss, "procedure RollbackTransaction");
        string rollbackBody = iss[
            rollback..Position(
                iss, "function RetryRolledBackCleanupDebt", rollback)];
        int derive = Position(rollbackBody, "HadUserStateIntent :=");
        string derivation = rollbackBody[
            derive..Position(rollbackBody, "HadProfileMutationIntent :=", derive)];

        Assert.Contains("(SeedReceiptHash <> '')", derivation);
        Assert.DoesNotContain(
            "(JournalPhase = PhaseUserStateIntent)", derivation);
        Assert.Contains(
            "(JournalPhase = PhaseUserStatePrepared)", derivation);
    }

    [Fact]
    public void Installer_ReceiptlessRollbackDebtOnlyDiscardsPrepareAndRestartsRuntime()
    {
        string iss = InstallerText("famo-setup.iss");
        int retry = Position(iss, "function RetryRolledBackCleanupDebt");
        string retryBody = iss[
            retry..Position(
                iss, "procedure CleanupObsoleteVersions", retry)];
        int receiptless = Position(
            retryBody, "if HasUserDebt and (SeedReceiptHash = '') then");
        string receiptlessBody = retryBody[
            receiptless..Position(
                retryBody, "else if HasUserDebt then", receiptless)];

        Assert.Contains("--discard-seed-transaction", receiptlessBody);
        Assert.Contains(
            "RunBoundDesktopExitCode(PreviousServer", receiptlessBody);
        Assert.DoesNotContain("--remove-input-tip", receiptlessBody);
        Assert.DoesNotContain("cleanup-user-state", receiptlessBody);
    }

    [Fact]
    public void Installer_LogsTheOriginalFailureBeforeRollbackCompensation()
    {
        string iss = InstallerText("famo-setup.iss");
        int changed = Position(iss, "procedure CurStepChanged");
        string changedBody = iss[
            changed..Position(iss, "function NeedRestart", changed)];
        int capture = Position(
            changedBody, "Failure := GetExceptionMessage");
        int log = Position(
            changedBody,
            "Log('installation failed before rollback: ' + Failure)",
            capture);
        int rollback = Position(
            changedBody, "RollbackTransaction", log);
        int raise = Position(
            changedBody, "RaiseException(Failure)", rollback);
        Assert.True(capture < log && log < rollback && rollback < raise);
    }

    [Fact]
    public void SilentRecovery_AbortsWithoutBlockingAfterDurableRollback()
    {
        string iss = InstallerText("famo-setup.iss");
        int changed = Position(iss, "procedure CurStepChanged");
        string changedBody = iss[
            changed..Position(iss, "function NeedRestart", changed)];

        int rollbackTry = Position(changedBody, "try", Position(
            changedBody, "installation failed before rollback"));
        int rollback = Position(changedBody, "RollbackTransaction", rollbackTry);
        int compensationLog = Position(
            changedBody, "rollback compensation failed", rollback);
        int ready = Position(changedBody, "InstallReady := True", compensationLog);
        int silent = Position(changedBody, "if WizardSilent then", ready);
        int silentLog = Position(
            changedBody, "silent setup aborted after durable rollback", silent);
        int abort = Position(changedBody, "Abort;", silentLog);
        int raise = Position(changedBody, "RaiseException(Failure);", abort);

        Assert.True(
            rollbackTry < rollback &&
            rollback < compensationLog &&
            compensationLog < ready &&
            ready < silent &&
            silent < silentLog &&
            silentLog < abort &&
            abort < raise);
        Assert.Equal(
            raise,
            changedBody.LastIndexOf(
                "RaiseException(Failure);",
                StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public void Installer_RejectsDirOverridesAndReparseProtectedRootsBeforeCapture()
    {
        string iss = InstallerText("famo-setup.iss");
        int fixedRoot = Position(iss, "function FixedInstallRoot");
        int rootGuard = Position(iss, "procedure RequireFixedProtectedInstallRoot");
        int selectedRootGuard = Position(iss, "procedure RequireSelectedFixedInstallRoot", rootGuard);
        string fixedRootBody = iss[fixedRoot..rootGuard];
        string rootGuardBody = iss[rootGuard..selectedRootGuard];
        string selectedRootBody = iss[selectedRootGuard..Position(
            iss, "procedure CaptureOriginalUserIdentity", selectedRootGuard)];

        Assert.Contains("NormalizeDirectoryPath(ExpandConstant('{autopf}\\Famo'))", fixedRootBody);
        Assert.Contains("AppRoot := FixedInstallRoot", rootGuardBody);
        Assert.Contains("PathIsNonReparseOrMissing", rootGuardBody);
        Assert.Contains("ValidateProtectedChild", rootGuardBody);
        Assert.Contains("NormalizeDirectoryPath(ExpandConstant('{app}'))", selectedRootBody);
        Assert.Contains("if not PathSame(SelectedRoot, FixedInstallRoot) then", selectedRootBody);
        Assert.Equal(1, iss.Split("ExpandConstant('{app}')").Length - 1);

        int initialize = Position(iss, "function InitializeSetup");
        string initializeBody = iss[initialize..Position(iss, "procedure CurStepChanged", initialize)];
        Assert.Contains("RequireFixedProtectedInstallRoot", initializeBody);
        Assert.DoesNotContain("ExpandConstant('{app}')", initializeBody);
        Assert.True(
            Position(initializeBody, "RequireFixedProtectedInstallRoot") <
            Position(initializeBody, "LoadPendingState"));

        int prepare = Position(iss, "procedure PrepareTransaction");
        string prepareBody = iss[prepare..Position(iss, "procedure SwitchRegistration", prepare)];
        Assert.True(
            Position(prepareBody, "RequireSelectedFixedInstallRoot") <
            Position(prepareBody, "EnsureTransactionTarget"));
    }

    [Fact]
    public void Installer_ClearsTheExactUsersLegacyComShadowBeforeEnablement()
    {
        string iss = InstallerText("famo-setup.iss");
        string tool = RepoText("native/windows-tsf-famo/text-service/tools/dev_profile_main.cpp");
        string registration = RepoText("native/windows-tsf-famo/text-service/src/registration.cpp");

        Assert.Contains("ClearCurrentUserComShadow", tool);
        Assert.Contains("CurrentProcessTokenMatchesSid", tool);
        Assert.Contains("clear-user-com-shadow", tool);
        Assert.Contains("RegDeleteTreeW(HKEY_CURRENT_USER, com.c_str())", tool);
        Assert.Contains("!UserKeyPresent(com)", tool);

        int userState = Position(iss, "procedure InstallUserState");
        string userBody = iss[userState..Position(iss, "procedure RollbackTransaction", userState)];
        int clearShadow = Position(userBody, "'clear-user-com-shadow ' + OriginalUserSid");
        int enable = Position(userBody, "'enable', True", clearShadow);
        Assert.True(clearShadow < enable);

        int machine = Position(registration, "HRESULT RegisterMachineProfile()");
        string machineBody = registration[machine..Position(registration, "HRESULT UnregisterDevelopmentProfile", machine)];
        Assert.Contains("RegisterComServer(false)", machineBody);
        Assert.Contains("RegisterTsfProfile(false)", machineBody);
        Assert.DoesNotContain("HKEY_CURRENT_USER", machineBody);
    }

    [Fact]
    public void Installer_RecoversPreparedAndTreatsTerminalRecoverAsNoOp()
    {
        string iss = InstallerText("famo-setup.iss");
        int prepare = Position(iss, "procedure PrepareTransaction");
        string prepareBody = iss[prepare..Position(iss, "procedure CheckDowngradePolicy", prepare)];
        Assert.Contains("(JournalPhase <> PhasePrepared) and", prepareBody);
        Assert.Contains("not DirExists(TransactionTarget)", prepareBody);
        int snapshot = Position(prepareBody, "SnapshotPreviousState");
        int clearResumeInstaller = Position(
            prepareBody, "JournalResumeInstaller := ''", snapshot);
        int clearResumeHash = Position(
            prepareBody, "JournalResumeInstallerHash := ''", clearResumeInstaller);
        int clearTask = Position(
            prepareBody, "JournalTaskName := ''", clearResumeHash);
        int prepared = Position(
            prepareBody, "TransitionTransactionPhase(PhasePrepared)", clearTask);
        Assert.True(
            snapshot < clearResumeInstaller &&
            clearResumeInstaller < clearResumeHash &&
            clearResumeHash < clearTask &&
            clearTask < prepared);

        int initialize = Position(iss, "function InitializeSetup");
        string initializeBody = iss[initialize..Position(iss, "procedure CurStepChanged", initialize)];
        Assert.Contains("(JournalPhase = PhaseReady) or", initializeBody);
        Assert.Contains("(JournalPhase = PhaseRolledBack)", initializeBody);
        Assert.Contains("RecoverTerminalTransaction", initializeBody);
        Assert.Contains("Result := False", initializeBody);
    }

    [Fact]
    public void Installer_DerivesAndValidatesRecoveryArtifactsAcrossTheStagingGap()
    {
        string iss = InstallerText("famo-setup.iss");
        int cleanup = Position(iss, "procedure DeleteRecoveryTask");
        string cleanupBody = iss[cleanup..Position(iss, "procedure WritePendingRegistry", cleanup)];

        Assert.Contains("ExpectedRecoveryTaskName(TransactionId)", cleanupBody);
        Assert.Contains("ExpectedRecoveryInstaller(TransactionId)", cleanupBody);
        Assert.Contains("' /Query /TN '", cleanupBody);
        Assert.Contains("ValidateRecoveryTaskXml", cleanupBody);
        Assert.Contains("ValidateRecoveryArtifactPath", cleanupBody);
        Assert.Contains("JournalOwnsRecoveryTask(TaskName)", cleanupBody);
        Assert.Contains("DisableAndDeleteOwnedRecoveryTaskByCom(TaskName)", cleanupBody);
        Assert.Contains("Owned recovery task XML is invalid", cleanupBody);
        Assert.True(
            Position(cleanupBody, "ValidateRecoveryTaskXml") <
            Position(cleanupBody, "DisableAndDeleteOwnedRecoveryTaskByCom"));
    }

    [Fact]
    public void Installer_OnlyTreatsARecoveryTaskAsAbsentAfterComEnumeration()
    {
        string iss = InstallerText("famo-setup.iss");
        int enumerate = Position(iss, "function RecoveryTaskExistsByCom");
        int ownership = Position(iss, "function JournalOwnsRecoveryTask", enumerate);
        int cleanup = Position(iss, "procedure DeleteRecoveryTask", enumerate);
        string enumerateBody = iss[enumerate..cleanup];
        string enumerateOnly = iss[enumerate..ownership];
        string cleanupBody = iss[cleanup..Position(iss, "procedure WritePendingRegistry", cleanup)];

        foreach (string contract in new[]
        {
            "CreateOleObject('Schedule.Service')", "Service.Connect",
            "RootFolder.GetFolders(0)", "Folder.GetTasks(1)",
            "CompareText(Task.Path, TaskName)", "if Matches > 1 then Exit",
            "RecoveryTaskFolderSecurityDescriptor", "Subfolders.Count <> 0",
            "if CompareText(Task.Path, TaskName) <> 0 then Exit",
            "Candidate := Folders.Item(I)", "Folder := Candidate",
        })
        {
            Assert.Contains(contract, enumerateBody);
        }
        Assert.DoesNotContain("Folder := Folders.Item(I)", enumerateOnly);

        int confirmPresent = Position(
            cleanupBody, "RecoveryTaskExistsByCom(TaskName, TaskExists)");
        int queryXml = Position(cleanupBody, "' /Query /TN '", confirmPresent);
        int deleteTask = Position(
            cleanupBody, "DisableAndDeleteOwnedRecoveryTaskByCom(TaskName)",
            queryXml);
        int confirmAbsent = Position(
            cleanupBody, "RecoveryTaskExistsByCom(TaskName, TaskExists)", deleteTask);
        int removeFolder = Position(
            cleanupBody, "CleanupEmptyRecoveryTaskFolderByCom", confirmAbsent);
        Assert.True(
            confirmPresent < queryXml &&
            queryXml < deleteTask &&
            deleteTask < confirmAbsent &&
            confirmAbsent < removeFolder);
        Assert.Contains(
            "RaiseException('cannot enumerate recovery tasks during cleanup')",
            cleanupBody);
        Assert.Contains(
            "RaiseException('recovery task deletion readback failed')",
            cleanupBody);
        Assert.Contains("Task.Enabled := False", enumerateBody);
        Assert.Contains("Folder.DeleteTask(TaskLeaf, 0)", enumerateBody);
        Assert.Contains("RootFolder.DeleteFolder('Famo', 0)", enumerateBody);
        Assert.Contains("Tasks.Count <> 0", enumerateBody);
        Assert.Contains("Subfolders.Count <> 0", enumerateBody);
    }

    [Fact]
    public void Installer_RetainsOnlyVerifiedActiveAndPreviousVersionObjects()
    {
        string iss = InstallerText("famo-setup.iss");
        int cleanup = Position(iss, "procedure CleanupObsoleteVersions");
        string cleanupBody = iss[cleanup..Position(iss, "procedure VerifyActiveInstall", cleanup)];

        Assert.Contains("TryGetFinalObjectInfo", cleanupBody);
        Assert.Contains("FinalObjectsSame", cleanupBody);
        Assert.Contains("ValidateVersionDirectoryForCleanup", cleanupBody);
        Assert.Contains("FileAttributeReparsePoint", cleanupBody);
        Assert.Contains("CleanupDebt", cleanupBody);
        Assert.Contains("CleanupDebtCount", cleanupBody);
        Assert.Contains("VerifyManagedPayloadForCleanup", iss);
        Assert.Contains("ParseFileEntryDetailed", iss);
        Assert.Contains("VerifyActualPayloadFiles", iss);
        Assert.Contains("DelTree(VersionTarget, True, True, True)", cleanupBody);

        foreach (string terminalFlow in new[] { "procedure CompletePendingTransaction", "if CurStep = ssPostInstall" })
        {
            int at = Position(iss, terminalFlow);
            string body = iss[at..];
            int ready = Position(body, "TransitionTransactionPhase(PhaseReady)");
            int prune = Position(body, "CleanupObsoleteVersions", ready);
            Assert.True(ready < prune);
        }
    }

    [Fact]
    public void Installer_PreservesThePreviousVersionsRollbackPointerUntilReady()
    {
        string iss = InstallerText("famo-setup.iss");
        foreach (string value in new[]
        {
            "PriorPreviousTarget", "PriorPreviousFinalTarget",
            "PriorPreviousObjectId",
        })
        {
            Assert.Contains(value, iss);
        }

        int snapshot = Position(iss, "procedure SnapshotPreviousState");
        string snapshotBody = iss[snapshot..Position(iss, "function FindPathInList", snapshot)];
        Assert.Contains("'PreviousTarget',", snapshotBody);

        int restore = Position(iss, "procedure RestorePreviousRegistry");
        string restoreBody = iss[restore..Position(iss, "function NormalizeDirectoryPath", restore)];
        Assert.Contains("WriteOrDelete('PreviousTarget', PriorPreviousTarget)", restoreBody);
        Assert.DoesNotContain("RegDeleteKeyIncludingSubkeys(HKLM64, BrandKey)", restoreBody);

        int apply = Position(iss, "procedure ApplyTransactionJournal");
        string applyBody = iss[apply..Position(iss, "function LoadTransactionJournal", apply)];
        Assert.Equal(
            1,
            applyBody.Split(
                "PriorPreviousTarget := Journal.PriorPreviousTarget",
                StringSplitOptions.None).Length - 1);
    }

    [Fact]
    public void Installer_UninstallValidatesEveryJournalTaskBeforeDeletingTheJournal()
    {
        string iss = InstallerText("famo-setup.iss");
        int cleanupAll = Position(iss, "procedure CleanupAllValidatedRecoveryArtifacts");
        string cleanupBody = iss[cleanupAll..Position(iss, "function InitializeSetup", cleanupAll)];
        Assert.Contains("RegGetSubkeyNames", cleanupBody);
        Assert.Contains("ValidateJournalSemantics", cleanupBody);
        Assert.Contains("DeleteRecoveryTask", cleanupBody);
        Assert.Contains("foreign unfinished transaction blocks uninstall", cleanupBody);
        Assert.Contains("RecoveryTaskFolderAbsentByCom", cleanupBody);
        Assert.Contains("foreign recovery task folder blocks uninstall", cleanupBody);
        Assert.True(
            Position(cleanupBody, "DeleteRecoveryTask") <
            Position(cleanupBody, "RecoveryTaskFolderAbsentByCom",
                Position(cleanupBody, "DeleteRecoveryTask")));

        int post = Position(iss, "procedure CurUninstallStepChanged");
        string postBody = iss[post..];
        int cleanup = Position(postBody, "CleanupAllValidatedRecoveryArtifacts");
        int deleteJournal = Position(postBody,
            "RegDeleteKeyIncludingSubkeys(HKLM64, BrandKey)", cleanup);
        Assert.True(cleanup < deleteJournal);
        Assert.Contains("RequireFixedProtectedInstallRoot", postBody);
        Assert.Contains("ValidateCleanupTree", postBody);
        Assert.DoesNotContain(
            @"ExpandConstant('{localappdata}\Famo')",
            postBody,
            StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void BuildInstaller_ConsumesStableRuntimeAndBridgeArtifacts()
    {
        string script = InstallerText("build-installer.ps1");

        Assert.Contains("$NativeOutput", script);
        Assert.Contains("Join-Path $NativeOutput $Configuration", script);
        Assert.Contains("FAMO_IDENTITY:STRING=Stable", script);
        foreach (string file in new[] { "FamoTextService.dll", "FamoRuntime.exe", "FamoRimeEngine.dll", "FamoProfileTool.exe", "rime.dll" })
        {
            Assert.Contains(file, script);
        }
        Assert.Contains("$BridgeArtifact", script);

        foreach (string forbidden in new[] { "$WeaselOutput", "$EngineDll", "weaselx64.dll", "WeaselServer.exe", "WeaselDeployer.exe", "FamoDeploy.exe" })
        {
            Assert.DoesNotContain(forbidden, script, StringComparison.OrdinalIgnoreCase);
        }
        Assert.Contains("Need (Join-Path $settingsStage 'WinSparkle.dll')", script);
    }

    [Fact]
    public void Installer_NeverRewritesAnExistingFrozenBridgeArtifact()
    {
        string iss = InstallerText("famo-setup.iss");
        string[] bridgeEntries = iss
            .Split('\n')
            .Where(line =>
                line.StartsWith(@"Source: ""{#StagingDir}\bridge\", StringComparison.Ordinal))
            .ToArray();

        Assert.Equal(2, bridgeEntries.Length);
        Assert.All(
            bridgeEntries,
            entry => Assert.Contains("onlyifdoesntexist", entry));
    }

    [Fact]
    public void Installer_RejectsAConflictingFrozenBridgeBeforePreparingTheTransaction()
    {
        string iss = InstallerText("famo-setup.iss");
        int preflight = Position(iss, "procedure VerifyFrozenBridgePreflight");
        string preflightBody = iss[preflight..Position(
            iss, "function TransactionChangedBridge", preflight)];

        Assert.Contains("FileExists(FixedBridgeDll)", preflightBody);
        Assert.Contains("GetSHA256OfFile(FixedBridgeDll)", preflightBody);
        Assert.Contains("'{#BridgeHash}'", preflightBody);
        Assert.Contains("frozen Bridge conflict", preflightBody);

        int install = Position(iss, "if CurStep = ssInstall then");
        string installBody = iss[install..Position(
            iss, "if CurStep = ssPostInstall then", install)];
        Assert.True(
            Position(installBody, "VerifyFrozenBridgePreflight") <
            Position(installBody, "PrepareTransaction"));
    }

    [Fact]
    public void TerminalRecovery_ValidatesPayloadAgainstRecoveredJournal()
    {
        string iss = InstallerText("famo-setup.iss");
        int verifyPayload = Position(iss, "procedure VerifyPayloadOrFail");
        string body = iss[verifyPayload..Position(
            iss,
            "function CachedCurrentPayloadExecutionProofMatches",
            verifyPayload)];

        Assert.Contains(
            "CompareText(ActualHash, JournalManifestHash)",
            body);
        Assert.Contains(
            "Copy(JournalManifestHash, 1, 12)",
            body);
        Assert.Contains(
            "Lines[I] = 'version=' + JournalAppVersion",
            body);
        Assert.DoesNotContain(
            "CompareText(ActualHash, '{#ManifestHash}')",
            body);
        Assert.DoesNotContain(
            "Lines[I] = 'version={#AppVersion}'",
            body);
    }

    [Fact]
    public void TerminalRecovery_LogsTheRejectedPendingStateStage()
    {
        string iss = InstallerText("famo-setup.iss");
        int load = Position(iss, "function LoadPendingState");
        string body = iss[load..Position(
            iss,
            "function InspectJournalGenerations",
            load)];

        Assert.Contains("pending state load rejected: journal", body);
        Assert.Contains("pending state load rejected: target path", body);
        Assert.Contains("pending state load rejected: target object", body);
        Assert.Contains(
            "pending state load rejected: previous target identity",
            body);
        Assert.Contains(
            "pending state load rejected: recovery installer identity",
            body);
    }

    [Fact]
    public void TerminalRecovery_AllowsOnlyAReadyJournalToReuseTheActiveTarget()
    {
        string iss = InstallerText("famo-setup.iss");
        int validate = Position(iss, "function ValidateTransactionTarget");
        string validateBody = iss[validate..Position(
            iss,
            "procedure PrepareTransaction",
            validate)];
        int load = Position(iss, "function LoadPendingState");
        string loadBody = iss[load..Position(
            iss,
            "function InspectJournalGenerations",
            load)];
        int prepare = Position(iss, "procedure PrepareTransaction");
        string prepareBody = iss[prepare..Position(
            iss,
            "procedure CheckDowngradePolicy",
            prepare)];

        Assert.Contains("AllowCurrentActiveTarget: Boolean", validateBody);
        Assert.Contains(
            "(not AllowCurrentActiveTarget) and",
            validateBody);
        Assert.Contains(
            "JournalPhase = PhaseReady, NormalizedTarget",
            loadBody);
        Assert.Contains("False, ValidatedTarget", prepareBody);
    }

    [Fact]
    public void RuntimeOnlySmoke_WaitsForTheInstallerButNotItsRuntimeDescendant()
    {
        string smoke = InstallerText("smoke-harness.ps1");
        int arguments = Position(smoke, "$arguments = @(");
        string install = smoke[arguments..Position(
            smoke,
            "if ($RequireRuntimeOnly)",
            arguments)];

        Assert.Contains("$install.WaitForExit()", install);
        Assert.DoesNotContain("-Wait -PassThru", install);
    }

    [Fact]
    public void BuildInstaller_EmitsAndChecksOnePayloadManifest()
    {
        string script = InstallerText("build-installer.ps1");

        Assert.Contains("payload-manifest.txt", script);
        Assert.Contains("format=1", script);
        Assert.Contains("protocol=1", script);
        Assert.Contains("architecture=x64", script);
        Assert.Contains("identity=Stable", script);
        Assert.Contains("file_count=", script);
        Assert.Contains("Get-FileHash", script);
        Assert.Contains("Get-AuthenticodeSignature", script);
        Assert.Contains("AllowUnsignedDevelopment", script);
        Assert.Contains("Test-PayloadManifest", script);
        Assert.Contains("/DManifestPrefix=", script);
        Assert.Contains("/DManifestHash=$manifestHash", script);
    }

    [Fact]
    public void BuildInstaller_WaitsForGuiSubsystemToolsThroughCmdHost()
    {
        string script = InstallerText("build-installer.ps1");

        Assert.Contains("$nativeArgs = @('/d', '/c', 'call', $FilePath) + $Arguments", script);
        Assert.Contains("& $env:ComSpec @nativeArgs", script);
        Assert.Contains("$exitCode = $LASTEXITCODE", script);
        Assert.Contains(
            "Invoke-NativeProcess -FilePath $candidate -Arguments @('--list-sdks')",
            script);
        Assert.Contains(
            "Invoke-NativeProcess -FilePath $dotnet -Arguments $publishArguments",
            script);
        Assert.Contains(
            "Invoke-NativeProcess -FilePath $iscc -Arguments $isccArguments",
            script);
        Assert.DoesNotContain("[Diagnostics.ProcessStartInfo]::new()", script);
        Assert.DoesNotContain("& $dotnet publish", script);
        Assert.DoesNotContain("& $iscc", script);
    }

    [Fact]
    public void Installer_BindsTheCompleteManifestHashAndDeclaredFileSizes()
    {
        string iss = InstallerText("famo-setup.iss");
        int verify = Position(iss, "procedure VerifyPayloadOrFail");
        string body = iss[verify..Position(iss, "function RunRegSvr32", verify)];

        Assert.Contains("CompareText(ActualHash, JournalManifestHash)", body);
        Assert.DoesNotContain("CompareText(ActualHash, '{#ManifestHash}')", body);
        Assert.Contains("payload manifest full hash mismatch", body);
        Assert.Contains("ParseFileEntryDetailed", body);
        Assert.Contains("TryGetFileSize64", body);
        Assert.Contains("ActualSize <> ExpectedSize", body);
    }

    [Fact]
    public void InnoSetup_QuotesEveryMachineRuntimeRunCommand()
    {
        string iss = InstallerText("famo-setup.iss");
        int writeActive = Position(iss, "procedure WriteActiveRegistry");
        string writeActiveBody = iss[writeActive..Position(iss, "procedure RestorePreviousRegistry", writeActive)];
        int restore = Position(iss, "procedure RestorePreviousRegistry");
        string restoreBody = iss[restore..Position(iss, "function NormalizeDirectoryPath", restore)];

        Assert.Contains(
            "RegWriteStringValue(HKLM64, RunKey, 'FamoRuntime'," +
            " AddQuotes(AddBackslash(Target) + 'FamoRuntime.exe'))",
            writeActiveBody);
        Assert.Contains(
            "RegWriteStringValue(HKLM64, RunKey, 'FamoRuntime', AddQuotes(PreviousServer))",
            restoreBody);
        Assert.DoesNotContain(
            "RegWriteStringValue(HKLM64, RunKey, 'FamoRuntime', PreviousServer)",
            restoreBody);
    }

    [Fact]
    public void InnoSetup_RejectsDuplicateManifestPathsAndNonHexHashes()
    {
        string iss = InstallerText("famo-setup.iss");
        int segmentValidator = Position(iss, "function ValidManifestPathSegment");
        int pathValidator = Position(iss, "function NormalizeSafeRelativePath", segmentValidator);
        int hashValidator = Position(iss, "function IsSha256Hex", pathValidator);
        int parseEntry = Position(iss, "function ParseFileEntry", hashValidator);
        string parseBody = iss[segmentValidator..Position(iss, "procedure ReadPreviousHostMetadata", parseEntry)];
        int verify = Position(iss, "procedure VerifyPayloadOrFail");
        string verifyBody = iss[verify..Position(iss, "function RunRegSvr32", verify)];

        // One accepted spelling per Windows path: no slash aliases, empty or
        // dot segments, nor Win32-trimmed trailing dots/spaces.
        Assert.Contains("NormalizedValue := PathNormalizeSlashes(Value)", parseBody);
        Assert.Contains("if NormalizedValue <> Value then Exit", parseBody);
        Assert.Contains("(Segment = '.') or (Segment = '..')", parseBody);
        Assert.Contains("Result := (Segment[Length(Segment)] <> '.') and", parseBody);
        Assert.Contains("(Segment[Length(Segment)] <> ' ')", parseBody);
        Assert.Contains("NormalizeSafeRelativePath(RawRelativePath, RelativePath)", parseBody);

        Assert.Contains("Length(Value) <> 64", parseBody);
        Assert.Contains("(Value[I] >= '0') and (Value[I] <= '9')", parseBody);
        Assert.Contains("(Value[I] >= 'A') and (Value[I] <= 'F')", parseBody);
        Assert.Contains("(Value[I] >= 'a') and (Value[I] <= 'f')", parseBody);
        Assert.Contains("Result := IsSha256Hex(ExpectedHash)", parseBody);

        Assert.Contains("SeenPaths := TStringList.Create", verifyBody);
        Assert.Contains("SeenPaths.CaseSensitive := False", verifyBody);
        Assert.Contains("TransactionRoot := NormalizeDirectoryPath(EnsureTransactionTarget)", verifyBody);
        Assert.Contains("FullPath := ExpandFileName(PathCombine(TransactionRoot, RelativePath))", verifyBody);
        Assert.Contains("PathStartsWith(FullPath, AddBackslash(TransactionRoot), True)", verifyBody);
        Assert.Contains("SeenPaths.IndexOf(FullPath) >= 0", verifyBody);
        Assert.Contains("RaiseException('duplicate payload manifest path: ' + RelativePath)", verifyBody);
        Assert.Contains("SeenPaths.Add(FullPath)", verifyBody);
        Assert.Contains("SeenPaths.Free", verifyBody);
    }

    [Fact]
    public void InnoSetup_MatchesManifestToCanonicalActualFileSet()
    {
        string iss = InstallerText("famo-setup.iss");
        int pathLookup = Position(iss, "function FindPathInList");
        int enumerate = Position(iss, "procedure VerifyActualPayloadFiles", pathLookup);
        int verify = Position(iss, "procedure VerifyPayloadOrFail", enumerate);
        string enumerationHelpers = iss[pathLookup..verify];
        string verifyBody = iss[verify..Position(iss, "function RunRegSvr32", verify)];

        Assert.Contains("(FindRec.Attributes and FileAttributeReparsePoint) <> 0", enumerationHelpers);
        Assert.Contains("TryGetFinalObjectInfo(Path, FinalPath, ObjectId)", enumerationHelpers);
        Assert.Contains("PathStartsWith(FinalPath, AddBackslash(FinalRoot), True)", enumerationHelpers);
        Assert.Contains("FindPathInList(ManifestFinalPaths, FinalPath) < 0", enumerationHelpers);
        Assert.Contains("FindPathInList(SeenActualPaths, FinalPath) >= 0", enumerationHelpers);
        Assert.Contains("SeenActualObjectIds.IndexOf(ObjectId) >= 0", enumerationHelpers);

        Assert.Contains("TryGetFinalObjectInfo(FullPath, FinalPath, ObjectId)", verifyBody);
        Assert.Contains("FindPathInList(SeenFinalPaths, FinalPath) >= 0", verifyBody);
        Assert.Contains("SeenObjectIds.IndexOf(ObjectId) >= 0", verifyBody);
        Assert.Contains("VerifyActualPayloadFiles(TransactionRoot, FinalRoot,", verifyBody);
        Assert.Contains("ActualCount <> EntryCount", verifyBody);
        Assert.DoesNotContain("CountFiles(", verifyBody);
    }

    [Fact]
    public void BuildInstaller_CopiesRequiredWinuiResourcesAndPropagatesCompilerFailure()
    {
        string script = InstallerText("build-installer.ps1");

        foreach (string resource in new[] { "FamoSettings.pri", "App.xbf", "MainWindow.xbf", "Theming" })
        {
            Assert.Contains(resource, script);
        }
        Assert.Contains("-p:Version=$AppVersion", script);
        Assert.Contains("-p:InformationalVersion=$AppVersion", script);
        Assert.Contains("if ($compile.ExitCode -ne 0) { throw 'ISCC 编译失败。' }", script);
    }

    [Fact]
    public void BuildInstaller_StagesSpdxWithoutExternalReleaseMatrix()
    {
        string script = InstallerText("build-installer.ps1");

        Assert.Contains("SBOM.spdx.json", script);
        Assert.DoesNotContain("Test-FamoReleaseGate.ps1", script);
        Assert.DoesNotContain("Windows 10", script, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("Windows 11", script, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void InnoSetup_InstallsAnImmutableTransactionTarget()
    {
        string iss = InstallerText("famo-setup.iss");

        Assert.Contains(@"Source: ""{#StagingDir}\payload\*""; DestDir: ""{code:GetTransactionTarget}""", iss);
        Assert.Contains(@"versions\{#AppVersion}-{#ManifestPrefix}-", iss);
        Assert.Contains("TransactionId := NewIdentityNonce", iss);
        Assert.Contains("function ValidTransactionId", iss);
        Assert.DoesNotContain("restartreplace", iss[..Position(iss, "function InitializeUninstall")], StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain(@"DestDir: ""{app}""", EffectiveInnoContent(iss), StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void InnoSetup_ValidatesPendingTargetBeforeRollbackOrResume()
    {
        string iss = InstallerText("famo-setup.iss");
        int normalize = Position(iss, "function NormalizeDirectoryPath");
        int validateTarget = Position(iss, "function ValidateTransactionTarget", normalize);
        int validatePending = Position(iss, "function ValidatePendingTransaction", validateTarget);
        int load = Position(iss, "function LoadPendingState", validatePending);
        string validationHelpers = iss[normalize..load];
        string loadBody = iss[load..Position(iss, "function InitializeSetup", load)];

        // Canonicalization plus exact-parent equality rejects the versions root,
        // other drives, and grandchildren instead of relying on a string prefix.
        Assert.Contains("RemoveBackslashUnlessRoot(ExpandFileName(PathNormalizeSlashes(Path)))", validationHelpers);
        Assert.Contains(@"Pos('\..\', '\' + PathNormalizeSlashes(Path) + '\') > 0", validationHelpers);
        Assert.Contains("not PathIsRooted(Target)", validationHelpers);
        Assert.Contains("ContainsParentTraversal(Target)", validationHelpers);
        Assert.Contains("NormalizedTarget := NormalizeDirectoryPath(Target)", validationHelpers);
        Assert.Contains("if PathSame(NormalizedTarget, VersionsRoot) or", validationHelpers);
        Assert.Contains("PathSame(ExtractFileDir(NormalizedTarget), VersionsRoot)", validationHelpers);
        Assert.Contains(
            "NormalizeSafeRelativePath(ExpectedVersion, NormalizedVersion)",
            validationHelpers);
        Assert.Contains("IsSha256Hex(ExpectedManifestHash)", validationHelpers);
        Assert.Contains(
            "Copy(ExpectedManifestHash, 1, 12) + '-' + ExpectedId",
            validationHelpers);
        Assert.Contains("CompareText(ExtractFileName(NormalizedTarget), ExpectedLeaf)", validationHelpers);
        Assert.Contains("ActiveTarget := ReadActiveTarget", validationHelpers);
        Assert.Contains("ProtectedPathIsDifferent(NormalizedTarget, TargetExists,", validationHelpers);
        Assert.Contains("TargetFinalPath, TargetObjectId, ActiveTarget", validationHelpers);
        Assert.Contains("TargetFinalPath, TargetObjectId, ProtectedPreviousTarget", validationHelpers);

        // Existing junctions/symlinks are rejected. A genuinely absent fresh
        // transaction target remains valid so Setup can create it.
        Assert.Contains("GetFileAttributesW", iss);
        Assert.Contains("FileAttributeReparsePoint", validationHelpers);
        Assert.Contains("InvalidFileAttributes", validationHelpers);
        Assert.Contains("ErrorFileNotFound", validationHelpers);
        Assert.Contains("ErrorPathNotFound", validationHelpers);
        Assert.Contains("Attributes and FileAttributeReparsePoint", validationHelpers);
        Assert.Contains("PathIsNonReparseOrMissing(VersionsRoot)", validationHelpers);
        Assert.Contains("PathIsNonReparseOrMissing(NormalizedTarget)", validationHelpers);

        int prepare = Position(iss, "procedure PrepareTransaction");
        string prepareBody = iss[prepare..Position(iss, "procedure SwitchRegistration", prepare)];
        int prepareValidation = Position(prepareBody,
            "ValidateTransactionTarget(TransactionTarget, TransactionId,");
        int freshTargetCheck = Position(prepareBody, "if DirExists(TransactionTarget)", prepareValidation);
        Assert.True(prepareValidation < freshTargetCheck);
        Assert.Contains("TransactionTarget := ValidatedTarget;", prepareBody);

        Assert.Contains("CompareText(PendingManifest,", validationHelpers);
        Assert.Contains("AddBackslash(NormalizedTarget) + 'payload-manifest.txt'", validationHelpers);
        Assert.Contains("if not Result then NormalizedTarget := '';", validationHelpers);

        // An untrusted registry value must not reach the global deletion target
        // until the shared resume/rollback loader has validated it.
        int validateCall = Position(loadBody, "LoadTransactionJournal(ExpectedId)");
        Assert.Contains("ValidateTransactionTarget(TransactionTarget, TransactionId,", loadBody);
        int targetValidation = Position(loadBody,
            "ValidateTransactionTarget(TransactionTarget, TransactionId,", validateCall);
        Assert.True(validateCall < targetValidation);
        Assert.DoesNotContain(
            "RegQueryStringValue(HKLM64, BrandKey, 'PendingTarget', TransactionTarget)",
            loadBody);

        // Re-check the mutable filesystem boundary immediately before recursive
        // deletion instead of trusting only the earlier pending-state load.
        int rollback = Position(iss, "procedure RollbackTransaction");
        string rollbackBody = iss[rollback..Position(iss, "procedure VerifyActiveInstall", rollback)];
        int deleteValidation = Position(rollbackBody,
            "ValidateTransactionTarget(TransactionTarget, TransactionId,");
        Assert.Contains(
            "JournalAppVersion, JournalManifestHash, PreviousTarget,",
            rollbackBody);
        Assert.Contains("ValidatedTarget", rollbackBody);
        int deleteTree = Position(
            rollbackBody,
            "if not DelTree(ValidatedTarget, True, True, True) then",
            deleteValidation);
        int deleteFailure = Position(
            rollbackBody,
            "RaiseException('transaction target deletion failed during rollback')",
            deleteTree);
        int rollbackComplete = Position(
            rollbackBody,
            "RollbackComplete := True;",
            deleteFailure);
        Assert.True(deleteValidation < deleteTree);
        Assert.True(deleteTree < deleteFailure);
        Assert.True(deleteFailure < rollbackComplete);
        Assert.DoesNotContain("DelTree(TransactionTarget", rollbackBody);
    }

    [Fact]
    public void InnoSetup_ProtectsExistingTargetsByFinalObjectIdentity()
    {
        string iss = InstallerText("famo-setup.iss");
        int finalObject = Position(iss, "function NormalizeFinalObjectPath");
        int validateTarget = Position(iss, "function ValidateTransactionTarget", finalObject);
        string finalObjectHelpers = iss[finalObject..validateTarget];
        string validateTargetBody = iss[validateTarget..Position(iss, "procedure PrepareTransaction", validateTarget)];

        Assert.Contains("CreateFileW", iss);
        Assert.Contains("GetFinalPathNameByHandleW", iss);
        Assert.Contains("GetFileInformationByHandle", iss);
        Assert.Contains("CloseHandle", iss);
        Assert.Contains("FileFlagBackupSemantics", finalObjectHelpers);
        Assert.Contains(@"PathStartsWith(Result, '\\?\UNC\', True)", finalObjectHelpers);
        Assert.Contains(@"Result := '\\' + Copy(Result, 9, Length(Result))", finalObjectHelpers);
        Assert.Contains(@"PathStartsWith(Result, '\\?\', True)", finalObjectHelpers);
        Assert.Contains("PathSame(FirstFinalPath, SecondFinalPath)", finalObjectHelpers);
        Assert.Contains("FirstObjectId <> ''", finalObjectHelpers);
        Assert.Contains("CompareText(FirstObjectId, SecondObjectId) = 0", finalObjectHelpers);
        Assert.Contains(
            "if (FirstObjectId <> '') and (SecondObjectId <> '') then",
            finalObjectHelpers);
        Assert.DoesNotContain(
            "PathSame(FirstFinalPath, SecondFinalPath) or",
            finalObjectHelpers);

        Assert.Contains("ProtectedPathIsDifferent(NormalizedTarget, TargetExists,", validateTargetBody);
        Assert.Contains("ActiveTarget", validateTargetBody);
        Assert.Contains("ProtectedPreviousTarget", validateTargetBody);
        Assert.Contains("PathSame(ExtractFileDir(TargetFinalPath), VersionsFinalPath)", validateTargetBody);
    }

    [Fact]
    public void InnoSetup_VerifiesManifestBeforeRegistrationSwitch()
    {
        string iss = InstallerText("famo-setup.iss");

        int verify = Position(iss, "procedure VerifyPayloadOrFail");
        int switchRegistration = Position(iss, "procedure SwitchRegistration");
        int postInstall = Position(iss, "VerifyPayloadOrFail;", Position(iss, "ssPostInstall"));
        int switchCall = Position(iss, "SwitchRegistration;", postInstall);

        Assert.Contains("GetSHA256OfFile", iss);
        Assert.Contains("payload-manifest.txt", iss);
        Assert.Contains("file_count", iss);
        Assert.True(verify < switchRegistration);
        Assert.True(postInstall < switchCall);
    }

    [Fact]
    public void InnoSetup_RecordsOnlyTheFourTerminalStates()
    {
        string iss = InstallerText("famo-setup.iss");

        foreach (string state in new[] { "Ready", "RolledBack", "PendingReboot", "NotInstalled" })
        {
            Assert.Contains(state, iss);
        }
        foreach (string value in new[] { "TransactionId", "PreviousTarget", "PreviousDefault", "ActiveManifest", "InstallState" })
        {
            Assert.Contains(value, iss);
        }
    }

    [Fact]
    public void InnoSetup_UsesNativeRuntimeControlAndOriginalUserSeed()
    {
        string iss = InstallerText("famo-setup.iss");

        int install = Position(iss, "procedure InstallUserState");
        string installBody =
            iss[install..Position(iss, "function CommitSeedReceiptAfterReady", install)];
        int prepare = Position(installBody, "--prepare-seed-transaction");
        int apply = Position(installBody, "--apply-seed-transaction", prepare);
        int start = Position(installBody, "StartRuntimeAsOriginalUser", apply);
        int deploy = Position(installBody, "--control deploy", start);
        Assert.Contains("'desktop-run-for ' + OriginalUserSid", iss);
        Assert.DoesNotContain("ExecAsOriginalUser", iss);
        Assert.True(prepare < apply && apply < start && start < deploy);
        Assert.Contains("is-active", iss);
        Assert.DoesNotContain("new profile activation failed", iss);
        int shutdown = Position(iss, "StopRuntimeAsOriginalUser(PreviousServer)");
        int switchRegistration = Position(iss, "SwitchRegistration;", shutdown);
        Assert.True(shutdown < switchRegistration);
        Assert.DoesNotContain("'/quit'", iss);
        Assert.Contains("else if Parameters = '/q' then Operation := 'quit'", iss);
        Assert.Contains("runtime deploy attempt ", installBody);
        Assert.Contains("StartRuntimeAsOriginalUser;", installBody[start..]);
        Assert.DoesNotContain("--control status", iss);
        Assert.DoesNotContain("FamoDeploy.exe", iss, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void InnoSetup_ResumeStartsRuntimeThroughNativeDesktopTokenBroker()
    {
        string iss = InstallerText("famo-setup.iss");
        int start = Position(iss, "procedure StartRuntimeAsOriginalUser");
        int seed = Position(iss, "procedure InstallUserState", start);
        string startBody = iss[start..seed];

        Assert.Contains("Broker := ProfileTool(TransactionTarget)", startBody);
        Assert.Contains("FlushMachineRegistryKey(BrandKey)", startBody);
        Assert.Contains("'InstallState'", startBody);
        Assert.Contains("'InstallDir'", startBody);
        Assert.Contains("'ActiveVersion'", startBody);
        Assert.Contains("'ServerExecutable'", startBody);
        Assert.Contains("runtime activation projection readback failed", startBody);
        Assert.Contains("ValidateCurrentPayloadForExecution", startBody);
        Assert.Contains("'start-runtime-for ' + OriginalUserSid", startBody);
        Assert.Contains("ewWaitUntilTerminated", startBody);
        Assert.DoesNotContain("Shell.Application", startBody);
        Assert.DoesNotContain("FindWindowSW", startBody);
    }

    [Fact]
    public void InnoSetup_ReprojectsActivatingStateAfterDurableUserStateBeforeRuntimeStart()
    {
        string iss = InstallerText("famo-setup.iss");
        int install = Position(iss, "procedure InstallUserState");
        string installBody =
            iss[install..Position(iss, "procedure PersistUserCleanupDebtBeforeReady", install)];

        int durableUserState = Position(
            installBody,
            "TransitionTransactionPhase(PhaseUserStateApplied);");
        int activatingProjection = Position(
            installBody,
            "WriteActiveRegistry(TransactionTarget, 'Activating');",
            durableUserState);
        int startRuntime = Position(
            installBody,
            "StartRuntimeAsOriginalUser;",
            activatingProjection);

        Assert.True(
            durableUserState < activatingProjection &&
            activatingProjection < startRuntime);
    }

    [Fact]
    public void RuntimeUpgrade_WaitsForExactPredecessorExitBeforeStartingReplacement()
    {
        string iss = InstallerText("famo-setup.iss");
        string tool = RepoText(
            "native/windows-tsf-famo/text-service/tools/dev_profile_main.cpp");

        int stop = Position(tool, "HRESULT StopRuntimeAsDesktopUser");
        int resolve = Position(tool, "bool ResolveDesktopOperation", stop);
        string stopBody = tool[stop..resolve];
        Assert.Contains("RunAsDesktopUser(runtime, L\"--control shutdown\"", stopBody);
        Assert.Contains("WaitForExecutableExit(runtime)", stopBody);
        Assert.Contains("RunAsDesktopUser(runtime, L\"/q\"", stopBody);
        Assert.Contains("ForceStopExactRuntime(runtime, expected_sid)", stopBody);
        Assert.Contains("RmShutdown(session, RmForceShutdown", tool);
        Assert.Contains("QueryFullProcessImageNameW", tool);
        Assert.Contains("TokenMatchesSid(token, expected_sid)", tool);
        Assert.Contains("stop-runtime-for", tool);

        int postInstall = Position(iss, "if CurStep = ssPostInstall then");
        int stopCall = Position(
            iss, "StopRuntimeAsOriginalUser(PreviousServer)", postInstall);
        int detectLoaded = Position(
            iss, "LoadedHostDetected := DetectLoadedPreviousHost", stopCall);
        Assert.True(stopCall < detectLoaded);
    }

    [Fact]
    public void RebootResume_StopsLoginRacePredecessorBeforeStartingReplacement()
    {
        string iss = InstallerText("famo-setup.iss");
        int complete = Position(iss, "procedure CompletePendingTransaction");
        string body = iss[complete..Position(
            iss, "function ValidatePendingTransaction", complete)];

        int validate = Position(
            body, "ValidatePreviousPayloadForExecution");
        int stop = Position(
            body, "StopRuntimeAsOriginalUser(PreviousServer)", validate);
        int activate = Position(
            body, "TransitionTransactionPhase(PhaseActivateIntent)", stop);

        Assert.True(validate < stop && stop < activate);
        Assert.Contains(
            "previous runtime did not exit before pending activation",
            body);
    }

    [Fact]
    public void ReadyCommit_RequiresStableUserProfileAndInputTipReadback()
    {
        string iss = InstallerText("famo-setup.iss");
        int ensure = Position(iss, "procedure EnsureStableUserProfileState");
        string ensureBody = iss[ensure..Position(
            iss, "procedure VerifyActiveInstall", ensure)];

        Assert.Contains("'check'", ensureBody);
        Assert.Contains("'--is-input-tip'", ensureBody);
        Assert.Contains("'enable'", ensureBody);
        Assert.Contains("'--add-input-tip'", ensureBody);
        Assert.Contains("StableReadbacks := StableReadbacks + 1", ensureBody);
        Assert.Contains("StableReadbacks >= 2", ensureBody);
        Assert.Contains("Sleep(2000)", ensureBody);

        int verify = Position(iss, "procedure VerifyActiveInstall");
        string verifyBody = iss[verify..Position(
            iss, "procedure CompletePendingTransaction", verify)];
        Assert.Contains("EnsureStableUserProfileState", verifyBody);
    }

    [Fact]
    public void InnoSetup_PinsTheRunningSetupObjectAcrossRecoveryCopy()
    {
        string iss = InstallerText("famo-setup.iss");
        int initialize = Position(iss, "function InitializeSetup");
        string initializeBody =
            iss[initialize..Position(iss, "procedure CurStepChanged", initialize)];
        Assert.True(
            Position(initializeBody, "PinRunningSetupSource") <
            Position(initializeBody, "RequireFixedProtectedInstallRoot"));

        int pin = Position(iss, "procedure PinRunningSetupSource");
        int retain = Position(iss, "procedure RetainRecoveryInstaller", pin);
        string pinBody = iss[pin..retain];
        Assert.Contains("TryGetFinalObjectInfo(SetupSourcePath", pinBody);
        Assert.Contains("SetupSourceObjectId = ''", pinBody);
        Assert.Contains("GetSHA256OfFile(SetupSourcePath)", pinBody);

        int schedule = Position(iss, "function RecoveryTaskExistsByCom", retain);
        string retainBody = iss[retain..schedule];
        int proofBefore = Position(
            retainBody, "TryGetFinalObjectInfo(Source, SourceFinalPath");
        int copy = Position(
            retainBody, "CopyFile(Source, Destination, True)", proofBefore);
        int proofAfter = Position(
            retainBody,
            "TryGetFinalObjectInfo(Source, SourceFinalPath",
            copy);
        int destinationProof = Position(
            retainBody,
            "TryGetFinalObjectInfo(Destination, DestinationFinalPath",
            proofAfter);
        Assert.True(
            proofBefore < copy &&
            copy < proofAfter &&
            proofAfter < destinationProof);
        Assert.Contains("SetupSourceFinalPath, SetupSourceObjectId", retainBody);
        Assert.Contains("GetSHA256OfFile(Source), SetupSourceHash", retainBody);
        Assert.Contains(
            "GetSHA256OfFile(Destination), SetupSourceHash", retainBody);
        Assert.Contains(
            "PathSame(ExtractFileDir(DestinationFinalPath),", retainBody);
    }

    [Fact]
    public void InnoSetup_RevalidatesManagedAndEmbeddedBrokersImmediatelyBeforeExecution()
    {
        string iss = InstallerText("famo-setup.iss");

        int runBound = Position(iss, "function RunBoundDesktopExitCode");
        string runBoundBody =
            iss[runBound..Position(iss, "function RunAndRequire", runBound)];
        Assert.DoesNotContain(
            "if not ValidateCurrentPayloadForExecution", runBoundBody);
        Assert.Contains(
            "ValidateManagedExecutableForExecution(FileName)", runBoundBody);

        int runRequire = Position(iss, "function RunAndRequire");
        string runRequireBody =
            iss[runRequire..Position(iss, "function RunExitCode", runRequire)];
        Assert.Contains(
            "ValidateManagedExecutableForExecution(FileName)", runRequireBody);

        int cleanup = Position(iss, "function RunTrustedDirectMachineUnregister");
        string cleanupBody =
            iss[cleanup..Position(iss, "function TransactionJournalKey", cleanup)];
        int validate = Position(
            cleanupBody, "ValidatePinnedBrokerForExecution(ProtectedBroker");
        int execute = Position(
            cleanupBody, "RunAndRequire(ProtectedBroker", validate);
        Assert.True(validate < execute);
        Assert.Contains("FamoEmbeddedManifest.txt", cleanupBody);
        Assert.Contains("'unregister-machine-direct'", cleanupBody);
    }

    [Fact]
    public void InnoSetup_MakesPendingRebootAndRecoveryVisibleAndLogged()
    {
        string iss = InstallerText("famo-setup.iss");
        string appcast = InstallerText("make-appcast.ps1");
        string appcastSelfTest = InstallerText("make-appcast-selftest.ps1");
        int arguments = Position(iss, "function ExpectedRecoveryArguments");
        string argumentsBody = iss[arguments..Position(
            iss, "function EnsureRecoveryTaskFolderByCom", arguments)];
        int cached = Position(
            iss, "function CachedCurrentPayloadExecutionProofMatches");
        string cachedBody = iss[cached..Position(
            iss, "function ValidateCurrentPayloadForExecution", cached)];
        int validate = Position(
            iss, "function ValidateCurrentPayloadForExecution", cached);
        string validateBody = iss[validate..Position(
            iss, "function RunRegSvr32", validate)];

        Assert.Contains("SetupLogging=yes", iss);
        Assert.Contains("FinishedRestartLabel=", iss);
        Assert.Contains("必须重新启动电脑才能完成切换并显示新输入法", iss);
        Assert.Contains("' /SILENT /SP- /NORESTART'", argumentsBody);
        Assert.DoesNotContain("/VERYSILENT", argumentsBody);
        Assert.DoesNotContain("/SUPPRESSMSGBOXES", argumentsBody);
        Assert.Contains("Result := PendingTerminal", iss);
        Assert.Contains(
            "sparkle:installerArguments=\"/SILENT /SP- /NOICONS\"",
            appcast);
        Assert.DoesNotContain(
            "sparkle:installerArguments=\"/SILENT /SP- /NOICONS /NORESTART\"",
            appcast);
        Assert.Contains("'/SILENT /SP- /NOICONS'", appcastSelfTest);
        Assert.Contains(
            "CachedCurrentPayloadExecutionProofMatches", validateBody);
        Assert.Contains("FinalObjectsSame", cachedBody);
        Assert.Contains("GetSHA256OfFile(Manifest)", cachedBody);
    }

    [Fact]
    public void InnoSetup_RetriesBoundRollbackDebtBeforeDeletingItsAnchor()
    {
        string iss = InstallerText("famo-setup.iss");
        int retry = Position(iss, "function RetryRolledBackCleanupDebt");
        string retryBody =
            iss[retry..Position(iss, "function IsFixedHexText", retry)];
        int userCleanup = Position(retryBody, "'cleanup-user-state'");
        int priorEnable = Position(
            retryBody, "RunAndRequire(PreviousRegistrationTool", userCleanup);
        int seedRollback = Position(
            retryBody, "--rollback-seed-transaction", priorEnable);
        int targetDelete = Position(retryBody, "DelTree(ValidatedTarget", seedRollback);
        Assert.True(
            userCleanup < priorEnable &&
            priorEnable < seedRollback &&
            seedRollback < targetDelete);
        Assert.Contains("ValidateCurrentPayloadForExecution", retryBody);
        Assert.Contains("ValidatePreviousPayloadForExecution", retryBody);
        Assert.Contains(
            "ClearTransactionDebt('UserRollbackDebt', DebtKindUserRollback)",
            retryBody);

        int recover = Position(iss, "function RecoverTerminalTransaction");
        string recoverBody =
            iss[recover..Position(iss, "function InitializeSetup", recover)];
        int rolledBack = Position(
            recoverBody, "if JournalPhase = PhaseRolledBack then");
        int deleteTask =
            Position(recoverBody, "DeleteRecoveryTask", rolledBack);
        int capture =
            Position(recoverBody, "CaptureOriginalUserIdentity", deleteTask);
        int retryCall =
            Position(recoverBody, "RetryRolledBackCleanupDebt", capture);
        Assert.True(deleteTask < capture && capture < retryCall);
    }

    [Fact]
    public void ReadyTerminalRecovery_DoesNotRearmAnAlreadyCommittedSeedReceipt()
    {
        string iss = InstallerText("famo-setup.iss");
        int recover = Position(iss, "function RecoverTerminalTransaction");
        string recoverBody =
            iss[recover..Position(iss, "procedure ResetLoadedTransactionForFreshInstall", recover)];
        int ready = Position(
            recoverBody, "else if JournalPhase = PhaseReady then");
        string readyBody = recoverBody[ready..Position(
            recoverBody,
            "if (JournalPhase = PhaseReady) and HasRecoveryDebt",
            ready)];

        int debtThen = Position(
            readyBody, "'UserCleanupDebt', DebtKindSeedCommit) then");
        int branchBegin = Position(readyBody, "begin", debtThen);
        int capture = Position(
            readyBody, "CaptureOriginalUserIdentity", debtThen);
        int commit = Position(
            readyBody, "CommitSeedReceiptAfterReady", capture);
        int branchEnd = Position(readyBody, "end;", commit);
        Assert.True(
            debtThen < branchBegin &&
            branchBegin < capture &&
            capture < commit &&
            commit < branchEnd);
    }

    [Fact]
    public void Installer_RecoversNotNewerTerminalJournalsButNotPendingPayloads()
    {
        string iss = InstallerText("famo-setup.iss");
        int notNewer =
            Position(iss, "function IsVersionNotNewerThanInstaller");
        string notNewerBody = iss[notNewer..Position(
            iss, "function ValidateRecoverableJournalArtifact", notNewer)];
        int recoverable = Position(
            iss, "function ValidateRecoverableJournalArtifact");
        string recoverableBody = iss[recoverable..Position(
            iss, "function ValidatePreviousV2Transaction", recoverable)];
        int load = Position(iss, "function LoadTransactionJournal");
        string loadBody = iss[load..Position(
            iss, "function ValidateTransactionTarget", load)];
        int repair = Position(
            iss, "function RepairRolledBackActiveProjection");
        string repairBody = iss[repair..Position(
            iss, "function InspectJournalGenerations", repair)];
        int repairRead = Position(repairBody, "ReadActiveJournal");
        int repairGate = Position(
            repairBody, "ValidateRecoverableJournalArtifact", repairRead);
        int repairApply = Position(
            repairBody, "ApplyTransactionJournal", repairGate);
        int adopt = Position(
            iss, "function AdoptCompleteOrphanGeneration");
        string adoptBody = iss[adopt..Position(
            iss, "function MergeDebtOwner", adopt)];
        int adoptRead = Position(
            adoptBody, "ReadJournalGeneration");
        int adoptGate = Position(
            adoptBody, "ValidateRecoverableJournalArtifact", adoptRead);
        int adoptWrite = Position(
            adoptBody, "RequireJournalWrite", adoptGate);
        int find = Position(iss, "function FindRecoverableTransaction");
        string findBody = iss[find..Position(
            iss, "procedure CleanupAllValidatedRecoveryArtifacts", find)];
        int findGate = Position(
            findBody, "ValidateRecoverableJournalArtifact");
        int pointerWrite = Position(
            findBody, "RequireJournalWrite", findGate);

        Assert.Contains(
            "ValidateCurrentJournalArtifact(Journal, ExpectedId)",
            recoverableBody);
        Assert.Contains("ValidateJournalSemantics(Journal, ExpectedId)",
            recoverableBody);
        Assert.Contains("Journal.Phase = PhaseReady", recoverableBody);
        Assert.Contains("Journal.Phase = PhaseRolledBack", recoverableBody);
        Assert.Contains(
            "IsVersionNotNewerThanInstaller(Journal.Version)",
            recoverableBody);
        Assert.DoesNotContain("PhasePendingReboot", recoverableBody);
        Assert.Contains("StrToVersion(Value + '.0', CandidateVersion)",
            notNewerBody);
        Assert.Contains(
            "StrToVersion('{#AppVersion}.0', InstallerVersion)",
            notNewerBody);
        Assert.Contains(
            "ComparePackedVersion(CandidateVersion, InstallerVersion) <= 0",
            notNewerBody);
        Assert.Contains(
            "ValidateRecoverableJournalArtifact(Journal, ExpectedId)",
            loadBody);
        Assert.True(
            repairRead < repairGate && repairGate < repairApply);
        Assert.True(
            adoptRead < adoptGate && adoptGate < adoptWrite);
        Assert.True(findGate < pointerWrite);
        Assert.DoesNotContain(
            "not ValidateJournalSemantics(Journal, Id)",
            findBody);
    }

    [Fact]
    public void Installer_RetriesJournalBoundPartialTargetCleanup()
    {
        string iss = InstallerText("famo-setup.iss");
        int guard = Position(
            iss, "function ValidateJournalBoundPartialTargetForCleanup");
        string guardBody = iss[guard..Position(
            iss, "function ValidateVersionDirectoryForCleanup", guard)];
        int rollback = Position(iss, "procedure RollbackTransaction");
        int retry = Position(
            iss, "function RetryRolledBackCleanupDebt", rollback);
        string retryBody = iss[retry..Position(
            iss, "function IsFixedHexText", retry)];

        Assert.Contains("JournalPhase = PhaseRolledBack", guardBody);
        Assert.Contains(
            "'TargetCleanupDebt', DebtKindTargetCleanup", guardBody);
        Assert.Contains("JournalAppVersion, JournalManifestHash", guardBody);
        Assert.Contains(
            "JournalPendingFinalTarget, JournalPendingObjectId", guardBody);
        Assert.Contains("ValidateCleanupTree", guardBody);
        Assert.Contains(
            "ValidateJournalBoundPartialTargetForCleanup(", retryBody);
        Assert.Contains("ValidatedTarget)", retryBody);
        Assert.Contains(
            "TerminalRecoveryTargetDeleteBlocked := True", retryBody);
        Assert.DoesNotContain(
            "DirExists(TransactionTarget) and UserRollbackOk and\n     CurrentPayloadTrusted",
            retryBody.Replace("\r\n", "\n", StringComparison.Ordinal));
    }

    [Fact]
    public void Installer_ContinuesAfterOrdinaryTerminalRecoveryOrExplainsRestart()
    {
        string iss = InstallerText("famo-setup.iss");
        int initialize = Position(iss, "function InitializeSetup");
        string initializeBody = iss[initialize..Position(
            iss, "procedure CurStepChanged", initialize)];
        int discovered = Position(
            initializeBody, "if not FindRecoverableTransaction(RequestedId)");
        string discoveredBody = initializeBody[discovered..];
        int terminal = Position(
            discoveredBody, "(JournalPhase = PhaseReady)");
        int recover = Position(
            discoveredBody, "RecoverTerminalTransaction", terminal);
        int retryDiscovery = Position(
            discoveredBody, "FindRecoverableTransaction(RequestedId)", recover);
        int reset = Position(
            discoveredBody, "ResetLoadedTransactionForFreshInstall",
            retryDiscovery);
        int success = Position(discoveredBody, "Result := True", reset);

        Assert.True(terminal < recover && recover < retryDiscovery &&
            retryDiscovery < reset && reset < success);
        Assert.Contains(
            "上次更新的旧输入法文件仍未能清理", discoveredBody);
        Assert.Contains(
            "无法完成上次更新的安全恢复，安装未继续", discoveredBody);
        Assert.Contains("TerminalRecoveryTargetDeleteBlocked",
            discoveredBody);
        Assert.Contains("SuppressibleMsgBox(", discoveredBody);
        Assert.DoesNotContain("if not WizardSilent then", discoveredBody);

        int resetDefinition = Position(
            iss, "procedure ResetLoadedTransactionForFreshInstall");
        string resetBody = iss[resetDefinition..Position(
            iss, "function InitializeSetup", resetDefinition)];
        foreach (string field in new[]
        {
            "TransactionId := ''",
            "TransactionTarget := ''",
            "JournalPhase := ''",
            "JournalGeneration := 0",
            "OriginalUserSid := ''",
            "OriginalUserAccount := ''",
            "OriginalUserSession := ''",
            "CurrentOriginalUserSession := ''",
            "OriginalUserResumeCapable := False"
        })
        {
            Assert.Contains(field, resetBody);
        }
    }

    [Fact]
    public void InnoSetup_RollsBackInReverseOrder()
    {
        string iss = InstallerText("famo-setup.iss");
        int rollback = Position(iss, "procedure RollbackTransaction");

        int shutdownNew = Position(iss, "--control shutdown", rollback);
        int unregisterNew = Position(
            iss, "if not UnregisterTarget(TransactionTarget) then", shutdownNew);
        int unregisterReadback = Position(
            iss, "MachineComPointsToTarget(TransactionTarget)", unregisterNew);
        int restoreRegistry = Position(iss, "RestorePreviousRegistry", unregisterReadback);
        int restoreProfile = Position(iss, "RegisterPreviousRegistration", restoreRegistry);
        Assert.True(
            shutdownNew < unregisterNew &&
            unregisterNew < unregisterReadback &&
            unregisterReadback < restoreRegistry &&
            restoreRegistry < restoreProfile);
        Assert.Contains("new machine registration rollback failed", iss);
        Assert.Contains("new machine COM registration remains after rollback", iss);

        int unregister = Position(iss, "function UnregisterTarget");
        string unregisterBody = iss[unregister..Position(iss, "function UnregisterMachineTarget", unregister)];
        Assert.Contains("if not MachineComPointsToTarget(Target) then", unregisterBody);
        Assert.Contains("FileExists(ProfileTool(Target)) and", unregisterBody);
        Assert.Contains("not MachineComPointsToTarget(Target)", unregisterBody);
        Assert.Contains("previous runtime rollback failed", iss);
        Assert.Contains("UserRollbackDebt", iss);
        Assert.Contains("previous profile activation deferred; available via Win+Space", iss);
        Assert.DoesNotContain("previous profile activation rollback failed", iss);
    }

    [Fact]
    public void InnoSetup_HasDeterministicFailurePointsForTransactionRecovery()
    {
        string iss = InstallerText("famo-setup.iss");

        Assert.Contains("{param:FamoFail|}", iss);
        foreach (string phase in new[]
        {
            "after-prepare", "after-verify", "after-switch", "after-user-state", "after-active-verify",
            "after-pending-registration", "after-pending-state", "after-resume-registration", "after-resume-user-state",
            "after-seed-commit-before-recovery-cleanup",
            "ready-debt-before-ready",
            "ready-after-phase-before-seedcommit",
            "rollback-debts-before-terminal",
            "rolledback-before-debt-finalize",
            "rolledback-before-artifact-cleanup",
            "uninstall-intent-before-commit",
            "uninstall-intent-after-commit",
            "uninstall-delete-anchor-before-commit",
            "uninstall-delete-anchor-after-commit",
            "uninstall-brand-deleted-before-anchor-retire",
        })
        {
            Assert.Contains($"FailIfRequested('{phase}')", iss);
        }
    }

    [Fact]
    public void InnoSetup_UninstallIntentPinsReentryBeforeAutomaticFileDeletion()
    {
        string iss = InstallerText("famo-setup.iss");
        int persist = Position(iss, "procedure PersistUninstallIntent");
        string persistBody = iss[persist..Position(
            iss, "function LoadCommittedUninstallIntent", persist)];
        int owner = Position(persistBody, "'UninstallIntentOwner'");
        int target = Position(persistBody, "'UninstallIntentTarget'", owner);
        int finalTarget = Position(
            persistBody, "'UninstallIntentFinalTarget'", target);
        int objectId = Position(
            persistBody, "'UninstallIntentObjectId'", finalTarget);
        int commit = Position(
            persistBody, "'UninstallIntent', UninstallIntentSchema", objectId);
        int flush = Position(
            persistBody, "FlushMachineRegistryKey(BrandKey)", commit);
        Assert.True(
            owner < target &&
            target < finalTarget &&
            finalTarget < objectId &&
            objectId < commit &&
            commit < flush);

        int load = Position(iss, "function LoadCommittedUninstallIntent");
        string loadBody = iss[load..Position(
            iss, "procedure RemoveActiveInstall", load)];
        Assert.Contains("if JournalPhase = PhaseReady then", loadBody);
        Assert.Contains(
            "else if (JournalPhase = PhaseRolledBack) and", loadBody);
        Assert.Contains("JournalPreviousFinalTarget", loadBody);
        Assert.Contains("PreviousCompatibilityTransactionId", loadBody);

        int remove = Position(iss, "procedure RemoveActiveInstall");
        string removeBody = iss[remove..Position(
            iss, "procedure CurUninstallStepChanged", remove)];
        int loadIntent = Position(
            removeBody, "LoadCommittedUninstallIntent");
        int loadReady = Position(
            removeBody, "LoadPendingState", loadIntent);
        string reentry = removeBody[loadIntent..loadReady];
        Assert.Contains("RunTrustedDirectMachineUnregister", reentry);
        Assert.DoesNotContain("ValidateCurrentPayloadForExecution", reentry);
        Assert.DoesNotContain("RunAndRequire(ProfileTool", reentry);

        int unregister = Position(
            removeBody, "UnregisterMachineTarget(ActiveTarget)");
        int persistCall = Position(
            removeBody, "PersistUninstallIntent(", unregister);
        int legacyAnchor = Position(
            removeBody, "IsLegacyRollbackAnchorForProjection");
        int legacyIntentIdentity = Position(
            removeBody, "JournalPreviousFinalTarget", legacyAnchor);
        int prepared = Position(
            removeBody, "UninstallPrepared := True", persistCall);
        Assert.True(
            legacyAnchor < legacyIntentIdentity &&
            legacyIntentIdentity < persistCall &&
            unregister < persistCall &&
            persistCall < prepared);
    }

    [Fact]
    public void InnoSetup_UninstallRegistryDeletionUsesAnExternalCommittedAnchor()
    {
        string iss = InstallerText("famo-setup.iss");

        Assert.Contains(
            "UninstallDeleteAnchorKey = 'Software\\Famo\\UninstallRecovery'",
            iss);
        int persist = Position(
            iss, "procedure PersistUninstallDeleteAnchor");
        string persistBody = iss[persist..Position(
            iss, "function LoadCommittedUninstallDeleteAnchor", persist)];
        int owner = Position(persistBody, "'Owner'");
        int digest = Position(persistBody, "'Digest'", owner);
        int beforeCommit = Position(
            persistBody, "uninstall-delete-anchor-before-commit", digest);
        int commit = Position(
            persistBody, "'Commit', UninstallDeleteAnchorSchema",
            beforeCommit);
        int flush = Position(
            persistBody, "FlushMachineRegistryKey(UninstallDeleteAnchorKey)",
            commit);
        int afterCommit = Position(
            persistBody, "uninstall-delete-anchor-after-commit", flush);
        Assert.True(
            owner < digest &&
            digest < beforeCommit &&
            beforeCommit < commit &&
            commit < flush &&
            flush < afterCommit);

        int remove = Position(iss, "procedure RemoveActiveInstall");
        string removeBody = iss[remove..Position(
            iss, "procedure CurUninstallStepChanged", remove)];
        int loadAnchor = Position(
            removeBody, "LoadCommittedUninstallDeleteAnchor");
        int loadActive = Position(
            removeBody, "'ActiveTransactionId'", loadAnchor);
        Assert.True(loadAnchor < loadActive);
        Assert.Contains("RecoveryTaskFolderAbsentByCom", removeBody);

        int initialize = Position(iss, "function InitializeSetup");
        string initializeBody = iss[initialize..Position(
            iss, "procedure CurStepChanged", initialize)];
        int baseIntentBlock = Position(
            initializeBody, "'UninstallIntent'");
        int deleteAnchorBlock = Position(
            initializeBody, "UninstallDeleteAnchorKey", baseIntentBlock);
        int transactionRecovery = Position(
            initializeBody, "FindRecoverableTransaction", deleteAnchorBlock);
        Assert.True(
            baseIntentBlock < deleteAnchorBlock &&
            deleteAnchorBlock < transactionRecovery);

        int post = Position(iss, "procedure CurUninstallStepChanged");
        string postBody = iss[post..];
        int machineFlush = Position(
            postBody, "FlushMachineRegistryKey('Software\\Classes\\CLSID')");
        int cleanup = Position(
            postBody, "CleanupAllValidatedRecoveryArtifacts", machineFlush);
        int persistAnchor = Position(
            postBody, "PersistUninstallDeleteAnchor", cleanup);
        int deleteBrand = Position(
            postBody,
            "if not RegDeleteKeyIncludingSubkeys(HKLM64, BrandKey)",
            persistAnchor);
        int flushParent = Position(
            postBody, "FlushMachineRegistryKey(FamoRootKey)", deleteBrand);
        int absentReadback = Position(
            postBody, "RegKeyExists(HKLM64, BrandKey)", flushParent);
        int crash = Position(
            postBody,
            "uninstall-brand-deleted-before-anchor-retire",
            absentReadback);
        int retire = Position(
            postBody, "RetireUninstallDeleteAnchor", crash);
        Assert.True(
            machineFlush < cleanup &&
            cleanup < persistAnchor &&
            persistAnchor < deleteBrand &&
            deleteBrand < flushParent &&
            flushParent < absentReadback &&
            absentReadback < crash &&
            crash < retire);
    }

    [Fact]
    public void NativeProfileTool_ProbesLoadedHostAndSupportsDisabledRegistration()
    {
        string tool = RepoText("native/windows-tsf-famo/text-service/tools/dev_profile_main.cpp");
        string cmake = RepoText("native/windows-tsf-famo/text-service/CMakeLists.txt");
        string selfcheck = RepoText("native/windows-tsf-famo/text-service/tests/module_selfcheck.cpp");

        foreach (string api in new[] { "RmStartSession", "RmRegisterResources", "RmGetList", "RmEndSession" })
        {
            Assert.Contains(api, tool);
        }
        foreach (string command in new[] { "register-disabled", "check-disabled", "check-absent", "enable", "disable", "loaded <dll>" })
        {
            Assert.Contains(command, tool);
        }
        Assert.Contains("Rstrtmgr", cmake);
        Assert.Contains("$<TARGET_FILE:${FAMO_PROFILE_TOOL}>", cmake);
        Assert.Contains("loaded", selfcheck);
        Assert.Contains("if (!ProfileActive())", tool);
        Assert.Contains("return S_OK;", tool);
    }

    [Fact]
    public void NativeProfileTool_BindsExactUserWorkToTheInteractiveDesktop()
    {
        string tool = RepoText(
            "native/windows-tsf-famo/text-service/tools/dev_profile_main.cpp");
        int runAsDesktopUser = Position(tool, "HRESULT RunAsDesktopUser");
        string body = tool[runAsDesktopUser..Position(
            tool, "HRESULT StartRuntimeAsDesktopUser", runAsDesktopUser)];

        int startup = Position(body, "STARTUPINFOW startup");
        int desktop = Position(
            body, "startup.lpDesktop = interactive_desktop", startup);
        int create = Position(body, "CreateProcessWithTokenW", desktop);

        Assert.True(startup < desktop && desktop < create);
        Assert.Contains("L\"winsta0\\\\default\"", body);
        Assert.Contains("STARTUPINFOW", body);
    }

    [Fact]
    public void NativeProfileTool_BreaksPackagedInstallerContextThroughValidatedRelay()
    {
        string tool = RepoText(
            "native/windows-tsf-famo/text-service/tools/dev_profile_main.cpp");
        int direct = Position(
            tool, "int RunBoundDesktopOperationCurrent");
        string directBody = tool[
            direct..Position(tool, "int RunBoundDesktopOperation(", direct)];
        int outer = Position(tool, "int RunBoundDesktopOperation(", direct);
        string outerBody = tool[
            outer..Position(tool, "std::wstring TextServiceGuidText", outer)];

        Assert.Contains("CurrentProcessTokenMatchesSid(sid)", directBody);
        Assert.Contains("TokenIsMediumIntegrityDesktop", directBody);
        Assert.Contains("ResolveDesktopOperation", directBody);
        Assert.Contains("CreateProcessW", directBody);
        Assert.Contains("desktop-relay-for", outerBody);
        Assert.Contains("RunAsScheduledDesktopUser(", outerBody);
        Assert.Contains("ModulePath()", outerBody);
        Assert.Contains("RunBoundDesktopOperationCurrent(", tool);
        Assert.Contains(
            "argv[2], argv[3], argv[4], argv[5], argv[6]", tool);
        Assert.Contains("ITaskService", tool);
        Assert.Contains("TASK_LOGON_INTERACTIVE_TOKEN", tool);
        Assert.Contains("TASK_RUNLEVEL_LUA", tool);
        Assert.Contains("DeleteTask", tool);

        string cmake = RepoText(
            "native/windows-tsf-famo/text-service/CMakeLists.txt");
        Assert.Contains("taskschd", cmake, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("oleaut32", cmake, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void NativeProfileTool_BindsScheduledExitToThisTaskInvocation()
    {
        string tool = RepoText(
            "native/windows-tsf-famo/text-service/tools/dev_profile_main.cpp");
        int scheduled = Position(tool, "HRESULT RunAsScheduledDesktopUser");
        string body = tool[scheduled..Position(
            tool, "int RunBoundDesktopOperation(", scheduled)];

        int baseline = Position(
            body, "get_LastRunTime(&baseline_last_run_time)");
        int run = Position(body, "registered->Run(empty, &running)", baseline);
        int observed = Position(
            body, "get_LastRunTime(&last_run_time)", run);
        int sample = Position(
            body, "ScheduledTaskCompletionCanBeSampled(", observed);
        int state = Position(body, "registered->get_State(&state)", sample);
        int accept = Position(
            body, "TryAcceptScheduledTaskCompletion(", state);

        Assert.True(
            baseline < run && run < observed && observed < sample &&
            sample < state && state < accept);
        Assert.Contains(
            "last_run_time != baseline_last_run_time", body);

        string selfcheck = RepoText(
            "native/windows-tsf-famo/text-service/tests/user_data_cleanup_selfcheck.cpp");
        Assert.Contains("TASK_STATE_READY, 0, false", selfcheck);
        Assert.Contains("TASK_STATE_UNKNOWN, TASK_STATE_DISABLED", selfcheck);
        Assert.Contains("exit_code != STILL_ACTIVE", selfcheck);
    }

    [Fact]
    public void NativeRegistration_RemovesLegacyPerUserComOverrideOnRegister()
    {
        string source = RepoText("native/windows-tsf-famo/text-service/src/registration.cpp");
        int register = Position(source, "HRESULT RegisterComServer(bool cleanup_current_user)");
        int unregister = Position(source, "void UnregisterComServer(bool cleanup_current_user)", register);
        string registerBody = source[register..unregister];

        Assert.Contains("RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str())", registerBody);
        Assert.Contains("ERROR_FILE_NOT_FOUND", registerBody);
        Assert.Contains("RegDeleteTreeW(HKEY_LOCAL_MACHINE, root.c_str())", registerBody);
    }

    [Fact]
    public void InnoSetup_DoesNotTrustPerUserComPathForElevatedPreviousHost()
    {
        string iss = InstallerText("famo-setup.iss");
        int snapshot = Position(iss, "procedure SnapshotPreviousState");
        int next = Position(iss, "function FindPathInList", snapshot);
        string body = iss[snapshot..next].Replace("\r\n", "\n");

        Assert.Contains("PreviousHost := '';", body);
        Assert.Contains("RegQueryStringValue(HKLM64", body);
        Assert.Contains("ValidatePreviousV2Transaction", body);
        Assert.Contains("ValidateLegacyPreviousSnapshot", body);
        Assert.Contains("previous installation identity mismatch", body);
        Assert.DoesNotContain("RegQueryStringValue(HKCU,\n      'Software\\Classes\\CLSID\\'", body);
    }

    [Fact]
    public void InnoSetup_LoadedLegacyHostEntersCoherentPendingReboot()
    {
        string iss = InstallerText("famo-setup.iss");
        int detect = Position(iss, "DetectLoadedPreviousHost");
        int pending = Position(iss, "EnterPendingReboot", detect);

        Assert.True(detect < pending);
        foreach (string value in new[]
        {
            "LoadedHostPath", "LoadedHostHash", "LoadedHostVersion", "LoadedHostExpectedHash",
            "PendingTarget", "PendingManifest", "PendingVersion", "PendingReason", "ResumeInstaller",
        })
        {
            Assert.Contains(value, iss);
        }
        Assert.Contains("'loaded ' + AddQuotes(PreviousHost)", iss);
        Assert.DoesNotContain("RegisterTargetDisabled(TransactionTarget)",
            iss[pending..Position(iss, "procedure StartRuntimeAsOriginalUser", pending)]);
        Assert.Contains("'check-absent'", iss);
        Assert.Contains("RegDeleteValue(HKLM64, RunKey, 'FamoRuntime')", iss);
        int clearRuntimeRun = Position(iss, "RegDeleteValue(HKLM64, RunKey, 'FamoRuntime')", pending);
        int verifyPending = Position(iss, "VerifyPendingInstall;", pending);
        Assert.True(clearRuntimeRun < verifyPending);
        Assert.Contains("/FamoRecover=' + TransactionId", iss);
        Assert.Contains("StatePendingReboot", iss);
    }

    [Fact]
    public void InnoSetup_ResumeAndExplicitRollbackStayBoundToOneTransaction()
    {
        string iss = InstallerText("famo-setup.iss");
        int completePending = Position(iss, "procedure CompletePendingTransaction");
        int loadPending = Position(iss, "function LoadPendingState", completePending);
        string completePendingBody = iss[completePending..loadPending];

        Assert.Contains("{param:FamoResume|}", iss);
        Assert.Contains("{param:FamoRollback|}", iss);
        Assert.Contains("(CompareText(Journal.Transaction, ExpectedId) = 0)", iss);
        Assert.Contains("function ValidateCurrentJournalArtifact", iss);
        Assert.Contains("DeleteRecoveryTask", iss);
        Assert.Contains("ValidateRecoveryTaskXml", iss);
        Assert.Contains("LoadPendingState(RequestedId)", iss);
        Assert.DoesNotContain("restartreplace", completePendingBody, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("'activate', False", iss);
        Assert.Contains("RegisterTarget(TransactionTarget)", completePendingBody);
        Assert.DoesNotContain("pending profile activation failed", completePendingBody);
        Assert.DoesNotContain("RegisterTargetDisabled(TransactionTarget)", completePendingBody);
    }

    [Fact]
    public void InnoSetup_UninstallSplitsDesktopUserAndMachineCleanup()
    {
        string iss = InstallerText("famo-setup.iss");
        string profileTool = RepoText("native/windows-tsf-famo/text-service/tools/dev_profile_main.cpp");
        int uninstall = Position(iss, "procedure RemoveActiveInstall");
        string uninstallBody = iss[uninstall..Position(iss, "procedure CurUninstallStepChanged", uninstall)];

        int userCleanup = Position(uninstallBody, "'cleanup-user-for ' + OriginalUserSid");
        int userDataCleanup = Position(
            uninstallBody,
            "'delete-user-data-for ' + OriginalUserSid",
            userCleanup);
        int unregister = Position(
            uninstallBody, "UnregisterMachineTarget(ActiveTarget)",
            userDataCleanup);
        int deleteRun = Position(uninstallBody, "FamoRuntime", unregister);
        Assert.True(
            userCleanup < userDataCleanup &&
            userDataCleanup < unregister &&
            unregister < deleteRun);
        Assert.DoesNotContain("ExecAsOriginalUser", uninstallBody);
        Assert.DoesNotContain("'switch-away', False", uninstallBody);
        Assert.DoesNotContain("'--remove-input-tip', False", uninstallBody);
        Assert.Contains("GetShellWindow()", profileTool);
        Assert.Contains("TokenMatchesSid(shell_token, expected_sid)", profileTool);
        Assert.Contains("CreateProcessWithTokenW", profileTool);
        Assert.Contains("LOGON_WITH_PROFILE", profileTool);
        Assert.Contains("L\"cleanup-user-state\"", profileTool);
        Assert.Contains("L\"--remove-input-tip\"", profileTool);
        Assert.Contains("RegDeleteTreeW(HKEY_CURRENT_USER", profileTool);
        Assert.Contains("L\"delete-user-data-for\"", profileTool);
        Assert.Contains("L\"delete-user-data-current\"", profileTool);
        Assert.Contains("CurrentProcessTokenMatchesSid(expected_sid)", profileTool);
        Assert.Contains("SHGetKnownFolderPath(", profileTool);
        Assert.Contains("FILE_FLAG_OPEN_REPARSE_POINT", profileTool);
        Assert.Contains("FinalPathIsContained", profileTool);
        Assert.Contains("NtOpenFile(", profileTool);
        Assert.Contains("InitializeObjectAttributes(", profileTool);
        Assert.Contains("OpenRelativeNoDeleteShare(", profileTool);
        Assert.Contains("DeletePinnedTreeObject(", profileTool);
        Assert.Contains("SetFileInformationByHandle", profileTool);
        Assert.Contains(
            @"Global\\Famo.Settings.UserData.Transaction.",
            profileTool);
        Assert.Contains("Famo.UserDataLocks", profileTool);
        Assert.Contains(".transaction.lock", profileTool);
        Assert.Contains("OpenOrCreateRelativeLockFile(", profileTool);
        int deleteCurrentUserData =
            Position(profileTool, "HRESULT DeleteCurrentUserData");
        string deleteCurrentUserDataBody = profileTool[
            deleteCurrentUserData..Position(
                profileTool,
                "HRESULT DeleteDesktopUserData",
                deleteCurrentUserData)];
        int acquireUserDataLock = Position(
            deleteCurrentUserDataBody,
            "AcquireUserDataTransactionLock(");
        int deletePinnedData = Position(
            deleteCurrentUserDataBody,
            "DeletePinnedDirectoryChild(",
            acquireUserDataLock);
        Assert.True(acquireUserDataLock < deletePinnedData);
        Assert.Contains("DllUnregisterMachine", profileTool);
        Assert.Contains("DeleteUserData", iss);
        Assert.Contains("UninstallSilent", iss);
        Assert.Contains("AddBackslash(FixedInstallRoot) + 'versions'", iss);

        int post = Position(iss, "procedure CurUninstallStepChanged");
        string postBody = iss[post..];
        Assert.DoesNotContain(
            @"ExpandConstant('{localappdata}\Famo')",
            postBody,
            StringComparison.OrdinalIgnoreCase);

        string cleanupSelfcheck = RepoText(
            "native/windows-tsf-famo/text-service/tests/user_data_cleanup_selfcheck.cpp");
        Assert.Contains("AttemptAncestorSwap", cleanupSelfcheck);
        Assert.Contains("CreateSymbolicLinkW", cleanupSelfcheck);
        Assert.Contains("outside_sentinel", cleanupSelfcheck);
        Assert.Contains(
            "user_data_cleanup_selfcheck=ok",
            cleanupSelfcheck);
        Assert.Contains(
            "SettingsCompatibleLockBlocksDeletion",
            cleanupSelfcheck);
        Assert.Contains(
            "SettingsCompatibleLockDirectoryResistsSwap",
            cleanupSelfcheck);
    }

    [Fact]
    public void NativeProfileTool_DoesNotContinueUserCleanupWhileRuntimeIsLoaded()
    {
        string tool = RepoText(
            "native/windows-tsf-famo/text-service/tools/dev_profile_main.cpp");
        int cleanup = Position(tool, "HRESULT CleanupDesktopUser");
        string body = tool[cleanup..Position(
            tool, "HRESULT ClearCurrentUserComShadow", cleanup)];

        int shutdown = Position(body, "L\"--control shutdown\"");
        int proveAbsent =
            Position(body, "WaitForExecutableExit(runtime)", shutdown);
        int propagate =
            Position(body, "if (FAILED(runtime_absent))", proveAbsent);
        int profileCleanup =
            Position(body, "L\"cleanup-user-state\"", propagate);
        int inputCleanup =
            Position(body, "L\"--remove-input-tip\"", profileCleanup);
        Assert.True(
            shutdown < proveAbsent &&
            proveAbsent < propagate &&
            propagate < profileCleanup &&
            profileCleanup < inputCleanup);
        Assert.Contains(
            "return FAILED(shutdown) ? shutdown : runtime_absent", body);
        Assert.Contains("constexpr DWORD kShutdownTimeoutMs", tool);
        Assert.Contains("IsFileLoaded(path.c_str()", tool);
        Assert.Contains("HRESULT_FROM_WIN32(ERROR_TIMEOUT)", tool);
    }

    [Fact]
    public void Installer_PersistsRecoveryCleanupDebtBeforeReady()
    {
        string iss = InstallerText("famo-setup.iss");
        int persist = Position(
            iss, "procedure PersistRecoveryCleanupDebtBeforeReady");
        string persistBody = iss[persist..Position(
            iss, "procedure ClearRecoveryCleanupDebt", persist)];
        Assert.Contains(
            "ArmTransactionDebt('RecoveryCleanupDebt', " +
            "DebtKindRecoveryArtifacts)",
            persistBody);
        int armHelper = Position(iss, "procedure ArmTransactionDebt");
        string armBody = iss[armHelper..Position(
            iss, "procedure ClearTransactionDebt", armHelper)];
        int debtWrite = Position(armBody, "RequireJournalWrite");
        int debtFlush =
            Position(armBody, "FlushMachineRegistryKey", debtWrite);
        int debtReadback = Position(
            armBody, "RegQueryStringValue", debtFlush);
        Assert.True(debtWrite < debtFlush && debtFlush < debtReadback);

        foreach (string terminalFlow in new[]
        {
            "procedure CompletePendingTransaction",
            "if CurStep = ssPostInstall",
        })
        {
            int start = Position(iss, terminalFlow);
            string flow = iss[start..];
            int arm =
                Position(flow, "PersistRecoveryCleanupDebtBeforeReady");
            int projection = Position(
                flow,
                "WriteActiveRegistry(TransactionTarget, StateReady)",
                arm);
            int ready = Position(
                flow, "TransitionTransactionPhase(PhaseReady)", projection);
            int commit =
                Position(flow, "CommitSeedReceiptAfterReady", ready);
            int crashPoint = Position(
                flow,
                "after-seed-commit-before-recovery-cleanup",
                commit);
            int cleanup = Position(flow, "DeleteRecoveryTask", crashPoint);
            Assert.True(
                arm < projection &&
                projection < ready &&
                ready < commit &&
                commit < crashPoint &&
                crashPoint < cleanup);
        }

        int noResume =
            Position(iss, "if OriginalUserResumeCapable then");
        string noResumeFlow = iss[noResume..];
        Assert.Contains("RetainRecoveryInstaller", noResumeFlow);
        Assert.Contains(
            "TransitionTransactionPhase(PhaseResumeArmed)",
            noResumeFlow);

        string health = RepoText(
            "native/windows-tsf-famo/weasel-fork/tests/Test-FamoHealth.ps1");
        Assert.Contains("'RecoveryCleanupDebt'", health);
        Assert.Contains("$readyRecoveryInstallerPresent", health);
        Assert.Contains("$readyRecoveryDirectoryPresent", health);
        Assert.Contains("$readyRecoveryResidual", health);
        Assert.Contains("$recoveryCleanupDebtPresent", health);
        Assert.Contains(
            "$recoveryCleanupDebtPresent -and " +
            "-not $recoveryCleanupDebtBound",
            health);
        Assert.Contains(
            "foreign or malformed transaction debt blocks write",
            armBody);
        Assert.DoesNotContain("RegDeleteValue", persistBody);
    }

    [Theory]
    [InlineData("UserCleanupDebt", "DebtKindSeedCommit", "seed-commit")]
    [InlineData("UserRollbackDebt", "DebtKindUserRollback", "user-rollback")]
    [InlineData("TargetCleanupDebt", "DebtKindTargetCleanup", "target-cleanup")]
    [InlineData("RecoveryCleanupDebt", "DebtKindRecoveryArtifacts", "recovery-artifacts")]
    [InlineData("VersionCleanupDebt", "DebtKindVersionRetention", "version-retention")]
    public void Installer_UsesOneExactKindForEachTypedDebt(
        string debtName,
        string kindConstant,
        string serializedKind)
    {
        string iss = InstallerText("famo-setup.iss");
        string health = RepoText(
            "native/windows-tsf-famo/weasel-fork/tests/Test-FamoHealth.ps1");

        Assert.Contains($"{kindConstant} = '{serializedKind}'", iss);
        Assert.Contains($"'{debtName}', {kindConstant}, Owner", iss);
        Assert.Contains(
            $"[string]$brand.{debtName}) $journalInfo.id '{serializedKind}')",
            health.Replace("\r\n", "\n", StringComparison.Ordinal));
    }

    [Fact]
    public void Installer_PreflightsExactPhaseCompatibleDebtsBeforeTerminalRecovery()
    {
        string iss = InstallerText("famo-setup.iss");
        string health = RepoText(
            "native/windows-tsf-famo/weasel-fork/tests/Test-FamoHealth.ps1");

        int arm = Position(iss, "procedure ArmTransactionDebt");
        string debtHelpers = iss[arm..Position(
            iss, "function ValidLegacyVersionCleanupDebt", arm)];
        Assert.DoesNotContain("CompareText(Existing, Expected)", debtHelpers);
        Assert.DoesNotContain("CompareText(Readback, Expected)", debtHelpers);
        Assert.Contains("Existing <> Expected", debtHelpers);
        Assert.Contains("Readback <> Expected", debtHelpers);

        int merge = Position(iss, "function MergeDebtOwner");
        string ownerHelpers = iss[merge..Position(
            iss, "function FindRecoverableTransaction", merge)];
        Assert.Contains("(Owner = Candidate)", ownerHelpers);
        Assert.Contains("(Kind = ExpectedKind)", ownerHelpers);

        int phaseCheck = Position(
            iss, "function TerminalDebtSetMatchesPhase");
        string phaseCheckBody = iss[phaseCheck..Position(
            iss, "function RecoverTerminalTransaction", phaseCheck)];
        Assert.Contains("JournalPhase = PhaseReady", phaseCheckBody);
        Assert.Contains("HasUserRollback or HasTargetCleanup", phaseCheckBody);
        Assert.Contains(
            "HasUserCleanup and (SeedReceiptHash = '')",
            phaseCheckBody);
        Assert.Contains("JournalPhase = PhaseRolledBack", phaseCheckBody);
        Assert.Contains("HasUserCleanup or HasVersionCleanup", phaseCheckBody);
        Assert.Contains(
            "HasRecoveryCleanup and not HasRecoveryArtifacts",
            phaseCheckBody);
        Assert.Contains("ValidLegacyVersionCleanupDebt", phaseCheckBody);
        Assert.Contains("Legacy <> TransactionId", phaseCheckBody);

        int recover = Position(iss, "function RecoverTerminalTransaction");
        string recoverBody = iss[recover..Position(
            iss, "function InitializeSetup", recover)];
        Assert.True(
            Position(recoverBody, "TerminalDebtSetMatchesPhase") <
            Position(recoverBody, "RestorePreviousRegistry"));
        Assert.Contains(
            "'VersionCleanupDebt', DebtKindVersionRetention",
            recoverBody);
        Assert.Contains("RecoveryTaskFolderAbsentByCom", recoverBody);
        Assert.Contains("ExpectedRecoveryInstaller(TransactionId)", recoverBody);
        Assert.DoesNotContain(
            "HasRecoveryArtifacts and\n     not TransactionDebtPresent",
            recoverBody.Replace("\r\n", "\n", StringComparison.Ordinal));

        int rollback = Position(iss, "procedure RollbackTransaction");
        string rollbackBody = iss[rollback..Position(
            iss, "function RetryRolledBackCleanupDebt", rollback)];
        Assert.Contains(
            "HasReadyCommitDebt and (SeedReceiptHash = '')",
            rollbackBody);
        int rollbackDebt = Position(
            rollbackBody,
            "ArmTransactionDebt('UserRollbackDebt'");
        int supersededReadyDebt = Position(
            rollbackBody,
            "ClearTransactionDebt('UserCleanupDebt', DebtKindSeedCommit)",
            rollbackDebt);
        int rollbackTerminal = Position(
            rollbackBody,
            "TransitionTransactionPhase(PhaseRolledBack)",
            supersededReadyDebt);
        int terminalFault = Position(
            rollbackBody,
            "rolledback-before-debt-finalize",
            rollbackTerminal);
        Assert.True(
            rollbackDebt < supersededReadyDebt &&
            supersededReadyDebt < rollbackTerminal &&
            rollbackTerminal < terminalFault);

        int binding = Position(health, "function Test-TransactionDebtBinding");
        string bindingBody = health[binding..Position(
            health, "function Get-FinalObjectInfo", binding)];
        Assert.Contains("[System.StringComparison]::Ordinal)", bindingBody);
        Assert.DoesNotContain(
            "[System.StringComparison]::OrdinalIgnoreCase",
            bindingBody);
    }

    [Fact]
    public void Installer_TerminalDebtsAreDurableTypedAndRecoveredBeforeANewTransaction()
    {
        string iss = InstallerText("famo-setup.iss");

        Assert.Contains("DebtSchema = 'famo-debt-v2'", iss);
        foreach (string kind in new[]
        {
            "seed-commit",
            "user-rollback",
            "target-cleanup",
            "recovery-artifacts",
            "version-retention",
        })
        {
            Assert.Contains($"'{kind}'", iss);
        }

        int arm = Position(iss, "procedure ArmTransactionDebt");
        string armBody = iss[arm..Position(
            iss, "procedure ClearTransactionDebt", arm)];
        Assert.Contains("foreign or malformed transaction debt blocks write",
            armBody);
        Assert.True(
            Position(armBody, "RequireJournalWrite") <
            Position(armBody, "FlushMachineRegistryKey"));
        Assert.True(
            Position(armBody, "FlushMachineRegistryKey") <
            Position(armBody, "RegQueryStringValue",
                Position(armBody, "FlushMachineRegistryKey")));

        int clear = Position(iss, "procedure ClearTransactionDebt");
        string clearBody = iss[clear..Position(
            iss, "function TransactionDebtPresent", clear)];
        Assert.Contains("foreign or malformed transaction debt blocks clear",
            clearBody);
        int delete = Position(clearBody, "RegDeleteValue");
        int flush = Position(clearBody, "FlushMachineRegistryKey", delete);
        int absentReadback =
            Position(clearBody, "RegQueryStringValue", flush);
        Assert.True(delete < flush && flush < absentReadback);

        foreach (string terminalFlow in new[]
        {
            "procedure CompletePendingTransaction",
            "if CurStep = ssPostInstall",
        })
        {
            int start = Position(iss, terminalFlow);
            string body = iss[start..];
            int userDebt =
                Position(body, "PersistUserCleanupDebtBeforeReady");
            int artifactDebt =
                Position(body, "PersistRecoveryCleanupDebtBeforeReady", userDebt);
            int projection = Position(
                body, "WriteActiveRegistry(TransactionTarget, StateReady)",
                artifactDebt);
            int terminal =
                Position(body, "TransitionTransactionPhase(PhaseReady)", projection);
            int commit = Position(body, "CommitSeedReceiptAfterReady", terminal);
            Assert.True(
                userDebt < artifactDebt &&
                artifactDebt < projection &&
                projection < terminal &&
                terminal < commit);
        }

        int rollback = Position(iss, "procedure RollbackTransaction");
        string rollbackBody = iss[rollback..Position(
            iss, "function RetryRolledBackCleanupDebt", rollback)];
        int rollbackIntent =
            Position(rollbackBody, "TransitionTransactionPhase(PhaseRollbackIntent)");
        int userRollbackDebt =
            Position(rollbackBody, "ArmTransactionDebt('UserRollbackDebt'",
                rollbackIntent);
        int targetDebt =
            Position(rollbackBody, "ArmTransactionDebt('TargetCleanupDebt'",
                userRollbackDebt);
        int supersededReadyDebt =
            Position(
                rollbackBody,
                "ClearTransactionDebt('UserCleanupDebt', DebtKindSeedCommit)",
                targetDebt);
        int rollbackTerminal =
            Position(rollbackBody, "TransitionTransactionPhase(PhaseRolledBack)",
                supersededReadyDebt);
        int terminalFault =
            Position(rollbackBody, "rolledback-before-debt-finalize",
                rollbackTerminal);
        int seedRollback =
            Position(rollbackBody, "--rollback-seed-transaction",
                terminalFault);
        int clearUserRollback =
            Position(rollbackBody, "ClearTransactionDebt('UserRollbackDebt'",
                seedRollback);
        int deleteTarget =
            Position(rollbackBody, "DelTree(ValidatedTarget",
                clearUserRollback);
        int taskCleanup =
            Position(rollbackBody, "DeleteRecoveryTask", deleteTarget);
        Assert.True(
            rollbackIntent < userRollbackDebt &&
            userRollbackDebt < targetDebt &&
            targetDebt < supersededReadyDebt &&
            supersededReadyDebt < rollbackTerminal &&
            rollbackTerminal < terminalFault &&
            terminalFault < seedRollback &&
            seedRollback < clearUserRollback &&
            clearUserRollback < deleteTarget &&
            rollbackTerminal < taskCleanup);
        int rolledBackReentry =
            Position(rollbackBody, "if JournalPhase = PhaseRolledBack then");
        int retryDebt =
            Position(rollbackBody, "RetryRolledBackCleanupDebt",
                rolledBackReentry);
        int reentryComplete =
            Position(rollbackBody, "RollbackComplete := True", retryDebt);
        Assert.True(
            rolledBackReentry < retryDebt &&
            retryDebt < reentryComplete);
        Assert.Contains("rollback compensation remains durably recoverable",
            rollbackBody);

        int findOwner = Position(iss, "function FindTransactionDebtOwner");
        int findRecoverable = Position(
            iss, "function FindRecoverableTransaction", findOwner);
        string findBody = iss[findRecoverable..Position(
            iss, "procedure CleanupAllValidatedRecoveryArtifacts",
            findRecoverable)];
        Assert.Contains("DebtOwner", findBody);
        Assert.Contains("Journal.Phase = PhaseReady", findBody);
        Assert.Contains("Journal.Phase = PhaseRolledBack", findBody);

        int initialize = Position(iss, "function InitializeSetup");
        string initializeBody = iss[initialize..Position(
            iss, "procedure CurStepChanged", initialize)];
        Assert.Contains("FindRecoverableTransaction(RequestedId)", initializeBody);
        Assert.Contains("RecoverTerminalTransaction", initializeBody);
        Assert.Contains("Result := False", initializeBody);

        Assert.Contains("function ValidLegacyVersionCleanupDebt", iss);
        Assert.Contains(
            "function ValidLegacyVersionCleanupDebtForOwner",
            iss);
        Assert.Contains("procedure MigrateLegacyRollbackCleanupDebt", iss);
        Assert.Contains("procedure ClearAdoptedLegacyVersionCleanupDebt", iss);
        int clearLegacy = Position(
            iss, "procedure ClearAdoptedLegacyVersionCleanupDebt");
        string clearLegacyBody = iss[clearLegacy..Position(
            iss, "procedure BuildCurrentJournal", clearLegacy)];
        Assert.True(
            Position(
                clearLegacyBody,
                "ClearExactLegacyRegistryValue('CleanupDebtCount'") <
            Position(
                clearLegacyBody,
                "ClearExactLegacyRegistryValue('CleanupDebt'"));
        Assert.Contains(
            "ExactStoredTransactionDebt(",
            clearLegacyBody);

        int cleanupVersions = Position(iss, "procedure CleanupObsoleteVersions");
        string cleanupVersionsBody = iss[cleanupVersions..Position(
            iss, "procedure VerifyActiveInstall", cleanupVersions)];
        int clearLegacyAfterScan = Position(
            cleanupVersionsBody,
            "ClearAdoptedLegacyVersionCleanupDebt");
        Assert.True(
            clearLegacyAfterScan <
            Position(
                cleanupVersionsBody,
                "ClearTransactionDebt(",
                clearLegacyAfterScan));
        Assert.DoesNotContain(
            "RegWriteStringValue(HKLM64, BrandKey, 'CleanupDebt'",
            iss);
    }

    [Fact]
    public void Installer_HelperRecoveryOwnsAnEarlyLifetimeMutexBeforeAnyMutation()
    {
        string iss = InstallerText("famo-setup.iss");

        Assert.Contains("EarlyTransactionMutexName =", iss);
        Assert.Contains(
            "'Global\\FamoInstallerEarlyTransactionV2'", iss);
        Assert.Contains(
            "SetupMutex=FamoInstallerTransactionV2,Global\\FamoInstallerTransactionV2",
            iss);

        int acquire = Position(iss, "function AcquireEarlyTransactionMutex");
        string acquireBody = iss[acquire..Position(
            iss, "procedure ReleaseEarlyTransactionMutex", acquire)];
        Assert.Contains("SetLastError(0)", acquireBody);
        Assert.Contains("CreateMutexW", acquireBody);
        Assert.Contains("ErrorAlreadyExists", acquireBody);
        Assert.Contains("CloseHandle(Candidate)", acquireBody);

        int initialize = Position(iss, "function InitializeSetup");
        string initializeBody = iss[initialize..Position(
            iss, "procedure CurStepChanged", initialize)];
        int setupAcquire = Position(
            initializeBody, "AcquireEarlyTransactionMutex");
        Assert.True(
            setupAcquire < Position(initializeBody, "PinRunningSetupSource") &&
            setupAcquire <
            Position(initializeBody, "RequireFixedProtectedInstallRoot") &&
            setupAcquire <
            Position(initializeBody, "RecoverHelperCleanupDebt"));

        int deinitialize = Position(iss, "procedure DeinitializeSetup");
        string deinitializeBody = iss[deinitialize..Position(
            iss, "function InitializeUninstall", deinitialize)];
        Assert.Contains("finally", deinitializeBody);
        Assert.Contains("ReleaseEarlyTransactionMutex", deinitializeBody);

        int uninstall = Position(iss, "function InitializeUninstall");
        string uninstallBody = iss[uninstall..Position(
            iss, "procedure DeinitializeUninstall", uninstall)];
        int uninstallAcquire = Position(
            uninstallBody, "AcquireEarlyTransactionMutex");
        Assert.True(
            uninstallAcquire <
            Position(uninstallBody, "RequireFixedProtectedInstallRoot") &&
            uninstallAcquire <
            Position(uninstallBody, "RecoverHelperCleanupDebt"));

        int deinitializeUninstall = Position(
            iss, "procedure DeinitializeUninstall");
        string deinitializeUninstallBody = iss[deinitializeUninstall..Position(
            iss, "function OnlyLoadedHostResidue", deinitializeUninstall)];
        Assert.Contains(
            "ReleaseEarlyTransactionMutex", deinitializeUninstallBody);
    }

    [Fact]
    public void Installer_TransientHelpersAreDurablyRecordedAndExactlyRecovered()
    {
        string iss = InstallerText("famo-setup.iss");

        Assert.Contains("DebtKindIdentityHelper = 'identity-helper'", iss);
        Assert.Contains(
            "DebtKindMachineCleanupHelper = 'machine-cleanup-helper'", iss);

        int debtHelpers = Position(
            iss, "procedure ArmHelperCleanupDebt",
            Position(iss, "procedure ClearTransactionDebt"));
        string armBody = iss[debtHelpers..Position(
            iss, "procedure ClearHelperCleanupDebt", debtHelpers)];
        Assert.Contains(
            "TransactionDebtValue(TransactionId, Kind + ':' + Nonce)",
            armBody);
        int write = Position(armBody, "RequireJournalWrite");
        int flush = Position(armBody, "FlushMachineRegistryKey", write);
        int readback = Position(armBody, "RegQueryStringValue", flush);
        Assert.True(write < flush && flush < readback);

        int clearDebt = Position(
            iss, "procedure ClearHelperCleanupDebt", debtHelpers);
        string clearDebtBody = iss[clearDebt..Position(
            iss, "function ValidateExactHelperFile", clearDebt)];
        Assert.Contains("Existing <> Expected", clearDebtBody);
        int deleteDebt = Position(clearDebtBody, "RegDeleteValue");
        int flushClear = Position(
            clearDebtBody, "FlushMachineRegistryKey", deleteDebt);
        int absentReadback = Position(
            clearDebtBody, "RegQueryStringValue", flushClear);
        Assert.True(deleteDebt < flushClear && flushClear < absentReadback);

        int capture = Position(iss, "procedure CaptureOriginalUserIdentity");
        string captureBody = iss[capture..Position(
            iss, "function ReadPinnedManagedFileIdentity", capture)];
        int identityDebt = Position(
            captureBody,
            "ArmHelperCleanupDebt(DebtKindIdentityHelper, PipeId)");
        int identityCreate = Position(
            captureBody, "ForceDirectories(PendingRoot)", identityDebt);
        int identityFault = Position(
            captureBody, "FailIfRequested('after-identity-helper-debt')",
            identityDebt);
        Assert.True(identityDebt < identityFault && identityFault < identityCreate);
        Assert.Contains(
            "FailIfRequested('after-identity-helper-debt')", captureBody);
        Assert.Contains(
            "FailIfRequested('after-identity-helper-create')", captureBody);
        Assert.Contains("RecordLoaded := False", captureBody);
        int recordPoll = Position(captureBody, "for Attempts := 1 to 150");
        int recordLoad = Position(
            captureBody, "LoadStringsFromFile(IdentityRecord, Lines)", recordPoll);
        int recordBreak = Position(captureBody, "Break", recordLoad);
        Assert.True(recordPoll < recordLoad && recordLoad < recordBreak);
        Assert.DoesNotContain(
            "if FileExists(IdentityRecord) then Break", captureBody);
        Assert.Contains("CleanupExactHelperAndDebt(TransactionId", captureBody);
        Assert.Contains("DebtKindIdentityHelper, PipeId", captureBody);

        int machine = Position(
            iss, "function RunTrustedDirectMachineUnregister");
        string machineBody = iss[machine..Position(
            iss, "function TransactionJournalKey", machine)];
        int machineDebt = Position(
            machineBody,
            "ArmHelperCleanupDebt(DebtKindMachineCleanupHelper, Nonce)");
        int machineCreate = Position(
            machineBody, "ForceDirectories(PendingRoot)", machineDebt);
        int machineFault = Position(
            machineBody, "FailIfRequested('after-machine-helper-debt')",
            machineDebt);
        Assert.True(machineDebt < machineFault && machineFault < machineCreate);
        Assert.Contains(
            "FailIfRequested('after-machine-helper-debt')", machineBody);
        Assert.Contains(
            "FailIfRequested('after-machine-helper-create')", machineBody);
        Assert.Contains("CleanupExactHelperAndDebt(TransactionId", machineBody);
        Assert.Contains("DebtKindMachineCleanupHelper, Nonce", machineBody);

        int pinOpen = Position(
            iss, "function TryOpenPinnedHelperDirectory");
        string pinOpenBody = iss[pinOpen..Position(
            iss, "function PinExactHelperTree", pinOpen)];
        Assert.Contains(
            "FileShareRead or FileShareWrite, 0, OpenExisting",
            pinOpenBody);
        Assert.DoesNotContain("FileShareDelete", pinOpenBody);
        Assert.Contains("FileFlagOpenReparsePoint", pinOpenBody);
        Assert.Contains("Result := ObjectId <> ''", pinOpenBody);

        int pinTree = Position(iss, "function PinExactHelperTree");
        string pinTreeBody = iss[pinTree..Position(
            iss, "procedure FlushHelperCleanupVolume", pinTree)];
        Assert.Contains(
            "TryOpenPinnedHelperDirectory(AppRoot", pinTreeBody);
        Assert.Contains(
            "TryOpenPinnedHelperDirectory(PendingRoot", pinTreeBody);
        Assert.Contains(
            "TryOpenPinnedHelperDirectory(HelperDirectory", pinTreeBody);
        Assert.Contains(
            "PathSame(ExtractFileDir(PendingFinalPath), AppFinalPath)",
            pinTreeBody);
        Assert.Contains(
            "PathSame(ExtractFileDir(HelperFinalPath), PendingFinalPath)",
            pinTreeBody);

        int volumeFlush = Position(
            iss, "procedure FlushHelperCleanupVolume");
        string volumeFlushBody = iss[volumeFlush..Position(
            iss, "function ValidateExactHelperFile", volumeFlush)];
        Assert.Contains("VolumePath := '\\\\.\\' + Uppercase(Drive)", volumeFlushBody);
        Assert.Contains("CreateFileW(VolumePath, GenericWrite", volumeFlushBody);
        Assert.Contains("FlushFileBuffers(VolumeHandle)", volumeFlushBody);

        int cleanup = Position(
            iss, "function CleanupExactHelperAndDebt",
            Position(iss, "function DeleteExactHelperFileWithRetries"));
        string cleanupBody = iss[cleanup..Position(
            iss, "function RecoverHelperCleanupDebt", cleanup)];
        int recover = Position(iss, "function RecoverHelperCleanupDebt");
        string recoverBody = iss[recover..Position(
            iss, "procedure ClearExactLegacyRegistryValue", recover)];
        foreach (string exact in new[]
        {
            "'identity-' + Nonce",
            "'FamoIdentityBroker-' + Nonce + '.exe'",
            "'identity-' + Nonce + '.txt'",
            "'machine-cleanup-' + Nonce",
            "'payload-manifest.txt'",
            "'FamoMachineCleanup.exe'",
            "ValidateExactHelperDirectory",
            "DeleteExactHelperFile",
        })
        {
            Assert.Contains(exact, cleanupBody);
        }
        Assert.Contains("PinExactHelperTree", cleanupBody);
        Assert.Contains("ClosePinnedHelperHandle(HelperHandle)", cleanupBody);
        int removeDirectory = Position(
            cleanupBody, "RemoveDir(HelperDirectory)");
        int flushFilesystem = Position(
            cleanupBody, "FlushHelperCleanupVolume", removeDirectory);
        int clearRegistryDebt = Position(
            cleanupBody, "ClearHelperCleanupDebt", flushFilesystem);
        Assert.True(
            removeDirectory < flushFilesystem &&
            flushFilesystem < clearRegistryDebt);

        int missingDirectory = Position(
            cleanupBody, "if not HelperExists then");
        string missingDirectoryBody = cleanupBody[missingDirectory..Position(
            cleanupBody, "if not PinExactHelperTree", missingDirectory)];
        Assert.True(
            Position(missingDirectoryBody, "FlushHelperCleanupVolume") <
            Position(missingDirectoryBody, "ClearHelperCleanupDebt"));
        Assert.Contains(
            "FailIfRequested('after-helper-remove-before-volume-flush')",
            cleanupBody);
        Assert.Contains(
            "FailIfRequested('after-helper-volume-flush-before-debt-clear')",
            cleanupBody);
        Assert.Contains(
            "CleanupExactHelperAndDebt(", recoverBody);

        int validate = Position(iss, "function ValidateExactHelperDirectory");
        string validateBody = iss[validate..Position(
            iss, "function DeleteExactHelperFile", validate)];
        Assert.Contains("CompareText(Path, FirstFile)", validateBody);
        Assert.Contains("CompareText(Path, SecondFile)", validateBody);
        Assert.Contains("FileAttributeReparsePoint", validateBody);

        string helperDebtAndRecovery = iss[debtHelpers..Position(
            iss, "procedure ClearExactLegacyRegistryValue", debtHelpers)];
        Assert.DoesNotContain("DelTree(", helperDebtAndRecovery);

        int initialize = Position(iss, "function InitializeSetup");
        string initializeBody = iss[initialize..Position(
            iss, "procedure CurStepChanged", initialize)];
        int startupRecovery = Position(
            initializeBody, "RecoverHelperCleanupDebt");
        Assert.True(
            startupRecovery < Position(initializeBody, "ResumeId :=") &&
            startupRecovery <
            Position(initializeBody, "FindRecoverableTransaction"));

        int uninstall = Position(iss, "function InitializeUninstall");
        string uninstallBody = iss[uninstall..Position(
            iss, "function OnlyLoadedHostResidue", uninstall)];
        Assert.Contains("RecoverHelperCleanupDebt", uninstallBody);
    }

    [Fact]
    public void InnoSetup_UninstallDefersOnlyLockedTsfHostDeletionToRestart()
    {
        string iss = InstallerText("famo-setup.iss");
        int uninstall = Position(iss, "function InitializeUninstall");
        string uninstallBody = iss[uninstall..];

        Assert.Contains("uninsrestartdelete", iss, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("OnlyLoadedHostResidue", uninstallBody);
        Assert.Contains("RestartReplace(LoadedHost, '')", uninstallBody);
        Assert.Contains("RestartReplace(VersionTarget, '')", uninstallBody);
        Assert.Contains("RestartReplace(VersionsRoot, '')", uninstallBody);
        Assert.Contains("function UninstallNeedRestart: Boolean", uninstallBody);
        Assert.Contains("Result := UninstallRestartPending", uninstallBody);
        Assert.Contains("loaded TSF host scheduled for restart deletion", uninstallBody);
        Assert.DoesNotContain("cannot remove transaction version directories", uninstallBody);
    }

    [Fact]
    public void InnoSetup_PinsVisibleFamoIdentityAndNeverWritesUserRime()
    {
        string iss = InstallerText("famo-setup.iss");
        string effective = EffectiveInnoContent(iss);

        Assert.Contains("#define AppName       \"法墨输入法\"", iss);
        Assert.Contains("SetupIconFile={#SetupIconFile}", iss);
        Assert.Contains(@"IconFilename: ""{code:GetActiveSettings}""", iss);
        Assert.DoesNotContain(@"DestDir: ""{userappdata}", iss, StringComparison.OrdinalIgnoreCase);
        foreach (string forbidden in new[] { "WeaselServer.exe", "WeaselDeployer.exe", "weaselx64.dll", "FamoDeploy.exe", "小狼毫" })
        {
            Assert.DoesNotContain(forbidden, effective, StringComparison.OrdinalIgnoreCase);
        }
    }

    [Fact]
    public void NativeIdentityBrokerBoundsAndDrainsConnectAndProofRead()
    {
        string tool = RepoText("native/windows-tsf-famo/text-service/tools/dev_profile_main.cpp");
        string selfcheck = RepoText("native/windows-tsf-famo/text-service/tests/identity_broker_selfcheck.cpp");
        string cmake = RepoText("native/windows-tsf-famo/text-service/CMakeLists.txt");
        int helper = Position(tool, "DWORD WaitForOverlappedUntil");
        int cancel = Position(tool, "CancelIoEx(handle, operation)", helper);
        int drain = Position(tool,
            "GetOverlappedResult(handle, operation, transferred, TRUE)", cancel);
        int deadline = Position(
            tool, "GetTickCount64() + kIdentityPipeTimeoutMs", drain);
        int connectWait = Position(
            tool, "WaitForOverlappedUntil(pipe, &overlapped, deadline", deadline);
        int read = Position(tool, "ReadFile(pipe, message", connectWait);
        int readWait = Position(
            tool,
            "WaitForOverlappedUntil(pipe, &read_overlapped, deadline",
            read);
        int closeReadEvent = Position(
            tool, "CloseHandle(read_overlapped.hEvent)", readWait);

        Assert.True(
            cancel < drain &&
            drain < deadline &&
            deadline < connectWait &&
            connectWait < read &&
            read < readWait &&
            readWait < closeReadEvent);
        Assert.Contains("waited == WAIT_FAILED", tool);
        Assert.Contains("ERROR_OPERATION_ABORTED", tool);
        Assert.Contains("silent.txt", selfcheck);
        Assert.Contains("OpenProofPipe(silent_pipe_id)", selfcheck);
        Assert.Contains("timeout.txt", selfcheck);
        Assert.Contains("capture-original-user", selfcheck);
        Assert.Contains("identity_broker_selfcheck PROPERTIES TIMEOUT 70", cmake);
        Assert.Contains("constexpr DWORD kChildTimeoutMs = 120000", tool);
        Assert.Contains(
            "WaitForSingleObject(process.hProcess, kChildTimeoutMs)", tool);
        Assert.Contains("TerminateProcess(process.hProcess, ERROR_TIMEOUT)", tool);
        Assert.Contains("unregister-machine-direct", tool);
        Assert.Contains("UnregisterMachineWithoutLoadingServiceDll", tool);
    }

    [Fact]
    public void HealthCheckReadsRecoveryOnlyFromTheJournalAndTaskXml()
    {
        string health = RepoText(
            "native/windows-tsf-famo/weasel-fork/tests/Test-FamoHealth.ps1");

        Assert.Contains("ActiveTransactionId", health);
        Assert.Contains("ActiveGeneration", health);
        Assert.Contains("function Get-JournalDigest", health);
        Assert.Contains("generation digest mismatch", health);
        Assert.Contains("Famo\\versions\\$expectedLeaf", health);
        Assert.Contains("ResumeInstallerHash", health);
        Assert.Contains("ResumeTaskName", health);
        Assert.Contains("OriginalUserSid", health);
        Assert.Contains("schtasks.exe", health);
        Assert.Contains("expectedSddl", health);
        Assert.Contains("($principals[0]).GetAttribute('id')", health);
        Assert.Contains("($actions[0]).GetAttribute('Context')", health);
        Assert.Contains("triggerElements.Count -ne 1", health);
        Assert.Contains("actionElements.Count -ne 1", health);
        Assert.Contains("triggerEnabled.Count -gt 1", health);
        Assert.Contains("settingsEnabled.Count -gt 1", health);
        Assert.Contains("$principalUserIds", health);
        Assert.Contains("$triggerUserIds", health);
        Assert.Contains("$record.OriginalUserAccount", health);
        Assert.Contains("$principals = @(& $nodes 'Principal')", health);
        Assert.Contains("$commands = @(& $nodes 'Command')", health);
        Assert.Contains(".InnerText", health);
        Assert.DoesNotContain(".'#text'", health);
        Assert.DoesNotContain("$enabled.Count -ne 2", health);
        Assert.Contains("function Get-FamoRecoveryTaskInventory", health);
        Assert.Contains("GetSecurityDescriptor(4)", health);
        Assert.Contains("$famoFolder.GetTasks(1)", health);
        Assert.Contains("$famoFolder.GetFolders(0)", health);
        Assert.Contains("folderPresent", health);
        Assert.Contains("folderSddl", health);
        Assert.Contains("subfolders", health);
        Assert.Contains(
            "\"D:PAI(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;$($journalInfo.record.OriginalUserSid))\"",
            health);
        Assert.Contains("-not $recoveryTaskInventory.folderPresent", health);
        Assert.Contains("Add-Check 'H10' 'S0'", health);
        Assert.Contains("Add-Check 'H11' 'S1'", health);
        Assert.Contains("$exactUserContext", health);
        Assert.Contains("if ($exactUserContext)", health);
        Assert.Contains("$payloadExecutionTrusted", health);
        Assert.Contains("$manifestResult.targetObjectId", health);
        Assert.Contains("$journalInfo.record.PendingObjectId", health);
        Assert.Contains("$manifestResult.manifestHash", health);
        Assert.Contains("$userRollbackDebtBound", health);
        Assert.Contains("famo-debt-v2|$Owner|$Kind", health);
        Assert.Contains("'Unsupported'", health);
        Assert.Contains("InteractiveToken", health);
        Assert.Contains("HighestAvailable", health);
        Assert.DoesNotContain("RunOnce", health);
        Assert.DoesNotContain("FamoResumePending", health);

        int h3b = Position(health, "Add-Check 'H3b'");
        int profileGate = Position(
            health, "$payloadExecutionTrusted =", h3b);
        int profileInvoke = Position(
            health, "Invoke-ProfileTool -Path", profileGate);
        Assert.True(h3b < profileGate && profileGate < profileInvoke);
    }

    [Fact]
    public void JournalIdentityValidationKeepsHistoricalGenerationsStableAndCurrentTargetPinned()
    {
        string iss = InstallerText("famo-setup.iss");
        string health = RepoText(
            "native/windows-tsf-famo/weasel-fork/tests/Test-FamoHealth.ps1");
        int phaseValidation = Position(
            iss, "function ValidPendingObjectIdForPhase");
        string phaseValidationBody = iss[
            phaseValidation..Position(
                iss, "function ValidateJournalSemantics", phaseValidation)];
        int pendingLoad = Position(iss, "function LoadPendingState");
        string pendingLoadBody = iss[
            pendingLoad..Position(
                iss, "function InspectJournalGenerations", pendingLoad)];

        Assert.Contains("Journal.Phase = PhasePrepared", phaseValidationBody);
        Assert.Contains("Journal.Phase = PhaseRollbackIntent", phaseValidationBody);
        Assert.Contains("Journal.Phase = PhaseRolledBack", phaseValidationBody);
        Assert.DoesNotContain("DirExists(", phaseValidationBody);
        Assert.Contains("ValidPendingObjectIdForPhase(Journal)", iss);
        Assert.Contains("if DirExists(TransactionTarget)", pendingLoadBody);
        Assert.Contains("(JournalPendingObjectId <> '')", pendingLoadBody);
        Assert.Contains(
            "(CompareText(ObjectId, JournalPendingObjectId) = 0)",
            pendingLoadBody);
        Assert.Contains("$phaseAllowsAbsentPendingObject", health);
        Assert.Contains("'Prepared', 'RollbackIntent', 'RolledBack'", health);
        Assert.Contains(
            "$phaseAllowsAbsentPendingObject -and -not $pendingTargetExists",
            health);
        Assert.Contains("-not $pendingObjectIdValid", health);
    }

    [Fact]
    public void NativeBuildHasDistinctStableAndDevelopmentIdentitySets()
    {
        string cmake = RepoText("native/windows-tsf-famo/text-service/CMakeLists.txt");
        string tsf = RepoText("native/windows-tsf-famo/text-service/src/famo_guids.h");
        string runtime = RepoText("native/windows-tsf-famo/runtime-protocol/include/famo_runtime_identity.h");

        Assert.Contains("FAMO_IDENTITY", cmake);
        Assert.Contains("FAMO_STABLE_IDENTITY", cmake);
        Assert.Contains("54ead76a", tsf, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("a6e6f585", tsf, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("0158c2ba", tsf, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("189ab82f", tsf, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("dev-runtime-v2", runtime);
        Assert.Contains("runtime-v2", runtime);
    }

    private static int Position(string text, string needle, int start = 0)
    {
        int at = text.IndexOf(needle, start, StringComparison.OrdinalIgnoreCase);
        Assert.True(at >= 0, $"expected installer text: {needle}");
        return at;
    }

    private static string InstallerText(string name) =>
        RepoText($"native/windows-tsf-famo/installer/{name}");

    private static string RepoText(string relativePath)
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            string candidate = Path.Combine(dir, relativePath.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate)) return File.ReadAllText(candidate);
            dir = Directory.GetParent(dir)?.FullName;
        }
        throw new FileNotFoundException(relativePath);
    }

    private static string EffectiveInnoContent(string iss) =>
        string.Join("\n", iss.Split('\n')
            .Select(line => line.Trim())
            .Where(line => line.Length > 0)
            .Where(line => !line.StartsWith(';'))
            .Where(line => !line.StartsWith("//")));
}
