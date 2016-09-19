//==============================================================================
// Copyright (c) 2015 Advanced Micro Devices, Inc. All rights reserved.
/// \author AMD Developer Tools Team
/// \file
/// \brief  This class manages all the traces API objects
//==============================================================================

#include <string>
#include <algorithm>
#include <fstream>
#include "HSAFdnAPIInfoManager.h"
#include "HSAToolsRTModule.h"
#include "HSARTModuleLoader.h"
#include "FinalizerInfoManager.h"
#include "AutoGenerated/HSATraceInterception.h"
#include "AutoGenerated/HSATraceStringOutput.h"
#include "../Common/FileUtils.h"
#include "../Common/GlobalSettings.h"
#include "DeviceInfoUtils.h"
#include "../HSAFdnCommon/HSAFunctionDefsUtils.h"
#include <AMDTBaseTools/Include/gtAssert.h>

using namespace std;

HSAAPIInfoManager::HSAAPIInfoManager(void) : m_tracedApiCount(0)
{
    m_strTraceModuleName = "hsa";

    // add APIs that we should always intercept...
    m_mustInterceptAPIs.insert(HSA_API_Type_hsa_queue_create);               // needed so we can create a profiled queue for kernel timestamps
    m_mustInterceptAPIs.insert(HSA_API_Type_hsa_executable_get_symbol);      // needed to extract kernel name
    m_mustInterceptAPIs.insert(HSA_API_Type_hsa_executable_symbol_get_info); // needed to extract kernel name
    m_delayTimer = nullptr;
    m_durationTimer = nullptr;
}

HSAAPIInfoManager::~HSAAPIInfoManager(void)
{
    if (m_delayTimer)
    {
        m_delayTimer->stopTimer();
        SAFE_DELETE(m_delayTimer);
    }

    if (m_durationTimer)
    {
        m_durationTimer->stopTimer();
        SAFE_DELETE(m_durationTimer);
    }
}

bool HSAAPIInfoManager::WriteKernelTimestampEntry(std::ostream& sout, const hsa_profiler_kernel_time_t& record)
{
    FinalizerInfoManager* pFinalizerInfoMan = FinalizerInfoManager::Instance();

#ifdef _DEBUG
    Log(logMESSAGE, "Lookup %llu\n", record.kernel);

    Log(logMESSAGE, "Dump m_codeHandleToSymbolHandleMap\n");

    for (auto mapItem : pFinalizerInfoMan->m_codeHandleToSymbolHandleMap)
    {
        Log(logMESSAGE, "  Item: %llu == %llu\n", mapItem.first, mapItem.second);

        if (record.kernel == mapItem.first)
        {
            Log(logMESSAGE, "  Match found!\n");
        }
    }

    Log(logMESSAGE, "End Dump m_codeHandleToSymbolHandleMap\n");
#endif

    std::string symName;

    if (pFinalizerInfoMan->m_codeHandleToSymbolHandleMap.count(record.kernel) > 0)
    {
        uint64_t symHandle = pFinalizerInfoMan->m_codeHandleToSymbolHandleMap[record.kernel];

        if (pFinalizerInfoMan->m_symbolHandleToNameMap.count(symHandle) > 0)
        {
            symName = pFinalizerInfoMan->m_symbolHandleToNameMap[symHandle];
            Log(logMESSAGE, "Lookup: CodeHandle: %llu, SymHandle: %llu, symName: %s\n", record.kernel, symHandle, symName.c_str());
        }
    }

    if (symName.empty())
    {
        symName = "<UnknownKernelName>";
    }

    // Kernel name
    sout << std::left << std::setw(max((size_t)50, symName.length() + 1)) << symName;

    // Kernel pointer
    sout << std::left << std::setw(21) << StringUtils::ToHexString(record.kernel);

    // start time
    sout << std::left << std::setw(21) << record.time.start;

    // end time
    sout << std::left << std::setw(21) << record.time.end;

    uint32_t deviceId;
    hsa_status_t status = g_pRealCoreFunctions->hsa_agent_get_info_fn(record.agent, static_cast<hsa_agent_info_t>(HSA_AMD_AGENT_INFO_CHIP_ID), &deviceId);

    std::string strDeviceName = "<UnknownDeviceName>";

    if (HSA_STATUS_SUCCESS == status)
    {
        GDT_GfxCardInfo cardInfo;

        // TODO: need to get revision id from HSA runtime (SWDEV-79571)
        if (AMDTDeviceInfoUtils::Instance()->GetDeviceInfo(deviceId, REVISION_ID_ANY, cardInfo))
        {
            strDeviceName = std::string(cardInfo.m_szCALName);
        }
    }

    // agent (device) name
    sout << std::left << std::setw(64) << strDeviceName;

    // agent (device) handle
    sout << std::left << std::setw(21) << HSATraceStringUtils::Get_hsa_agent_t_String(record.agent);

    // queue index
    size_t queueIndex = 0;
    GetQueueIndex(record.queue, queueIndex);
    sout << std::left << std::setw(6) << StringUtils::ToString(queueIndex);

    // queue handle
    sout << std::left << std::setw(21) << StringUtils::ToHexString(record.queue);

    return true;
}

bool HSAAPIInfoManager::WriteAsyncCopyTimestamp(std::ostream& sout, const hsa_amd_profiling_async_copy_time_t& timestamp)
{
    sout << std::left << std::setw(21) << timestamp.start;
    sout << std::left << std::setw(21) << timestamp.end;
    return true;
}

void HSAAPIInfoManager::FlushNonAPITimestampData(const osProcessId& pid)
{
    if (HSARTModuleLoader<HSAToolsRTModule>::Instance()->IsLoaded())
    {
        HSAToolsRTModule* toolsRTModule = HSARTModuleLoader<HSAToolsRTModule>::Instance()->GetHSARTModule();

        if (toolsRTModule->IsModuleLoaded())
        {
            size_t count = toolsRTModule->ext_tools_get_kernel_times(0, NULL);

            if (count > 0)
            {
                hsa_profiler_kernel_time_t* records = new(std::nothrow) hsa_profiler_kernel_time_t[count];

                if (NULL != records)
                {
                    string tmpKernelTimestampFile = GetTempFileName(pid, 0, TMP_KERNEL_TIME_STAMP_EXT);
                    ofstream foutKTS(tmpKernelTimestampFile.c_str(), fstream::out | fstream::app);

                    count = toolsRTModule->ext_tools_get_kernel_times(count, records);

                    for (size_t i = 0; i < count; i++)
                    {
                        WriteKernelTimestampEntry(foutKTS, records[i]);
                        foutKTS << std::endl;
                    }

                    foutKTS.close();
                }
                else
                {
                    Log(logERROR, "FlushNonAPITimestampData: unable to allocate memory for kernel timestamps\n");
                }

                delete[] records;
            }
        }
        else
        {
            Log(logERROR, "FlushNonAPITimestampData: tools lib not loaded\n");
        }

        {
            AMDTScopeLock lock(m_asyncTimeStampsMtx);

            string tmpKernelTimestampFile = GetTempFileName(pid, 0, TMP_ASYNC_COPY_TIME_STAMP_EXT);
            ofstream foutCopyTS(tmpKernelTimestampFile.c_str(), fstream::out | fstream::app);

            for (auto timestamp : m_asyncCopyTimestamps)
            {
                WriteAsyncCopyTimestamp(foutCopyTS, timestamp);
                foutCopyTS << std::endl;
            }

            foutCopyTS.close();

            m_asyncCopyTimestamps.clear();
        }
    }
}

void HSAAPIInfoManager::AddAPIToFilter(const std::string& strAPIName)
{
    HSA_API_Type type = HSAFunctionDefsUtils::Instance()->ToHSAAPIType(strAPIName);

    if (type != HSA_API_Type_UNKNOWN)
    {
        m_filterAPIs.insert(type);
    }
    else
    {
        Log(logWARNING, "Unknown API name = %s\n", strAPIName.c_str());
    }
}

bool HSAAPIInfoManager::IsInFilterList(HSA_API_Type type) const
{
    return m_filterAPIs.find(type) != m_filterAPIs.end();
}

bool HSAAPIInfoManager::ShouldIntercept(HSA_API_Type type) const
{
    return !IsInFilterList(type) || m_mustInterceptAPIs.find(type) != m_mustInterceptAPIs.end();
}

bool HSAAPIInfoManager::IsCapReached() const
{
    return m_tracedApiCount >= GlobalSettings::GetInstance()->m_params.m_uiMaxNumOfAPICalls;
}

void HSAAPIInfoManager::AddQueue(const hsa_queue_t* pQueue)
{
    if (m_queueIndexMap.end() != m_queueIndexMap.find(pQueue))
    {
        Log(logWARNING, "Queue added to map more than once\n");
    }
    else
    {
        m_queueIndexMap.insert(QueueIndexMapPair(pQueue, m_queueIndexMap.size()));
    }
}

bool HSAAPIInfoManager::GetQueueIndex(const hsa_queue_t* pQueue, size_t& queueIndex) const
{
    bool retVal = false;

    QueueIndexMap::const_iterator it = m_queueIndexMap.find(pQueue);

    if (m_queueIndexMap.end() != it)
    {
        retVal = true;
        queueIndex = it->second;
    }

    return retVal;
}

struct AsyncHandlerParam
{
    hsa_signal_t m_signal;
};

bool AsyncSignalHandler(hsa_signal_value_t /* value */, void* arg)
{
    hsa_amd_profiling_async_copy_time_t asyncCopyTime;
    AsyncHandlerParam* pHandlerParam = reinterpret_cast<AsyncHandlerParam*>(arg);

    if (nullptr == pHandlerParam)
    {
        Log(logERROR, "AsyncSignalhandler called with a null user arg.\n");
    }
    else
    {
        hsa_status_t status = g_pRealAmdExtFunctions->hsa_amd_profiling_get_async_copy_time_fn(pHandlerParam->m_signal, &asyncCopyTime);

        if (HSA_STATUS_SUCCESS != status)
        {
            Log(logERROR, "Error returned from hsa_amd_profiling_get_dispatch_time\n");
        }
        else
        {
            HSAAPIInfoManager::Instance()->AddAsyncCopyTimestamp(asyncCopyTime);
        }

        delete pHandlerParam;
    }

    return false; // no longer monitor this signal (it will be re-added if necessary)
}

void HSAAPIInfoManager::AddAsyncCopyCompletionSignal(const hsa_signal_t& completionSignal)
{
    hsa_signal_value_t signalValue = g_pRealCoreFunctions->hsa_signal_load_scacquire_fn(completionSignal);

    AsyncHandlerParam* pHandlerParam = new(std::nothrow) AsyncHandlerParam();

    if (nullptr == pHandlerParam)
    {
        Log(logERROR, "AddAsyncCopyCompletionSignal: unable to allocate memory for async handler param\n");
    }
    else
    {
        pHandlerParam->m_signal = completionSignal;

        hsa_status_t status = g_pRealAmdExtFunctions->hsa_amd_signal_async_handler_fn(completionSignal, HSA_SIGNAL_CONDITION_LT, signalValue, AsyncSignalHandler, pHandlerParam);

        if (HSA_STATUS_SUCCESS != status)
        {
            Log(logERROR, "Error returned from hsa_amd_signal_async_handler\n");
        }
    }
}

void HSAAPIInfoManager::AddAsyncCopyTimestamp(const hsa_amd_profiling_async_copy_time_t& asyncCopyTime)
{
    AMDTScopeLock lock(m_asyncTimeStampsMtx);

    m_asyncCopyTimestamps.push_back(asyncCopyTime);
}

void HSATraceAgentTimerEndResponse(ProfilerTimerType timerType)
{
    switch (timerType)
    {
        case PROFILEDELAYTIMER:
            HSAAPIInfoManager::Instance()->ResumeTracing();
            unsigned long profilerDuration;

            if (HSAAPIInfoManager::Instance()->IsProfilerDurationEnabled(profilerDuration))
            {
                HSAAPIInfoManager::Instance()->CreateTimer(PROFILEDURATIONTIMER, profilerDuration);
                HSAAPIInfoManager::Instance()->SetTimerFinishHandler(PROFILEDURATIONTIMER, HSATraceAgentTimerEndResponse);
                HSAAPIInfoManager::Instance()->startTimer(PROFILEDURATIONTIMER);
            }

            break;

        case PROFILEDURATIONTIMER:
            HSAAPIInfoManager::Instance()->StopTracing();
            break;

        default:
            break;
    }
}

void HSAAPIInfoManager::EnableProfileDelayStart(bool doEnable, unsigned long delayInMilliseconds)
{
    m_bDelayStartEnabled = doEnable;
    m_delayInMilliseconds = doEnable ? delayInMilliseconds : 0;
}

void HSAAPIInfoManager::EnableProfileDuration(bool doEnable, unsigned long durationInMilliseconds)
{
    m_bProfilerDurationEnabled = doEnable;
    m_durationInMilliseconds = doEnable ? durationInMilliseconds : 0;
}

bool HSAAPIInfoManager::IsProfilerDelayEnabled(unsigned long& delayInMilliseconds)
{
    delayInMilliseconds = m_delayInMilliseconds;
    return m_bDelayStartEnabled;
}

bool HSAAPIInfoManager::IsProfilerDurationEnabled(unsigned long& durationInSeconds)
{
    durationInSeconds = m_durationInMilliseconds;
    return m_bProfilerDurationEnabled;
}

void HSAAPIInfoManager::SetTimerFinishHandler(ProfilerTimerType timerType, TimerEndHandler timerEndHandler)
{
    if (m_delayTimer || m_durationTimer)
    {

        switch (timerType)
        {
            case PROFILEDELAYTIMER:
                m_delayTimer->SetTimerFinishHandler(timerEndHandler);
                break;

            case PROFILEDURATIONTIMER:
                m_durationTimer->SetTimerFinishHandler(timerEndHandler);
                break;

            default:
                break;
        }
    }
}

void HSAAPIInfoManager::CreateTimer(ProfilerTimerType timerType, unsigned long timeIntervalInMilliseconds)
{
    switch (timerType)
    {
        case PROFILEDELAYTIMER:
            if (m_delayTimer == nullptr && timeIntervalInMilliseconds > 0)
            {
                m_delayTimer = new(std::nothrow) ProfilerTimer(timeIntervalInMilliseconds);

                if (nullptr == m_delayTimer)
                {
                    Log(logERROR, "CreateTimer: unable to allocate memory for delay timer\n");
                }
                else
                {
                    m_delayTimer->SetTimerType(PROFILEDELAYTIMER);
                    m_bDelayStartEnabled = true;
                    m_delayInMilliseconds = timeIntervalInMilliseconds;
                }
            }

            break;

        case PROFILEDURATIONTIMER:
            if (m_durationTimer == nullptr && timeIntervalInMilliseconds > 0)
            {
                m_durationTimer = new(std::nothrow) ProfilerTimer(timeIntervalInMilliseconds);

                if (nullptr == m_durationTimer)
                {
                    Log(logERROR, "CreateTimer: unable to allocate memory for duration timer\n");
                }
                else
                {
                    m_durationTimer->SetTimerType(PROFILEDURATIONTIMER);
                    m_bProfilerDurationEnabled = true;
                    m_durationInMilliseconds = timeIntervalInMilliseconds;
                }
            }

            break;

        default:
            break;
    }
}


void HSAAPIInfoManager::startTimer(ProfilerTimerType timerType)
{
    if (m_delayTimer || m_durationTimer)
    {
        switch (timerType)
        {
            case PROFILEDELAYTIMER:
                m_delayTimer->startTimer(true);
                break;

            case PROFILEDURATIONTIMER:
                m_durationTimer->startTimer(true);
                break;

            default:
                break;
        }
    }
}

void HSAAPIInfoManager::AddAPIInfoEntry(APIBase* api)
{
    HSAAPIBase* hsaAPI = dynamic_cast<HSAAPIBase*>(api);

    if (IsCapReached() || IsInFilterList(hsaAPI->m_type) || !IsTracing())
    {
        SAFE_DELETE(hsaAPI);
    }
    else
    {
        APIInfoManagerBase::AddTraceInfoEntry(hsaAPI);
        m_tracedApiCount++;
    }
}
