#include "cor_profiler.h"

#include <corprof.h>
#include <string>
#include "corhlpr.h"

#include "version.h"
#include "clr_helpers.h"
#include "dd_profiler_constants.h"
#include "dllmain.h"
#include "environment_variables.h"
#include "il_rewriter.h"
#include "il_rewriter_wrapper.h"
#include "integration_loader.h"
#include "logging.h"
#include "metadata_builder.h"
#include "module_metadata.h"
#include "pal.h"
#include "sig_helpers.h"
#include "resource.h"
#include "util.h"

namespace trace {

CorProfiler* profiler = nullptr;

//
// ICorProfilerCallback methods
//
HRESULT STDMETHODCALLTYPE
CorProfiler::Initialize(IUnknown* cor_profiler_info_unknown) {
  // check if debug mode is enabled
  const auto debug_enabled_value =
      GetEnvironmentValue(environment::debug_enabled);

  if (debug_enabled_value == "1"_W || debug_enabled_value == "true"_W) {
    debug_logging_enabled = true;
  }

  CorProfilerBase::Initialize(cor_profiler_info_unknown);

  // check if tracing is completely disabled
  const WSTRING tracing_enabled =
      GetEnvironmentValue(environment::tracing_enabled);

  if (tracing_enabled == "0"_W || tracing_enabled == "false"_W) {
    Info("DATADOG TRACER DIAGNOSTICS - Profiler disabled in ", environment::tracing_enabled);
    return E_FAIL;
  }

  const auto process_name = GetCurrentProcessName();
  const auto include_process_names =
      GetEnvironmentValues(environment::include_process_names);

  // if there is a process inclusion list, attach profiler only if this
  // process's name is on the list
  if (!include_process_names.empty() &&
      !Contains(include_process_names, process_name)) {
    Info("DATADOG TRACER DIAGNOSTICS - Profiler disabled: ", process_name, " not found in ",
         environment::include_process_names, ".");
    return E_FAIL;
  }

  const auto exclude_process_names =
      GetEnvironmentValues(environment::exclude_process_names);

  // attach profiler only if this process's name is NOT on the list
  if (Contains(exclude_process_names, process_name)) {
    Info("DATADOG TRACER DIAGNOSTICS - Profiler disabled: ", process_name, " found in ",
         environment::exclude_process_names, ".");
    return E_FAIL;
  }

  // get Profiler interface
  HRESULT hr = cor_profiler_info_unknown->QueryInterface<ICorProfilerInfo3>(
      &this->info_);
  if (FAILED(hr)) {
    Warn("DATADOG TRACER DIAGNOSTICS - Failed to attach profiler: interface ICorProfilerInfo3 not found.");
    return E_FAIL;
  }

  Info("Environment variables:");

  for (auto&& env_var : env_vars_to_display) {
    Info("  ", env_var, "=", GetEnvironmentValue(env_var));
  }

  const WSTRING azure_app_services_value =
      GetEnvironmentValue(environment::azure_app_services);

  if (azure_app_services_value == "1"_W) {
    Info("Profiler is operating within Azure App Services context.");
    in_azure_app_services = true;

    const auto app_pool_id_value =
        GetEnvironmentValue(environment::azure_app_services_app_pool_id);

    if (app_pool_id_value.size() > 1 && app_pool_id_value.at(0) == '~') {
      Info("DATADOG TRACER DIAGNOSTICS - Profiler disabled: ", environment::azure_app_services_app_pool_id,
           " ", app_pool_id_value,
           " is recognized as an Azure App Services infrastructure process.");
      return E_FAIL;
    }

    const auto cli_telemetry_profile_value = GetEnvironmentValue(
        environment::azure_app_services_cli_telemetry_profile_value);

    if (cli_telemetry_profile_value == "AzureKudu"_W) {
      Info("DATADOG TRACER DIAGNOSTICS - Profiler disabled: ", app_pool_id_value,
           " is recognized as Kudu, an Azure App Services reserved process.");
      return E_FAIL;
    }
  }

  // get path to integration definition JSON files
  const WSTRING integrations_paths =
      GetEnvironmentValue(environment::integrations_path);

  if (integrations_paths.empty()) {
    Warn("DATADOG TRACER DIAGNOSTICS - Profiler disabled: ", environment::integrations_path,
         " environment variable not set.");
    return E_FAIL;
  }

  // load all available integrations from JSON files
  const std::vector<Integration> all_integrations =
      LoadIntegrationsFromEnvironment();

  // get list of disabled integration names
  const std::vector<WSTRING> disabled_integration_names =
      GetEnvironmentValues(environment::disabled_integrations);

  // remove disabled integrations
  const std::vector<Integration> integrations =
      FilterIntegrationsByName(all_integrations, disabled_integration_names);

  // check if there are any enabled integrations left
  if (integrations.empty()) {
    Warn("DATADOG TRACER DIAGNOSTICS - Profiler disabled: no enabled integrations found.");
    return E_FAIL;
  }

  integration_methods_ = FlattenIntegrations(integrations);

  const WSTRING netstandard_enabled =
      GetEnvironmentValue(environment::netstandard_enabled);

  // temporarily skip the calls into netstandard.dll that were added in
  // https://github.com/DataDog/dd-trace-dotnet/pull/753.
  // users can opt-in to the additional instrumentation by setting environment
  // variable DD_TRACE_NETSTANDARD_ENABLED
  if (netstandard_enabled != "1"_W && netstandard_enabled != "true"_W) {
    integration_methods_ = FilterIntegrationsByTargetAssemblyName(
        integration_methods_, {"netstandard"_W});
  }

  DWORD event_mask = COR_PRF_MONITOR_JIT_COMPILATION |
                     COR_PRF_DISABLE_TRANSPARENCY_CHECKS_UNDER_FULL_TRUST |
                     COR_PRF_DISABLE_INLINING | COR_PRF_MONITOR_MODULE_LOADS |
                     COR_PRF_MONITOR_ASSEMBLY_LOADS |
                     COR_PRF_DISABLE_ALL_NGEN_IMAGES;

  if (DisableOptimizations()) {
    Info("Disabling all code optimizations.");
    event_mask |= COR_PRF_DISABLE_OPTIMIZATIONS;
  }

  const WSTRING domain_neutral_instrumentation =
      GetEnvironmentValue(environment::domain_neutral_instrumentation);

  if (domain_neutral_instrumentation == "1"_W || domain_neutral_instrumentation == "true"_W) {
    instrument_domain_neutral_assemblies = true;
  }

  // set event mask to subscribe to events and disable NGEN images
  // get ICorProfilerInfo6 for net452+
  ICorProfilerInfo6* info6;
  hr = cor_profiler_info_unknown->QueryInterface<ICorProfilerInfo6>(&info6);

  if (SUCCEEDED(hr)) {
    Debug("Interface ICorProfilerInfo6 found.");
    hr = info6->SetEventMask2(event_mask, COR_PRF_HIGH_ADD_ASSEMBLY_REFERENCES);

    if (instrument_domain_neutral_assemblies) {
      Info("Note: The ", environment::domain_neutral_instrumentation, " environment variable is not needed when running on .NET Framework 4.5.2 or higher, and will be ignored.");
    }
  } else {
    hr = this->info_->SetEventMask(event_mask);

    if (instrument_domain_neutral_assemblies) {
      Info("Detected environment variable ", environment::domain_neutral_instrumentation,
          "=", domain_neutral_instrumentation);
      Info("Enabling automatic instrumentation of methods called from domain-neutral assemblies. ",
          "Please ensure that there is only one AppDomain or, if applications are being hosted in IIS, ",
          "ensure that all Application Pools have at most one application each. ",
          "Otherwise, a sharing violation (HRESULT 0x80131401) may occur.");
    }
  }
  if (FAILED(hr)) {
    Warn("DATADOG TRACER DIAGNOSTICS - Failed to attach profiler: unable to set event mask.");
    return E_FAIL;
  }

  runtime_information_ = GetRuntimeInformation(this->info_);
  if (process_name == "w3wp.exe"_W  ||
      process_name == "iisexpress.exe"_W) {
    is_desktop_iis = runtime_information_.is_desktop();
  }

  // we're in!
  Info("Profiler attached.");
  this->info_->AddRef();
  is_attached_ = true;
  profiler = this;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::AssemblyLoadFinished(AssemblyID assembly_id,
    HRESULT hr_status) {
  if (FAILED(hr_status)) {
    // if assembly failed to load, skip it entirely,
    // otherwise we can crash the process if module is not valid
    CorProfilerBase::AssemblyLoadFinished(assembly_id, hr_status);
    return S_OK;
  }

  if (!is_attached_) {
    return S_OK;
  }

  if (debug_logging_enabled) {
    Debug("AssemblyLoadFinished: ", assembly_id, " ", hr_status);
  }

  // keep this lock until we are done using the module,
  // to prevent it from unloading while in use
  std::lock_guard<std::mutex> guard(module_id_to_info_map_lock_);

  const auto assembly_info = GetAssemblyInfo(this->info_, assembly_id);
  if (!assembly_info.IsValid()) {
    return S_OK;
  }

  ComPtr<IUnknown> metadata_interfaces;
  auto hr = this->info_->GetModuleMetaData(assembly_info.manifest_module_id, ofRead | ofWrite,
                                           IID_IMetaDataImport2,
                                           metadata_interfaces.GetAddressOf());

  if (FAILED(hr)) {
    Warn("AssemblyLoadFinished failed to get metadata interface for module id ", assembly_info.manifest_module_id,
         " from assembly ", assembly_info.name);
    return S_OK;
  }

  // Get the IMetaDataAssemblyImport interface to get metadata from the managed assembly
  const auto assembly_import = metadata_interfaces.As<IMetaDataAssemblyImport>(
      IID_IMetaDataAssemblyImport);
  const auto assembly_metadata = GetAssemblyImportMetadata(assembly_import);

  if (debug_logging_enabled) {
    Debug("AssemblyLoadFinished: AssemblyName=", assembly_info.name, " AssemblyVersion=", assembly_metadata.version.str());
  }

  if (assembly_info.name == "Datadog.Trace.ClrProfiler.Managed"_W) {
    // Configure a version string to compare with the profiler version
    WSTRINGSTREAM ws;
    ws << ToWSTRING(assembly_metadata.version.major) << '.'_W
       << ToWSTRING(assembly_metadata.version.minor) << '.'_W
       << ToWSTRING(assembly_metadata.version.build);

    auto assembly_version = ws.str();

    // Check that Major.Minor.Build match the profiler version
    if (assembly_version == ToWSTRING(PROFILER_VERSION)) {
      Info("AssemblyLoadFinished: Datadog.Trace.ClrProfiler.Managed v", assembly_version, " matched profiler version v", PROFILER_VERSION);
      managed_profiler_loaded_app_domains.insert(assembly_info.app_domain_id);

      if (runtime_information_.is_desktop() && corlib_module_loaded) {
        // Set the managed_profiler_loaded_domain_neutral flag whenever the managed profiler is loaded shared
        if (assembly_info.app_domain_id == corlib_app_domain_id) {
          Info("AssemblyLoadFinished: Datadog.Trace.ClrProfiler.Managed was loaded domain-neutral");
          managed_profiler_loaded_domain_neutral = true;
        }
        else {
          Info("AssemblyLoadFinished: Datadog.Trace.ClrProfiler.Managed was not loaded domain-neutral");
        }
      }
    }
    else {
      Warn("AssemblyLoadFinished: Datadog.Trace.ClrProfiler.Managed v", assembly_version, " did not match profiler version v", PROFILER_VERSION);
    }
  }

  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ModuleLoadFinished(ModuleID module_id,
                                                          HRESULT hr_status) {
  if (FAILED(hr_status)) {
    // if module failed to load, skip it entirely,
    // otherwise we can crash the process if module is not valid
    CorProfilerBase::ModuleLoadFinished(module_id, hr_status);
    return S_OK;
  }

  if (!is_attached_) {
    return S_OK;
  }

  // keep this lock until we are done using the module,
  // to prevent it from unloading while in use
  std::lock_guard<std::mutex> guard(module_id_to_info_map_lock_);

  const auto module_info = GetModuleInfo(this->info_, module_id);
  if (!module_info.IsValid()) {
    return S_OK;
  }

  if (debug_logging_enabled) {
    Debug("ModuleLoadFinished: ", module_id, " ", module_info.assembly.name,
          " AppDomain ", module_info.assembly.app_domain_id, " ",
          module_info.assembly.app_domain_name);
  }

  AppDomainID app_domain_id = module_info.assembly.app_domain_id;

  // Identify the AppDomain ID of mscorlib which will be the Shared Domain
  // because mscorlib is always a domain-neutral assembly
  if (!corlib_module_loaded &&
      (module_info.assembly.name == "mscorlib"_W ||
       module_info.assembly.name == "System.Private.CoreLib"_W)) {
    corlib_module_loaded = true;
    corlib_app_domain_id = app_domain_id;

    ComPtr<IUnknown> metadata_interfaces;
    auto hr = this->info_->GetModuleMetaData(module_id, ofRead | ofWrite, IID_IMetaDataImport2, metadata_interfaces.GetAddressOf());
    // Get the IMetaDataAssemblyImport interface to get metadata from the
    // managed assembly
    const auto assembly_import = metadata_interfaces.As<IMetaDataAssemblyImport>(IID_IMetaDataAssemblyImport);
    const auto assembly_metadata = GetAssemblyImportMetadata(assembly_import);
    
    hr = assembly_import->GetAssemblyProps(
        assembly_metadata.assembly_token, &corAssemblyProperty.ppbPublicKey,
        &corAssemblyProperty.pcbPublicKey, &corAssemblyProperty.pulHashAlgId,
        NULL, 0, NULL, &corAssemblyProperty.pMetaData,
        &corAssemblyProperty.assemblyFlags);

    if (FAILED(hr)) {
      Warn("AssemblyLoadFinished failed to get properties for COR assembly ");
    }

    corAssemblyProperty.szName = module_info.assembly.name;

    Info("COR library: ", corAssemblyProperty.szName, " ",
         corAssemblyProperty.pMetaData.usMajorVersion, ".",
         corAssemblyProperty.pMetaData.usMinorVersion, ".",
         corAssemblyProperty.pMetaData.usRevisionNumber);

    return S_OK;
  }

  // In IIS, the startup hook will be inserted into a method in System.Web (which is domain-neutral)
  // but the Datadog.Trace.ClrProfiler.Managed.Loader assembly that the startup hook loads from a
  // byte array will be loaded into a non-shared AppDomain.
  // In this case, do not insert another startup hook into that non-shared AppDomain
  if (module_info.assembly.name == "Datadog.Trace.ClrProfiler.Managed.Loader"_W) {
    Info("ModuleLoadFinished: Datadog.Trace.ClrProfiler.Managed.Loader loaded into AppDomain ",
          app_domain_id, " ", module_info.assembly.app_domain_name);
    first_jit_compilation_app_domains.insert(app_domain_id);
    return S_OK;
  }

  if (module_info.IsWindowsRuntime()) {
    // We cannot obtain writable metadata interfaces on Windows Runtime modules
    // or instrument their IL.
    Debug("ModuleLoadFinished skipping Windows Metadata module: ", module_id,
          " ", module_info.assembly.name);
    return S_OK;
  }

  for (auto&& skip_assembly_pattern : skip_assembly_prefixes) {
    if (module_info.assembly.name.rfind(skip_assembly_pattern, 0) == 0) {
      Debug("ModuleLoadFinished skipping module by pattern: ", module_id, " ",
            module_info.assembly.name);
      return S_OK;
    }
  }

  for (auto&& skip_assembly : skip_assemblies) {
    if (module_info.assembly.name == skip_assembly) {
      Debug("ModuleLoadFinished skipping known module: ", module_id, " ",
            module_info.assembly.name);
      return S_OK;
    }
  }

  std::vector<IntegrationMethod> filtered_integrations =
      FilterIntegrationsByCaller(integration_methods_, module_info.assembly);

  if (filtered_integrations.empty()) {
    // we don't need to instrument anything in this module, skip it
    Debug("ModuleLoadFinished skipping module (filtered by caller): ",
          module_id, " ", module_info.assembly.name);
    return S_OK;
  }

  ComPtr<IUnknown> metadata_interfaces;
  auto hr = this->info_->GetModuleMetaData(module_id, ofRead | ofWrite,
                                           IID_IMetaDataImport2,
                                           metadata_interfaces.GetAddressOf());

  if (FAILED(hr)) {
    Warn("ModuleLoadFinished failed to get metadata interface for ", module_id,
         " ", module_info.assembly.name);
    return S_OK;
  }

  const auto metadata_import =
      metadata_interfaces.As<IMetaDataImport2>(IID_IMetaDataImport);
  const auto metadata_emit =
      metadata_interfaces.As<IMetaDataEmit2>(IID_IMetaDataEmit);
  const auto assembly_import = metadata_interfaces.As<IMetaDataAssemblyImport>(
      IID_IMetaDataAssemblyImport);
  const auto assembly_emit =
      metadata_interfaces.As<IMetaDataAssemblyEmit>(IID_IMetaDataAssemblyEmit);

  // don't skip Microsoft.AspNetCore.Hosting so we can run the startup hook and
  // subscribe to DiagnosticSource events.
  // don't skip Dapper: it makes ADO.NET calls even though it doesn't reference
  // System.Data or System.Data.Common
  if (module_info.assembly.name != "Microsoft.AspNetCore.Hosting"_W &&
      module_info.assembly.name != "Dapper"_W) {
    filtered_integrations =
        FilterIntegrationsByTarget(filtered_integrations, assembly_import);

    if (filtered_integrations.empty()) {
      // we don't need to instrument anything in this module, skip it
      Debug("ModuleLoadFinished skipping module (filtered by target): ",
            module_id, " ", module_info.assembly.name);
      return S_OK;
    }
  }

  mdModule module;
  hr = metadata_import->GetModuleFromScope(&module);
  if (FAILED(hr)) {
    Warn("ModuleLoadFinished failed to get module metadata token for ",
         module_id, " ", module_info.assembly.name);
    return S_OK;
  }

  GUID module_version_id;
  hr = metadata_import->GetScopeProps(nullptr, 0, nullptr, &module_version_id);
  if (FAILED(hr)) {
    Warn("ModuleLoadFinished failed to get module_version_id for ", module_id,
         " ", module_info.assembly.name);
    return S_OK;
  }

  ModuleMetadata* module_metadata = new ModuleMetadata(
      metadata_import, metadata_emit, assembly_import, assembly_emit,
      module_info.assembly.name, app_domain_id,
      module_version_id, filtered_integrations);

  // store module info for later lookup
  module_id_to_info_map_[module_id] = module_metadata;

  Debug("ModuleLoadFinished stored metadata for ", module_id, " ",
        module_info.assembly.name, " AppDomain ",
        module_info.assembly.app_domain_id, " ",
        module_info.assembly.app_domain_name);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ModuleUnloadStarted(ModuleID module_id) {
  if (debug_logging_enabled) {
    const auto module_info = GetModuleInfo(this->info_, module_id);

    if (module_info.IsValid()) {
      Debug("ModuleUnloadStarted: ", module_id, " ", module_info.assembly.name,
            " AppDomain ", module_info.assembly.app_domain_id, " ",
            module_info.assembly.app_domain_name);
    } else {
      Debug("ModuleUnloadStarted: ", module_id);
    }
  }

  // take this lock so we block until the
  // module metadata is not longer being used
  std::lock_guard<std::mutex> guard(module_id_to_info_map_lock_);

  // remove module metadata from map
  if (module_id_to_info_map_.count(module_id) > 0) {
    ModuleMetadata* metadata = module_id_to_info_map_[module_id];

    // remove appdomain id from managed_profiler_loaded_app_domains set
    if (managed_profiler_loaded_app_domains.find(metadata->app_domain_id) !=
        managed_profiler_loaded_app_domains.end()) {
      managed_profiler_loaded_app_domains.erase(metadata->app_domain_id);
    }

    module_id_to_info_map_.erase(module_id);
    delete metadata;
  }

  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::Shutdown() {
  CorProfilerBase::Shutdown();

  // keep this lock until we are done using the module,
  // to prevent it from unloading while in use
  std::lock_guard<std::mutex> guard(module_id_to_info_map_lock_);

  is_attached_ = false;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::JITCompilationStarted(
    FunctionID function_id, BOOL is_safe_to_block) {
  if (!is_attached_ || !is_safe_to_block) {
    return S_OK;
  }

  // keep this lock until we are done using the module,
  // to prevent it from unloading while in use
  std::lock_guard<std::mutex> guard(module_id_to_info_map_lock_);

  ModuleID module_id;
  mdToken function_token = mdTokenNil;

  HRESULT hr = this->info_->GetFunctionInfo(function_id, nullptr, &module_id,
                                            &function_token);

  if (FAILED(hr)) {
    Warn("JITCompilationStarted: Call to ICorProfilerInfo3.GetFunctionInfo() failed for ", function_id);
    return S_OK;
  }

  // Verify that we have the metadata for this module
  ModuleMetadata* module_metadata = nullptr;
  if (module_id_to_info_map_.count(module_id) > 0) {
    module_metadata = module_id_to_info_map_[module_id];
  }

  if (module_metadata == nullptr) {
    // we haven't stored a ModuleMetadata for this module,
    // so we can't modify its IL
    return S_OK;
  }

  // get function info
  auto caller =
      GetFunctionInfo(module_metadata->metadata_import, function_token);
  if (!caller.IsValid()) {
    return S_OK;
  }

  if (debug_logging_enabled) {
    Debug("JITCompilationStarted: function_id=", function_id,
          " token=", function_token, " name=", caller.type.name, ".",
          caller.name, "()");
  }

  // IIS: Ensure that the startup hook is inserted into System.Web.Compilation.BuildManager.InvokePreStartInitMethods.
  // This will be the first call-site considered for the startup hook injection,
  // which correctly loads Datadog.Trace.ClrProfiler.Managed.Loader into the application's
  // own AppDomain because at this point in the code path, the ApplicationImpersonationContext
  // has been started.
  //
  // Note: This check must only run on desktop because it is possible (and the default) to host
  // ASP.NET Core in-process, so a new .NET Core runtime is instantiated and run in the same w3wp.exe process
  auto valid_startup_hook_callsite = true;
  if (is_desktop_iis) {
      valid_startup_hook_callsite =
            module_metadata->assemblyName == "System.Web"_W &&
            caller.type.name == "System.Web.Compilation.BuildManager"_W &&
            caller.name == "InvokePreStartInitMethods"_W;
  } else if (module_metadata->assemblyName == "System"_W ||
             module_metadata->assemblyName == "System.Net.Http"_W) {
    valid_startup_hook_callsite = false;
  }

  // The first time a method is JIT compiled in an AppDomain, insert our startup
  // hook which, at a minimum, must add an AssemblyResolve event so we can find
  // Datadog.Trace.ClrProfiler.Managed.dll and its dependencies on-disk since it
  // is no longer provided in a NuGet package
  if (valid_startup_hook_callsite &&
      first_jit_compilation_app_domains.find(module_metadata->app_domain_id) ==
      first_jit_compilation_app_domains.end()) {
    bool domain_neutral_assembly = runtime_information_.is_desktop() && corlib_module_loaded && module_metadata->app_domain_id == corlib_app_domain_id;
    Info("JITCompilationStarted: Startup hook registered in function_id=", function_id,
          " token=", function_token, " name=", caller.type.name, ".",
          caller.name, "(), assembly_name=", module_metadata->assemblyName,
          " app_domain_id=", module_metadata->app_domain_id,
          " domain_neutral=", domain_neutral_assembly);

    first_jit_compilation_app_domains.insert(module_metadata->app_domain_id);

    hr = RunILStartupHook(module_metadata->metadata_emit, module_id,
                          function_token);

    if (FAILED(hr)) {
      Warn("JITCompilationStarted: Call to RunILStartupHook() failed for ", module_id, " ", function_token);
      return S_OK;
    }
  }

  // we don't actually need to instrument anything in
  // Microsoft.AspNetCore.Hosting, it was included only to ensure the startup
  // hook is called for AspNetCore applications
  if (module_metadata->assemblyName == "Microsoft.AspNetCore.Hosting"_W) {
    return S_OK;
  }

  // Get valid method replacements for this caller method
  const auto method_replacements =
      module_metadata->GetMethodReplacementsForCaller(caller);
  if (method_replacements.empty()) {
    return S_OK;
  }

  // Perform method insertion calls
  hr = ProcessInsertionCalls(module_metadata,
                             function_id,
                             module_id,
                             function_token,
                             caller,
                             method_replacements);

  if (FAILED(hr)) {
    Warn("JITCompilationStarted: Call to ProcessInsertionCalls() failed for ", function_id, " ", module_id, " ", function_token);
    return S_OK;
  }

  // Perform method replacement calls
  hr = ProcessReplacementCalls(module_metadata,
                               function_id,
                               module_id,
                               function_token,
                               caller,
                               method_replacements);

  if (FAILED(hr)) {
    Warn("JITCompilationStarted: Call to ProcessReplacementCalls() failed for ", function_id, " ", module_id, " ", function_token);
    return S_OK;
  }

  // Perform method call target modification
  hr = caller.method_signature.TryParse();
  RETURN_OK_IF_FAILED(hr);
  hr = ProcessCallTargetModification(module_metadata,
                                     function_id,
                                     module_id,
                                     function_token,
                                     caller,
                                     method_replacements);

  if (FAILED(hr)) {
    Warn("JITCompilationStarted: Call to ProcessCallTargetModification() failed for ", function_id, " ", module_id, " ", function_token);
    return S_OK;
  }

  return S_OK;
}

//
// ICorProfilerCallback6 methods
//
HRESULT STDMETHODCALLTYPE CorProfiler::GetAssemblyReferences(
    const WCHAR* wszAssemblyPath,
    ICorProfilerAssemblyReferenceProvider* pAsmRefProvider) {
  if (in_azure_app_services) {
    Debug("GetAssemblyReferences skipping entire callback because this is running in Azure App Services, which isn't yet supported for this feature. AssemblyPath=", wszAssemblyPath);
    return S_OK;
  }

  // Convert the assembly path to the assembly name, assuming the assembly name
  // is either <assembly_name.ni.dll> or <assembly_name>.dll
  auto assemblyPathString = ToString(wszAssemblyPath);
  auto filename =
      assemblyPathString.substr(assemblyPathString.find_last_of("\\/") + 1);
  auto lastNiDllPeriodIndex = filename.rfind(".ni.dll");
  auto lastDllPeriodIndex = filename.rfind(".dll");
  if (lastNiDllPeriodIndex != std::string::npos) {
    filename.erase(lastNiDllPeriodIndex, 7);
  } else if (lastDllPeriodIndex != std::string::npos) {
    filename.erase(lastDllPeriodIndex, 4);
  }

  const WSTRING assembly_name = ToWSTRING(filename);

  // Skip known framework assemblies that we will not instrument and,
  // as a result, will not need an assembly reference to the
  // managed profiler
  for (auto&& skip_assembly_pattern : skip_assembly_prefixes) {
    if (assembly_name.rfind(skip_assembly_pattern, 0) == 0) {
      Debug("GetAssemblyReferences skipping module by pattern: Name=",
           assembly_name, " Path=", wszAssemblyPath);
      return S_OK;
    }
  }

  for (auto&& skip_assembly : skip_assemblies) {
    if (assembly_name == skip_assembly) {
      Debug("GetAssemblyReferences skipping known assembly: Name=",
          assembly_name, " Path=", wszAssemblyPath);
      return S_OK;
    }
  }

  // Construct an ASSEMBLYMETADATA structure for the managed profiler that can
  // be consumed by the runtime
  const AssemblyReference assemblyReference = trace::AssemblyReference(managed_profiler_full_assembly_version);
  ASSEMBLYMETADATA assembly_metadata{};

  assembly_metadata.usMajorVersion = assemblyReference.version.major;
  assembly_metadata.usMinorVersion = assemblyReference.version.minor;
  assembly_metadata.usBuildNumber = assemblyReference.version.build;
  assembly_metadata.usRevisionNumber = assemblyReference.version.revision;
  if (assemblyReference.locale == "neutral"_W) {
    assembly_metadata.szLocale = const_cast<WCHAR*>("\0"_W.c_str());
    assembly_metadata.cbLocale = 0;
  } else {
    assembly_metadata.szLocale =
        const_cast<WCHAR*>(assemblyReference.locale.c_str());
    assembly_metadata.cbLocale = (DWORD)(assemblyReference.locale.size());
  }

  DWORD public_key_size = 8;
  if (assemblyReference.public_key == trace::PublicKey()) {
    public_key_size = 0;
  }

  COR_PRF_ASSEMBLY_REFERENCE_INFO asmRefInfo;
  asmRefInfo.pbPublicKeyOrToken =
        (void*)&assemblyReference.public_key.data[0];
  asmRefInfo.cbPublicKeyOrToken = public_key_size;
  asmRefInfo.szName = assemblyReference.name.c_str();
  asmRefInfo.pMetaData = &assembly_metadata;
  asmRefInfo.pbHashValue = nullptr;
  asmRefInfo.cbHashValue = 0;
  asmRefInfo.dwAssemblyRefFlags = 0;

  // Attempt to extend the assembly closure of the provided assembly to include
  // the managed profiler
  auto hr = pAsmRefProvider->AddAssemblyReference(&asmRefInfo);
  if (FAILED(hr)) {
    Warn("GetAssemblyReferences failed for call from ", wszAssemblyPath);
    return S_OK;
  }

  Debug("GetAssemblyReferences extending assembly closure for ",
      assembly_name, " to include ", asmRefInfo.szName,
      ". Path=", wszAssemblyPath);
  instrument_domain_neutral_assemblies = true;

  return S_OK;
}

bool CorProfiler::IsAttached() const { return is_attached_; }

//
// Helper methods
//
HRESULT CorProfiler::ProcessReplacementCalls(
    ModuleMetadata* module_metadata,
    const FunctionID function_id,
    const ModuleID module_id,
    const mdToken function_token,
    const FunctionInfo& caller,
    const std::vector<MethodReplacement> method_replacements) {
  ILRewriter rewriter(this->info_, nullptr, module_id, function_token);
  bool modified = false;
  auto hr = rewriter.Import();

  if (FAILED(hr)) {
    Warn("ProcessReplacementCalls: Call to ILRewriter.Import() failed for ", module_id, " ", function_token);
    return hr;
  }

  // Perform method call replacements
  for (auto& method_replacement : method_replacements) {
    // Exit early if the method replacement isn't actually doing a replacement
    if (method_replacement.wrapper_method.action != "ReplaceTargetMethod"_W) {
      continue;
    }

    const auto& wrapper_method_key =
        method_replacement.wrapper_method.get_method_cache_key();
    // Exit early if we previously failed to store the method ref for this wrapper_method
    if (module_metadata->IsFailedWrapperMemberKey(wrapper_method_key)) {
      continue;
    }

    // for each IL instruction
    for (ILInstr* pInstr = rewriter.GetILList()->m_pNext;
         pInstr != rewriter.GetILList(); pInstr = pInstr->m_pNext) {
      // only CALL or CALLVIRT
      if (pInstr->m_opcode != CEE_CALL && pInstr->m_opcode != CEE_CALLVIRT) {
        continue;
      }

      // get the target function info, continue if its invalid
      auto target =
          GetFunctionInfo(module_metadata->metadata_import, pInstr->m_Arg32);
      if (!target.IsValid()) {
        continue;
      }

      // make sure the type and method names match
      if (method_replacement.target_method.type_name != target.type.name ||
          method_replacement.target_method.method_name != target.name) {
        continue;
      }

      // we add 3 parameters to every wrapper method: opcode, mdToken, and
      // module_version_id
      const short added_parameters_count = 3;

      auto wrapper_method_signature_size =
          method_replacement.wrapper_method.method_signature.data.size();

      if (wrapper_method_signature_size < (added_parameters_count + 3)) {
        // wrapper signature must have at least 6 bytes
        // 0:{CallingConvention}|1:{ParamCount}|2:{ReturnType}|3:{OpCode}|4:{mdToken}|5:{ModuleVersionId}
        if (debug_logging_enabled) {
          Debug(
              "JITCompilationStarted skipping function call: wrapper signature "
              "too short. function_id=",
              function_id, " token=", function_token,
              " wrapper_method=", method_replacement.wrapper_method.type_name,
              ".", method_replacement.wrapper_method.method_name,
              "() wrapper_method_signature_size=",
              wrapper_method_signature_size);
        }

        continue;
      }

      auto expected_number_args = method_replacement.wrapper_method
                                      .method_signature.NumberOfArguments();

      // subtract the last arguments we add to every wrapper
      expected_number_args = expected_number_args - added_parameters_count;

      if (target.signature.IsInstanceMethod()) {
        // We always pass the instance as the first argument
        expected_number_args--;
      }

      auto target_arg_count = target.signature.NumberOfArguments();

      if (expected_number_args != target_arg_count) {
        // Number of arguments does not match our wrapper method
        if (debug_logging_enabled) {
          Debug(
              "JITCompilationStarted skipping function call: argument counts "
              "don't match. function_id=",
              function_id, " token=", function_token,
              " target_name=", target.type.name, ".", target.name,
              "() expected_number_args=", expected_number_args,
              " target_arg_count=", target_arg_count);
        }

        continue;
      }

      // Resolve the MethodRef now. If the method is generic, we'll need to use it
      // to define a MethodSpec
      // Generate a method ref token for the wrapper method
      mdMemberRef wrapper_method_ref = mdMemberRefNil;
      mdTypeRef wrapper_type_ref = mdTypeRefNil;
      auto generated_wrapper_method_ref = GetWrapperMethodRef(module_metadata,
                                                              module_id,
                                                              method_replacement,
                                                              wrapper_method_ref, 
                                                              wrapper_type_ref);
      if (!generated_wrapper_method_ref) {
        Warn(
          "JITCompilationStarted failed to obtain wrapper method ref for ",
          method_replacement.wrapper_method.type_name, ".", method_replacement.wrapper_method.method_name, "().",
          " function_id=", function_id, " function_token=", function_token,
          " name=", caller.type.name, ".", caller.name, "()");
        continue;
      }

      auto method_def_md_token = target.id;

      if (target.is_generic) {
        if (target.signature.NumberOfTypeArguments() !=
            method_replacement.wrapper_method.method_signature
                .NumberOfTypeArguments()) {
          // Number of generic arguments does not match our wrapper method
          continue;
        }

        // we need to emit a method spec to populate the generic arguments
        wrapper_method_ref =
            DefineMethodSpec(module_metadata->metadata_emit, wrapper_method_ref,
                             target.function_spec_signature);
        method_def_md_token = target.method_def_id;
      }

      std::vector<WSTRING> actual_sig;
      const auto successfully_parsed_signature = TryParseSignatureTypes(
          module_metadata->metadata_import, target, actual_sig);
      auto expected_sig =
          method_replacement.target_method.signature_types;

      if (!successfully_parsed_signature) {
        if (debug_logging_enabled) {
          Debug(
              "JITCompilationStarted skipping function call: failed to parse "
              "signature. function_id=",
              function_id, " token=", function_token,
              " target_name=", target.type.name, ".", target.name, "()",
              " successfully_parsed_signature=", successfully_parsed_signature,
              " sig_types.size()=", actual_sig.size(),
              " expected_sig_types.size()=", expected_sig.size());
        }

        continue;
      }

      if (actual_sig.size() != expected_sig.size()) {
        // we can't safely assume our wrapper methods handle the types
        if (debug_logging_enabled) {
          Debug(
              "JITCompilationStarted skipping function call: unexpected type "
              "count. function_id=",
              function_id, " token=", function_token,
              " target_name=", target.type.name, ".", target.name,
              "() successfully_parsed_signature=",
              successfully_parsed_signature,
              " sig_types.size()=", actual_sig.size(),
              " expected_sig_types.size()=", expected_sig.size());
        }

        continue;
      }

      auto is_match = true;
      for (size_t i = 0; i < expected_sig.size(); i++) {
        if (expected_sig[i] == "_"_W) {
          // We are supposed to ignore this index
          continue;
        }
        if (expected_sig[i] != actual_sig[i]) {
          // we have a type mismatch, drop out
          if (debug_logging_enabled) {
            Debug(
                "JITCompilationStarted skipping function call: types don't "
                "match. function_id=",
                function_id, " token=", function_token,
                " target_name=", target.type.name, ".", target.name,
                "() actual[", i, "]=", actual_sig[i], ", expected[",
                i, "]=", expected_sig[i]);
          }

          is_match = false;
          break;
        }
      }

      if (!is_match) {
        // signatures don't match
        continue;
      }

      bool caller_assembly_is_domain_neutral =
          runtime_information_.is_desktop() && corlib_module_loaded &&
          module_metadata->app_domain_id == corlib_app_domain_id;

      // At this point we know we've hit a match. Error out if
      //   1) The managed profiler has not been loaded yet
      //   2) The caller is domain-neutral AND we want to instrument domain-neutral assemblies AND the Profiler has already been loaded
      if (!ProfilerAssemblyIsLoadedIntoAppDomain(module_metadata->app_domain_id)) {
        Warn(
            "JITCompilationStarted skipping method: Method replacement "
            "found but the managed profiler has not yet been loaded "
            "into AppDomain with id=", module_metadata->app_domain_id,
            " function_id=", function_id, " token=", function_token,
            " caller_name=", caller.type.name, ".", caller.name, "()",
            " target_name=", target.type.name, ".", target.name, "()");
        continue;
      }

      // At this point we know we've hit a match. Error out if
      //   1) The calling assembly is domain-neutral
      //   2) The profiler is not configured to instrument domain-neutral assemblies
      if (caller_assembly_is_domain_neutral && !instrument_domain_neutral_assemblies) {
        Warn(
            "JITCompilationStarted skipping method: Method replacement",
            " found but the calling assembly ", module_metadata->assemblyName,
            " has been loaded domain-neutral so its code is being shared across AppDomains,"
            " making it unsafe for automatic instrumentation.",
            " function_id=", function_id, " token=", function_token,
            " caller_name=", caller.type.name, ".", caller.name, "()",
            " target_name=", target.type.name, ".", target.name, "()");
        continue;
      }

      const auto original_argument = pInstr->m_Arg32;
      const void* module_version_id_ptr = &module_metadata->module_version_id;

      // Begin IL Modification
      ILRewriterWrapper rewriter_wrapper(&rewriter);
      rewriter_wrapper.SetILPosition(pInstr);

      // IL Modification #1: Replace original method call with a NOP, so that all original
      //                     jump targets resolve correctly and we correctly populate the
      //                     stack with additional arguments
      //
      // IMPORTANT: Conditional branches may jump to the original call instruction which
      // resulted in the InvalidProgramException seen in
      // https://github.com/DataDog/dd-trace-dotnet/pull/542. To avoid this, we'll do
      // the rest of our IL modifications AFTER this instruction.
      auto original_methodcall_opcode = pInstr->m_opcode;
      pInstr->m_opcode = CEE_NOP;
      pInstr = pInstr->m_pNext;
      rewriter_wrapper.SetILPosition(pInstr);

      // IL Modification #2: Conditionally box System.Threading.CancellationToken
      //                     if it is the last argument in the target method.
      //
      // If the last argument in the method signature is of the type
      // System.Threading.CancellationToken (a struct) then box it before calling our
      // integration method. This resolves https://github.com/DataDog/dd-trace-dotnet/issues/662,
      // in which we did not box the System.Threading.CancellationToken object, even though the
      // wrapper method expects an object. In that issue we observed some strange CLR behavior
      // when the target method was in System.Data and the environment was 32-bit .NET Framework:
      // the CLR swapped the values of the CancellationToken argument and the opCode argument.
      // For example, the VIRTCALL opCode is '0x6F' and this value would be placed at the memory
      // location assigned to the CancellationToken variable. Since we treat the CancellationToken
      // variable as an object, this '0x6F' would be dereference to access the underlying object,
      // and an invalid memory read would occur and crash the application.
      //
      // Currently, all integrations that use System.Threading.CancellationToken (a struct)
      // have the argument as the last argument in the signature (lucky us!).
      // For now, we'll do the following:
      //   1) Get the method signature of the original target method
      //   2) Read the signature until the final argument type
      //   3) If the type begins with `ELEMENT_TYPE_VALUETYPE`, uncompress the compressed type token that follows
      //   4) If the type token represents System.Threading.CancellationToken, emit a 'box <type_token>' IL instruction before calling our wrapper method
      auto original_method_def = target.id;
      size_t argument_count = target.signature.NumberOfArguments();
      size_t return_type_index = target.signature.IndexOfReturnType();
      PCCOR_SIGNATURE pSigCurrent = PCCOR_SIGNATURE(&target.signature.data[return_type_index]); // index to the location of the return type
      bool signature_read_success = true;

      // iterate until the pointer is pointing at the last argument
      for (size_t signature_types_index = 0; signature_types_index < argument_count; signature_types_index++) {
        if (!ParseType(&pSigCurrent)) {
          signature_read_success = false;
          break;
        }
      }

      // read the last argument type
      if (signature_read_success && *pSigCurrent == ELEMENT_TYPE_VALUETYPE) {
        pSigCurrent++;
        mdToken valuetype_type_token = CorSigUncompressToken(pSigCurrent);

        // Currently, we only expect to see `System.Threading.CancellationToken` as a valuetype in this position
        // If we expand this to a general case, we would always perform the boxing regardless of type
        if (GetTypeInfo(module_metadata->metadata_import, valuetype_type_token).name == "System.Threading.CancellationToken"_W) {
          rewriter_wrapper.Box(valuetype_type_token);
        }
      }

      // IL Modification #3: Insert a non-virtual call (CALL) to the instrumentation wrapper.
      //                     Always use CALL because the wrapper methods are all static.
      rewriter_wrapper.CallMember(wrapper_method_ref, false);
      rewriter_wrapper.SetILPosition(pInstr->m_pPrev); // Set ILPosition to method call

      // IL Modification #4: Push the following additional arguments on the evaluation stack in the
      //                     following order, which all integration wrapper methods expect:
      //                       1) [int32] original CALL/CALLVIRT opCode
      //                       2) [int32] mdToken for original method call target
      //                       3) [int64] pointer to MVID
      rewriter_wrapper.LoadInt32(original_methodcall_opcode);
      rewriter_wrapper.LoadInt32(method_def_md_token);
      rewriter_wrapper.LoadInt64(reinterpret_cast<INT64>(module_version_id_ptr));

      // IL Modification #5: Conditionally emit an unbox.any instruction on the return value
      //                     of the wrapper method if we return an object but the original
      //                     method call returned a valuetype or a generic type.
      //
      // This resolves https://github.com/DataDog/dd-trace-dotnet/pull/566, which raised a
      // System.EntryPointNotFoundException. This occurred because the return type of the
      // generic method was a generic type that evaluated to a value type at runtime. As a
      // result, this caller method expected an unboxed representation of the return value,
      // even though we can only return values of type object. So if we detect that the
      // expected return type is a valuetype or a generic type, issue an unbox.any
      // instruction that will unbox it.
      mdToken typeToken;
      if (method_replacement.wrapper_method.method_signature.ReturnTypeIsObject()
          && ReturnTypeIsValueTypeOrGeneric(module_metadata->metadata_import,
                              module_metadata->metadata_emit,
                              module_metadata->assembly_emit,
                              target.id,
                              target.signature,
                              &typeToken)) {
        if (debug_logging_enabled) {
          Debug(
              "JITCompilationStarted inserting 'unbox.any ", typeToken,
              "' instruction after calling target function."
              " function_id=", function_id,
              " token=", function_token,
              " target_name=", target.type.name, ".", target.name,"()");
        }
        rewriter_wrapper.UnboxAnyAfter(typeToken);
      }

      // End IL Modification
      modified = true;
      Info("*** JITCompilationStarted() replaced calls from ", caller.type.name,
           ".", caller.name, "() to ",
           method_replacement.target_method.type_name, ".",
           method_replacement.target_method.method_name, "() ",
           original_argument, " with calls to ",
           method_replacement.wrapper_method.type_name, ".",
           method_replacement.wrapper_method.method_name, "() ",
           wrapper_method_ref);
    }
  }

  if (modified) {
    hr = rewriter.Export();

    if (FAILED(hr)) {
      Warn("ProcessReplacementCalls: Call to ILRewriter.Export() failed for ModuleID=", module_id, " ", function_token);
      return hr;
    }
  }

  return S_OK;
}

HRESULT CorProfiler::ProcessInsertionCalls(
    ModuleMetadata* module_metadata,
    const FunctionID function_id,
    const ModuleID module_id,
    const mdToken function_token,
    const FunctionInfo& caller,
    const std::vector<MethodReplacement> method_replacements) {

  ILRewriter rewriter(this->info_, nullptr, module_id, function_token);
  bool modified = false;

  auto hr = rewriter.Import();

  if (FAILED(hr)) {
    Warn("ProcessInsertionCalls: Call to ILRewriter.Import() failed for ", module_id, " ", function_token);
    return hr;
  }

  ILRewriterWrapper rewriter_wrapper(&rewriter);
  ILInstr* firstInstr = rewriter.GetILList()->m_pNext;
  ILInstr* lastInstr = rewriter.GetILList()->m_pPrev; // Should be a 'ret' instruction

  for (auto& method_replacement : method_replacements) {
    if (method_replacement.wrapper_method.action == "ReplaceTargetMethod"_W) {
      continue;
    }

    const auto& wrapper_method_key =
        method_replacement.wrapper_method.get_method_cache_key();

    // Exit early if we previously failed to store the method ref for this wrapper_method
    if (module_metadata->IsFailedWrapperMemberKey(wrapper_method_key)) {
      continue;
    }

    // Generate a method ref token for the wrapper method
    mdMemberRef wrapper_method_ref = mdMemberRefNil;
    mdTypeRef wrapper_type_ref = mdTypeRefNil;
    auto generated_wrapper_method_ref = GetWrapperMethodRef(module_metadata,
                                                            module_id,
                                                            method_replacement,
                                                            wrapper_method_ref, 
                                                            wrapper_type_ref);
    if (!generated_wrapper_method_ref) {
      Warn(
        "JITCompilationStarted failed to obtain wrapper method ref for ",
        method_replacement.wrapper_method.type_name, ".", method_replacement.wrapper_method.method_name, "().",
        " function_id=", function_id, " function_token=", function_token,
        " name=", caller.type.name, ".", caller.name, "()");
      continue;
    }

    // After successfully getting the method reference, insert a call to it
    if (method_replacement.wrapper_method.action == "InsertFirst"_W) {
      // Get first instruction and set the rewriter to that location
      rewriter_wrapper.SetILPosition(firstInstr);
      rewriter_wrapper.CallMember(wrapper_method_ref, false);
      firstInstr = firstInstr->m_pPrev;
      modified = true;

      Info("*** JITCompilationStarted() : InsertFirst inserted call to ",
        method_replacement.wrapper_method.type_name, ".",
        method_replacement.wrapper_method.method_name, "() ", wrapper_method_ref,
        " to the beginning of method",
        caller.type.name,".", caller.name, "()");
    }
  }

  if (modified) {
    hr = rewriter.Export();

    if (FAILED(hr)) {
      Warn("ProcessInsertionCalls: Call to ILRewriter.Export() failed for ModuleID=", module_id, " ", function_token);
      return hr;
    }
  }

  return S_OK;
}

HRESULT CorProfiler::ProcessCallTargetModification(
    ModuleMetadata* module_metadata,
    const FunctionID function_id,
    const ModuleID module_id,
    const mdToken function_token,
    const FunctionInfo& caller,
    const std::vector<MethodReplacement> method_replacements) {
  ILRewriter rewriter(this->info_, nullptr, module_id, function_token);
  bool modified = false;
  auto hr = rewriter.Import();

  if (FAILED(hr)) {
    Warn("ProcessCallTargetModification: Call to ILRewriter.Import() failed for ",
         module_id, " ", function_token);
    return hr;
  }

  // Perform target modification
  for (auto& method_replacement : method_replacements) {
    // Exit early if the method replacement isn't actually doing a replacement
    if (method_replacement.wrapper_method.action != "CallTargetModification"_W) {
      continue;
    }

    const auto& wrapper_method_key =
        method_replacement.wrapper_method.get_method_cache_key();
    // Exit early if we previously failed to store the method ref for this
    // wrapper_method
    if (module_metadata->IsFailedWrapperMemberKey(wrapper_method_key)) {
      continue;
    }

    if (method_replacement.target_method.assembly.name == module_metadata->assemblyName &&
        method_replacement.target_method.type_name == caller.type.name &&
        method_replacement.target_method.method_name == caller.name) {
    
        // return ref is not supported
        unsigned elementType;
        auto retTypeFlags = caller.method_signature.GetRet().GetTypeFlags(elementType);
        if (retTypeFlags & TypeFlagByRef) {
          Warn("[UNSUPPORTED] Module: ", module_metadata->assemblyName,
               " Type: ", caller.type.name,
               " Method: ", caller.name);
          return S_OK;
        }

        auto isStatic = !(caller.method_signature.CallingConvention() & IMAGE_CEE_CS_CALLCONV_HASTHIS);
        
        // *** Ensure all CallTarget refs
        EnsureCallTargetRefs(module_metadata);

        mdMemberRef wrapper_method_ref = mdMemberRefNil;
        mdTypeRef wrapper_type_ref = mdTypeRefNil;
        GetWrapperMethodRef(module_metadata, module_id, method_replacement,
                            wrapper_method_ref, wrapper_type_ref);

        Debug("Modifying locals signature");

        // Modify locals signature to add returnvalue, exception, and the object with the delegate to call after the method finishes.
        hr = ModifyLocalSig(module_metadata, rewriter,
                            module_metadata->exTypeRef,
                            module_metadata->callTargetStateTypeRef);
        if (FAILED(hr)) {
          Warn("Error modifying the locals signature.");
          return S_OK;
        }

        Debug("Starting rewriting.");
        Info(GetILCodes("Original Code: ", &rewriter, caller));

        /*
            The method body will be changed to something like:

            {
                object returnValue = null;
                Exception exception = null;
                CallTargetState state = CallTargetState.GetDefault();
                try
                {
                    try
                    {
                        state = CallTargetInvoker.BeginMethod(...); 
                        if (!state.ShouldExecuteMethod())
                        {
                            return;
                        }
                    }
                    catch (Exception ex)
                    {
                        CallTargetInvoker.LogException(ex);
                    }
                    
                    //
                    // [ORIGINAL METHOD BODY]
                    //

                }
                catch (Exception globalEx)
                {
                    exception = globalEx;
                    if (state.ShouldRethrowOnException()) 
                    {
                        throw;
                    }
                }
                finally
                {
                    try
                    {
                        returnValue = CallTargetInvoker.EndMethod(...);
                    }
                    catch (Exception ex)
                    {
                        CallTargetInvoker.LogException(ex);
                    }
                }
            }

        */

        // metadata helpers
        auto pEmit = module_metadata->metadata_emit;
        auto pImport = module_metadata->metadata_import;
        auto pReWriter = &rewriter;

        // new local indexes
        auto indexRet = rewriter.cNewLocals - 3;
        auto indexEx = rewriter.cNewLocals - 2;
        auto indexState = rewriter.cNewLocals - 1;

        // rewrites
        ILRewriterWrapper reWriterWrapper(pReWriter);
        ILInstr* pFirstOriginalInstr = pReWriter->GetILList()->m_pNext;
        reWriterWrapper.SetILPosition(pFirstOriginalInstr);


        //
        // BEGINNING OF THE METHOD
        //

        // clear locals
        reWriterWrapper.LoadNull();
        reWriterWrapper.StLocal(indexRet);
        reWriterWrapper.LoadNull();
        reWriterWrapper.StLocal(indexEx);
        reWriterWrapper.CallMember(module_metadata->getDefaultMemberRef, false);
        reWriterWrapper.StLocal(indexState);
        
        // load caller type into the stack
        ILInstr* pTryStartInstr = reWriterWrapper.LoadToken(caller.type.id);

        // load instance into the stack (if not static)
        if (isStatic) {
          reWriterWrapper.LoadNull();
        } else {
          reWriterWrapper.LoadArgument(0);
        }

        // create and load arguments array
        auto argNum = caller.method_signature.NumberOfArguments();
        if (argNum == 0) {
          // if num of arguments is zero, we avoid to create the object array
          reWriterWrapper.LoadNull();
        } else {
          reWriterWrapper.CreateArray(module_metadata->objectTypeRef, argNum);
          auto arguments = caller.method_signature.GetMethodArguments();

          for (unsigned i = 0; i < argNum; i++) {
            reWriterWrapper.BeginLoadValueIntoArray(i);
            reWriterWrapper.LoadArgument(i + (isStatic ? 0 : 1));
            auto argTypeFlags = arguments[i].GetTypeFlags(elementType);
            if (argTypeFlags & TypeFlagByRef) {
              reWriterWrapper.LoadIND(elementType);
            }
            if (argTypeFlags & TypeFlagBoxedType) {
              auto tok = arguments[i].GetTypeTok(
                  pEmit, module_metadata->corLibAssemblyRef);
              if (tok == mdTokenNil) {
                return S_OK;
              }
              reWriterWrapper.Box(tok);
            }
            reWriterWrapper.EndLoadValueIntoArray();
          }
        }

        // load wrapper type token into the stack
        reWriterWrapper.LoadToken(wrapper_type_ref);

        // We call the BeginMethod and store the result to the local
        reWriterWrapper.CallMember(module_metadata->beginMemberRef, false);
        reWriterWrapper.StLocal(indexState);
        
        // Check if should execute method is true
        reWriterWrapper.LoadLocalAddress(indexState);
        reWriterWrapper.CallMember(module_metadata->shouldExecuteMethodMemberRef, true);
        ILInstr* pStateShouldExecuteBranchInstr =reWriterWrapper.CreateInstr(CEE_BRTRUE_S);
        ILInstr* pStateLeaveToEndOriginalMethod = reWriterWrapper.CreateInstr(CEE_LEAVE_S);
        ILInstr* pStateLeaveToBeginOriginalMethod = reWriterWrapper.CreateInstr(CEE_LEAVE_S);
        pStateShouldExecuteBranchInstr->m_pTarget = pStateLeaveToBeginOriginalMethod;

        // BeginMethod call catch
        ILInstr* beginMethodCatchFInstr = reWriterWrapper.CallMember(module_metadata->logExceptionRef, false);
        ILInstr* beginMethodCatchLeave = reWriterWrapper.CreateInstr(CEE_LEAVE_S);
        
        // BeginMethod exception handling clause
        EHClause beginMethodExClause{};
        beginMethodExClause.m_Flags = COR_ILEXCEPTION_CLAUSE_NONE;
        beginMethodExClause.m_pTryBegin = pTryStartInstr;
        beginMethodExClause.m_pTryEnd = beginMethodCatchFInstr;
        beginMethodExClause.m_pHandlerBegin = beginMethodCatchFInstr;
        beginMethodExClause.m_pHandlerEnd = beginMethodCatchLeave;
        beginMethodExClause.m_ClassToken = module_metadata->exTypeRef;


        // ***
        // METHOD EXECUTION
        // ***
        ILInstr* beginOriginalMethod = reWriterWrapper.NOP();
        pStateLeaveToBeginOriginalMethod->m_pTarget = beginOriginalMethod;
        beginMethodCatchLeave->m_pTarget = beginOriginalMethod;


        // Gets if the return type of the original method is boxed
        bool isVoidMethod = (retTypeFlags & TypeFlagVoid) > 0;
        auto ret = caller.method_signature.GetRet();

        bool retIsBoxedType = false;
        mdToken retTypeTok;
        if (!isVoidMethod) {
          Debug("Return token name: ", ret.GetTypeTokName(pImport));

          retTypeTok =
              ret.GetTypeTok(pEmit, module_metadata->corLibAssemblyRef);
          if (ret.GetTypeFlags(elementType) & TypeFlagBoxedType)
            retIsBoxedType = true;
        }
        Debug("Return type is boxed: ", retIsBoxedType ? "YES" : "NO");
        Debug("Return token: ", retTypeTok);

        
        //
        // ENDING OF THE METHOD EXECUTION
        //

        // Create return instruction and insert it at the end
        ILInstr* pRetInstr = pReWriter->NewILInstr();
        pRetInstr->m_opcode = CEE_RET;
        pReWriter->InsertAfter(pReWriter->GetILList()->m_pPrev, pRetInstr);
        reWriterWrapper.SetILPosition(pRetInstr);

        // Resolving the should execute method branch
        ILInstr* pAfterMethodExecutionInstr = reWriterWrapper.CreateInstr(CEE_LEAVE_S);
        pStateLeaveToEndOriginalMethod->m_pTarget = pAfterMethodExecutionInstr;


        //
        // EXCEPTION CATCH
        //

        // Store exception to local
        ILInstr* startExceptionCatch = reWriterWrapper.StLocal(indexEx);

        // Check if should rethrow on exception is true
        reWriterWrapper.LoadLocalAddress(indexState);
        reWriterWrapper.CallMember(module_metadata->shouldRethrowOnExceptionMemberRef, true);
        ILInstr* pStateShouldRethrowBranchInstr =
            reWriterWrapper.CreateInstr(CEE_BRFALSE_S);

        // Rethrow
        reWriterWrapper.SetILPosition(pRetInstr);
        reWriterWrapper.Rethrow();

        // Leave the catch
        ILInstr* pAfterRethrowInstr = reWriterWrapper.CreateInstr(CEE_LEAVE_S);
        pStateShouldRethrowBranchInstr->m_pTarget = pAfterRethrowInstr;
        

        //
        // EXCEPTION FINALLY
        //

        // Finally handler calls the EndMethod
        reWriterWrapper.NOP();
        ILInstr* endMethodTryStartInstr = reWriterWrapper.LoadLocal(indexRet);
        reWriterWrapper.LoadLocal(indexEx);
        reWriterWrapper.LoadLocal(indexState);
        reWriterWrapper.LoadToken(wrapper_type_ref);
        reWriterWrapper.CallMember(module_metadata->endMemberRef, false);
        reWriterWrapper.StLocal(indexRet);
        ILInstr* endMethodTryLeave = reWriterWrapper.CreateInstr(CEE_LEAVE_S);

        // EndMethod call catch
        ILInstr* endMethodCatchFInstr = reWriterWrapper.CallMember(module_metadata->logExceptionRef, false);
        ILInstr* endMethodCatchLeave = reWriterWrapper.CreateInstr(CEE_LEAVE_S);
        
        // EndMethod exception handling clause
        EHClause endMethodExClause{};
        endMethodExClause.m_Flags = COR_ILEXCEPTION_CLAUSE_NONE;
        endMethodExClause.m_pTryBegin = endMethodTryStartInstr;
        endMethodExClause.m_pTryEnd = endMethodCatchFInstr;
        endMethodExClause.m_pHandlerBegin = endMethodCatchFInstr;
        endMethodExClause.m_pHandlerEnd = endMethodCatchLeave;
        endMethodExClause.m_ClassToken = module_metadata->exTypeRef;

        // endfinally
        ILInstr* pEndFinallyInstr = reWriterWrapper.EndFinally();
        endMethodTryLeave->m_pTarget = pEndFinallyInstr;
        endMethodCatchLeave->m_pTarget = pEndFinallyInstr;

        //
        // METHOD RETURN
        //

        // Prepare to return the value
        if (!isVoidMethod) {
          reWriterWrapper.LoadLocal(indexRet);
          if (retIsBoxedType) {
            reWriterWrapper.UnboxAny(retTypeTok);
          } else {
              //TODO: This is causing BadImageException when the returning type is generic
            //reWriterWrapper.Cast(retTypeTok);
          }
        }

        // Resolving branching to the end of the method
        pAfterMethodExecutionInstr->m_pTarget = pEndFinallyInstr->m_pNext;
        pAfterRethrowInstr->m_pTarget = pEndFinallyInstr->m_pNext;

        // Changes all returns to a BOX+LEAVE.S
        for (ILInstr* pInstr = pReWriter->GetILList()->m_pNext;
             pInstr != pReWriter->GetILList(); pInstr = pInstr->m_pNext) {
          switch (pInstr->m_opcode) {
            case CEE_RET: {
              if (pInstr != pRetInstr) {
                if (!isVoidMethod) {
                  reWriterWrapper.SetILPosition(pInstr);
                  if (retIsBoxedType) {
                    reWriterWrapper.Box(retTypeTok);
                  }
                  reWriterWrapper.StLocal(indexRet);
                }
                pInstr->m_opcode = CEE_LEAVE_S;
                pInstr->m_pTarget = pEndFinallyInstr->m_pNext;
              }
              break;
            }
            default:
              break;
          }
        }

        // Exception handling clauses
        EHClause exClause{};
        exClause.m_Flags = COR_ILEXCEPTION_CLAUSE_NONE;
        exClause.m_pTryBegin = pTryStartInstr;
        exClause.m_pTryEnd = startExceptionCatch;
        exClause.m_pHandlerBegin = startExceptionCatch;
        exClause.m_pHandlerEnd = pAfterRethrowInstr;
        exClause.m_ClassToken = module_metadata->exTypeRef;

        EHClause finallyClause{};
        finallyClause.m_Flags = COR_ILEXCEPTION_CLAUSE_FINALLY;
        finallyClause.m_pTryBegin = pTryStartInstr;
        finallyClause.m_pTryEnd = pAfterRethrowInstr->m_pNext;
        finallyClause.m_pHandlerBegin = pAfterRethrowInstr->m_pNext;
        finallyClause.m_pHandlerEnd = pEndFinallyInstr;

        // Copy previous clauses
        auto m_pEHNew = new EHClause[rewriter.m_nEH + 4];
        for (unsigned i = 0; i < rewriter.m_nEH; i++) {
          m_pEHNew[i] = rewriter.m_pEH[i];
        }

        // add the new clauses
        rewriter.m_nEH += 4;
        m_pEHNew[rewriter.m_nEH - 4] = beginMethodExClause;
        m_pEHNew[rewriter.m_nEH - 3] = endMethodExClause;
        m_pEHNew[rewriter.m_nEH - 2] = exClause;
        m_pEHNew[rewriter.m_nEH - 1] = finallyClause;
        rewriter.m_pEH = m_pEHNew;

        modified = true;
        Info("*** JITCompilationStarted() target modification of ", caller.type.name, ".", caller.name, "()");
        Info(GetILCodes("Target Modification: ", &rewriter, caller));
        break;
    }
  }

  if (modified) {
    hr = rewriter.Export();

    if (FAILED(hr)) {
      Warn(
          "ProcessCallTargetModification: Call to ILRewriter.Export() failed for "
          "ModuleID=",
          module_id, " ", function_token);
      return hr;
    }
  }

  return S_OK;
}

bool CorProfiler::GetWrapperMethodRef(
    ModuleMetadata* module_metadata,
    ModuleID module_id,
    const MethodReplacement& method_replacement,
    mdMemberRef& wrapper_method_ref,
    mdTypeRef& wrapper_type_ref) {
  const auto& wrapper_method_key =
      method_replacement.wrapper_method.get_method_cache_key();
  const auto& wrapper_type_key =
      method_replacement.wrapper_method.get_type_cache_key();

  // Resolve the MethodRef now. If the method is generic, we'll need to use it
  // later to define a MethodSpec
  if (!module_metadata->TryGetWrapperMemberRef(wrapper_method_key,
                                                   wrapper_method_ref)) {
    const auto module_info = GetModuleInfo(this->info_, module_id);
    if (!module_info.IsValid()) {
      return false;
    }

    mdModule module;
    auto hr = module_metadata->metadata_import->GetModuleFromScope(&module);
    if (FAILED(hr)) {
      Warn(
          "JITCompilationStarted failed to get module metadata token for "
          "module_id=", module_id, " module_name=", module_info.assembly.name);
      return false;
    }

    const MetadataBuilder metadata_builder(
        *module_metadata, module, module_metadata->metadata_import,
        module_metadata->metadata_emit, module_metadata->assembly_import,
        module_metadata->assembly_emit);

    // for each wrapper assembly, emit an assembly reference
    hr = metadata_builder.EmitAssemblyRef(
        method_replacement.wrapper_method.assembly);
    if (FAILED(hr)) {
      Warn(
          "JITCompilationStarted failed to emit wrapper assembly ref for assembly=",
          method_replacement.wrapper_method.assembly.name,
          ", Version=", method_replacement.wrapper_method.assembly.version.str(),
          ", Culture=", method_replacement.wrapper_method.assembly.locale,
          " PublicKeyToken=", method_replacement.wrapper_method.assembly.public_key.str());
      return false;
    }

    // for each method replacement in each enabled integration,
    // emit a reference to the instrumentation wrapper methods
    hr = metadata_builder.StoreWrapperMethodRef(method_replacement);
    if (FAILED(hr)) {
      Warn(
        "JITCompilationStarted failed to obtain wrapper method ref for ",
        method_replacement.wrapper_method.type_name, ".", method_replacement.wrapper_method.method_name, "().");
      return false;
    } else {
      module_metadata->TryGetWrapperMemberRef(wrapper_method_key,
                                              wrapper_method_ref);
      module_metadata->TryGetWrapperParentTypeRef(wrapper_type_key,
                                                  wrapper_type_ref);
    }
  }
  module_metadata->TryGetWrapperParentTypeRef(wrapper_type_key,
                                              wrapper_type_ref);
  return true;
}

bool CorProfiler::ProfilerAssemblyIsLoadedIntoAppDomain(AppDomainID app_domain_id) {
  return managed_profiler_loaded_domain_neutral ||
         managed_profiler_loaded_app_domains.find(app_domain_id) !=
             managed_profiler_loaded_app_domains.end();
}

// Modify the local signature of a method to add ret ex methodTrace var to locals
HRESULT CorProfiler::ModifyLocalSig(ModuleMetadata* module_metadata,
                                    ILRewriter& reWriter, mdTypeRef exTypeRef,
                                    mdTypeRef callTargetStateTypeRef) {
  HRESULT hr;
  PCCOR_SIGNATURE rgbOrigSig = NULL;
  ULONG cbOrigSig = 0;
  UNALIGNED INT32 temp = 0;

  if (reWriter.m_tkLocalVarSig != mdTokenNil) {
    IfFailRet(module_metadata->metadata_import->GetSigFromToken(reWriter.m_tkLocalVarSig, &rgbOrigSig, &cbOrigSig));

    // Check Is ReWrite or not
    const auto len = CorSigCompressToken(callTargetStateTypeRef, &temp);
    if (cbOrigSig - len > 0) {
      if (rgbOrigSig[cbOrigSig - len - 1] == ELEMENT_TYPE_CLASS) {
        if (memcmp(&rgbOrigSig[cbOrigSig - len], &temp, len) == 0)
          return E_FAIL;
      }
    }
  }

  auto exTypeRefSize = CorSigCompressToken(exTypeRef, &temp);
  auto callTargetStateTypeRefSize = CorSigCompressToken(callTargetStateTypeRef, &temp);

  ULONG cbNewSize = cbOrigSig + 1 + 1 + callTargetStateTypeRefSize + 1 + exTypeRefSize;
  ULONG cOrigLocals;
  ULONG cNewLocalsLen;
  ULONG cbOrigLocals = 0;

  if (cbOrigSig == 0) {
    cbNewSize += 2;
    reWriter.cNewLocals = 3;
    cNewLocalsLen = CorSigCompressData(reWriter.cNewLocals, &temp);
  } else {
    cbOrigLocals = CorSigUncompressData(rgbOrigSig + 1, &cOrigLocals);
    reWriter.cNewLocals = cOrigLocals + 3;
    cNewLocalsLen = CorSigCompressData(reWriter.cNewLocals, &temp);
    cbNewSize += cNewLocalsLen - cbOrigLocals;
  }

  const auto rgbNewSig = new COR_SIGNATURE[cbNewSize];
  *rgbNewSig = IMAGE_CEE_CS_CALLCONV_LOCAL_SIG;

  ULONG rgbNewSigOffset = 1;
  memcpy(rgbNewSig + rgbNewSigOffset, &temp, cNewLocalsLen);
  rgbNewSigOffset += cNewLocalsLen;

  if (cbOrigSig > 0) {
    const auto cbOrigCopyLen = cbOrigSig - 1 - cbOrigLocals;
    memcpy(rgbNewSig + rgbNewSigOffset, rgbOrigSig + 1 + cbOrigLocals, cbOrigCopyLen);
    rgbNewSigOffset += cbOrigCopyLen;
  }

  // return value
  rgbNewSig[rgbNewSigOffset++] = ELEMENT_TYPE_OBJECT;

  // exception value
  rgbNewSig[rgbNewSigOffset++] = ELEMENT_TYPE_CLASS;
  exTypeRefSize = CorSigCompressToken(exTypeRef, &temp);
  memcpy(rgbNewSig + rgbNewSigOffset, &temp, exTypeRefSize);
  rgbNewSigOffset += exTypeRefSize;

  // calltarget state value
  rgbNewSig[rgbNewSigOffset++] = ELEMENT_TYPE_VALUETYPE;
  callTargetStateTypeRefSize = CorSigCompressToken(callTargetStateTypeRef, &temp);
  memcpy(rgbNewSig + rgbNewSigOffset, &temp, callTargetStateTypeRefSize);
  rgbNewSigOffset += callTargetStateTypeRefSize;

  return module_metadata->metadata_emit->GetTokenFromSig(
      &rgbNewSig[0], cbNewSize, &reWriter.m_tkLocalVarSig);
}

std::string CorProfiler::GetILCodes(std::string title, ILRewriter* rewriter,
                                    const FunctionInfo& caller) {
  std::stringstream orig_sstream;
  orig_sstream << title;
  orig_sstream << ToString(caller.type.name);
  orig_sstream << ".";
  orig_sstream << ToString(caller.name.c_str());
  orig_sstream << " : ";
  for (ILInstr* cInstr = rewriter->GetILList()->m_pNext;
       cInstr != rewriter->GetILList(); cInstr = cInstr->m_pNext) {
    orig_sstream << "0x";
    orig_sstream << std::setw(2) << std::setfill('0') << std::hex
                 << cInstr->m_opcode;
    if (cInstr->m_Arg64 != 0) {
      orig_sstream << "-";
      orig_sstream << cInstr->m_Arg64;
    }
    orig_sstream << " ";
  }
  return orig_sstream.str();
}

HRESULT CorProfiler::EnsureCallTargetRefs(ModuleMetadata* module_metadata) {
  Debug("Runtime is desktop: ",
        runtime_information_.is_desktop() ? "YES" : "NO");
  Debug("Loading CorAssembly: ", corAssemblyProperty.szName);

  // *** Ensure corlib assembly ref
  if (module_metadata->corLibAssemblyRef == mdAssemblyRefNil) {
    auto hr = module_metadata->assembly_emit->DefineAssemblyRef(
        corAssemblyProperty.ppbPublicKey, corAssemblyProperty.pcbPublicKey,
        corAssemblyProperty.szName.data(), &corAssemblyProperty.pMetaData,
        &corAssemblyProperty.pulHashAlgId,
        sizeof(corAssemblyProperty.pulHashAlgId),
        corAssemblyProperty.assemblyFlags, &module_metadata->corLibAssemblyRef);
    if (module_metadata->corLibAssemblyRef == mdAssemblyRefNil) {
      Warn("Wrapper corLibAssemblyRef could not be defined.");
      return hr;
    }
  }

  // *** Ensure System.Object type ref
  if (module_metadata->objectTypeRef == mdTypeRefNil) {
    auto hr = module_metadata->metadata_emit->DefineTypeRefByName(
        module_metadata->corLibAssemblyRef, SystemObject.data(),
        &module_metadata->objectTypeRef);
    if (FAILED(hr)) {
      Warn("Wrapper objectTypeRef could not be defined.");
      return hr;
    }
  }

  // *** Ensure System.Exception type ref
  if (module_metadata->exTypeRef == mdTypeRefNil) {
    auto hr = module_metadata->metadata_emit->DefineTypeRefByName(
        module_metadata->corLibAssemblyRef, SystemException.data(),
        &module_metadata->exTypeRef);
    if (FAILED(hr)) {
      Warn("Wrapper exTypeRef could not be defined.");
      return hr;
    }
  }

  // *** Ensure System.Type type ref
  if (module_metadata->typeRef == mdTypeRefNil) {
    auto hr = module_metadata->metadata_emit->DefineTypeRefByName(
        module_metadata->corLibAssemblyRef, SystemTypeName.data(),
        &module_metadata->typeRef);
    if (FAILED(hr)) {
      Warn("Wrapper typeRef could not be defined.");
      return hr;
    }
  }

  // *** Ensure System.RuntimeTypeHandle type ref
  if (module_metadata->runtimeTypeHandleRef == mdTypeRefNil) {
    auto hr = module_metadata->metadata_emit->DefineTypeRefByName(
        module_metadata->corLibAssemblyRef, RuntimeTypeHandleTypeName.data(),
        &module_metadata->runtimeTypeHandleRef);
    if (FAILED(hr)) {
      Warn("Wrapper runtimeTypeHandleRef could not be defined.");
      return hr;
    }
  }
  
  // *** Ensure Type.GetTypeFromHandle token
  if (module_metadata->getTypeFromHandleToken == mdTokenNil) {
    unsigned runtimeTypeHandle_buffer;
    auto runtimeTypeHandle_size = CorSigCompressToken(
        module_metadata->runtimeTypeHandleRef, &runtimeTypeHandle_buffer);

    unsigned type_buffer;
    auto type_size =
        CorSigCompressToken(module_metadata->typeRef, &type_buffer);

    auto* signature = new COR_SIGNATURE[runtimeTypeHandle_size + type_size + 4];
    unsigned offset = 0;

    signature[offset++] = IMAGE_CEE_CS_CALLCONV_DEFAULT;
    signature[offset++] = 0x01;
    signature[offset++] = ELEMENT_TYPE_CLASS;
    memcpy(&signature[offset], &type_buffer, type_size);
    offset += type_size;
    signature[offset++] = ELEMENT_TYPE_VALUETYPE;
    memcpy(&signature[offset], &runtimeTypeHandle_buffer,
           runtimeTypeHandle_size);
    offset += runtimeTypeHandle_size;

    auto hr = module_metadata->metadata_emit->DefineMemberRef(
        module_metadata->typeRef, GetTypeFromHandleMethodName.data(), signature,
        sizeof(signature), &module_metadata->getTypeFromHandleToken);
    if (FAILED(hr)) {
      Warn("Wrapper getTypeFromHandleToken could not be defined.");
      return hr;
    }
  }

  // *** Ensure System.RuntimeMethodHandle type ref
  if (module_metadata->runtimeMethodHandleRef == mdTypeRefNil) {
    auto hr = module_metadata->metadata_emit->DefineTypeRefByName(
        module_metadata->corLibAssemblyRef, RuntimeMethodHandleTypeName.data(),
        &module_metadata->runtimeMethodHandleRef);
    if (FAILED(hr)) {
      Warn("Wrapper runtimeMethodHandleRef could not be defined.");
      return hr;
    }
  }

  // *** Ensure profiler assembly ref
  if (module_metadata->profilerAssemblyRef == mdAssemblyRefNil) {
    const AssemblyReference assemblyReference =
        managed_profiler_full_assembly_version;
    ASSEMBLYMETADATA assembly_metadata{};

    assembly_metadata.usMajorVersion = assemblyReference.version.major;
    assembly_metadata.usMinorVersion = assemblyReference.version.minor;
    assembly_metadata.usBuildNumber = assemblyReference.version.build;
    assembly_metadata.usRevisionNumber = assemblyReference.version.revision;
    if (assemblyReference.locale == "neutral"_W) {
      assembly_metadata.szLocale = const_cast<WCHAR*>("\0"_W.c_str());
      assembly_metadata.cbLocale = 0;
    } else {
      assembly_metadata.szLocale =
          const_cast<WCHAR*>(assemblyReference.locale.c_str());
      assembly_metadata.cbLocale = (DWORD)(assemblyReference.locale.size());
    }

    DWORD public_key_size = 8;
    if (assemblyReference.public_key == trace::PublicKey()) {
      public_key_size = 0;
    }

    auto hr = module_metadata->assembly_emit->DefineAssemblyRef(
        &assemblyReference.public_key.data, public_key_size,
        assemblyReference.name.data(), &assembly_metadata, NULL, NULL, 0,
        &module_metadata->profilerAssemblyRef);

    if (FAILED(hr)) {
      Warn("Wrapper profilerAssemblyRef could not be defined.");
      return hr;
    }
  }

  // *** Ensure calltarget type ref
  if (module_metadata->callTargetTypeRef == mdTypeRefNil) {
    auto hr = module_metadata->metadata_emit->DefineTypeRefByName(
        module_metadata->profilerAssemblyRef,
        managed_profiler_calltarget_type.data(),
        &module_metadata->callTargetTypeRef);
    if (FAILED(hr)) {
      Warn("Wrapper callTargetTypeRef could not be defined.");
      return hr;
    }
  }

  // *** Ensure calltargetstate type ref
  if (module_metadata->callTargetStateTypeRef == mdTypeRefNil) {
    auto hr = module_metadata->metadata_emit->DefineTypeRefByName(
        module_metadata->profilerAssemblyRef,
        managed_profiler_calltarget_statetype.data(),
        &module_metadata->callTargetStateTypeRef);
    if (FAILED(hr)) {
      Warn("Wrapper callTargetStateTypeRef could not be defined.");
      return hr;
    }
  }

  // *** Ensure calltarget beginmethod member ref
  if (module_metadata->beginMemberRef == mdMemberRefNil) {
    unsigned runtimeTypeHandle_buffer;
    auto runtimeTypeHandle_size = CorSigCompressToken(
        module_metadata->runtimeTypeHandleRef, &runtimeTypeHandle_buffer);

    unsigned callTargetState_buffer;
    auto callTargetState_size = CorSigCompressToken(
        module_metadata->callTargetStateTypeRef, &callTargetState_buffer);

    auto signatureLength = 8 + runtimeTypeHandle_size + runtimeTypeHandle_size +
                           callTargetState_size;
    auto* signature = new COR_SIGNATURE[signatureLength];
    unsigned offset = 0;

    signature[offset++] = IMAGE_CEE_CS_CALLCONV_DEFAULT;
    signature[offset++] = 0x04;

    signature[offset++] = ELEMENT_TYPE_VALUETYPE;
    memcpy(&signature[offset], &callTargetState_buffer, callTargetState_size);
    offset += callTargetState_size;

    signature[offset++] = ELEMENT_TYPE_VALUETYPE;
    memcpy(&signature[offset], &runtimeTypeHandle_buffer,
           runtimeTypeHandle_size);
    offset += runtimeTypeHandle_size;
    signature[offset++] = ELEMENT_TYPE_OBJECT;
    signature[offset++] = ELEMENT_TYPE_SZARRAY;
    signature[offset++] = ELEMENT_TYPE_OBJECT;
    signature[offset++] = ELEMENT_TYPE_VALUETYPE;
    memcpy(&signature[offset], &runtimeTypeHandle_buffer,
           runtimeTypeHandle_size);
    offset += runtimeTypeHandle_size;

    auto hr = module_metadata->metadata_emit->DefineMemberRef(
        module_metadata->callTargetTypeRef,
        managed_profiler_calltarget_beginmethod_name.data(), signature,
        signatureLength, &module_metadata->beginMemberRef);
    if (FAILED(hr)) {
      Warn("Wrapper beginMemberRef could not be defined.");
      return hr;
    }
  }

  // *** Ensure calltarget endmethod member ref
  if (module_metadata->endMemberRef == mdMemberRefNil) {
    unsigned exType_buffer;
    auto exType_size = CorSigCompressToken(module_metadata->exTypeRef, &exType_buffer);

    unsigned callTargetState_buffer;
    auto callTargetState_size = CorSigCompressToken(
        module_metadata->callTargetStateTypeRef, &callTargetState_buffer);

    unsigned runtimeTypeHandle_buffer;
    auto runtimeTypeHandle_size = CorSigCompressToken(
        module_metadata->runtimeTypeHandleRef, &runtimeTypeHandle_buffer);

    auto signatureLength = 7 + exType_size + callTargetState_size + runtimeTypeHandle_size;
    auto* signature = new COR_SIGNATURE[signatureLength];
    unsigned offset = 0;

    signature[offset++] = IMAGE_CEE_CS_CALLCONV_DEFAULT;
    signature[offset++] = 0x04;
    signature[offset++] = ELEMENT_TYPE_OBJECT;

    signature[offset++] = ELEMENT_TYPE_OBJECT;
    signature[offset++] = ELEMENT_TYPE_CLASS;
    memcpy(&signature[offset], &exType_buffer, exType_size);
    offset += exType_size;

    signature[offset++] = ELEMENT_TYPE_VALUETYPE;
    memcpy(&signature[offset], &callTargetState_buffer, callTargetState_size);
    offset += callTargetState_size;

    signature[offset++] = ELEMENT_TYPE_VALUETYPE;
    memcpy(&signature[offset], &runtimeTypeHandle_buffer, runtimeTypeHandle_size);
    offset += runtimeTypeHandle_size;

    auto hr = module_metadata->metadata_emit->DefineMemberRef(
        module_metadata->callTargetTypeRef,
        managed_profiler_calltarget_endmethod_name.data(), signature,
        signatureLength, &module_metadata->endMemberRef);
    if (FAILED(hr)) {
      Warn("Wrapper endMemberRef could not be defined.");
      return hr;
    }
  }
  
  // *** Ensure calltarget logexception member ref
  if (module_metadata->logExceptionRef == mdMemberRefNil) {
    unsigned exType_buffer;
    auto exType_size =
        CorSigCompressToken(module_metadata->exTypeRef, &exType_buffer);

    auto signatureLength = 4 + exType_size;
    auto* signature = new COR_SIGNATURE[signatureLength];
    unsigned offset = 0;

    signature[offset++] = IMAGE_CEE_CS_CALLCONV_DEFAULT;
    signature[offset++] = 0x01;
    signature[offset++] = ELEMENT_TYPE_VOID;

    signature[offset++] = ELEMENT_TYPE_CLASS;
    memcpy(&signature[offset], &exType_buffer, exType_size);
    offset += exType_size;

    auto hr = module_metadata->metadata_emit->DefineMemberRef(
        module_metadata->callTargetTypeRef,
        managed_profiler_calltarget_logexception_name.data(), signature,
        signatureLength, &module_metadata->logExceptionRef);
    if (FAILED(hr)) {
      Warn("Wrapper logExceptionRef could not be defined.");
      return hr;
    }
  }

  // *** Ensure calltargetstate shouldrethrow member ref
  if (module_metadata->shouldRethrowOnExceptionMemberRef == mdMemberRefNil) {
    auto hr = module_metadata->metadata_emit->DefineMemberRef(
        module_metadata->callTargetStateTypeRef,
        managed_profiler_calltarget_statetype_shouldrethrowonexception_name.data(),
        ShouldRethrowOnExceptionSig, sizeof(ShouldRethrowOnExceptionSig),
        &module_metadata->shouldRethrowOnExceptionMemberRef);
    if (FAILED(hr)) {
      Warn("Wrapper shouldRethrowMemberRef could not be defined.");
      return hr;
    }
  }

  // *** Ensure calltargetstate shouldexecutemethod member ref
  if (module_metadata->shouldExecuteMethodMemberRef == mdMemberRefNil) {
    auto hr = module_metadata->metadata_emit->DefineMemberRef(
        module_metadata->callTargetStateTypeRef,
        managed_profiler_calltarget_statetype_shouldexecutemethod_name.data(),
        ShouldExecuteMethodSig, sizeof(ShouldExecuteMethodSig),
        &module_metadata->shouldExecuteMethodMemberRef);
    if (FAILED(hr)) {
      Warn("Wrapper shouldExecuteMethodMemberRef could not be defined.");
      return hr;
    }
  }

  // *** Ensure calltargetstate getdefault member ref
  if (module_metadata->getDefaultMemberRef == mdMemberRefNil) {
    unsigned callTargetState_buffer;
    auto callTargetState_size = CorSigCompressToken(
        module_metadata->callTargetStateTypeRef, &callTargetState_buffer);

    auto signatureLength = 3 + callTargetState_size;
    auto* signature = new COR_SIGNATURE[signatureLength];
    unsigned offset = 0;

    signature[offset++] = IMAGE_CEE_CS_CALLCONV_DEFAULT;
    signature[offset++] = 0x00;

    signature[offset++] = ELEMENT_TYPE_VALUETYPE;
    memcpy(&signature[offset], &callTargetState_buffer, callTargetState_size);
    offset += callTargetState_size;

    auto hr = module_metadata->metadata_emit->DefineMemberRef(
        module_metadata->callTargetStateTypeRef,
        managed_profiler_calltarget_statetype_getdefault_name.data(), signature,
        signatureLength,
        &module_metadata->getDefaultMemberRef);
    if (FAILED(hr)) {
      Warn("Wrapper getDefaultMemberRef could not be defined.");
      return hr;
    }
  }

  return S_OK;
}

//
// Startup methods
//
HRESULT CorProfiler::RunILStartupHook(
    const ComPtr<IMetaDataEmit2>& metadata_emit, const ModuleID module_id,
    const mdToken function_token) {
  mdMethodDef ret_method_token;
  auto hr = GenerateVoidILStartupMethod(module_id, &ret_method_token);

  if (FAILED(hr)) {
    Warn("RunILStartupHook: Call to GenerateVoidILStartupMethod failed for ", module_id);
    return hr;
  }

  ILRewriter rewriter(this->info_, nullptr, module_id, function_token);
  hr = rewriter.Import();

  if (FAILED(hr)) {
    Warn("RunILStartupHook: Call to ILRewriter.Import() failed for ", module_id, " ", function_token);
    return hr;
  }

  ILRewriterWrapper rewriter_wrapper(&rewriter);

  // Get first instruction and set the rewriter to that location
  ILInstr* pInstr = rewriter.GetILList()->m_pNext;
  rewriter_wrapper.SetILPosition(pInstr);
  rewriter_wrapper.CallMember(ret_method_token, false);
  hr = rewriter.Export();

  if (FAILED(hr)) {
    Warn("RunILStartupHook: Call to ILRewriter.Export() failed for ModuleID=", module_id, " ", function_token);
    return hr;
  }

  return S_OK;
}

HRESULT CorProfiler::GenerateVoidILStartupMethod(const ModuleID module_id,
                                                 mdMethodDef* ret_method_token) {
  ComPtr<IUnknown> metadata_interfaces;
  auto hr = this->info_->GetModuleMetaData(module_id, ofRead | ofWrite,
                                           IID_IMetaDataImport2,
                                           metadata_interfaces.GetAddressOf());
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: failed to get metadata interface for ", module_id);
    return hr;
  }

  const auto metadata_import =
      metadata_interfaces.As<IMetaDataImport2>(IID_IMetaDataImport);
  const auto metadata_emit =
      metadata_interfaces.As<IMetaDataEmit2>(IID_IMetaDataEmit);
  const auto assembly_import = metadata_interfaces.As<IMetaDataAssemblyImport>(
      IID_IMetaDataAssemblyImport);
  const auto assembly_emit =
      metadata_interfaces.As<IMetaDataAssemblyEmit>(IID_IMetaDataAssemblyEmit);

  mdModuleRef mscorlib_ref;
  hr = CreateAssemblyRefToMscorlib(assembly_emit, &mscorlib_ref);

  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: failed to define AssemblyRef to mscorlib");
    return hr;
  }

  // Define a TypeRef for System.Object
  mdTypeRef object_type_ref;
  hr = metadata_emit->DefineTypeRefByName(mscorlib_ref, "System.Object"_W.c_str(),
                                          &object_type_ref);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: DefineTypeRefByName failed");
    return hr;
  }

  // Define a new TypeDef __DDVoidMethodType__ that extends System.Object
  mdTypeDef new_type_def;
  hr = metadata_emit->DefineTypeDef("__DDVoidMethodType__"_W.c_str(), tdAbstract | tdSealed,
                               object_type_ref, NULL, &new_type_def);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: DefineTypeDef failed");
    return hr;
  }

  // Define a new static method __DDVoidMethodCall__ on the new type that has a void return type and takes no arguments
  BYTE initialize_signature[] = {
    IMAGE_CEE_CS_CALLCONV_DEFAULT, // Calling convention
    0,                             // Number of parameters
    ELEMENT_TYPE_VOID,             // Return type
    ELEMENT_TYPE_OBJECT            // List of parameter types
  };
  hr = metadata_emit->DefineMethod(new_type_def,
                              "__DDVoidMethodCall__"_W.c_str(),
                              mdStatic,
                              initialize_signature,
                              sizeof(initialize_signature),
                              0,
                              0,
                              ret_method_token);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: DefineMethod failed");
    return hr;
  }

  // Define a method on the managed side that will PInvoke into the profiler method:
  // C++: void GetAssemblyAndSymbolsBytes(BYTE** pAssemblyArray, int* assemblySize, BYTE** pSymbolsArray, int* symbolsSize)
  // C#: static extern void GetAssemblyAndSymbolsBytes(out IntPtr assemblyPtr, out int assemblySize, out IntPtr symbolsPtr, out int symbolsSize)
  mdMethodDef pinvoke_method_def;
  COR_SIGNATURE get_assembly_bytes_signature[] = {
      IMAGE_CEE_CS_CALLCONV_DEFAULT, // Calling convention
      4,                             // Number of parameters
      ELEMENT_TYPE_VOID,             // Return type
      ELEMENT_TYPE_BYREF,            // List of parameter types
      ELEMENT_TYPE_I,
      ELEMENT_TYPE_BYREF,
      ELEMENT_TYPE_I4,
      ELEMENT_TYPE_BYREF,
      ELEMENT_TYPE_I,
      ELEMENT_TYPE_BYREF,
      ELEMENT_TYPE_I4,
  };
  hr = metadata_emit->DefineMethod(
      new_type_def, "GetAssemblyAndSymbolsBytes"_W.c_str(), mdStatic | mdPinvokeImpl | mdHideBySig,
      get_assembly_bytes_signature, sizeof(get_assembly_bytes_signature), 0, 0,
      &pinvoke_method_def);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: DefineMethod failed");
    return hr;
  }

  metadata_emit->SetMethodImplFlags(pinvoke_method_def, miPreserveSig);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: SetMethodImplFlags failed");
    return hr;
  }

#ifdef _WIN32
  WSTRING native_profiler_file = "DATADOG.TRACE.CLRPROFILER.NATIVE.DLL"_W;
#else // _WIN32

#ifdef BIT64
  WSTRING native_profiler_file = GetEnvironmentValue("CORECLR_PROFILER_PATH_64"_W);
  Debug("GenerateVoidILStartupMethod: Linux: CORECLR_PROFILER_PATH_64 defined as: ", native_profiler_file);
  if (native_profiler_file == ""_W) {
    native_profiler_file = GetEnvironmentValue("CORECLR_PROFILER_PATH"_W);
    Debug("GenerateVoidILStartupMethod: Linux: CORECLR_PROFILER_PATH defined as: ", native_profiler_file);
  }
#else // BIT64
  WSTRING native_profiler_file = GetEnvironmentValue("CORECLR_PROFILER_PATH_32"_W);
  Debug("GenerateVoidILStartupMethod: Linux: CORECLR_PROFILER_PATH_32 defined as: ", native_profiler_file);
  if (native_profiler_file == ""_W) {
    native_profiler_file = GetEnvironmentValue("CORECLR_PROFILER_PATH"_W);
    Debug("GenerateVoidILStartupMethod: Linux: CORECLR_PROFILER_PATH defined as: ", native_profiler_file);
  }
#endif // BIT64
Debug("GenerateVoidILStartupMethod: Linux: Setting the PInvoke native profiler library path to ", native_profiler_file);

#endif // _WIN32

  mdModuleRef profiler_ref;
  hr = metadata_emit->DefineModuleRef(native_profiler_file.c_str(),
                                      &profiler_ref);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: DefineModuleRef failed");
    return hr;
  }

  hr = metadata_emit->DefinePinvokeMap(pinvoke_method_def,
                                       0,
                                       "GetAssemblyAndSymbolsBytes"_W.c_str(),
                                       profiler_ref);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: DefinePinvokeMap failed");
    return hr;
  }

  // Get a TypeRef for System.Byte
  mdTypeRef byte_type_ref;
  hr = metadata_emit->DefineTypeRefByName(mscorlib_ref,
                                          "System.Byte"_W.c_str(),
                                          &byte_type_ref);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: DefineTypeRefByName failed");
    return hr;
  }

  // Get a TypeRef for System.Runtime.InteropServices.Marshal
  mdTypeRef marshal_type_ref;
  hr = metadata_emit->DefineTypeRefByName(mscorlib_ref,
                                          "System.Runtime.InteropServices.Marshal"_W.c_str(),
                                          &marshal_type_ref);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: DefineTypeRefByName failed");
    return hr;
  }

  // Get a MemberRef for System.Runtime.InteropServices.Marshal.Copy(IntPtr, Byte[], int, int)
  mdMemberRef marshal_copy_member_ref;
  COR_SIGNATURE marshal_copy_signature[] = {
      IMAGE_CEE_CS_CALLCONV_DEFAULT, // Calling convention
      4,                             // Number of parameters
      ELEMENT_TYPE_VOID,             // Return type
      ELEMENT_TYPE_I,                // List of parameter types
      ELEMENT_TYPE_SZARRAY,
      ELEMENT_TYPE_U1,
      ELEMENT_TYPE_I4,
      ELEMENT_TYPE_I4
  };
  hr = metadata_emit->DefineMemberRef(
      marshal_type_ref, "Copy"_W.c_str(), marshal_copy_signature,
      sizeof(marshal_copy_signature), &marshal_copy_member_ref);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: DefineMemberRef failed");
    return hr;
  }

  // Get a TypeRef for System.Reflection.Assembly
  mdTypeRef system_reflection_assembly_type_ref;
  hr = metadata_emit->DefineTypeRefByName(mscorlib_ref,
                                          "System.Reflection.Assembly"_W.c_str(),
                                          &system_reflection_assembly_type_ref);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: DefineTypeRefByName failed");
    return hr;
  }

  // Get a MemberRef for System.Object.ToString()
  mdTypeRef system_object_type_ref;
  hr = metadata_emit->DefineTypeRefByName(mscorlib_ref,
                                          "System.Object"_W.c_str(),
                                          &system_object_type_ref);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: DefineTypeRefByName failed");
    return hr;
  }

  // Get a TypeRef for System.AppDomain
  mdTypeRef system_appdomain_type_ref;
  hr = metadata_emit->DefineTypeRefByName(mscorlib_ref,
                                          "System.AppDomain"_W.c_str(),
                                          &system_appdomain_type_ref);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: DefineTypeRefByName failed");
    return hr;
  }

  // Get a MemberRef for System.AppDomain.get_CurrentDomain()
  // and System.AppDomain.Assembly.Load(byte[], byte[])

  // Create method signature for AppDomain.CurrentDomain property
  COR_SIGNATURE appdomain_get_current_domain_signature_start[] = {
      IMAGE_CEE_CS_CALLCONV_DEFAULT,
      0,
      ELEMENT_TYPE_CLASS, // ret = System.AppDomain
      // insert compressed token for System.AppDomain TypeRef here
  };
  ULONG start_length = sizeof(appdomain_get_current_domain_signature_start);

  BYTE system_appdomain_type_ref_compressed_token[4];
  ULONG token_length = CorSigCompressToken(system_appdomain_type_ref, system_appdomain_type_ref_compressed_token);

  COR_SIGNATURE* appdomain_get_current_domain_signature = new COR_SIGNATURE[start_length + token_length];
  memcpy(appdomain_get_current_domain_signature,
         appdomain_get_current_domain_signature_start,
         start_length);
  memcpy(&appdomain_get_current_domain_signature[start_length],
         system_appdomain_type_ref_compressed_token,
         token_length);

  mdMemberRef appdomain_get_current_domain_member_ref;
  hr = metadata_emit->DefineMemberRef(
      system_appdomain_type_ref,
      "get_CurrentDomain"_W.c_str(),
      appdomain_get_current_domain_signature,
      start_length + token_length,
      &appdomain_get_current_domain_member_ref);
  delete[] appdomain_get_current_domain_signature;

  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: DefineMemberRef failed");
    return hr;
  }

  // Create method signature for AppDomain.Load(byte[], byte[])
  COR_SIGNATURE appdomain_load_signature_start[] = {
      IMAGE_CEE_CS_CALLCONV_HASTHIS,
      2,
      ELEMENT_TYPE_CLASS  // ret = System.Reflection.Assembly
      // insert compressed token for System.Reflection.Assembly TypeRef here
  };
  COR_SIGNATURE appdomain_load_signature_end[] = {
      ELEMENT_TYPE_SZARRAY,
      ELEMENT_TYPE_U1,
      ELEMENT_TYPE_SZARRAY,
      ELEMENT_TYPE_U1
  };
  start_length = sizeof(appdomain_load_signature_start);
  ULONG end_length = sizeof(appdomain_load_signature_end);

  BYTE system_reflection_assembly_type_ref_compressed_token[4];
  token_length = CorSigCompressToken(system_reflection_assembly_type_ref, system_reflection_assembly_type_ref_compressed_token);

  COR_SIGNATURE* appdomain_load_signature = new COR_SIGNATURE[start_length + token_length + end_length];
  memcpy(appdomain_load_signature,
         appdomain_load_signature_start,
         start_length);
  memcpy(&appdomain_load_signature[start_length],
         system_reflection_assembly_type_ref_compressed_token,
         token_length);
  memcpy(&appdomain_load_signature[start_length + token_length],
         appdomain_load_signature_end,
         end_length);

  mdMemberRef appdomain_load_member_ref;
  hr = metadata_emit->DefineMemberRef(
      system_appdomain_type_ref, "Load"_W.c_str(),
      appdomain_load_signature,
      start_length + token_length + end_length,
      &appdomain_load_member_ref);
  delete[] appdomain_load_signature;

  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: DefineMemberRef failed");
    return hr;
  }

  // Create method signature for Assembly.CreateInstance(string)
  COR_SIGNATURE assembly_create_instance_signature[] = {
      IMAGE_CEE_CS_CALLCONV_HASTHIS,
      1,
      ELEMENT_TYPE_OBJECT,  // ret = System.Object
      ELEMENT_TYPE_STRING
  };

  mdMemberRef assembly_create_instance_member_ref;
  hr = metadata_emit->DefineMemberRef(
      system_reflection_assembly_type_ref, "CreateInstance"_W.c_str(),
      assembly_create_instance_signature,
      sizeof(assembly_create_instance_signature),
      &assembly_create_instance_member_ref);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: DefineMemberRef failed");
    return hr;
  }

  // Create a string representing "Datadog.Trace.ClrProfiler.Managed.Loader.Startup"
  // Create OS-specific implementations because on Windows, creating the string via
  // "Datadog.Trace.ClrProfiler.Managed.Loader.Startup"_W.c_str() does not create the
  // proper string for CreateInstance to successfully call
#ifdef _WIN32
  LPCWSTR load_helper_str =
      L"Datadog.Trace.ClrProfiler.Managed.Loader.Startup";
  auto load_helper_str_size = wcslen(load_helper_str);
#else
  char16_t load_helper_str[] =
      u"Datadog.Trace.ClrProfiler.Managed.Loader.Startup";
  auto load_helper_str_size = std::char_traits<char16_t>::length(load_helper_str);
#endif

  mdString load_helper_token;
  hr = metadata_emit->DefineUserString(load_helper_str, (ULONG) load_helper_str_size,
                                  &load_helper_token);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: DefineUserString failed");
    return hr;
  }

  ULONG string_len = 0;
  WCHAR string_contents[kNameMaxSize]{};
  hr = metadata_import->GetUserString(load_helper_token, string_contents,
                                      kNameMaxSize, &string_len);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: fail quickly", module_id);
    return hr;
  }

  // Generate a locals signature defined in the following way:
  //   [0] System.IntPtr ("assemblyPtr" - address of assembly bytes)
  //   [1] System.Int32  ("assemblySize" - size of assembly bytes)
  //   [2] System.IntPtr ("symbolsPtr" - address of symbols bytes)
  //   [3] System.Int32  ("symbolsSize" - size of symbols bytes)
  //   [4] System.Byte[] ("assemblyBytes" - managed byte array for assembly)
  //   [5] System.Byte[] ("symbolsBytes" - managed byte array for symbols)
  //   [6] class System.Reflection.Assembly ("loadedAssembly" - assembly instance to save loaded assembly)
  mdSignature locals_signature_token;
  COR_SIGNATURE locals_signature[15] = {
      IMAGE_CEE_CS_CALLCONV_LOCAL_SIG, // Calling convention
      7,                               // Number of variables
      ELEMENT_TYPE_I,                  // List of variable types
      ELEMENT_TYPE_I4,
      ELEMENT_TYPE_I,
      ELEMENT_TYPE_I4,
      ELEMENT_TYPE_SZARRAY,
      ELEMENT_TYPE_U1,
      ELEMENT_TYPE_SZARRAY,
      ELEMENT_TYPE_U1,
      ELEMENT_TYPE_CLASS
      // insert compressed token for System.Reflection.Assembly TypeRef here
  };
  CorSigCompressToken(system_reflection_assembly_type_ref,
                      &locals_signature[11]);
  hr = metadata_emit->GetTokenFromSig(locals_signature, sizeof(locals_signature),
                                 &locals_signature_token);
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: Unable to generate locals signature. ModuleID=", module_id);
    return hr;
  }

  /////////////////////////////////////////////
  // Add IL instructions into the void method
  ILRewriter rewriter_void(this->info_, nullptr, module_id, *ret_method_token);
  rewriter_void.InitializeTiny();
  rewriter_void.SetTkLocalVarSig(locals_signature_token);
  ILInstr* pFirstInstr = rewriter_void.GetILList()->m_pNext;
  ILInstr* pNewInstr = NULL;

  // Step 1) Call void GetAssemblyAndSymbolsBytes(out IntPtr assemblyPtr, out int assemblySize, out IntPtr symbolsPtr, out int symbolsSize)

  // ldloca.s 0 : Load the address of the "assemblyPtr" variable (locals index 0)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDLOCA_S;
  pNewInstr->m_Arg32 = 0;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // ldloca.s 1 : Load the address of the "assemblySize" variable (locals index 1)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDLOCA_S;
  pNewInstr->m_Arg32 = 1;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // ldloca.s 2 : Load the address of the "symbolsPtr" variable (locals index 2)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDLOCA_S;
  pNewInstr->m_Arg32 = 2;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // ldloca.s 3 : Load the address of the "symbolsSize" variable (locals index 3)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDLOCA_S;
  pNewInstr->m_Arg32 = 3;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // call void GetAssemblyAndSymbolsBytes(out IntPtr assemblyPtr, out int assemblySize, out IntPtr symbolsPtr, out int symbolsSize)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_CALL;
  pNewInstr->m_Arg32 = pinvoke_method_def;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // Step 2) Call void Marshal.Copy(IntPtr source, byte[] destination, int startIndex, int length) to populate the managed assembly bytes

  // ldloc.1 : Load the "assemblySize" variable (locals index 1)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDLOC_1;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // newarr System.Byte : Create a new Byte[] to hold a managed copy of the assembly data
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_NEWARR;
  pNewInstr->m_Arg32 = byte_type_ref;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // stloc.s 4 : Assign the Byte[] to the "assemblyBytes" variable (locals index 4)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_STLOC_S;
  pNewInstr->m_Arg8 = 4;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // ldloc.0 : Load the "assemblyPtr" variable (locals index 0)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDLOC_0;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // ldloc.s 4 : Load the "assemblyBytes" variable (locals index 4)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDLOC_S;
  pNewInstr->m_Arg8 = 4;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // ldc.i4.0 : Load the integer 0 for the Marshal.Copy startIndex parameter
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDC_I4_0;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // ldloc.1 : Load the "assemblySize" variable (locals index 1) for the Marshal.Copy length parameter
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDLOC_1;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // call Marshal.Copy(IntPtr source, byte[] destination, int startIndex, int length)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_CALL;
  pNewInstr->m_Arg32 = marshal_copy_member_ref;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // Step 3) Call void Marshal.Copy(IntPtr source, byte[] destination, int startIndex, int length) to populate the symbols bytes

  // ldloc.3 : Load the "symbolsSize" variable (locals index 3)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDLOC_3;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // newarr System.Byte : Create a new Byte[] to hold a managed copy of the symbols data
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_NEWARR;
  pNewInstr->m_Arg32 = byte_type_ref;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // stloc.s 5 : Assign the Byte[] to the "symbolsBytes" variable (locals index 5)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_STLOC_S;
  pNewInstr->m_Arg8 = 5;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // ldloc.2 : Load the "symbolsPtr" variables (locals index 2)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDLOC_2;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // ldloc.s 5 : Load the "symbolsBytes" variable (locals index 5)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDLOC_S;
  pNewInstr->m_Arg8 = 5;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // ldc.i4.0 : Load the integer 0 for the Marshal.Copy startIndex parameter
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDC_I4_0;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // ldloc.3 : Load the "symbolsSize" variable (locals index 3) for the Marshal.Copy length parameter
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDLOC_3;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // call void Marshal.Copy(IntPtr source, byte[] destination, int startIndex, int length)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_CALL;
  pNewInstr->m_Arg32 = marshal_copy_member_ref;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // Step 4) Call System.Reflection.Assembly System.AppDomain.CurrentDomain.Load(byte[], byte[]))

  // call System.AppDomain System.AppDomain.CurrentDomain property
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_CALL;
  pNewInstr->m_Arg32 = appdomain_get_current_domain_member_ref;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // ldloc.s 4 : Load the "assemblyBytes" variable (locals index 4) for the first byte[] parameter of AppDomain.Load(byte[], byte[])
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDLOC_S;
  pNewInstr->m_Arg8 = 4;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // ldloc.s 5 : Load the "symbolsBytes" variable (locals index 5) for the second byte[] parameter of AppDomain.Load(byte[], byte[])
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDLOC_S;
  pNewInstr->m_Arg8 = 5;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // callvirt System.Reflection.Assembly System.AppDomain.Load(uint8[], uint8[])
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_CALLVIRT;
  pNewInstr->m_Arg32 = appdomain_load_member_ref;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // stloc.s 6 : Assign the System.Reflection.Assembly object to the "loadedAssembly" variable (locals index 6)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_STLOC_S;
  pNewInstr->m_Arg8 = 6;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // Step 4) Call instance method Assembly.CreateInstance("Datadog.Trace.ClrProfiler.Managed.Loader.Startup")

  // ldloc.s 6 : Load the "loadedAssembly" variable (locals index 6) to call Assembly.CreateInstance
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDLOC_S;
  pNewInstr->m_Arg8 = 6;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // ldstr "Datadog.Trace.ClrProfiler.Managed.Loader.Startup"
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_LDSTR;
  pNewInstr->m_Arg32 = load_helper_token;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // callvirt System.Object System.Reflection.Assembly.CreateInstance(string)
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_CALLVIRT;
  pNewInstr->m_Arg32 = assembly_create_instance_member_ref;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // pop the returned object
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_POP;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  // return
  pNewInstr = rewriter_void.NewILInstr();
  pNewInstr->m_opcode = CEE_RET;
  rewriter_void.InsertBefore(pFirstInstr, pNewInstr);

  hr = rewriter_void.Export();
  if (FAILED(hr)) {
    Warn("GenerateVoidILStartupMethod: Call to ILRewriter.Export() failed for ModuleID=", module_id);
    return hr;
  }

  return S_OK;
}

#ifndef _WIN32
extern uint8_t dll_start[] asm("_binary_Datadog_Trace_ClrProfiler_Managed_Loader_dll_start");
extern uint8_t dll_end[] asm("_binary_Datadog_Trace_ClrProfiler_Managed_Loader_dll_end");

extern uint8_t pdb_start[] asm("_binary_Datadog_Trace_ClrProfiler_Managed_Loader_pdb_start");
extern uint8_t pdb_end[] asm("_binary_Datadog_Trace_ClrProfiler_Managed_Loader_pdb_end");
#endif

void CorProfiler::GetAssemblyAndSymbolsBytes(BYTE** pAssemblyArray, int* assemblySize, BYTE** pSymbolsArray, int* symbolsSize) const {
#ifdef _WIN32
  HINSTANCE hInstance = DllHandle;
  LPCWSTR dllLpName;
  LPCWSTR symbolsLpName;

  if (runtime_information_.is_desktop()) {
    dllLpName = MAKEINTRESOURCE(NET45_MANAGED_ENTRYPOINT_DLL);
    symbolsLpName = MAKEINTRESOURCE(NET45_MANAGED_ENTRYPOINT_SYMBOLS);
  } else {
    dllLpName = MAKEINTRESOURCE(NETCOREAPP20_MANAGED_ENTRYPOINT_DLL);
    symbolsLpName = MAKEINTRESOURCE(NETCOREAPP20_MANAGED_ENTRYPOINT_SYMBOLS);
  }

  HRSRC hResAssemblyInfo = FindResource(hInstance, dllLpName, L"ASSEMBLY");
  HGLOBAL hResAssembly = LoadResource(hInstance, hResAssemblyInfo);
  *assemblySize = SizeofResource(hInstance, hResAssemblyInfo);
  *pAssemblyArray = (LPBYTE)LockResource(hResAssembly);

  HRSRC hResSymbolsInfo = FindResource(hInstance, symbolsLpName, L"SYMBOLS");
  HGLOBAL hResSymbols = LoadResource(hInstance, hResSymbolsInfo);
  *symbolsSize = SizeofResource(hInstance, hResSymbolsInfo);
  *pSymbolsArray = (LPBYTE)LockResource(hResSymbols);
#else
  *assemblySize = dll_end - dll_start;
  *pAssemblyArray = (BYTE*)dll_start;

  *symbolsSize = pdb_end - pdb_start;
  *pSymbolsArray = (BYTE*)pdb_start;
#endif
  return;
}
}  // namespace trace
