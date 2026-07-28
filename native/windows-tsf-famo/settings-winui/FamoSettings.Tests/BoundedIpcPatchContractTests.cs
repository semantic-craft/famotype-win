using Xunit;

namespace Famo.Settings.Tests;

public sealed class BoundedIpcPatchContractTests
{
    [Fact]
    public void BoundedIpcPatchCapsConnectReadAndWrite()
    {
        string patch = File.ReadAllText(WeaselForkFile("features", "bounded-ipc-connect.patch"));

        Assert.Contains("kFamoPipeConnectBudgetMs = 1500", patch);
        Assert.Contains("kFamoPipeIoBudgetMs = 1500", patch);
        Assert.Contains("FILE_FLAG_OVERLAPPED", patch);
        Assert.Contains("WaitForSingleObject(overlapped.hEvent, kFamoPipeIoBudgetMs)", patch);
        Assert.Contains("CancelIoEx(pipe, &overlapped)", patch);
        Assert.Contains("GetOverlappedResult(pipe, &overlapped, &transferred, TRUE)", patch);
        Assert.Contains("completion_error == ERROR_OPERATION_ABORTED", patch);
        Assert.Contains("cancel_error == ERROR_NOT_FOUND", patch);
        Assert.DoesNotContain("+      ::CancelIo(pipe)", patch);
        Assert.Contains("_ThrowCode(ERROR_SEM_TIMEOUT)", patch);
        Assert.Contains("-  ::FlushFileBuffers(pipe);", patch);
        Assert.DoesNotContain("+  ::FlushFileBuffers(pipe);", patch);
    }

    [Fact]
    public void BoundedIpcPatchTreatsMalformedResponsesAsFailOpenProtocolFailures()
    {
        string patch = File.ReadAllText(WeaselForkFile("features", "bounded-ipc-connect.patch"));

        Assert.Contains("ERROR_INVALID_DATA", patch);
        Assert.Contains("lread < rec_len", patch);
        Assert.Contains("body_error == ERROR_MORE_DATA ? ERROR_INVALID_DATA", patch);
        Assert.Contains("wait == WAIT_FAILED", patch);
        Assert.Contains("FamoCancelPipeIoAndWait(pipe, overlapped, wait_error)", patch);
        Assert.Contains("WaitForSingleObject(overlapped.hEvent, INFINITE)", patch);
        Assert.Contains("catch (DWORD ex)", patch);
        Assert.Contains("ex == ERROR_SEM_TIMEOUT", patch);
        Assert.Contains("wait_error != ERROR_FILE_NOT_FOUND", patch);
        Assert.Contains("wait_error != ERROR_PIPE_BUSY", patch);
        Assert.Contains("_ThrowCode(wait_error)", patch);
        Assert.Contains("if (!_Ensure())", patch);
        Assert.Contains("_ThrowCode(ERROR_PIPE_NOT_CONNECTED)", patch);
        Assert.Contains("HANDLE* retry_pipe = _GetPipeHandle()", patch);
        Assert.Contains("_WritePipe(*retry_pipe, data_sz, pbuff)", patch);
        Assert.DoesNotContain("+      _WritePipe(pipe, data_sz, pbuff);", patch);
        Assert.Contains("// Covers the initial write, reconnect, and retry write.", patch);
        Assert.Contains("ClearBufferStream();", patch);
    }

    [Fact]
    public void EnginePatchKeepsReplacementHandleCleanupAndFailsOpenTheLastInput()
    {
        string boundedPatch = File.ReadAllText(WeaselForkFile("features", "bounded-ipc-connect.patch"));
        string patch = File.ReadAllText(WeaselForkFile("features", "engine-abi.patch"));

        // The replacement handle is introduced by the earlier bounded-I/O
        // layer; engine-abi then preserves its retry write while adding the
        // wider transaction cleanup. Check the composed patch contract rather
        // than requiring an unchanged declaration to be duplicated.
        Assert.Contains("HANDLE* retry_pipe = _GetPipeHandle()", boundedPatch);
        Assert.Contains("_WritePipe(*retry_pipe, data_sz, pbuff)", patch);
        Assert.Contains("ClearTransactionBuffer();", patch);
        Assert.Contains("This also covers a reconnect/retry failure", patch);

        // A pending receipt may precede the final physical key in a session.
        // Recovery therefore has to finish, or fail that key open, in this
        // callback; a queue would have no future event that is guaranteed to
        // drain it.
        Assert.DoesNotContain("deferred_actions", patch);
        Assert.DoesNotContain("pending_commit_prefix", patch);
        int pendingResponseGate = patch.IndexOf(
            "if (session_status->pending_response_result &&",
            StringComparison.Ordinal);
        int pendingResponseDelivery = patch.IndexOf(
            "_DeliverPendingResponse(ipc_id, session_status, eat)",
            pendingResponseGate,
            StringComparison.Ordinal);
        int pending = patch.IndexOf(
            "if (session_status->pending_recovery_action != 0)",
            pendingResponseDelivery,
            StringComparison.Ordinal);
        int boundedRecovery = patch.IndexOf(
            "for (uint32_t attempt = 0; attempt < 3u; ++attempt)",
            pending,
            StringComparison.Ordinal);
        int failedRecovery = patch.IndexOf(
            "if (!recovered)",
            boundedRecovery,
            StringComparison.Ordinal);
        int failOpen = patch.IndexOf(
            "result.handled = false",
            failedRecovery,
            StringComparison.Ordinal);
        int returnWithoutDispatch = patch.IndexOf(
            "return result;",
            failOpen,
            StringComparison.Ordinal);
        int recoveredLease = patch.IndexOf(
            "session_status->pending_response_result = std::move(m_abi_result)",
            returnWithoutDispatch,
            StringComparison.Ordinal);
        int clearReceipt = patch.IndexOf(
            "session_status->pending_recovery_action = 0",
            recoveredLease,
            StringComparison.Ordinal);
        int deliverRecovered = patch.IndexOf(
            "_DeliverPendingResponse(ipc_id, session_status, eat)",
            clearReceipt,
            StringComparison.Ordinal);
        int currentAction = patch.IndexOf(
            "m_engine_host.ExecuteActionRecovering(",
            deliverRecovered,
            StringComparison.Ordinal);

        Assert.True(
            pendingResponseGate >= 0 &&
            pendingResponseDelivery > pendingResponseGate &&
            pending > pendingResponseDelivery &&
            boundedRecovery > pending &&
            failedRecovery > boundedRecovery &&
            failOpen > failedRecovery &&
            returnWithoutDispatch > failOpen &&
            recoveredLease > returnWithoutDispatch &&
            clearReceipt > recoveredLease &&
            deliverRecovered > clearReceipt &&
            currentAction > deliverRecovered,
            "pending recovery must either fail the last key open before dispatch, " +
            "or retain and deliver its exact engine-owned result before the current key");
        Assert.Contains(
            "Do not retain it\n+      // waiting for another user event: fail open in this callback.",
            patch);

        int deliveryStart = patch.IndexOf(
            "+bool RimeWithWeaselHandler::_DeliverPendingResponse(",
            StringComparison.Ordinal);
        int deliveryEnd = patch.IndexOf(
            "+bool RimeWithWeaselHandler::_Respond(",
            deliveryStart,
            StringComparison.Ordinal);
        Assert.True(deliveryStart >= 0 && deliveryEnd > deliveryStart);
        string delivery = patch[deliveryStart..deliveryEnd];
        string addedDelivery = string.Join(
            '\n',
            delivery.Split('\n')
                .Where(line => line.StartsWith('+') && !line.StartsWith("+++"))
                .Select(line => line[1..]));
        Assert.Contains("FAMO_TEST_WEASEL_FORMAT_FAILURE", patch);
        Assert.Contains("throw std::bad_alloc()", patch);
        Assert.Contains("catch (...)", addedDelivery);
        Assert.Contains("engine-owned lease remains the exact retry anchor", addedDelivery);
        Assert.Contains("kMaxPendingCommitBytes = 1024u * 1024u", addedDelivery);
        Assert.Contains("if (!eat(response))", addedDelivery);
        Assert.DoesNotContain("eat(header)", addedDelivery);
        Assert.DoesNotContain("eat(body)", addedDelivery);

        Assert.Contains("bool TryWrite(const std::wstring& cnt)", patch);
        Assert.Contains("cnt.size() > capacity - used", patch);
        Assert.Contains("stream.write(cnt.data()", patch);
        Assert.Contains("return channel->TryWrite(msg)", patch);
        Assert.Contains("bool complete = false", patch);
        Assert.Contains("p_commit->append(unescape_string(value))", patch);
        Assert.Contains("--recovered-order", patch);
    }

    [Fact]
    public void BoundedIpcPatchRunsBeforeFeaturePatchesThatAddNewIpcCommands()
    {
        string apply = File.ReadAllText(WeaselForkFile("apply-famo-features.ps1"));

        int bounded = apply.IndexOf("features/bounded-ipc-connect.patch", StringComparison.Ordinal);
        int instant = apply.IndexOf("features/instant-apply.patch", StringComparison.Ordinal);
        int select = apply.IndexOf("features/select-schema.patch", StringComparison.Ordinal);

        Assert.True(bounded >= 0, "bounded IPC patch must remain in the feature chain");
        Assert.True(instant > bounded, "bounded IPC must be applied before instant reload IPC commands");
        Assert.True(select > bounded, "bounded IPC must be applied before select-schema IPC commands");
    }

    private static string WeaselForkFile(params string[] parts)
    {
        string[] path = new[] { "native", "windows-tsf-famo", "weasel-fork" }.Concat(parts).ToArray();
        return RepoFile(path);
    }

    private static string RepoFile(string[] pathParts)
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            string candidate = Path.Combine(new[] { dir }.Concat(pathParts).ToArray());
            if (File.Exists(candidate))
            {
                return candidate;
            }

            dir = Directory.GetParent(dir)?.FullName;
        }

        throw new FileNotFoundException($"Could not locate {Path.Combine(pathParts)}");
    }
}
