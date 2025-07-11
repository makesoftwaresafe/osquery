/**
 * Copyright (c) 2014-present, The osquery authors
 *
 * This source code is licensed as defined by the LICENSE file found in the
 * root directory of this source tree.
 *
 * SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
 */

#include <boost/algorithm/string.hpp>

#include <osquery/core/system.h>
#include <osquery/core/tables.h>
#include <osquery/sql/sql.h>

#include <osquery/core/windows/wmi.h>
#include <osquery/logger/logger.h>
#include <osquery/tables/system/windows/registry.h>
#include <osquery/utils/conversions/tryto.h>

namespace osquery {
namespace tables {

QueryData genSystemInfo(QueryContext& context) {
  Row r;
  r["hostname"] = osquery::getFqdn();
  r["computer_name"] = osquery::getHostname();
  r["local_hostname"] = r["computer_name"];
  getHostUUID(r["uuid"]);

  auto qd = SQL::selectAllFrom("cpuid");
  for (const auto& row : qd) {
    if (row.at("feature") == "product_name") {
      r["cpu_brand"] = row.at("value");
      boost::trim(r["cpu_brand"]);
    }
  }

  const auto wmiSystemReq =
      WmiRequest::CreateWmiRequest("select * from Win32_ComputerSystem");
  auto wmiExecutedSuccessful = wmiSystemReq.isValue();
  // Why not select * here?  Because we only use NumberOfCores out of this
  // structure, however getting the full structure takes a 1s per CPU core
  // penalty, which really hurts on large CPU systems
  const auto wmiSystemReqProc =
      WmiRequest::CreateWmiRequest("select NumberOfCores from Win32_Processor");
  wmiExecutedSuccessful &= wmiSystemReqProc.isValue();
  if (wmiExecutedSuccessful && !wmiSystemReq->results().empty() &&
      !wmiSystemReqProc->results().empty()) {
    const std::vector<WmiResultItem>& wmiResults = wmiSystemReq->results();
    const std::vector<WmiResultItem>& wmiResultsProc =
        wmiSystemReqProc->results();
    long numProcs = 0;
    wmiResults[0].GetLong("NumberOfLogicalProcessors", numProcs);
    r["cpu_logical_cores"] = INTEGER(numProcs);
    wmiResults[0].GetLong("NumberOfProcessors", numProcs);
    r["cpu_sockets"] = INTEGER(numProcs);
    wmiResultsProc[0].GetLong("NumberOfCores", numProcs);
    r["cpu_physical_cores"] = INTEGER(numProcs);
    wmiResults[0].GetString("Manufacturer", r["hardware_vendor"]);
    wmiResults[0].GetString("Model", r["hardware_model"]);

    // For Lenovo models, the hardware_model property is not set propertly.
    // Instead we can get the required info from the Win32_ComputerSystemProduct
    // table.
    std::string lcModel = r["hardware_vendor"];
    std::transform(lcModel.begin(), lcModel.end(), lcModel.begin(), ::tolower);
    if (lcModel == "lenovo") {
      std::string version = r["hardware_model"];
      const auto wmiSystemProductReq = WmiRequest::CreateWmiRequest(
          "select * from Win32_ComputerSystemProduct");
      if (wmiSystemProductReq.isValue() &&
          !wmiSystemProductReq->results().empty()) {
        const std::vector<WmiResultItem>& wmiProductResults =
            wmiSystemProductReq->results();
        wmiProductResults[0].GetString("Version", r["hardware_model"]);
        r["hardware_version"] = version;
      }
    }
  } else {
    r["cpu_logical_cores"] = "-1";
    r["cpu_sockets"] = "-1";
    r["cpu_physical_cores"] = "-1";
    r["hardware_vendor"] = "-1";
    r["hardware_model"] = "-1";
  }

  // win32-computersystem mis-reports TotalPhysicalMemory -- it's
  // actually reporting post bios allocations. (See note at
  // https://docs.microsoft.com/en-us/windows/win32/cimwin32prov/win32-computersystem)
  // So instead, we use an API call --
  // https://docs.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getphysicallyinstalledsystemmemory
  uint64_t physicallyInstallMemory;
  if (GetPhysicallyInstalledSystemMemory(&physicallyInstallMemory)) {
    r["physical_memory"] = BIGINT(physicallyInstallMemory * 1024);
  } else {
    auto lastError = GetLastError();
    if (lastError == ERROR_INVALID_DATA) {
      LOG(INFO)
          << "Got error trying to determine the physically installed memory: "
          << "SMBIOS data is malformed";
    } else {
      LOG(INFO)
          << "Got error trying to determine the physically installed memory: "
          << std::to_string(lastError);
    }
    r["physical_memory"] = "-1";
  }

  QueryData regResults;
  queryKey(
      "HKEY_LOCAL_MACHINE\\"
      "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0\\",
      regResults);
  for (const auto& key : regResults) {
    if (key.at("name") == "Update Revision") {
      if (key.at("data").size() >= 16) {
        auto revision_exp =
            tryTo<unsigned long int>(key.at("data").substr(8, 2), 16);
        r["cpu_microcode"] = std::to_string(revision_exp.takeOr(0ul));
      }
      break;
    }
  }

  const auto wmiBiosReq =
      WmiRequest::CreateWmiRequest("select * from Win32_Bios");
  if (wmiBiosReq && !wmiBiosReq->results().empty()) {
    wmiBiosReq->results()[0].GetString("SerialNumber", r["hardware_serial"]);
  } else {
    r["hardware_serial"] = "-1";
  }

  SYSTEM_INFO systemInfo;
  GetSystemInfo(&systemInfo);
  switch (systemInfo.wProcessorArchitecture) {
  case PROCESSOR_ARCHITECTURE_AMD64:
    r["cpu_type"] = "x86_64";
    break;
  case PROCESSOR_ARCHITECTURE_ARM:
    r["cpu_type"] = "ARM";
    break;
  case PROCESSOR_ARCHITECTURE_IA64:
    r["cpu_type"] = "x64 Itanium";
    break;
  case PROCESSOR_ARCHITECTURE_INTEL:
    r["cpu_type"] = "x86";
    break;
  case PROCESSOR_ARCHITECTURE_UNKNOWN:
    r["cpu_type"] = "Unknown";
  default:
    break;
  }

  // If we're running in ARM x86 emulation, we can get the true processor type
  // Only available on Win 10 and later
  typedef BOOL(WINAPI * LPFN_ISWOW64PROCESS2)(
      HANDLE hProcess, USHORT * pProcessMachine, USHORT * pNativeMachine);

  auto pIsWow64Process2 = reinterpret_cast<LPFN_ISWOW64PROCESS2>(
      GetProcAddress(GetModuleHandle(L"kernel32.dll"), "IsWow64Process2"));
  if (pIsWow64Process2 != nullptr) {
    USHORT pProcessMachine, pNativeMachine;

    if (pIsWow64Process2(
            GetCurrentProcess(), &pProcessMachine, &pNativeMachine)) {
      if (pNativeMachine == IMAGE_FILE_MACHINE_ARM64) {
        r["emulated_cpu_type"] = r["cpu_type"];
        r["cpu_type"] = "ARM";
      }
    }
  }

  r["cpu_subtype"] = "-1";
  r["hardware_version"] = "-1";
  return {r};
}
}
}
