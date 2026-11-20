// Expects NWNX_LOG_LOG_LEVEL=6, NWNX_LOG_LOGGER_LEVELS=verbose:7,quiet:3 and
// NWNX_CORE_ALLOW_NWNX_FUNCTIONS_IN_EXECUTE_SCRIPT_CHUNK=true. Runs itself with ExecuteScript() and the
// script parameter "mode" for the nested-run cases.
#include "nwnx_log"
#include "nwnx_tests"

const string NWNX_LOG_T = "nwnx_log_t";

string GetBinding(string sKey)
{
    json jValue = JsonObjectGet(NWNX_Log_GetBindings(), sKey);
    return JsonGetType(jValue) == JSON_TYPE_STRING ? JsonGetString(jValue) : JsonDump(jValue);
}

int HasBinding(string sKey)
{
    return JsonGetType(JsonObjectGet(NWNX_Log_GetBindings(), sKey)) != JSON_TYPE_NULL;
}

void RunMode(string sMode, string sTest = "", string sCase = "")
{
    SetScriptParam("mode", sMode);
    SetScriptParam("test", sTest);
    SetScriptParam("case", sCase);
    ExecuteScript(NWNX_LOG_T, OBJECT_SELF);
}

// Bind "leak", then end the run early. The flags prove the run got that far and no further.
void BindAndAbort(string sCase)
{
    NWNX_Log_Bind(JsonParse("{\"leak\":\"" + sCase + "\"}"));
    SetLocalInt(GetModule(), "NWNX_LOG_T_BOUND_" + sCase, TRUE);
    AbortRunningScript();
    SetLocalInt(GetModule(), "NWNX_LOG_T_RAN_ON_" + sCase, TRUE);
}

void BindAndLoop(string sCase)
{
    NWNX_Log_Bind(JsonParse("{\"leak\":\"" + sCase + "\"}"));
    SetLocalInt(GetModule(), "NWNX_LOG_T_BOUND_" + sCase, TRUE);
    int i;
    while (TRUE)
        i++;
    SetLocalInt(GetModule(), "NWNX_LOG_T_RAN_ON_" + sCase, TRUE);
}

void CheckNoLeak(string sTest, string sCase)
{
    object oModule = GetModule();
    int bEnded = GetLocalInt(oModule, "NWNX_LOG_T_BOUND_" + sCase) && !GetLocalInt(oModule, "NWNX_LOG_T_RAN_ON_" + sCase);
    NWNX_Tests_Report("NWNX_Log", sTest, bEnded && !HasBinding("leak"));
}

void CheckDelay()
{
    NWNX_Tests_Report("NWNX_Log", "NoCarryIntoDelayCommand", !HasBinding("txn") && GetBinding("shard") == "main");
    NWNX_Log_Info("from-delay");
}

void Finish()
{
    NWNX_Log_SetGlobal(JSON_NULL);
    NWNX_Tests_Report("NWNX_Log", "SetGlobalClear", JsonGetLength(NWNX_Log_GetBindings()) == 0);
    WriteTimestampedLogEntry("NWNX_Log unit test end.");
}

void RunChild(string sMode, string sTest)
{
    if (sMode == "inherit")
    {
        NWNX_Tests_Report("NWNX_Log", "InheritIntoExecuteScript", GetBinding("txn") == "a1");
        NWNX_Tests_Report("NWNX_Log", "GlobalInExecuteScript", GetBinding("shard") == "main");
        NWNX_Log_Bind(JsonParse("{\"txn\":\"b2\",\"child\":true}"));
        NWNX_Tests_Report("NWNX_Log", "ChildOverride", GetBinding("txn") == "b2" && HasBinding("child"));
        NWNX_Log_Info("from-child");
        RunMode("grandchild");
        NWNX_Tests_Report("NWNX_Log", "ChildKeepsAfterGrandchild", GetBinding("txn") == "b2" && !HasBinding("grandchild"));
    }
    else if (sMode == "grandchild")
    {
        NWNX_Tests_Report("NWNX_Log", "NestedInherit", GetBinding("txn") == "b2" && HasBinding("child"));
        NWNX_Log_Bind(JsonParse("{\"grandchild\":1}"));
    }
    else if (sMode == "unbind")
    {
        NWNX_Log_Unbind("txn");
        NWNX_Tests_Report("NWNX_Log", "UnbindKeepsEnclosing", GetBinding("txn") == "a1");
    }
    else if (sMode == "check")
        CheckNoLeak(sTest, GetScriptParam("case"));
}

void main()
{
    string sMode = GetScriptParam("mode");
    // Called from main: 8193.37 crashes when AbortRunningScript() runs two calls deep in a nested run.
    if (sMode == "abort")
    {
        BindAndAbort(GetScriptParam("test"));
        return;
    }
    if (sMode == "loop")
    {
        BindAndLoop(GetScriptParam("test"));
        return;
    }
    if (sMode != "")
    {
        RunChild(sMode, GetScriptParam("test"));
        return;
    }

    WriteTimestampedLogEntry("NWNX_Log unit test begin..");

    NWNX_Tests_Report("NWNX_Log", "IsEnabled",
        NWNX_Log_IsEnabled(NWNX_LOG_LEVEL_INFO) && NWNX_Log_IsEnabled(NWNX_LOG_LEVEL_ERROR) && !NWNX_Log_IsEnabled(NWNX_LOG_LEVEL_DEBUG));
    NWNX_Tests_Report("NWNX_Log", "IsEnabledLogger",
        NWNX_Log_IsEnabled(NWNX_LOG_LEVEL_DEBUG, "verbose") && !NWNX_Log_IsEnabled(NWNX_LOG_LEVEL_WARNING, "quiet") &&
        NWNX_Log_IsEnabled(NWNX_LOG_LEVEL_ERROR, "quiet") && !NWNX_Log_IsEnabled(NWNX_LOG_LEVEL_DEBUG, "other"));

    NWNX_Log_SetLogger("verbose");
    int bBound = NWNX_Log_IsEnabled(NWNX_LOG_LEVEL_DEBUG);
    NWNX_Log_Unbind("logger");
    NWNX_Tests_Report("NWNX_Log", "IsEnabledBoundLogger", bBound && !NWNX_Log_IsEnabled(NWNX_LOG_LEVEL_DEBUG));

    NWNX_Log_Bind(JsonParse("{\"txn\":\"a1\",\"gold\":250}"));
    NWNX_Tests_Report("NWNX_Log", "Bind", GetBinding("txn") == "a1" && GetBinding("gold") == "250");
    NWNX_Log_Bind(JsonParse("{\"gold\":300}"));
    NWNX_Tests_Report("NWNX_Log", "BindMerge", GetBinding("txn") == "a1" && GetBinding("gold") == "300");
    NWNX_Log_Bind(JsonArray());
    NWNX_Tests_Report("NWNX_Log", "BindNonObjectIgnored", JsonGetLength(NWNX_Log_GetBindings()) == 2);

    NWNX_Log_SetGlobal(JsonParse("{\"shard\":\"main\"}"));
    NWNX_Tests_Report("NWNX_Log", "SetGlobal", GetBinding("shard") == "main");
    NWNX_Log_Bind(JsonParse("{\"shard\":\"run\"}"));
    NWNX_Tests_Report("NWNX_Log", "RunOverridesGlobal", GetBinding("shard") == "run");
    NWNX_Log_Unbind("shard");
    NWNX_Tests_Report("NWNX_Log", "UnbindRevealsGlobal", GetBinding("shard") == "main");

    RunMode("inherit");
    NWNX_Tests_Report("NWNX_Log", "ResetAfterExecuteScript", GetBinding("txn") == "a1" && !HasBinding("child"));
    RunMode("unbind");

    // Runs a chunk at the next level; ALLOW_NWNX_FUNCTIONS_IN_EXECUTE_SCRIPT_CHUNK must be on.
    ExecuteScriptChunk("#include \"nwnx_log\"\nvoid main() { " +
        "SetLocalString(GetModule(), \"NWNX_LOG_T_CHUNK\", JsonDump(JsonObjectGet(NWNX_Log_GetBindings(), \"txn\"))); " +
        "NWNX_Log_Bind(JsonParse(\"{\\\"leak\\\":\\\"chunk\\\"}\")); NWNX_Log_Info(\"from-chunk\"); }", OBJECT_SELF, FALSE);
    NWNX_Tests_Report("NWNX_Log", "InheritIntoScriptChunk", GetLocalString(GetModule(), "NWNX_LOG_T_CHUNK") == "\"a1\"");
    NWNX_Tests_Report("NWNX_Log", "ResetAfterScriptChunk", !HasBinding("leak"));

    // Check in the server log: "deposit ... shard=main gold=250 note="two words" txn=call", "verbose-debug" at D
    // with logger=verbose, "array-fields ... data=[1,2]" at E, and neither "filtered-debug" nor "quiet-warning".
    NWNX_Log_Info("deposit", JsonParse("{\"gold\":250,\"note\":\"two words\",\"txn\":\"call\"}"));
    NWNX_Log_Debug("filtered-debug");
    NWNX_Log_Write(NWNX_LOG_LEVEL_DEBUG, "verbose-debug", JSON_NULL, "verbose");
    NWNX_Log_Write(NWNX_LOG_LEVEL_WARNING, "quiet-warning", JSON_NULL, "quiet");
    NWNX_Log_Error("array-fields", JsonParse("[1,2]"));

    NWNX_Log_Unbind("gold");
    NWNX_Tests_Report("NWNX_Log", "Unbind", !HasBinding("gold") && GetBinding("txn") == "a1");
    NWNX_Log_Unbind();
    NWNX_Tests_Report("NWNX_Log", "UnbindAll", !HasBinding("txn") && GetBinding("shard") == "main");

    // The NWNX target does nothing yet and counts as done; the server log is open while the module runs.
    NWNX_Tests_Report("NWNX_Log", "RotateNwnx", NWNX_Log_Rotate(NWNX_LOG_ROTATE_NWNX));
    NWNX_Tests_Report("NWNX_Log", "RotateServer", NWNX_Log_Rotate(NWNX_LOG_ROTATE_SERVER));

    NWNX_Log_Bind(JsonParse("{\"txn\":\"delay\"}"));
    DelayCommand(0.5, CheckDelay());

    // Abnormal ends, nested and at top level; the check runs at the same depth afterwards.
    DelayCommand(1.0, RunMode("abort", "abort_nested"));
    DelayCommand(1.5, RunMode("check", "ResetAfterAbortNested", "abort_nested"));
    DelayCommand(2.0, BindAndAbort("abort"));
    DelayCommand(2.5, CheckNoLeak("ResetAfterAbortTop", "abort"));
    DelayCommand(3.0, RunMode("loop", "loop_nested"));
    DelayCommand(3.5, RunMode("check", "ResetAfterInstructionLimitNested", "loop_nested"));
    DelayCommand(4.0, BindAndLoop("loop"));
    DelayCommand(4.5, CheckNoLeak("ResetAfterInstructionLimitTop", "loop"));
    DelayCommand(5.0, Finish());
}
