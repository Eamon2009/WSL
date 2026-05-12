/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Plugin.cpp

Abstract:

    This file contains a test plugin.

--*/

#include "precomp.h"
#include "WslPluginApi.h"

#include <atomic>
#include <future>

#include "PluginTests.h"

using namespace wsl::windows::common::registry;

std::ofstream g_logfile;
std::optional<GUID> g_distroGuid;

const WSLPluginAPIV1* g_api = nullptr;
PluginTestType g_testType = PluginTestType::Invalid;

std::optional<uint32_t> g_previousInitPid;

// Serializes writes to g_logfile from multiple threads in modes that spawn
// worker threads (ConcurrentApiCalls, AsyncApiCall, CallbackDuringTermination).
// Hook-thread writes that don't overlap with worker writes don't need to take
// this — but it's harmless to do so.
std::mutex g_logMutex;

void LogLine(const std::string& line)
{
    std::lock_guard guard{g_logMutex};
    g_logfile << line << std::endl;
}

// State for AsyncApiCall: worker thread launched in OnDistroStarted, joined
// in OnDistroStopping. The promise carries the result so the hook can log it
// after the join. The future is retrieved exactly once (in OnDistroStarted)
// and consumed in OnDistroStopping — std::promise::get_future() can only be
// called once per promise instance.
std::optional<std::thread> g_asyncWorker;
std::optional<std::promise<HRESULT>> g_asyncWorkerResult;
std::future<HRESULT> g_asyncWorkerFuture;
std::string g_asyncWorkerOutput;

// State for CallbackDuringTermination: detached workers exit when they observe
// a failure AFTER OnVmStopping has set g_drainStopAfterFailure. Using an
// explicit signal (rather than counting consecutive failures) avoids the
// possibility of a worker "reviving" if a subsequent StartWsl creates a new
// VM with the same session/distro IDs before the worker has accumulated
// enough failures to self-terminate. The signal is set inside the OnVmStopping
// hook, which fires BEFORE _VmTerminate's exclusive-lock drain (see
// LxssUserSession::_VmTerminate), so workers still race the drain — they just
// stop deterministically once it completes and m_utilityVm.reset() makes
// further calls fail.
//
// g_drainWorkersStarted prevents a second OnDistroStarted (triggered by the
// post-shutdown StartWsl the test uses to verify the service survived) from
// spawning a fresh batch of workers and leaking them past the end of the
// test — workers spawn at most once per test execution.
std::atomic<int> g_drainSuccess{0};
std::atomic<int> g_drainFailures{0};
std::atomic<bool> g_drainStopAfterFailure{false};
std::atomic<bool> g_drainWorkersStarted{false};

std::vector<char> ReadFromSocket(SOCKET socket)
{
    // Simplified error handling for the sake of the demo.
    int result = 0;
    int offset = 0;

    std::vector<char> content(1024);
    while ((result = recv(socket, content.data() + offset, 1024, 0)) > 0)
    {
        offset += result;
        content.resize(offset + 1024);
    }

    content.resize(offset);
    return content;
}

HRESULT OnVmStarted(const WSLSessionInformation* Session, const WSLVmCreationSettings* Settings)
{
    g_logfile << "VM created (settings->CustomConfigurationFlags=" << Settings->CustomConfigurationFlags << ")" << std::endl;

    if (g_testType == PluginTestType::FailToStartVm)
    {
        g_logfile << "OnVmStarted: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }
    else if (g_testType == PluginTestType::FailToStartVmWithPluginErrorMessage)
    {
        g_logfile << "OnVmStarted: E_UNEXPECTED" << std::endl;
        g_api->PluginError(L"Plugin error message");
        return E_UNEXPECTED;
    }
    else if (WI_IsFlagSet(Settings->CustomConfigurationFlags, WSLUserConfigurationCustomKernel))
    {
        g_logfile << "OnVmStarted: E_ACCESSDENIED" << std::endl;
        return E_ACCESSDENIED;
    }
    else if (g_testType == PluginTestType::Success)
    {
        // Get the current module's directory
        std::filesystem::path modulePath = wil::GetModuleFileNameW(wil::GetModuleInstanceHandle()).get();
        auto mountSource = modulePath.parent_path().wstring();

        // Mount the folder with the linux binary in the vm
        RETURN_IF_FAILED(
            g_api->MountFolder(Session->SessionId, mountSource.c_str(), L"/test-plugin/deep/folder", true, L"test-plugin-mount"));

        g_logfile << "Folder mounted (" << wsl::shared::string::WideToMultiByte(mountSource) << " -> /test-plugin)" << std::endl;

        // Create a file with dummy content
        std::ofstream file(mountSource + L"\\test-file.txt");
        if (!file || !(file << "OK"))
        {
            g_logfile << "Failed to open test-file.txt in: " << wsl::shared::string::WideToMultiByte(mountSource) << std::endl;
            return E_ABORT;
        }

        file.close();

        // Launch the process
        std::vector<const char*> arguments = {"/bin/cat", "/test-plugin/deep/folder/test-file.txt", nullptr};
        wil::unique_socket socket;
        RETURN_IF_FAILED(g_api->ExecuteBinary(Session->SessionId, arguments[0], arguments.data(), &socket));
        g_logfile << "Process created" << std::endl;

        // Read the socket output
        auto output = ReadFromSocket(socket.get());
        if (output != std::vector<char>{'O', 'K'})
        {
            g_logfile << "Got unexpected output from bash" << std::endl;
            return E_ABORT;
        }
    }
    else if (g_testType == PluginTestType::ApiErrors)
    {
        auto result = g_api->MountFolder(Session->SessionId, L"C:\\DoesNotExit", L"/dummy", true, L"test-plugin-mount");
        if (result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        {
            g_logfile << "Unexpected error for MountFolder(): " << result << std::endl;
            return E_ABORT;
        }

        wil::unique_socket socket;
        std::vector<const char*> arguments = {"/bin/does-no-exist", nullptr};
        result = g_api->ExecuteBinary(Session->SessionId, arguments[0], arguments.data(), &socket);
        if (result != E_FAIL)
        {
            g_logfile << "Unexpected error for ExecuteBinary(): " << result << std::endl;
            return E_ABORT;
        }

        result = g_api->ExecuteBinary(0xcafe, arguments[0], arguments.data(), &socket);
        if (result != RPC_E_DISCONNECTED)
        {
            g_logfile << "Unexpected error for ExecuteBinary(): " << result << std::endl;
            return E_ABORT;
        }

        // Call PluginError asynchronously to verify that we handle this properly.

        std::thread thread{[Session]() {
            const auto result = g_api->PluginError(L"Dummy");

            if (result != E_ILLEGAL_METHOD_CALL)
            {
                g_logfile << "Unexpected error for async PluginError(): " << result << std::endl;
            }
        }};

        thread.join();

        g_logfile << "API error tests passed" << std::endl;
    }
    else if (g_testType == PluginTestType::ErrorMessageStartVm)
    {
        auto result = g_api->PluginError(L"StartVm plugin error message");
        if (FAILED(result))
        {
            g_logfile << "Unexpected error from PluginError(): " << result << std::endl;
        }
        g_logfile << "OnVmStarted: E_FAIL" << std::endl;
        return E_FAIL;
    }
    else if (g_testType == PluginTestType::GetUsername)
    {
        try
        {
            auto info = wil::get_token_information<TOKEN_USER>(Session->UserToken);

            DWORD size{};
            DWORD domainSize{};
            SID_NAME_USE use{};
            LookupAccountSid(nullptr, info->User.Sid, nullptr, &size, nullptr, &domainSize, &use);

            THROW_HR_IF(E_UNEXPECTED, size < 1);
            std::wstring user(size - 1, '\0');
            std::wstring domain(domainSize - 1, '\0');

            THROW_IF_WIN32_BOOL_FALSE(LookupAccountSid(nullptr, info->User.Sid, user.data(), &size, domain.data(), &domainSize, &use));

            g_logfile << "Username: " << wsl::shared::string::WideToMultiByte(domain) << "\\"
                      << wsl::shared::string::WideToMultiByte(user) << std::endl;
        }
        catch (...)
        {
            g_logfile << "OnVmStarted: get_token_information failed: " << wil::ResultFromCaughtException() << std::endl;
            return E_FAIL;
        }

        return S_OK;
    }
    else if (g_testType == PluginTestType::HostCrash)
    {
        // Validate plugin host crash isolation. Forcefully exit the host
        // process so the COM RPC returns one of the HRESULTs in IsHostCrash
        // (RPC_E_DISCONNECTED / RPC_E_SERVER_DIED / ...). The service should
        // treat this as non-fatal and continue.
        LogLine("Crashing host");
        g_logfile.flush();
        TerminateProcess(GetCurrentProcess(), 1);
        // Unreachable.
        return E_UNEXPECTED;
    }
    else if (g_testType == PluginTestType::ConcurrentApiCalls)
    {
        // Validate concurrent service-side callbacks under the new
        // m_callbackLock (shared_mutex). N threads call MountFolder +
        // ExecuteBinary in parallel via a start-gate so the shared_lock has
        // multiple readers in flight at once.
        constexpr int N = 4;

        std::filesystem::path modulePath = wil::GetModuleFileNameW(wil::GetModuleInstanceHandle()).get();
        const auto mountSource = modulePath.parent_path().wstring();

        std::mutex gateMutex;
        std::condition_variable gateCv;
        int arrived = 0;
        bool released = false;

        std::atomic<int> successes{0};
        std::atomic<int> failures{0};

        const auto worker = [&](int index) {
            // Wait for all workers to reach the gate so the API calls overlap.
            {
                std::unique_lock lock{gateMutex};
                ++arrived;
                if (arrived == N)
                {
                    released = true;
                    gateCv.notify_all();
                }
                else
                {
                    gateCv.wait(lock, [&]() { return released; });
                }
            }

            const auto linuxPath = L"/test-plugin/concurrent-" + std::to_wstring(index);
            const auto mountName = L"test-plugin-concurrent-" + std::to_wstring(index);
            HRESULT hr = g_api->MountFolder(Session->SessionId, mountSource.c_str(), linuxPath.c_str(), true, mountName.c_str());
            if (FAILED(hr))
            {
                ++failures;
                return;
            }

            wil::unique_socket socket;
            std::vector<const char*> args = {"/bin/true", nullptr};
            hr = g_api->ExecuteBinary(Session->SessionId, args[0], args.data(), &socket);
            if (FAILED(hr))
            {
                ++failures;
                return;
            }

            ++successes;
        };

        std::vector<std::thread> threads;
        threads.reserve(N);
        for (int i = 0; i < N; ++i)
        {
            threads.emplace_back(worker, i);
        }
        for (auto& t : threads)
        {
            t.join();
        }

        LogLine(
            "Concurrent callbacks complete: success=" + std::to_string(successes.load()) + " failures=" + std::to_string(failures.load()));

        if (failures.load() != 0)
        {
            return E_FAIL;
        }
    }

    return S_OK;
}

HRESULT OnVmStopping(const WSLSessionInformation* Session)
{
    if (g_testType == PluginTestType::CallbackDuringTermination)
    {
        // Signal detached drain workers to exit on their next failure. Fires
        // before _VmTerminate's exclusive m_callbackLock acquire, so workers
        // still race the drain — they just stop deterministically once
        // m_utilityVm.reset() starts failing subsequent calls.
        g_drainStopAfterFailure = true;
    }

    g_logfile << "VM Stopping" << std::endl;

    if (g_testType == PluginTestType::FailToStopVm)
    {
        g_logfile << "OnVmStopping: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }

    return S_OK;
}

HRESULT OnDistroStarted(const WSLSessionInformation* Session, const WSLDistributionInformation* Distribution)
{
    g_logfile << "Distribution started, name=" << wsl::shared::string::WideToMultiByte(Distribution->Name)
              << ", package=" << wsl::shared::string::WideToMultiByte(Distribution->PackageFamilyName)
              << ", PidNs=" << Distribution->PidNamespace << ", InitPid=" << Distribution->InitPid
              << ", Flavor=" << wsl::shared::string::WideToMultiByte(Distribution->Flavor)
              << ", Version=" << wsl::shared::string::WideToMultiByte(Distribution->Version) << std::endl;

    if (g_testType == PluginTestType::FailToStartDistro)
    {
        g_logfile << "OnDistroStarted: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }
    else if (g_testType == PluginTestType::SameDistroId)
    {
        if (g_distroGuid.has_value())
        {
            if (IsEqualGUID(g_distroGuid.value(), Distribution->Id))
            {
                g_logfile << "OnDistroStarted: received same GUID" << std::endl;
            }
            else
            {
                g_logfile << "OnDistroStarted: received different GUID" << std::endl;
            }
        }
        else
        {
            g_distroGuid = Distribution->Id;
        }
    }
    else if (g_testType == PluginTestType::ErrorMessageStartDistro)
    {
        g_logfile << "OnDistroStarted: E_FAIL" << std::endl;
        g_api->PluginError(L"StartDistro plugin error message");
        return E_FAIL;
    }
    else if (g_testType == PluginTestType::InitPidIsDifferent)
    {
        if (g_previousInitPid.has_value())
        {
            if (g_previousInitPid.value() != Distribution->InitPid)
            {
                g_logfile << "Init's pid is different (" << Distribution->InitPid << " ! = " << g_previousInitPid.value() << ")" << std::endl;
            }
            else
            {
                g_logfile << "Init's pid did not change (" << g_previousInitPid.value() << ")" << std::endl;
                return E_FAIL;
            }
        }
        else
        {
            g_previousInitPid = Distribution->InitPid;
        }
    }
    else if (g_testType == PluginTestType::RunDistroCommand)
    {
        // Launch a process
        std::vector<const char*> arguments = {"/bin/sh", "-c", "cat /etc/issue.net", nullptr};
        wil::unique_socket socket;
        RETURN_IF_FAILED(g_api->ExecuteBinaryInDistribution(Session->SessionId, &Distribution->Id, arguments[0], arguments.data(), &socket));
        g_logfile << "Process created" << std::endl;

        // Validate that the process actually ran inside the distro.
        auto output = ReadFromSocket(socket.get());
        const auto expected = "Debian GNU/Linux 13\n";
        if (std::string(output.begin(), output.end()) != expected)
        {
            g_logfile << "Got unexpected output from bash: " << std::string(output.begin(), output.end())
                      << ", expected: " << expected << std::endl;
            return E_ABORT;
        }

        // Verify that failure to launch a process behaves properly.
        arguments = {"/does-not-exist"};
        g_logfile << "Failed process launch returned:  "
                  << g_api->ExecuteBinaryInDistribution(Session->SessionId, &Distribution->Id, arguments[0], arguments.data(), &socket)
                  << std::endl;

        const GUID guid{};
        g_logfile << "Invalid distro launch returned:  "
                  << g_api->ExecuteBinaryInDistribution(Session->SessionId, &guid, arguments[0], arguments.data(), &socket) << std::endl;
    }
    else if (g_testType == PluginTestType::AsyncApiCall)
    {
        // Validate plugin API calls from a worker thread that outlives the
        // hook. The worker thread is joined in OnDistroStopping — joining is
        // unconditional (no timeout) because letting the worker outlive
        // g_pluginHost (cleared in ~PluginHost) would dereference freed memory.
        g_asyncWorkerOutput.clear();
        g_asyncWorkerResult.emplace();
        g_asyncWorkerFuture = g_asyncWorkerResult->get_future();

        const DWORD sessionId = Session->SessionId;
        const GUID distroId = Distribution->Id;

        g_asyncWorker.emplace([sessionId, distroId]() {
            // Sleep briefly so the call is guaranteed to happen after the
            // hook has returned — exercises the cross-apartment callback
            // path from a non-hook thread that hasn't called CoInitializeEx.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            wil::unique_socket socket;
            std::vector<const char*> args = {"/bin/echo", "hello-from-worker", nullptr};
            const HRESULT hr = g_api->ExecuteBinaryInDistribution(sessionId, &distroId, args[0], args.data(), &socket);

            if (SUCCEEDED(hr))
            {
                const auto output = ReadFromSocket(socket.get());
                std::string captured(output.begin(), output.end());
                // Strip trailing newline added by /bin/echo so the log line
                // doesn't get split when ValidateLogFile splits on '\n'.
                while (!captured.empty() && (captured.back() == '\n' || captured.back() == '\r'))
                {
                    captured.pop_back();
                }
                std::lock_guard guard{g_logMutex};
                g_asyncWorkerOutput = std::move(captured);
            }

            g_asyncWorkerResult->set_value(hr);
        });
    }
    else if (g_testType == PluginTestType::CallbackDuringTermination)
    {
        // Validate that the new exclusive m_callbackLock acquire in
        // _VmTerminate drains in-flight callbacks before m_utilityVm.reset().
        // Workers are intentionally DETACHED so they keep calling into the
        // service across OnDistroStopping / _VmTerminate.
        //
        // Termination protocol: OnVmStopping (which fires before the drain)
        // sets g_drainStopAfterFailure. Workers continue until they observe
        // a failure AFTER the flag is set — the natural race result, since
        // post-drain calls fail (m_utilityVm is null). This guarantees
        // workers exit before any subsequent StartWsl creates a new VM,
        // preventing a "revival" against a fresh m_utilityVm with the same
        // session/distro IDs.
        //
        // Scope: this test exercises only the *happy-path* drain — the
        // callback (/bin/true) returns in sub-millisecond, so workers are
        // almost always between iterations when the exclusive lock is
        // acquired. It is *not* a regression test for the hung-callback
        // case, where a service-side callback is stuck inside CreateLinuxProcess
        // waiting on a non-responsive Linux init; that scenario requires
        // termination-event plumbing through WslCoreInstance::CreateLinuxProcess
        // and is tracked separately.
        constexpr int N = 4;

        // Spawn at most once. The post-shutdown StartWsl in
        // CallbacksDuringTerminationDoNotCrash triggers another
        // OnDistroStarted; we must not start a fresh batch then or the
        // workers will leak past end-of-test.
        if (g_drainWorkersStarted.exchange(true))
        {
            return S_OK;
        }

        g_drainSuccess = 0;
        g_drainFailures = 0;
        g_drainStopAfterFailure = false;

        const DWORD sessionId = Session->SessionId;
        const GUID distroId = Distribution->Id;

        for (int i = 0; i < N; ++i)
        {
            std::thread worker([sessionId, distroId]() {
                while (true)
                {
                    wil::unique_socket socket;
                    std::vector<const char*> args = {"/bin/true", nullptr};
                    const HRESULT hr = g_api->ExecuteBinaryInDistribution(sessionId, &distroId, args[0], args.data(), &socket);
                    if (SUCCEEDED(hr))
                    {
                        ++g_drainSuccess;
                    }
                    else
                    {
                        ++g_drainFailures;
                        if (g_drainStopAfterFailure.load())
                        {
                            return;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
            worker.detach();
        }
    }

    return S_OK;
}

HRESULT OnDistroStopping(const WSLSessionInformation* Session, const WSLDistributionInformation* Distribution)
{
    // For AsyncApiCall we defer the "Distribution Stopping" line until after
    // the worker thread has been joined, so the worker's "Async worker output"
    // line is guaranteed to appear before it in the log.
    auto logDistroStopping = [&]() {
        g_logfile << "Distribution Stopping, name=" << wsl::shared::string::WideToMultiByte(Distribution->Name)
                  << ", package=" << wsl::shared::string::WideToMultiByte(Distribution->PackageFamilyName)
                  << ", PidNs=" << Distribution->PidNamespace << ", Flavor=" << wsl::shared::string::WideToMultiByte(Distribution->Flavor)
                  << ", Version=" << wsl::shared::string::WideToMultiByte(Distribution->Version) << std::endl;
    };

    if (g_testType == PluginTestType::AsyncApiCall)
    {
        if (g_asyncWorker.has_value())
        {
            // Unconditional join — letting the worker outlive g_pluginHost
            // (cleared in ~PluginHost) would dereference freed memory.
            g_asyncWorker->join();
            g_asyncWorker.reset();

            HRESULT workerHr = S_OK;
            if (g_asyncWorkerFuture.valid())
            {
                workerHr = g_asyncWorkerFuture.get();
                g_asyncWorkerResult.reset();
            }

            if (SUCCEEDED(workerHr))
            {
                LogLine("Async worker output: " + g_asyncWorkerOutput);
            }
            else
            {
                LogLine("Async worker failed: " + std::to_string(workerHr));
            }
        }

        logDistroStopping();
        return S_OK;
    }

    logDistroStopping();

    if (g_testType == PluginTestType::FailToStopDistro)
    {
        g_logfile << "OnDistroStopping: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }
    else if (g_testType == PluginTestType::SameDistroId && g_distroGuid.has_value())
    {
        if (!IsEqualGUID(g_distroGuid.value(), Distribution->Id))
        {
            g_logfile << "OnDistroStarted: received different GUID" << std::endl;
        }
    }

    return S_OK;
}

HRESULT OnDistributionRegistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* Distribution)
{
    g_logfile << "Distribution registered, name=" << wsl::shared::string::WideToMultiByte(Distribution->Name)
              << ", package=" << wsl::shared::string::WideToMultiByte(Distribution->PackageFamilyName)
              << ", Flavor=" << wsl::shared::string::WideToMultiByte(Distribution->Flavor)
              << ", Version=" << wsl::shared::string::WideToMultiByte(Distribution->Version) << std::endl;

    if (g_testType == PluginTestType::FailToRegisterUnregisterDistro)
    {
        g_logfile << "OnDistributionRegistered: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }

    return S_OK;
}

HRESULT OnDistributionUnregistered(const WSLSessionInformation* Session, const WslOfflineDistributionInformation* Distribution)
{
    g_logfile << "Distribution unregistered, name=" << wsl::shared::string::WideToMultiByte(Distribution->Name)
              << ", package=" << wsl::shared::string::WideToMultiByte(Distribution->PackageFamilyName)
              << ", Flavor=" << wsl::shared::string::WideToMultiByte(Distribution->Flavor)
              << ", Version=" << wsl::shared::string::WideToMultiByte(Distribution->Version) << std::endl;

    if (g_testType == PluginTestType::FailToRegisterUnregisterDistro)
    {
        g_logfile << "OnDistributionUnregistered: E_UNEXPECTED" << std::endl;
        return E_UNEXPECTED;
    }

    return S_OK;
}

EXTERN_C __declspec(dllexport) HRESULT WSLPLUGINAPI_ENTRYPOINTV1(const WSLPluginAPIV1* Api, WSLPluginHooksV1* Hooks)
{
    try
    {
        const auto key = OpenTestRegistryKey(KEY_READ);

        const std::wstring outputFile = ReadString(key.get(), nullptr, c_logFile);
        g_logfile.open(outputFile);
        THROW_HR_IF(E_UNEXPECTED, !g_logfile);

        g_testType = static_cast<PluginTestType>(ReadDword(key.get(), nullptr, c_testType, static_cast<DWORD>(PluginTestType::Invalid)));
        THROW_HR_IF(E_INVALIDARG, static_cast<DWORD>(g_testType) <= 0 || static_cast<DWORD>(g_testType) > static_cast<DWORD>(PluginTestType::CallbackDuringTermination));

        g_logfile << "Plugin loaded. TestMode=" << static_cast<DWORD>(g_testType) << std::endl;
        g_api = Api;
        Hooks->OnVMStarted = &OnVmStarted;
        Hooks->OnVMStopping = &OnVmStopping;
        Hooks->OnDistributionStarted = &OnDistroStarted;
        Hooks->OnDistributionStopping = &OnDistroStopping;
        Hooks->OnDistributionRegistered = &OnDistributionRegistered;
        Hooks->OnDistributionUnregistered = &OnDistributionUnregistered;

        if (g_testType == PluginTestType::FailToLoad)
        {
            g_logfile << "OnLoad: E_UNEXPECTED" << std::endl;
            return E_UNEXPECTED;
        }
        else if (g_testType == PluginTestType::PluginRequiresUpdate)
        {
            g_logfile << "OnLoad: WSL_E_PLUGINREQUIRESUPDATE" << std::endl;

            WSL_PLUGIN_REQUIRE_VERSION(9999, 99, 99, Api);
        }
    }
    catch (...)
    {
        const auto error = wil::ResultFromCaughtException();
        if (g_logfile)
        {
            g_logfile << "Failed to initialize plugin, " << error << std::endl;
        }

        return error;
    }
    return S_OK;
}