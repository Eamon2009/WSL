/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PluginManager.cpp

Abstract:

    This file contains the PluginManager helper class implementation.
    Plugins are loaded in isolated wslpluginhost.exe processes via COM,
    so a crashing plugin cannot take down the WSL service.

--*/

#include "precomp.h"
#include "install.h"
#include "PluginManager.h"
#include "WslPluginApi.h"
#include "WslPluginHost.h"
#include "LxssUserSessionFactory.h"

using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;
using wsl::windows::service::PluginHostCallbackImpl;
using wsl::windows::service::PluginManager;

constexpr auto c_pluginPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Lxss\\Plugins";

// --- IWslPluginHostCallback implementation (service-side) ---
// These methods handle API calls from the plugin host process.

STDMETHODIMP PluginHostCallbackImpl::MountFolder(_In_ DWORD SessionId, _In_ LPCWSTR WindowsPath, _In_ LPCWSTR LinuxPath, _In_ BOOL ReadOnly, _In_ LPCWSTR Name)
try
{
    RETURN_HR_IF(E_INVALIDARG, WindowsPath == nullptr || LinuxPath == nullptr || Name == nullptr);

    WSL_LOG(
        "PluginCallbackMountFolderBegin",
        TraceLoggingValue(WindowsPath, "WindowsPath"),
        TraceLoggingValue(SessionId, "SessionId"));
    const auto session = FindSessionByCookie(SessionId);
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    auto result = session->MountRootNamespaceFolder(WindowsPath, LinuxPath, ReadOnly, Name);

    WSL_LOG("PluginCallbackMountFolderEnd", TraceLoggingValue(WindowsPath, "WindowsPath"), TraceLoggingValue(result, "Result"));

    return result;
}
CATCH_RETURN();

STDMETHODIMP PluginHostCallbackImpl::ExecuteBinary(
    _In_ DWORD SessionId, _In_ LPCSTR Path, _In_ DWORD ArgumentCount, _In_reads_opt_(ArgumentCount) LPCSTR* Arguments, _Out_ HANDLE* Socket)
try
{
    RETURN_HR_IF(E_POINTER, Socket == nullptr);
    *Socket = nullptr;
    RETURN_HR_IF(E_INVALIDARG, Path == nullptr);
    RETURN_HR_IF(E_INVALIDARG, ArgumentCount > 0 && Arguments == nullptr);

    WSL_LOG("PluginCallbackExecuteBinaryBegin", TraceLoggingValue(Path, "Path"), TraceLoggingValue(SessionId, "SessionId"));
    const auto session = FindSessionByCookie(SessionId);
    WSL_LOG(
        "PluginCallbackExecuteBinaryFoundSession",
        TraceLoggingValue(Path, "Path"),
        TraceLoggingValue(session != nullptr, "Found"));
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    // Build NULL-terminated argument array expected by CreateLinuxProcess.
    std::vector<LPCSTR> args;
    if (Arguments != nullptr)
    {
        args.assign(Arguments, Arguments + ArgumentCount);
    }
    args.push_back(nullptr);

    WSL_LOG("PluginCallbackExecuteBinaryCallingCreateProcess", TraceLoggingValue(Path, "Path"));
    wil::unique_socket sock;
    auto result = session->CreateLinuxProcess(nullptr, Path, args.data(), &sock);

    WSL_LOG("PluginCallbackExecuteBinaryEnd", TraceLoggingValue(Path, "Path"), TraceLoggingValue(result, "Result"));

    if (SUCCEEDED(result))
    {
        // Return socket as HANDLE — COM's system_handle marshaling will
        // duplicate it into the host process automatically.
        *Socket = reinterpret_cast<HANDLE>(sock.release());
    }

    return result;
}
CATCH_RETURN();

STDMETHODIMP PluginHostCallbackImpl::ExecuteBinaryInDistribution(
    _In_ DWORD SessionId,
    _In_ const GUID* DistributionId,
    _In_ LPCSTR Path,
    _In_ DWORD ArgumentCount,
    _In_reads_opt_(ArgumentCount) LPCSTR* Arguments,
    _Out_ HANDLE* Socket)
try
{
    RETURN_HR_IF(E_POINTER, Socket == nullptr);
    *Socket = nullptr;
    RETURN_HR_IF(E_INVALIDARG, DistributionId == nullptr);
    RETURN_HR_IF(E_INVALIDARG, Path == nullptr);
    RETURN_HR_IF(E_INVALIDARG, ArgumentCount > 0 && Arguments == nullptr);

    const auto session = FindSessionByCookie(SessionId);
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    std::vector<LPCSTR> args;
    if (Arguments != nullptr)
    {
        args.assign(Arguments, Arguments + ArgumentCount);
    }
    args.push_back(nullptr);

    wil::unique_socket sock;
    auto result = session->CreateLinuxProcess(DistributionId, Path, args.data(), &sock);

    WSL_LOG("PluginExecuteBinaryInDistributionCall", TraceLoggingValue(Path, "Path"), TraceLoggingValue(result, "Result"));

    if (SUCCEEDED(result))
    {
        *Socket = reinterpret_cast<HANDLE>(sock.release());
    }

    return result;
}
CATCH_RETURN();

// --- PluginManager implementation ---

PluginManager::~PluginManager()
{
    // Release all COM proxies, which will cause the host processes to exit.
    m_plugins.clear();
}

void PluginManager::LoadPlugins()
{
    ExecutionContext context(Context::Plugin);

    const auto key = common::registry::CreateKey(HKEY_LOCAL_MACHINE, c_pluginPath, KEY_READ);
    const auto values = common::registry::EnumValues(key.get());

    std::set<std::wstring, wsl::shared::string::CaseInsensitiveCompare> loaded;
    for (const auto& e : values)
    {
        if (e.second != REG_SZ)
        {
            LOG_HR_MSG(E_UNEXPECTED, "Plugin value: '%ls' has incorrect type: %lu, skipping", e.first.c_str(), e.second);
            continue;
        }

        auto path = common::registry::ReadString(key.get(), nullptr, e.first.c_str());

        if (!loaded.insert(path).second)
        {
            LOG_HR_MSG(E_UNEXPECTED, "Module '%ls' has already been loaded, skipping plugin '%ls'", path.c_str(), e.first.c_str());
            continue;
        }

        // Record the plugin for deferred activation. The actual COM host process
        // is created in EnsureInitialized(), which runs after the service's COM
        // initialization is complete (CoInitializeSecurity must happen first).
        OutOfProcPlugin plugin{};
        plugin.name = e.first;
        plugin.path = path;
        m_plugins.emplace_back(std::move(plugin));

        // Discovery-only event. The plugin DLL is no longer loaded into
        // wslservice.exe — actual load happens out-of-process via COM
        // activation in EnsureInitialized(). See "PluginLoad" emitted from
        // that path for the real load result.
        WSL_LOG_TELEMETRY(
            "PluginDiscovered",
            PDT_ProductAndServiceUsage,
            TraceLoggingValue(e.first.c_str(), "Name"),
            TraceLoggingValue(path.c_str(), "Path"));
    }
}

PluginManager::ScopedComInit::ScopedComInit() : initHr(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))
{
}

PluginManager::ScopedComInit::~ScopedComInit()
{
    if (SUCCEEDED(initHr))
    {
        ::CoUninitialize();
    }
}

PluginManager::ScopedComInit::ScopedComInit(ScopedComInit&& other) noexcept : initHr(other.initHr)
{
    // Suppress uninit in moved-from instance.
    other.initHr = RPC_E_CHANGED_MODE;
}

HRESULT PluginManager::ScopedComInit::Result() const noexcept
{
    return (initHr == RPC_E_CHANGED_MODE) ? S_OK : initHr;
}

PluginManager::ScopedComInit PluginManager::EnsureInitialized()
{
    // Join the calling thread to the MTA for the duration of the dispatch. The
    // returned guard must outlive any subsequent e.host->Method(...) calls because
    // those are cross-process COM calls that require an initialized apartment.
    ScopedComInit coInit;
    THROW_IF_FAILED(coInit.Result());

    std::call_once(m_initOnce, [this]() {
        m_callback = Microsoft::WRL::Make<PluginHostCallbackImpl>();
        THROW_IF_NULL_ALLOC(m_callback);

        for (auto& e : m_plugins)
        {
            auto loadResult = wil::ResultFromException(WI_DIAGNOSTICS_INFO, [&]() { LoadPlugin(e); });

            // Canonical "plugin was actually loaded" telemetry — matches the
            // semantics of the pre-refactor PluginLoad event (emitted after
            // the entry point ran). PluginHostActivation below is the more
            // granular event covering the COM activation path specifically.
            WSL_LOG_TELEMETRY(
                "PluginLoad",
                PDT_ProductAndServiceUsage,
                TraceLoggingValue(e.name.c_str(), "Name"),
                TraceLoggingValue(e.path.c_str(), "Path"),
                TraceLoggingValue(loadResult, "Result"));

            WSL_LOG_TELEMETRY(
                "PluginHostActivation",
                PDT_ProductAndServiceUsage,
                TraceLoggingValue(e.name.c_str(), "Name"),
                TraceLoggingValue(e.path.c_str(), "Path"),
                TraceLoggingValue(loadResult, "Result"));

            if (FAILED(loadResult))
            {
                // Treat host-process crashes and benign COM activation races (server is
                // shutting down or its exec failed) as non-fatal — the plugin is simply
                // unavailable for this session. All other failures, including registration
                // errors (REGDB_E_CLASSNOTREG), access denials, and plugin-reported errors
                // from Initialize, are treated as fatal plugin load failures so the user
                // gets a clear error rather than a silently-disabled plugin.
                if (IsHostCrash(loadResult) || loadResult == CO_E_SERVER_EXEC_FAILURE || loadResult == CO_E_SERVER_STOPPING)
                {
                    LOG_HR_MSG(loadResult, "Plugin host activation failed for: '%ls', skipping", e.name.c_str());
                }
                else
                {
                    m_pluginError.emplace(PluginError{e.name, loadResult});
                }
            }
        }
    });

    return coInit;
}

void PluginManager::LoadPlugin(OutOfProcPlugin& plugin)
{
    // Activate the plugin host via COM. The LocalServer32 registration causes COM
    // to spawn wslpluginhost.exe automatically.
    Microsoft::WRL::ComPtr<IWslPluginHost> host;
    HRESULT activationHr = CoCreateInstance(CLSID_WslPluginHost, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&host));
    WSL_LOG(
        "PluginHostActivation",
        TraceLoggingValue(plugin.name.c_str(), "Plugin"),
        TraceLoggingValue(plugin.path.c_str(), "Path"),
        TraceLoggingValue(activationHr, "CoCreateInstanceResult"));
    THROW_IF_FAILED_MSG(activationHr, "Failed to create plugin host for: '%ls'", plugin.path.c_str());

    THROW_IF_FAILED_MSG(
        host->Initialize(m_callback.Get(), plugin.path.c_str(), plugin.name.c_str()),
        "Plugin host failed to initialize: '%ls'",
        plugin.path.c_str());

    // Add the plugin host process to our job object so it is automatically
    // terminated if wslservice exits or crashes. If this fails the host will
    // still be reaped via CoReleaseServerProcess on clean shutdown, but won't
    // be killed on a service crash — surface the failure so it's diagnosable.
    EnsureJobObjectCreated();
    wil::unique_handle process;
    const HRESULT getProcessHr = host->GetProcessHandle(&process);
    LOG_IF_FAILED_MSG(getProcessHr, "Failed to get plugin host process handle for: '%ls'", plugin.path.c_str());
    if (SUCCEEDED(getProcessHr))
    {
        LOG_IF_WIN32_BOOL_FALSE_MSG(
            AssignProcessToJobObject(m_jobObject.get(), process.get()),
            "Failed to assign plugin host to job object for: '%ls'",
            plugin.path.c_str());
    }

    plugin.host = std::move(host);
}

void PluginManager::EnsureJobObjectCreated()
{
    std::call_once(m_jobObjectOnce, [this]() {
        m_jobObject.reset(CreateJobObjectW(nullptr, nullptr));
        THROW_LAST_ERROR_IF(!m_jobObject);

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo{};
        jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(m_jobObject.get(), JobObjectExtendedLimitInformation, &jobInfo, sizeof(jobInfo)));
    });
}

std::vector<BYTE> PluginManager::SerializeSid(PSID Sid)
{
    const DWORD sidLength = GetLengthSid(Sid);
    std::vector<BYTE> buffer(sidLength);
    CopySid(sidLength, buffer.data(), Sid);
    return buffer;
}

void PluginManager::OnVmStarted(const WSLSessionInformation* Session, const WSLVmCreationSettings* Settings)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (!e.host)
        {
            continue;
        }
        WSL_LOG("PluginOnVmStartedCall", TraceLoggingValue(e.name.c_str(), "Plugin"), TraceLoggingValue(Session->UserSid, "Sid"));

        wil::unique_cotaskmem_string errorMessage;
        WSL_LOG("PluginOnVmStartedBeginRpc", TraceLoggingValue(e.name.c_str(), "Plugin"));
        HRESULT hr = e.host->OnVMStarted(
            Session->SessionId,
            Session->UserToken,
            static_cast<DWORD>(sidData.size()),
            sidData.data(),
            static_cast<DWORD>(Settings->CustomConfigurationFlags),
            &errorMessage);
        WSL_LOG("PluginOnVmStartedEndRpc", TraceLoggingValue(e.name.c_str(), "Plugin"), TraceLoggingValue(hr, "Result"));

        if (IsHostCrash(hr))
        {
            LogPluginHostCrash(e, hr, "OnVmStarted");
            continue;
        }

        ThrowIfPluginError(hr, errorMessage.get(), Session->SessionId, e.name.c_str());
    }
}

void PluginManager::OnVmStopping(const WSLSessionInformation* Session)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (!e.host)
        {
            continue;
        }
        WSL_LOG("PluginOnVmStoppingCall", TraceLoggingValue(e.name.c_str(), "Plugin"), TraceLoggingValue(Session->UserSid, "Sid"));

        const auto result =
            e.host->OnVMStopping(Session->SessionId, Session->UserToken, static_cast<DWORD>(sidData.size()), sidData.data());

        if (IsHostCrash(result))
        {
            LogPluginHostCrash(e, result, "OnVmStopping");
            continue;
        }

        LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
    }
}

void PluginManager::OnDistributionStarted(const WSLSessionInformation* Session, const WSLDistributionInformation* Distribution)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (!e.host)
        {
            continue;
        }
        WSL_LOG(
            "PluginOnDistroStartedCall",
            TraceLoggingValue(e.name.c_str(), "Plugin"),
            TraceLoggingValue(Session->UserSid, "Sid"),
            TraceLoggingValue(Distribution->Id, "DistributionId"));

        wil::unique_cotaskmem_string errorMessage;
        HRESULT hr = e.host->OnDistributionStarted(
            Session->SessionId,
            Session->UserToken,
            static_cast<DWORD>(sidData.size()),
            sidData.data(),
            &Distribution->Id,
            Distribution->Name,
            Distribution->PidNamespace,
            Distribution->PackageFamilyName,
            Distribution->InitPid,
            Distribution->Flavor,
            Distribution->Version,
            &errorMessage);

        if (IsHostCrash(hr))
        {
            LogPluginHostCrash(e, hr, "OnDistributionStarted");
            continue;
        }

        ThrowIfPluginError(hr, errorMessage.get(), Session->SessionId, e.name.c_str());
    }
}

void PluginManager::OnDistributionStopping(const WSLSessionInformation* Session, const WSLDistributionInformation* Distribution)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (!e.host)
        {
            continue;
        }
        WSL_LOG(
            "PluginOnDistroStoppingCall",
            TraceLoggingValue(e.name.c_str(), "Plugin"),
            TraceLoggingValue(Session->UserSid, "Sid"),
            TraceLoggingValue(Distribution->Id, "DistributionId"));

        const auto result = e.host->OnDistributionStopping(
            Session->SessionId,
            Session->UserToken,
            static_cast<DWORD>(sidData.size()),
            sidData.data(),
            &Distribution->Id,
            Distribution->Name,
            Distribution->PidNamespace,
            Distribution->PackageFamilyName,
            Distribution->InitPid,
            Distribution->Flavor,
            Distribution->Version);

        if (IsHostCrash(result))
        {
            LogPluginHostCrash(e, result, "OnDistributionStopping");
            continue;
        }

        LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
    }
}

void PluginManager::OnDistributionRegistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* Distribution)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (!e.host)
        {
            continue;
        }
        WSL_LOG(
            "PluginOnDistributionRegisteredCall",
            TraceLoggingValue(e.name.c_str(), "Plugin"),
            TraceLoggingValue(Session->UserSid, "Sid"),
            TraceLoggingValue(Distribution->Id, "DistributionId"));

        const auto result = e.host->OnDistributionRegistered(
            Session->SessionId,
            Session->UserToken,
            static_cast<DWORD>(sidData.size()),
            sidData.data(),
            &Distribution->Id,
            Distribution->Name,
            Distribution->PackageFamilyName,
            Distribution->Flavor,
            Distribution->Version);

        if (IsHostCrash(result))
        {
            LogPluginHostCrash(e, result, "OnDistributionRegistered");
            continue;
        }

        LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
    }
}

void PluginManager::OnDistributionUnregistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* Distribution)
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    auto sidData = SerializeSid(Session->UserSid);

    for (auto& e : m_plugins)
    {
        if (!e.host)
        {
            continue;
        }
        WSL_LOG(
            "PluginOnDistributionUnregisteredCall",
            TraceLoggingValue(e.name.c_str(), "Plugin"),
            TraceLoggingValue(Session->UserSid, "Sid"),
            TraceLoggingValue(Distribution->Id, "DistributionId"));

        const auto result = e.host->OnDistributionUnregistered(
            Session->SessionId,
            Session->UserToken,
            static_cast<DWORD>(sidData.size()),
            sidData.data(),
            &Distribution->Id,
            Distribution->Name,
            Distribution->PackageFamilyName,
            Distribution->Flavor,
            Distribution->Version);

        if (IsHostCrash(result))
        {
            LogPluginHostCrash(e, result, "OnDistributionUnregistered");
            continue;
        }

        LOG_IF_FAILED_MSG(result, "Error thrown from plugin: '%ls'", e.name.c_str());
    }
}

void PluginManager::ThrowIfPluginError(HRESULT Result, LPWSTR ErrorMessage, WSLSessionId Session, LPCWSTR Plugin)
{
    // If the host process crashed, don't propagate as a fatal plugin error —
    // log it and let the caller decide. The plugin is already dead.
    if (IsHostCrash(Result))
    {
        LOG_HR_MSG(Result, "Plugin host process crashed for plugin: '%ls'", Plugin);
        return;
    }

    if (FAILED(Result))
    {
        if (ErrorMessage != nullptr && ErrorMessage[0] != L'\0')
        {
            THROW_HR_WITH_USER_ERROR(Result, wsl::shared::Localization::MessageFatalPluginErrorWithMessage(Plugin, ErrorMessage));
        }
        else
        {
            THROW_HR_WITH_USER_ERROR(Result, wsl::shared::Localization::MessageFatalPluginError(Plugin));
        }
    }
    else if (ErrorMessage != nullptr && ErrorMessage[0] != L'\0')
    {
        THROW_HR_MSG(E_ILLEGAL_STATE_CHANGE, "Plugin '%ls' emitted an error message but returned success", Plugin);
    }
}

bool PluginManager::IsHostCrash(HRESULT hr)
{
    // Each of these unambiguously indicates the COM server process has gone
    // away. RPC_E_CALL_REJECTED is deliberately excluded: it means a busy
    // server rejected the call, not that the server died — treating it as a
    // crash would silently disable the plugin for the rest of the session.
    switch (hr)
    {
    case RPC_E_DISCONNECTED:
    case RPC_E_SERVER_DIED:
    case RPC_E_SERVER_DIED_DNE:
    case CO_E_OBJNOTCONNECTED:
    case HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE):
    case HRESULT_FROM_WIN32(RPC_S_CALL_FAILED):
    case HRESULT_FROM_WIN32(RPC_S_CALL_FAILED_DNE):
        return true;
    default:
        return false;
    }
}

void PluginManager::LogPluginHostCrash(OutOfProcPlugin& plugin, HRESULT result, const char* stage)
{
    LOG_HR_MSG(result, "Plugin host crashed at %hs for: '%ls'", stage, plugin.name.c_str());

    // Fire telemetry only on first observation per plugin: a dead plugin will
    // hit this path on every subsequent VM/distro lifecycle event, and we
    // don't want to flood the telemetry channel with duplicates.
    if (!plugin.crashTelemetryFired.exchange(true))
    {
        WSL_LOG_TELEMETRY(
            "PluginHostCrash",
            PDT_ProductAndServiceUsage,
            TraceLoggingValue(plugin.name.c_str(), "Name"),
            TraceLoggingValue(plugin.path.c_str(), "Path"),
            TraceLoggingValue(result, "Result"),
            TraceLoggingValue(stage, "Stage"));
    }
}

void PluginManager::ThrowIfFatalPluginError()
{
    ExecutionContext context(Context::Plugin);
    auto coInit = EnsureInitialized();

    if (!m_pluginError.has_value())
    {
        return;
    }
    else if (m_pluginError->error == WSL_E_PLUGIN_REQUIRES_UPDATE)
    {
        THROW_HR_WITH_USER_ERROR(
            WSL_E_PLUGIN_REQUIRES_UPDATE, wsl::shared::Localization::MessagePluginRequiresUpdate(m_pluginError->plugin));
    }
    else
    {
        THROW_HR_WITH_USER_ERROR(m_pluginError->error, wsl::shared::Localization::MessageFatalPluginError(m_pluginError->plugin));
    }
}
