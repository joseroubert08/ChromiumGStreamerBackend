// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromecast/browser/metrics/cast_metrics_service_client.h"

#include "base/command_line.h"
#include "base/guid.h"
#include "base/i18n/rtl.h"
#include "base/logging.h"
#include "base/prefs/pref_registry_simple.h"
#include "base/prefs/pref_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/thread_task_runner_handle.h"
#include "chromecast/base/cast_sys_info_util.h"
#include "chromecast/base/chromecast_switches.h"
#include "chromecast/base/path_utils.h"
#include "chromecast/base/version.h"
#include "chromecast/browser/metrics/cast_stability_metrics_provider.h"
#include "chromecast/browser/metrics/platform_metrics_providers.h"
#include "chromecast/common/pref_names.h"
#include "chromecast/public/cast_sys_info.h"
#include "components/metrics/client_info.h"
#include "components/metrics/gpu/gpu_metrics_provider.h"
#include "components/metrics/metrics_provider.h"
#include "components/metrics/metrics_service.h"
#include "components/metrics/metrics_state_manager.h"
#include "components/metrics/net/net_metrics_log_uploader.h"
#include "components/metrics/net/network_metrics_provider.h"
#include "components/metrics/profiler/profiler_metrics_provider.h"
#include "components/metrics/url_constants.h"
#include "content/public/common/content_switches.h"

#if defined(OS_LINUX)
#include "chromecast/browser/metrics/external_metrics.h"
#endif  // defined(OS_LINUX)

namespace chromecast {
namespace metrics {

namespace {

const int kStandardUploadIntervalMinutes = 5;

const char kMetricsOldClientID[] = "user_experience_metrics.client_id";

#if !defined(OS_ANDROID)
const char kExternalUmaEventsRelativePath[] = "metrics/uma-events";
const char kPlatformUmaEventsPath[] = "/data/share/chrome/metrics/uma-events";

const struct ChannelMap {
  const char* chromecast_channel;
  const ::metrics::SystemProfileProto::Channel chrome_channel;
} kMetricsChannelMap[] = {
  { "canary-channel", ::metrics::SystemProfileProto::CHANNEL_CANARY },
  { "dev-channel", ::metrics::SystemProfileProto::CHANNEL_DEV },
  { "developer-channel", ::metrics::SystemProfileProto::CHANNEL_DEV },
  { "beta-channel", ::metrics::SystemProfileProto::CHANNEL_BETA },
  { "dogfood-channel", ::metrics::SystemProfileProto::CHANNEL_BETA },
  { "stable-channel", ::metrics::SystemProfileProto::CHANNEL_STABLE },
};

::metrics::SystemProfileProto::Channel
GetReleaseChannelFromUpdateChannelName(const std::string& channel_name) {
  if (channel_name.empty())
    return ::metrics::SystemProfileProto::CHANNEL_UNKNOWN;

  for (const auto& channel_map : kMetricsChannelMap) {
    if (channel_name.compare(channel_map.chromecast_channel) == 0)
      return channel_map.chrome_channel;
  }

  // Any non-empty channel name is considered beta channel
  return ::metrics::SystemProfileProto::CHANNEL_BETA;
}
#endif  // !defined(OS_ANDROID)

}  // namespace

// static
scoped_ptr<CastMetricsServiceClient> CastMetricsServiceClient::Create(
    base::TaskRunner* io_task_runner,
    PrefService* pref_service,
    net::URLRequestContextGetter* request_context) {
  return make_scoped_ptr(new CastMetricsServiceClient(
      io_task_runner, pref_service, request_context));
}

void CastMetricsServiceClient::RegisterPrefs(PrefRegistrySimple* registry) {
  registry->RegisterStringPref(kMetricsOldClientID, std::string());
}

void CastMetricsServiceClient::SetMetricsClientId(
    const std::string& client_id) {
  client_id_ = client_id;
  LOG(INFO) << "Metrics client ID set: " << client_id;
  PlatformSetClientID(cast_service_, client_id);
}

void CastMetricsServiceClient::OnRecordingDisabled() {
}

void CastMetricsServiceClient::StoreClientInfo(
    const ::metrics::ClientInfo& client_info) {
  const std::string& client_id = client_info.client_id;
  DCHECK(client_id.empty() || base::IsValidGUID(client_id));
  // backup client_id or reset to empty.
  SetMetricsClientId(client_id);
}

scoped_ptr< ::metrics::ClientInfo> CastMetricsServiceClient::LoadClientInfo() {
  scoped_ptr< ::metrics::ClientInfo> client_info(new ::metrics::ClientInfo);

  // kMetricsIsNewClientID would be missing if either the device was just
  // FDR'ed, or it is on pre-v1.2 build.
  if (!pref_service_->GetBoolean(prefs::kMetricsIsNewClientID)) {
    // If the old client id exists, the device must be on pre-v1.2 build,
    // instead of just being FDR'ed.
    if (!pref_service_->GetString(kMetricsOldClientID).empty()) {
      // Force old client id to be regenerated. See b/9487011.
      client_info->client_id = base::GenerateGUID();
      pref_service_->SetBoolean(prefs::kMetricsIsNewClientID, true);
      return client_info.Pass();
    }
    // else the device was just FDR'ed, pass through.
  }

  const std::string client_id(GetPlatformClientID(cast_service_));
  if (!client_id.empty() && base::IsValidGUID(client_id)) {
    client_info->client_id = client_id;
    return client_info.Pass();
  } else {
    if (client_id.empty()) {
      LOG(WARNING) << "Empty client id from platform,"
                   << " assuming this is the first boot up of a new device.";
    } else {
      LOG(ERROR) << "Invalid client id " << client_id << " from platform.";
    }
    return scoped_ptr< ::metrics::ClientInfo>();
  }
}

bool CastMetricsServiceClient::IsOffTheRecordSessionActive() {
  // Chromecast behaves as "off the record" w/r/t recording browsing state,
  // but this value is about not disabling metrics because of it.
  return false;
}

int32_t CastMetricsServiceClient::GetProduct() {
  // Chromecast currently uses the same product identifier as Chrome.
  return ::metrics::ChromeUserMetricsExtension::CHROME;
}

std::string CastMetricsServiceClient::GetApplicationLocale() {
  return base::i18n::GetConfiguredLocale();
}

bool CastMetricsServiceClient::GetBrand(std::string* brand_code) {
  return false;
}

::metrics::SystemProfileProto::Channel CastMetricsServiceClient::GetChannel() {
  scoped_ptr<CastSysInfo> sys_info = CreateSysInfo();

#if defined(OS_ANDROID)
  switch (sys_info->GetBuildType()) {
    case CastSysInfo::BUILD_ENG:
      return ::metrics::SystemProfileProto::CHANNEL_UNKNOWN;
    case CastSysInfo::BUILD_BETA:
      return ::metrics::SystemProfileProto::CHANNEL_BETA;
    case CastSysInfo::BUILD_PRODUCTION:
      return ::metrics::SystemProfileProto::CHANNEL_STABLE;
  }
  NOTREACHED();
  return ::metrics::SystemProfileProto::CHANNEL_UNKNOWN;
#else
  // Use the system (or signed) release channel here to avoid the noise in the
  // metrics caused by the virtual channel which could be temporary or
  // arbitrary.
  return GetReleaseChannelFromUpdateChannelName(
      sys_info->GetSystemReleaseChannel());
#endif  // defined(OS_ANDROID)
}

std::string CastMetricsServiceClient::GetVersionString() {
  int build_number;
  if (!base::StringToInt(CAST_BUILD_INCREMENTAL, &build_number))
    build_number = 0;

  // Sample result: 31.0.1650.0-K15386-devel
  std::string version_string(PRODUCT_VERSION);

  version_string.append("-K");
  version_string.append(base::IntToString(build_number));

  int is_official_build =
      build_number > 0 &&
      GetChannel() != ::metrics::SystemProfileProto::CHANNEL_UNKNOWN;
  if (!is_official_build)
    version_string.append("-devel");

  return version_string;
}

void CastMetricsServiceClient::OnLogUploadComplete() {
}

void CastMetricsServiceClient::StartGatheringMetrics(
    const base::Closure& done_callback) {
  done_callback.Run();
}

void CastMetricsServiceClient::CollectFinalMetrics(
    const base::Closure& done_callback) {
  done_callback.Run();
}

scoped_ptr< ::metrics::MetricsLogUploader>
CastMetricsServiceClient::CreateUploader(
    const base::Callback<void(int)>& on_upload_complete) {
  std::string uma_server_url(::metrics::kDefaultMetricsServerUrl);
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kOverrideMetricsUploadUrl)) {
    uma_server_url.assign(
        command_line->GetSwitchValueASCII(switches::kOverrideMetricsUploadUrl));
  }
  DCHECK(!uma_server_url.empty());
  return scoped_ptr< ::metrics::MetricsLogUploader>(
      new ::metrics::NetMetricsLogUploader(request_context_,
                                           uma_server_url,
                                           ::metrics::kDefaultMetricsMimeType,
                                           on_upload_complete));
}

base::TimeDelta CastMetricsServiceClient::GetStandardUploadInterval() {
  return base::TimeDelta::FromMinutes(kStandardUploadIntervalMinutes);
}

void CastMetricsServiceClient::EnableMetricsService(bool enabled) {
  if (!task_runner_->BelongsToCurrentThread()) {
    task_runner_->PostTask(
        FROM_HERE, base::Bind(&CastMetricsServiceClient::EnableMetricsService,
                              base::Unretained(this), enabled));
    return;
  }

  if (enabled) {
    metrics_service_->Start();
  } else {
    metrics_service_->Stop();
  }
}

CastMetricsServiceClient::CastMetricsServiceClient(
    base::TaskRunner* io_task_runner,
    PrefService* pref_service,
    net::URLRequestContextGetter* request_context)
    : io_task_runner_(io_task_runner),
      pref_service_(pref_service),
      cast_service_(nullptr),
#if defined(OS_LINUX)
      external_metrics_(nullptr),
      platform_metrics_(nullptr),
#endif  // defined(OS_LINUX)
      task_runner_(base::ThreadTaskRunnerHandle::Get()),
      request_context_(request_context) {
}

CastMetricsServiceClient::~CastMetricsServiceClient() {
#if defined(OS_LINUX)
  DCHECK(!external_metrics_);
  DCHECK(!platform_metrics_);
#endif  // defined(OS_LINUX)
}

void CastMetricsServiceClient::OnApplicationNotIdle() {
  metrics_service_->OnApplicationNotIdle();
}

void CastMetricsServiceClient::Initialize(CastService* cast_service) {
  DCHECK(cast_service);
  DCHECK(!cast_service_);
  cast_service_ = cast_service;

  metrics_state_manager_ = ::metrics::MetricsStateManager::Create(
      pref_service_,
      base::Bind(&CastMetricsServiceClient::IsReportingEnabled,
                 base::Unretained(this)),
      base::Bind(&CastMetricsServiceClient::StoreClientInfo,
                 base::Unretained(this)),
      base::Bind(&CastMetricsServiceClient::LoadClientInfo,
                 base::Unretained(this)));
  metrics_service_.reset(new ::metrics::MetricsService(
      metrics_state_manager_.get(), this, pref_service_));

  // Always create a client id as it may also be used by crash reporting,
  // (indirectly) included in feedback, and can be queried during setup.
  // For UMA and crash reporting, associated opt-in settings will control
  // sending reports as directed by the user.
  // For Setup (which also communicates the user's opt-in preferences),
  // report the client-id and expect that setup will handle the current opt-in
  // value.
  metrics_state_manager_->ForceClientIdCreation();

  CastStabilityMetricsProvider* stability_provider =
      new CastStabilityMetricsProvider(metrics_service_.get());
  metrics_service_->RegisterMetricsProvider(
      scoped_ptr< ::metrics::MetricsProvider>(stability_provider));

  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (!command_line->HasSwitch(switches::kDisableGpu)) {
    metrics_service_->RegisterMetricsProvider(
        scoped_ptr< ::metrics::MetricsProvider>(
            new ::metrics::GPUMetricsProvider));
  }
  metrics_service_->RegisterMetricsProvider(
      scoped_ptr< ::metrics::MetricsProvider>(
          new ::metrics::NetworkMetricsProvider(io_task_runner_)));
  metrics_service_->RegisterMetricsProvider(
      scoped_ptr< ::metrics::MetricsProvider>(
          new ::metrics::ProfilerMetricsProvider));
  RegisterPlatformMetricsProviders(metrics_service_.get(), cast_service_);

  metrics_service_->InitializeMetricsRecordingState();
#if !defined(OS_ANDROID)
  // Reset clean_shutdown bit after InitializeMetricsRecordingState().
  metrics_service_->LogNeedForCleanShutdown();
#endif  // !defined(OS_ANDROID)

  if (IsReportingEnabled())
    metrics_service_->Start();

#if defined(OS_LINUX)
  // Start external metrics collection, which feeds data from external
  // processes into the main external metrics.
  external_metrics_ = new ExternalMetrics(
      stability_provider,
      GetHomePathASCII(kExternalUmaEventsRelativePath).value());
  external_metrics_->Start();
  platform_metrics_ =
      new ExternalMetrics(stability_provider, kPlatformUmaEventsPath);
  platform_metrics_->Start();
#endif  // defined(OS_LINUX)
}

void CastMetricsServiceClient::Finalize() {
#if !defined(OS_ANDROID)
  // Set clean_shutdown bit.
  metrics_service_->RecordCompletedSessionEnd();
#endif  // !defined(OS_ANDROID)

#if defined(OS_LINUX)
  // Stop metrics service cleanly before destructing CastMetricsServiceClient.
  // The pointer will be deleted in StopAndDestroy().
  external_metrics_->StopAndDestroy();
  external_metrics_ = nullptr;
  platform_metrics_->StopAndDestroy();
  platform_metrics_ = nullptr;
#endif  // defined(OS_LINUX)
  metrics_service_->Stop();
}

bool CastMetricsServiceClient::IsReportingEnabled() {
  return pref_service_->GetBoolean(prefs::kOptInStats);
}

}  // namespace metrics
}  // namespace chromecast
