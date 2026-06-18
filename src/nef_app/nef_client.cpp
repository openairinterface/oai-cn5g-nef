/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_client.hpp"

#include <cctype>
#include <thread>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <nlohmann/json.hpp>

#include "AmfCreatedEventSubscription.h"
#include "NFProfile.h"
#include "NFType.h"
#include "NFType_anyOf.h"
#include "NFStatus.h"
#include "NFStatus_anyOf.h"
#include "http_client.hpp"
#include "logger.hpp"
#include "nef_config.hpp"
#include "nef_retry_helper.hpp"
#include "nef_sbi_helper.hpp"
#include "nrf_discovery_cache.hpp"
#include "sbi_resilience.hpp"
#include "sbi_helper.hpp"

extern std::shared_ptr<oai::http::http_client> http_client_inst;
extern std::unique_ptr<oai::config::nef::nef_config> nef_config_inst;

using namespace oai::nef::app;
using namespace oai::config::nef;
using namespace oai::config;
using namespace oai::nef::api;
using namespace oai::common::sbi;

// Helpers for NRF registration and discovery — build URIs, parse responses,
// etc.

//------------------------------------------------------------------------------
/**
 * Build nf_addr_t from a shared_ptr<nf> config object.
 */
static nf_addr_t nf_to_addr(const std::shared_ptr<oai::config::nf>& nf_cfg) {
  nf_addr_t addr;
  addr.uri_root    = nf_cfg->get_url();
  addr.api_version = nf_cfg->get_sbi().get_api_version();
  return addr;
}

//------------------------------------------------------------------------------
/**
 * Build the NRF NFManagement URI for this NEF instance:
 *   http://<nrf_host>:<port>/nnrf-nfm/v1/nf-instances/<instance_id>
 */
static std::string build_nrf_nf_instance_uri(const std::string& instance_id) {
  auto nrf_cfg       = nef_config_inst->get_nf(oai::config::NRF_CONFIG_NAME);
  nf_addr_t nrf_addr = nf_to_addr(nrf_cfg);
  std::string uri;
  sbi_helper::get_nrf_nf_instance_uri(nrf_addr, instance_id, uri);
  return uri;
}

//------------------------------------------------------------------------------
/**
 * Build the NRF NF-Discovery URI for searching a specific NF type:
 *   http://<nrf_host>:<port>/nnrf-disc/v1/nf-instances
 */
static std::string build_nrf_disc_uri() {
  auto nrf_cfg       = nef_config_inst->get_nf(oai::config::NRF_CONFIG_NAME);
  nf_addr_t nrf_addr = nf_to_addr(nrf_cfg);
  std::string uri    = {};
  sbi_helper::get_nrf_disc_search_nf_instances_uri(nrf_addr, uri);
  return uri;
}

//------------------------------------------------------------------------------
/**
 * Map our internal nf_type_t to the 3GPP NFType string for NRF queries.
 */
static std::string nf_type_to_str(nf_type_t t) {
  switch (t) {
    case NF_TYPE_AMF:
      return "AMF";
    case NF_TYPE_SMF:
      return "SMF";
    case NF_TYPE_PCF:
      return "PCF";
    case NF_TYPE_UDR:
      return "UDR";
    case NF_TYPE_UDM:
      return "UDM";
    case NF_TYPE_NRF:
      return "NRF";
    case NF_TYPE_AUSF:
      return "AUSF";
    default:
      return "UNKNOWN";
  }
}

//------------------------------------------------------------------------------
static bool is_2xx_status(const int status_code) {
  return status_code >= http_status_code::OK &&
         status_code < http_status_code::MULTIPLE_CHOICES;
}

//------------------------------------------------------------------------------
static std::string get_header_case_insensitive(
    const cpr::Header& headers, const std::string& key) {
  for (const auto& [k, v] : headers) {
    if (k.size() != key.size()) continue;
    bool same = true;
    for (size_t i = 0; i < k.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(k[i])) !=
          std::tolower(static_cast<unsigned char>(key[i]))) {
        same = false;
        break;
      }
    }
    if (same) return v;
  }
  return "";
}

//------------------------------------------------------------------------------
static std::string extract_last_path_segment(const std::string& uri) {
  if (uri.empty()) return "";

  size_t end = uri.size();
  while (end > 0 && uri[end - 1] == '/') --end;
  if (end == 0) return "";

  size_t begin = uri.rfind('/', end - 1);
  if (begin == std::string::npos) return uri.substr(0, end);

  return uri.substr(begin + 1, end - begin - 1);
}

//------------------------------------------------------------------------------
// Constructor
nef_client::nef_client() {
  m_nef_instance_id =
      boost::uuids::to_string(boost::uuids::random_generator()());
  Logger::nef_app().debug(
      "NEF client instance ID: %s", m_nef_instance_id.c_str());
}

//------------------------------------------------------------------------------
// Destructor
nef_client::~nef_client() {
  Logger::nef_app().debug("Delete NEF Client instance...");
}

// NRF registration
//------------------------------------------------------------------------------
bool nef_client::register_to_nrf() {
  if (!nef_config_inst->register_nrf()) {
    Logger::nef_app().info("NRF registration is disabled in config.");
    return true;
  }

  Logger::nef_app().info(
      "Registering NEF to NRF (instance: %s)...", m_nef_instance_id.c_str());

  // Build the NF Profile`
  // Obtain NEF's own SBI address from config
  auto local_nf         = nef_config_inst->get_local();
  const auto& local_sbi = local_nf->get_sbi();

  if (!local_nf) {
    return false;
  }

  // IPv4 address + SBI port (needed both for NFProfile and NFService)
  struct in_addr addr4 = local_sbi.get_addr4();

  nlohmann::json profile;
  {
    // Build typed NFProfile for correct field serialization
    oai::_3gpp::model::NFProfile nf_profile;
    nf_profile.setNfInstanceId(m_nef_instance_id);
    nf_profile.setNfInstanceName(local_nf->get_host());
    nf_profile.setHeartBeatTimer(50);
    nf_profile.setPriority(1);
    nf_profile.setCapacity(100);

    oai::_3gpp::model::NFType nf_type;
    nf_type.setEnumValue(oai::_3gpp::model::NFType_anyOf::eNFType_anyOf::NEF);
    nf_profile.setNfType(nf_type);

    oai::_3gpp::model::NFStatus nf_status;
    nf_status.setEnumValue(
        oai::_3gpp::model::NFStatus_anyOf::eNFStatus_anyOf::REGISTERED);
    nf_profile.setNfStatus(nf_status);

    nf_profile.setIpv4Addresses({inet_ntoa(addr4)});

    to_json(profile, nf_profile);
  }

  // NF Services are built as raw JSON since the service name strings
  // (nnef-trafficinfluence, nnef-bdt, etc.) are not in ServiceName_anyOf.
  nlohmann::json nf_services = nlohmann::json::array();
  auto add_service           = [&](const std::string& svc_name,
                         const std::string& api_name,
                         const std::string& version) {
    nlohmann::json svc;
    svc["serviceInstanceId"] = m_nef_instance_id;
    svc["serviceName"]       = svc_name;
    svc["versions"]          = nlohmann::json::array({nlohmann::json{
        {"apiVersionInUri", version}, {"apiFullVersion", version}}});
    svc["scheme"]            = "http";
    svc["nfServiceStatus"]   = "REGISTERED";
    nlohmann::json ep;
    ep["ipv4Address"]  = inet_ntoa(addr4);
    ep["port"]         = local_sbi.get_port();
    svc["ipEndPoints"] = nlohmann::json::array({ep});
    svc["apiPrefix"]   = api_name;
    nf_services.push_back(svc);
  };

  add_service("nnef-eventexposure", "/3gpp-monitoring-event/v1", "v1.0.0");
  add_service("nnef-trafficinfluence", "/3gpp-traffic-influence/v1", "v1.0.0");
  add_service("nnef-pfdmanagement", "/3gpp-pfd-management/v1", "v1.0.0");
  add_service("nnef-bdt", "/3gpp-bdt/v1", "v1.0.0");
  add_service("nnef-qosmonitoring", "/3gpp-as-session-with-qos/v1", "v1.0.0");
  add_service("nnef-analyticsexposure", "/3gpp-analyticsexposure/v1", "v1.0.0");

  profile["nfServices"] = nf_services;

  // PUT to NRF NF Management API
  std::string nrf_uri = build_nrf_nf_instance_uri(m_nef_instance_id);
  Logger::nef_app().debug("NRF registration URI: %s", nrf_uri.c_str());

  oai::http::response nrf_register_resp{};
  auto sbi_sleep_nrf = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log_nrf = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  sbi_call_with_retry(
      "NRF", /*is_post=*/true,
      [&]() -> int {
        oai::http::request req =
            http_client_inst->prepare_json_request(nrf_uri, profile.dump());
        nrf_register_resp = http_client_inst->send_http_request(
            oai::common::sbi::method_e::PUT, req);
        return static_cast<int>(nrf_register_resp.status_code);
      },
      sbi_circuit_breaker_registry::instance(), sbi_sleep_nrf, sbi_log_nrf);

  if (nrf_register_resp.status_code == http_status_code::OK ||
      nrf_register_resp.status_code == http_status_code::CREATED) {
    Logger::nef_app().info(
        "NEF successfully registered to NRF (status %d)",
        nrf_register_resp.status_code);
    return true;
  }

  Logger::nef_app().warn(
      "NEF NRF registration failed (status %d): %s",
      nrf_register_resp.status_code, nrf_register_resp.body.c_str());
  return false;
}

//------------------------------------------------------------------------------
bool nef_client::deregister_from_nrf() {
  if (!nef_config_inst->register_nrf()) return true;

  Logger::nef_app().info("Deregistering NEF from NRF...");
  std::string nrf_uri = build_nrf_nf_instance_uri(m_nef_instance_id);

  oai::http::request req = http_client_inst->prepare_json_request(nrf_uri, "");
  auto resp              = http_client_inst->send_http_request(
      oai::common::sbi::method_e::DELETE, req);

  if (resp.status_code == http_status_code::NO_CONTENT) {
    Logger::nef_app().info("NEF deregistered from NRF");
    return true;
  }
  Logger::nef_app().warn(
      "NRF deregistration failed (status %d)", resp.status_code);
  return false;
}

//------------------------------------------------------------------------------
bool nef_client::send_heartbeat_to_nrf() {
  if (!nef_config_inst->register_nrf()) return true;

  Logger::nef_app().debug("Sending heartbeat to NRF...");

  // PATCH /nf-instances/<id>  with
  // [{"op":"replace","path":"/nfStatus","value":"REGISTERED"}]
  nlohmann::json patch_body = nlohmann::json::array();
  patch_body.push_back(
      {{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}});

  std::string nrf_uri = build_nrf_nf_instance_uri(m_nef_instance_id);
  oai::http::request req =
      http_client_inst->prepare_json_request(nrf_uri, patch_body.dump());
  auto resp = http_client_inst->send_http_request(
      oai::common::sbi::method_e::PATCH, req);

  if (resp.status_code == http_status_code::OK ||
      resp.status_code == http_status_code::NO_CONTENT) {
    return true;
  }
  Logger::nef_app().warn(
      "NRF heartbeat failed (status %d) — will re-register", resp.status_code);
  return register_to_nrf();
}

// NF discovery
//------------------------------------------------------------------------------
bool nef_client::discover_nf(nf_type_t nf_type, std::string& nf_endpoint) {
  // First, try to read the NF endpoint directly from local config
  // (for deployments that don't use dynamic NF discovery via NRF)
  std::string cfg_key = {};
  switch (nf_type) {
    case NF_TYPE_AMF:
      cfg_key = AMF_CONFIG_NAME;
      break;
    case NF_TYPE_SMF:
      cfg_key = SMF_CONFIG_NAME;
      break;
    case NF_TYPE_PCF:
      cfg_key = PCF_CONFIG_NAME;
      break;
    case NF_TYPE_UDR:
      cfg_key = UDR_CONFIG_NAME;
      break;
    default:
      cfg_key = "";
  }

  if (!cfg_key.empty()) {
    try {
      auto nf_cfg = nef_config_inst->get_nf(cfg_key);
      if (nf_cfg) {
        nf_endpoint = nf_cfg->get_url(nef_config_inst->enable_tls());
        Logger::nef_app().debug(
            "NF %s endpoint from config: %s", cfg_key.c_str(), nf_endpoint);
        return true;
      }
    } catch (...) {
    }
  }

  // Fall back to NRF discovery if config-based lookup failed
  if (!nef_config_inst->register_nrf()) {
    Logger::nef_app().warn(
        "NRF discovery disabled and no static config for NF type %d",
        static_cast<int>(nf_type));
    return false;
  }

  std::string nf_type_str = nf_type_to_str(nf_type);

  // Cache check — if we have a cached endpoint for this NF type, use it without
  // querying NRF
  {
    std::string cached_ep = {};
    if (nrf_discovery_cache::instance().get(nf_type_str, cached_ep)) {
      Logger::nef_app().debug(
          "NF discovery cache hit: %s → %s", nf_type_str.c_str(),
          cached_ep.c_str());
      nf_endpoint = cached_ep;
      return true;
    }
  }

  // Cache miss — query NRF for the NF type's endpoint
  std::string disc_uri = build_nrf_disc_uri() + "?" +
                         "target-nf-type=" + nf_type_str +
                         "&requester-nf-type=NEF";

  Logger::nef_app().debug("NF discovery URI (NRF): %s", disc_uri.c_str());

  oai::http::response last_disc_resp{};
  auto sbi_sleep = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  sbi_call_with_retry(
      "NRF", /*is_post=*/false,
      [&]() -> int {
        oai::http::request req =
            http_client_inst->prepare_json_request(disc_uri, "");
        last_disc_resp = http_client_inst->send_http_request(
            oai::common::sbi::method_e::GET, req);
        return static_cast<int>(last_disc_resp.status_code);
      },
      sbi_circuit_breaker_registry::instance(), sbi_sleep, sbi_log);

  if (last_disc_resp.status_code != http_status_code::OK) {
    Logger::nef_app().warn(
        "NF discovery for %s failed (status %d)", nf_type_str.c_str(),
        last_disc_resp.status_code);
    return false;
  }

  try {
    nlohmann::json j = nlohmann::json::parse(last_disc_resp.body);
    // SearchResult → nfInstances[0] → nfServices[0] → ipEndPoints[0]
    auto& instances = j.at("nfInstances");
    if (instances.empty()) {
      Logger::nef_app().warn(
          "NF discovery: no instances found for %s", nf_type_str.c_str());
      return false;
    }

    // NF selection
    bool found = false;
    for (auto& inst : instances) {
      Logger::nef_app().debug(
          "NF discovery candidate: instanceId=%s, nfType=%s",
          inst.value("instanceId", "").c_str(),
          inst.value("nfType", "").c_str());

      std::string scheme = "http";

      if (inst.contains("nfServices") && !inst["nfServices"].empty()) {
        auto& ep    = inst["nfServices"][0].at("ipEndPoints")[0];
        nf_endpoint = scheme + "://" + ep.value("ipv4Address", "") + ":" +
                      std::to_string(ep.value("port", 8080));
        found = true;
      } else if (
          inst.contains("ipv4Addresses") && !inst["ipv4Addresses"].empty()) {
        nf_endpoint =
            "http://" + inst["ipv4Addresses"][0].get<std::string>() + ":8080";
        found = true;
      }
      // TODO: for now, do not do NF selection, just take the first valid one
      if (found) break;
    }

    Logger::nef_app().debug(
        "NF discovery: %s → %s", nf_type_str.c_str(), nf_endpoint.c_str());
    // Cache the result
    nrf_discovery_cache::instance().put(nf_type_str, nf_endpoint);
    return found;
  } catch (nlohmann::json::exception& e) {
    Logger::nef_app().warn("NF discovery parse error: %s", e.what());
    return false;
  }
}

// AMF event-exposure
//-----------------------------------------------------------------------------
bool nef_client::subscribe_amf_event_exposure(
    const nlohmann::json& subscription_data, std::string& amf_sub_id) {
  std::string amf_url = {};
  if (!discover_nf(nf_type_t::NF_TYPE_AMF, amf_url)) {
    Logger::nef_app().warn(
        "AMF not found — cannot subscribe to event exposure");
    return false;
  }
  std::string url =
      amf_url + nef_sbi_helper::AmfEventExposureBase + "v1/subscriptions";

  // Inject NEF's own callback URL so AMF knows where to send event
  // notifications. We use a placeholder sub-id here; after creation we update
  // the NF→AF mapping. TS 29.518: field is "subsChangeNotifyUri"
  // This subscription is created by an NEF on behalf of AF
  nlohmann::json sub_body             = subscription_data;
  const std::string nef_callback_base = get_nef_notify_uri("_2");
  // Strip the placeholder; AMF will POST to base + sub-id suffix if needed,
  // but we set a fixed URL that the HTTP/2 server parses by path segment.
  sub_body["eventNotifyUri"] = nef_config_inst->get_local()->get_url() +
                               nef_sbi_helper::NefNotifyBase + "v1/notify/amf";
  // TODO: sub_body["notifyCorrelationId"] = ;
  // TODO: verify whether we need to set subsChangeNotifyUri,
  // subsChangeNotifyCorrelationId (from AF)

  std::string body = sub_body.dump();

  oai::http::response amf_sub_resp{};
  auto sbi_sleep_amf = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log_amf = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  sbi_call_with_retry(
      "AMF", /*is_post=*/true,
      [&]() -> int {
        oai::http::request req =
            http_client_inst->prepare_json_request(url, body);
        amf_sub_resp = http_client_inst->send_http_request(
            oai::common::sbi::method_e::POST, req);
        return static_cast<int>(amf_sub_resp.status_code);
      },
      sbi_circuit_breaker_registry::instance(), sbi_sleep_amf, sbi_log_amf);

  if (amf_sub_resp.status_code == http_status_code::CREATED) {
    try {
      nlohmann::json j = nlohmann::json::parse(amf_sub_resp.body);
      // Try typed parse first for accurate field access
      oai::_3gpp::model::AmfCreatedEventSubscription created;
      try {
        from_json(j, created);
        amf_sub_id = created.getSubscriptionId();
      } catch (...) {
        // Fall back to raw JSON on parse failure
        amf_sub_id = j.value("subscriptionId", "");
        if (amf_sub_id.empty() && j.contains("eventsSubscription")) {
          amf_sub_id = j["eventsSubscription"].value("subscriptionId", "");
        }
      }
      if (!amf_sub_id.empty()) {
        Logger::nef_app().info(
            "AMF event subscription created: %s", amf_sub_id.c_str());
        return true;
      }
    } catch (...) {
      Logger::nef_app().warn("Failed to parse AMF subscription response");
    }
  }
  // Invalidate discovery cache on connection failure or 503 so next
  // discover_nf() call re-queries NRF instead of serving stale endpoint.
  if (amf_sub_resp.status_code == 0 ||
      amf_sub_resp.status_code == http_status_code::SERVICE_UNAVAILABLE) {
    nrf_discovery_cache::instance().invalidate("AMF");
  }
  Logger::nef_app().warn(
      "AMF event subscription failed (status %d)", amf_sub_resp.status_code);
  return false;
}

//------------------------------------------------------------------------------
bool nef_client::unsubscribe_amf_event_exposure(const std::string& amf_sub_id) {
  std::string amf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_AMF, amf_url)) return false;

  std::string url = amf_url + nef_sbi_helper::AmfEventExposureBase +
                    "v1/subscriptions/" + amf_sub_id;
  oai::http::request req = http_client_inst->prepare_json_request(url, "");
  auto resp              = http_client_inst->send_http_request(
      oai::common::sbi::method_e::DELETE, req);
  return (
      resp.status_code == http_status_code::NO_CONTENT ||
      resp.status_code == http_status_code::OK);
}

//------------------------------------------------------------------------------
// SMF event-exposure
bool nef_client::subscribe_smf_event_exposure(
    const nlohmann::json& smf_body, const std::string& notif_id,
    const std::string& notif_uri, std::string& smf_sub_id) {
  std::string smf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_SMF, smf_url)) {
    Logger::nef_app().warn(
        "SMF not found — cannot subscribe to event exposure");
    return false;
  }
  std::string url =
      smf_url + nef_sbi_helper::SmfEventExposureBase + "v1/subscriptions";

  // T5: the caller (nef_app) builds the full NsmfEventExposure body (eventSubs,
  // target filters). Here we inject the NEF-chosen correlation id (notifId) and
  // the per-subscription inbound notification URI (notifUri, TS 29.508).
  nlohmann::json sub_body = smf_body;
  sub_body["notifId"]     = notif_id;
  sub_body["notifUri"]    = notif_uri;

  std::string body = sub_body.dump();

  oai::http::response smf_sub_resp{};
  auto sbi_sleep_smf = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log_smf = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  sbi_call_with_retry(
      "SMF", /*is_post=*/true,
      [&]() -> int {
        oai::http::request req =
            http_client_inst->prepare_json_request(url, body);
        smf_sub_resp = http_client_inst->send_http_request(
            oai::common::sbi::method_e::POST, req);
        return static_cast<int>(smf_sub_resp.status_code);
      },
      sbi_circuit_breaker_registry::instance(), sbi_sleep_smf, sbi_log_smf);

  if (smf_sub_resp.status_code == http_status_code::CREATED) {
    try {
      nlohmann::json j = nlohmann::json::parse(smf_sub_resp.body);
      smf_sub_id       = j.value("subscriptionId", "");
      Logger::nef_app().info(
          "SMF event subscription created: %s", smf_sub_id.c_str());
      return true;
    } catch (...) {
    }
  }
  Logger::nef_app().warn(
      "SMF event subscription failed (status %d)", smf_sub_resp.status_code);
  return false;
}

//------------------------------------------------------------------------------
bool nef_client::unsubscribe_smf_event_exposure(const std::string& smf_sub_id) {
  std::string smf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_SMF, smf_url)) return false;

  std::string url = smf_url + nef_sbi_helper::SmfEventExposureBase +
                    "v1/subscriptions/" + smf_sub_id;
  oai::http::request req = http_client_inst->prepare_json_request(url, "");
  auto resp              = http_client_inst->send_http_request(
      oai::common::sbi::method_e::DELETE, req);
  return (
      resp.status_code == http_status_code::NO_CONTENT ||
      resp.status_code == http_status_code::OK);
}

//------------------------------------------------------------------------------
bool nef_client::update_smf_event_exposure(
    const std::string& smf_sub_id, const nlohmann::json& smf_body) {
  if (smf_sub_id.empty()) {
    Logger::nef_app().warn(
        "SMF event-exposure update: empty subscription id — skipped");
    return false;
  }
  std::string smf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_SMF, smf_url)) {
    Logger::nef_app().warn("SMF not found — cannot update event exposure");
    return false;
  }
  // T8: Nsmf_EventExposure has no PATCH; PUT does a full replace of the
  // individual subscription resource. The caller has already embedded the
  // notifId/notifUri in smf_body (via build_smf_qos_body).
  std::string url = smf_url + nef_sbi_helper::SmfEventExposureBase +
                    "v1/subscriptions/" + smf_sub_id;
  std::string body = smf_body.dump();

  oai::http::response smf_resp{};
  auto sbi_sleep_smf = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log_smf = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  sbi_call_with_retry(
      "SMF", /*is_post=*/false,
      [&]() -> int {
        oai::http::request req =
            http_client_inst->prepare_json_request(url, body);
        smf_resp = http_client_inst->send_http_request(
            oai::common::sbi::method_e::PUT, req);
        return static_cast<int>(smf_resp.status_code);
      },
      sbi_circuit_breaker_registry::instance(), sbi_sleep_smf, sbi_log_smf);

  const bool ok =
      (smf_resp.status_code == http_status_code::OK ||
       smf_resp.status_code == http_status_code::CREATED ||
       smf_resp.status_code == http_status_code::NO_CONTENT);
  if (!ok) {
    Logger::nef_app().warn(
        "SMF event-exposure update failed (status %d)", smf_resp.status_code);
  }
  return ok;
}

// PCF policy authorization

//------------------------------------------------------------------------------
bool nef_client::create_pcf_policy_auth(
    const nlohmann::json& request_body, std::string& app_session_id,
    uint32_t& http_code) {
  http_code = 0;
  std::string pcf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_PCF, pcf_url)) {
    Logger::nef_app().warn("PCF not found");
    return false;
  }
  std::string url =
      pcf_url + nef_sbi_helper::PcfPolicyAuthBase + "v1/app-sessions";
  std::string body = request_body.dump();

  oai::http::response pcf_auth_resp{};
  auto sbi_sleep_pcf = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log_pcf = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  sbi_call_with_retry(
      "PCF", /*is_post=*/true,
      [&]() -> int {
        oai::http::request req =
            http_client_inst->prepare_json_request(url, body);
        pcf_auth_resp = http_client_inst->send_http_request(
            oai::common::sbi::method_e::POST, req);
        return static_cast<int>(pcf_auth_resp.status_code);
      },
      sbi_circuit_breaker_registry::instance(), sbi_sleep_pcf, sbi_log_pcf);
  http_code = pcf_auth_resp.status_code;

  if (pcf_auth_resp.status_code == http_status_code::CREATED ||
      pcf_auth_resp.status_code == http_status_code::OK) {
    try {
      nlohmann::json j = nlohmann::json::parse(pcf_auth_resp.body);
      app_session_id   = j.value("appSessionId", "");
    } catch (...) {
    }

    if (app_session_id.empty()) {
      std::string location =
          get_header_case_insensitive(pcf_auth_resp.headers, "Location");
      app_session_id = extract_last_path_segment(location);
    }

    return is_2xx_status(pcf_auth_resp.status_code) && !app_session_id.empty();
  }
  return false;
}

//------------------------------------------------------------------------------
bool nef_client::update_pcf_policy_auth(
    const std::string& app_session_id, const nlohmann::json& request_body,
    uint32_t& http_code) {
  http_code = 0;
  std::string pcf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_PCF, pcf_url)) return false;

  std::string url = pcf_url + nef_sbi_helper::PcfPolicyAuthBase +
                    "v1/app-sessions/" + app_session_id + "/modify";
  std::string body       = request_body.dump();
  oai::http::request req = http_client_inst->prepare_json_request(url, body);
  auto resp              = http_client_inst->send_http_request(
      oai::common::sbi::method_e::POST, req);
  http_code = resp.status_code;
  return (
      resp.status_code == http_status_code::OK ||
      resp.status_code == http_status_code::NO_CONTENT);
}

//------------------------------------------------------------------------------
bool nef_client::delete_pcf_policy_auth(
    const std::string& app_session_id, uint32_t& http_code) {
  http_code = 0;
  std::string pcf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_PCF, pcf_url)) return false;

  std::string url = pcf_url + nef_sbi_helper::PcfPolicyAuthBase +
                    "v1/app-sessions/" + app_session_id + "/delete";
  oai::http::request req = http_client_inst->prepare_json_request(url, "{}");
  auto resp              = http_client_inst->send_http_request(
      oai::common::sbi::method_e::POST, req);
  http_code = resp.status_code;
  return (
      resp.status_code == http_status_code::NO_CONTENT ||
      resp.status_code == http_status_code::OK);
}

//------------------------------------------------------------------------------
bool nef_client::create_pcf_bdt_policy(
    const nlohmann::json& bdt_req, std::string& pcf_bdt_id,
    uint32_t& http_code) {
  http_code = 0;
  std::string pcf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_PCF, pcf_url)) {
    Logger::nef_app().warn("PCF not found");
    return false;
  }

  const std::string url =
      pcf_url + nef_sbi_helper::PcfBdtPolicyControlBase + "v1/bdtpolicies";
  oai::http::request req =
      http_client_inst->prepare_json_request(url, bdt_req.dump());
  auto resp = http_client_inst->send_http_request(
      oai::common::sbi::method_e::POST, req);
  http_code = resp.status_code;

  if (!is_2xx_status(resp.status_code) &&
      resp.status_code != http_status_code::SEE_OTHER) {
    return false;
  }

  std::string location = get_header_case_insensitive(resp.headers, "Location");
  pcf_bdt_id           = extract_last_path_segment(location);

  if (pcf_bdt_id.empty() && !resp.body.empty()) {
    try {
      nlohmann::json j = nlohmann::json::parse(resp.body);
      pcf_bdt_id       = j.value("bdtPolicyId", "");
      if (pcf_bdt_id.empty()) pcf_bdt_id = j.value("bdtRefId", "");
      if (pcf_bdt_id.empty() && j.contains("bdtPolData")) {
        pcf_bdt_id = j["bdtPolData"].value("bdtRefId", "");
      }
    } catch (...) {
    }
  }

  return !pcf_bdt_id.empty();
}

//------------------------------------------------------------------------------
bool nef_client::update_pcf_bdt_policy(
    const std::string& bdt_policy_id, const nlohmann::json& bdt_patch,
    uint32_t& http_code) {
  http_code = 0;
  std::string pcf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_PCF, pcf_url)) return false;

  const std::string url = pcf_url + nef_sbi_helper::PcfBdtPolicyControlBase +
                          "v1/bdtpolicies/" + bdt_policy_id;
  oai::http::request req =
      http_client_inst->prepare_json_request(url, bdt_patch.dump());
  auto resp = http_client_inst->send_http_request(
      oai::common::sbi::method_e::PATCH, req);
  http_code = resp.status_code;

  return (
      resp.status_code == http_status_code::OK ||
      resp.status_code == http_status_code::NO_CONTENT);
}

//------------------------------------------------------------------------------
bool nef_client::delete_pcf_bdt_policy(
    const std::string& bdt_policy_id, uint32_t& http_code) {
  http_code = 0;
  std::string pcf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_PCF, pcf_url)) return false;

  const std::string url = pcf_url + nef_sbi_helper::PcfBdtPolicyControlBase +
                          "v1/bdtpolicies/" + bdt_policy_id;
  oai::http::request req = http_client_inst->prepare_json_request(url, "");
  auto resp              = http_client_inst->send_http_request(
      oai::common::sbi::method_e::DELETE, req);
  http_code = resp.status_code;

  return (
      resp.status_code == http_status_code::NO_CONTENT ||
      resp.status_code == http_status_code::OK);
}

// UDR PFD data
//------------------------------------------------------------------------------
bool nef_client::udr_put_pfd_data(
    const std::string& app_id, const nlohmann::json& pfd_data) {
  std::string udr_url;
  if (!discover_nf(nf_type_t::NF_TYPE_UDR, udr_url)) {
    Logger::nef_app().warn("UDR not found");
    return false;
  }
  // Nudr_DataRepository: PUT /nudr-dr/v1/application-data/pfds/{appId}
  std::string url = udr_url + nef_sbi_helper::UdrDataRepositoryBase +
                    "v1/application-data/pfds/" + app_id;
  std::string body = pfd_data.dump();

  auto sbi_sleep_udr = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log_udr = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  int status = sbi_call_with_retry(
      "UDR", /*is_post=*/false,
      [&]() -> int {
        oai::http::request req =
            http_client_inst->prepare_json_request(url, body);
        auto resp = http_client_inst->send_http_request(
            oai::common::sbi::method_e::PUT, req);
        return static_cast<int>(resp.status_code);
      },
      sbi_circuit_breaker_registry::instance(), sbi_sleep_udr, sbi_log_udr);
  return (
      status == http_status_code::OK || status == http_status_code::CREATED);
}

//------------------------------------------------------------------------------
bool nef_client::udr_delete_pfd_data(const std::string& app_id) {
  std::string udr_url;
  if (!discover_nf(nf_type_t::NF_TYPE_UDR, udr_url)) return false;

  std::string url = udr_url + nef_sbi_helper::UdrDataRepositoryBase +
                    "v1/application-data/pfds/" + app_id;

  auto sbi_sleep_udrd = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log_udrd = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  int status = sbi_call_with_retry(
      "UDR", /*is_post=*/false,
      [&]() -> int {
        oai::http::request req =
            http_client_inst->prepare_json_request(url, "");
        auto resp = http_client_inst->send_http_request(
            oai::common::sbi::method_e::DELETE, req);
        return static_cast<int>(resp.status_code);
      },
      sbi_circuit_breaker_registry::instance(), sbi_sleep_udrd, sbi_log_udrd);
  return (status == http_status_code::NO_CONTENT);
}

//------------------------------------------------------------------------------
void nef_client::udr_get_pfd_data(
    const std::string& app_id, nlohmann::json& result, uint32_t& http_code) {
  result    = nlohmann::json::object();
  http_code = 0;

  std::string udr_url;
  if (!discover_nf(nf_type_t::NF_TYPE_UDR, udr_url)) {
    Logger::nef_app().warn("UDR not found");
    return;
  }

  const std::string url = udr_url + nef_sbi_helper::UdrDataRepositoryBase +
                          "v2/application-data/pfds/" + app_id;

  oai::http::response last_get_resp{};
  auto sbi_sleep_udrg = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log_udrg = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  sbi_call_with_retry(
      "UDR", /*is_post=*/false,
      [&]() -> int {
        oai::http::request req =
            http_client_inst->prepare_json_request(url, "");
        last_get_resp = http_client_inst->send_http_request(
            oai::common::sbi::method_e::GET, req);
        return static_cast<int>(last_get_resp.status_code);
      },
      sbi_circuit_breaker_registry::instance(), sbi_sleep_udrg, sbi_log_udrg);

  http_code = last_get_resp.status_code;
  if (last_get_resp.body.empty()) return;

  try {
    result = nlohmann::json::parse(last_get_resp.body);
  } catch (...) {
    Logger::nef_app().warn("Failed to parse UDR PFD GET response body");
  }
}

//------------------------------------------------------------------------------
bool nef_client::udr_put_influence_data(
    const std::string& ti_id, const nlohmann::json& data, uint32_t& http_code) {
  http_code = 0;
  std::string udr_url;
  if (!discover_nf(nf_type_t::NF_TYPE_UDR, udr_url)) {
    Logger::nef_app().warn("UDR not found");
    return false;
  }

  const std::string url = udr_url + nef_sbi_helper::UdrDataRepositoryBase +
                          "v2/application-data/influenceData/" + ti_id;
  oai::http::request req =
      http_client_inst->prepare_json_request(url, data.dump());
  auto resp =
      http_client_inst->send_http_request(oai::common::sbi::method_e::PUT, req);
  http_code = resp.status_code;

  return (
      resp.status_code == http_status_code::OK ||
      resp.status_code == http_status_code::CREATED ||
      resp.status_code == http_status_code::NO_CONTENT);
}

//------------------------------------------------------------------------------
bool nef_client::udr_delete_influence_data(
    const std::string& ti_id, uint32_t& http_code) {
  http_code = 0;
  std::string udr_url;
  if (!discover_nf(nf_type_t::NF_TYPE_UDR, udr_url)) return false;

  const std::string url = udr_url + nef_sbi_helper::UdrDataRepositoryBase +
                          "v2/application-data/influenceData/" + ti_id;
  oai::http::request req = http_client_inst->prepare_json_request(url, "");
  auto resp              = http_client_inst->send_http_request(
      oai::common::sbi::method_e::DELETE, req);
  http_code = resp.status_code;

  return (
      resp.status_code == http_status_code::NO_CONTENT ||
      resp.status_code == http_status_code::OK);
}

// NEF own callback URL (for southbound subscriptions)
//------------------------------------------------------------------------------
std::string nef_client::get_nef_notify_uri(const std::string& nf_sub_id) {
  // Build:  http://<nef_host>:<port>/nef-notify/v1/notify/<nf_sub_id>
  std::string nef_url = nef_config_inst->get_local()->get_url();
  // get_url() returns "http://host:port" (no trailing slash)
  return nef_url + nef_sbi_helper::NefNotifyBase + "v1/notify/" + nf_sub_id;
}

// Forward notification to AF
//------------------------------------------------------------------------------
bool nef_client::forward_notification_to_af(
    const std::string& af_notif_uri, const nlohmann::json& payload) {
  Logger::nef_app().debug(
      "Forwarding notification to AF: %s", af_notif_uri.c_str());

  const std::string body     = payload.dump();
  const std::string endpoint = cb_endpoint_key(af_notif_uri);

  auto attempt_fn = [&]() -> int {
    oai::http::request req =
        http_client_inst->prepare_json_request(af_notif_uri, body);
    auto resp = http_client_inst->send_http_request(
        oai::common::sbi::method_e::POST, req);
    return static_cast<int>(resp.status_code);
  };

  auto log_fn = [](retry_log_level level, const std::string& msg) {
    switch (level) {
      case retry_log_level::WARN_LEVEL:
        Logger::nef_app().warn("%s", msg.c_str());
        break;
      case retry_log_level::CRIT_LEVEL:
        Logger::nef_app().error("%s", msg.c_str());
        break;
      case retry_log_level::ERR_LEVEL:
        Logger::nef_app().error("%s", msg.c_str());
        break;
    }
  };

  auto sleep_fn = [](std::chrono::seconds d) {
    std::this_thread::sleep_for(d);
  };

  return retry_with_backoff(
      endpoint, attempt_fn, log_fn, sleep_fn,
      /*max_attempts=*/3, circuit_breaker_registry::instance());
}
