/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_client.hpp"

#include <cctype>
#include <thread>
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
#include "sbi_resilience.hpp"
#include "sbi_helper.hpp"

extern std::shared_ptr<oai::nghttp2::http_client> http_client_inst;
extern std::unique_ptr<oai::config::nef::nef_config> nef_config_inst;

using namespace oai::nef::app;

// Every *_async call below follows the same pattern: it builds the same request
// as its synchronous counterpart, then sends it without waiting. The endpoint
// is looked up with the blocking discover_nf(), so both versions build the
// request the same way; only the southbound call itself is asynchronous.
//
// The callback gets the raw response and nothing more. It does not touch
// nef_app state, and it does not parse ids out of the body; that is the
// caller's job. A target that cannot be resolved arrives as status_code 0 with
// an empty body.
//
// The comments below only note where a call differs from this pattern.
using namespace oai::config::nef;
using namespace oai::config;
using namespace oai::nef::api;
using namespace oai::common::sbi;

// Helpers for NRF registration and discovery: URI building, response parsing,
// and the like.

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
/**
 * Map a 3GPP NFType string to the key its endpoint is pinned under in
 * nef.yaml. Returns "" when NEF has no static configuration slot for that NF.
 */
static std::string nf_type_to_config_name(const std::string& nf_type) {
  if (nf_type == "AMF") return AMF_CONFIG_NAME;
  if (nf_type == "SMF") return SMF_CONFIG_NAME;
  if (nf_type == "PCF") return PCF_CONFIG_NAME;
  if (nf_type == "UDR") return UDR_CONFIG_NAME;
  return "";
}

//------------------------------------------------------------------------------
static bool is_2xx_status(const int status_code) {
  return status_code >= http_status_code::OK &&
         status_code < http_status_code::MULTIPLE_CHOICES;
}

//------------------------------------------------------------------------------
static std::string get_header_case_insensitive(
    std::map<std::string, std::string>& headers, const std::string& key) {
  // TODO: restore the loop below once the HTTP client exposes response headers.
  // Until then this always returns "", so the callers that read a Location
  // header (the PCF app-session and BDT-policy creates) get nothing from it and
  // take the id from the response body instead.
  /*
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
*/

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
// The nf_service base generates nf_instance_id and keeps the event subscriber
// and the http_client, so this only logs the instance id. Where an NRF
// procedure below does not use the base's version, its own comment says why.
nef_client::nef_client(
    const std::shared_ptr<oai::sba::nf_event>& ev,
    const std::shared_ptr<oai::nghttp2::http_client>& client_inst)
    : oai::sba::nf_service(ev, client_inst) {
  Logger::nef_app().debug("NEF client instance ID: %s", nf_instance_id.c_str());
}

//------------------------------------------------------------------------------
// Destructor
nef_client::~nef_client() {
  Logger::nef_app().debug("Delete NEF Client instance...");
}

// NRF registration
//------------------------------------------------------------------------------
// Builds the NEF NFProfile and passes it to nf_service, which sends the PUT.
// The NEF-specific behaviour comes from this class's overrides of the base's
// hooks, further down this file:
//  - send_with_policy: sends with retries and the circuit breaker;
//  - registration_succeeded: 200 or 201 counts as success;
//  - on_registration_outcome: nef_app, not the base, owns the heartbeat and
//    the re-registration schedule.
//
// The register_nrf gate is checked here as well as in the hook, so an NEF with
// NRF registration disabled does not build a profile at all.
bool nef_client::register_to_nrf() {
  if (!nef_config_inst->register_nrf()) {
    Logger::nef_app().info("NRF registration is disabled in config.");
    return true;
  }

  Logger::nef_app().info(
      "Registering NEF to NRF (instance: %s)...", nf_instance_id.c_str());

  // Build the NF profile.
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
    nf_profile.setNfInstanceId(nf_instance_id);
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

  // NF services are built as raw JSON: the service name strings
  // (nnef-trafficinfluence, nnef-bdt and the rest) are not in
  // ServiceName_anyOf.
  nlohmann::json nf_services = nlohmann::json::array();
  auto add_service           = [&](const std::string& svc_name,
                         const std::string& api_name,
                         const std::string& version) {
    nlohmann::json svc;
    svc["serviceInstanceId"] = nf_instance_id;
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

  // PUT to NRF NF Management API, sent by nf_service::send_nf_registration().
  auto nrf_cfg = nef_config_inst->get_nf(oai::config::NRF_CONFIG_NAME);
  return oai::sba::nf_service::register_to_nrf(nf_to_addr(nrf_cfg), profile);
}

//------------------------------------------------------------------------------
// The nf_service base class does the work: it sends an empty-bodied DELETE to
// this NEF's NFManagement URI and treats 204 as success.
bool nef_client::deregister_from_nrf() {
  if (!nef_config_inst->register_nrf()) return true;

  Logger::nef_app().info("Deregistering NEF from NRF...");
  auto nrf_cfg  = nef_config_inst->get_nf(oai::config::NRF_CONFIG_NAME);
  const bool ok = oai::sba::nf_service::deregister_to_nrf();
  if (ok) {
    Logger::nef_app().info("NEF deregistered from NRF");
  } else {
    Logger::nef_app().warn("NRF deregistration failed");
  }
  return ok;
}

//------------------------------------------------------------------------------
// Called from nef_app's 50 s task, not from nf_service's own heartbeat timer.
// That timer defaults to 10 s, and running it as well would mean two timers
// sending the PATCH.
//
// On failure this re-registers through register_to_nrf(), so the config gate
// and the retry policy apply here too.
bool nef_client::send_heartbeat_to_nrf() {
  if (!nef_config_inst->register_nrf()) return true;

  Logger::nef_app().debug("Sending heartbeat to NRF...");

  // PATCH /nf-instances/<id>  with
  // [{"op":"replace","path":"/nfStatus","value":"REGISTERED"}]
  nlohmann::json patch_body = nlohmann::json::array();
  patch_body.push_back(
      {{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}});

  std::string nrf_uri = build_nrf_nf_instance_uri(nf_instance_id);
  oai::nghttp2::request req =
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
// nf_service::discover_nf() does the work. It sends the NRF request and sets
// the lookup order: local config -> enablement gate -> cache -> NRF
// SearchNFInstances. Each step that NEF customises is one of the hooks below.
//
// The empty service name means "the first service the instance lists".
bool nef_client::discover_nf(nf_type_t nf_type, std::string& nf_endpoint) {
  oai::common::sbi::nf_addr_t nrf_addr = {};
  try {
    // Only looked up when it can actually be used: an NEF configured without
    // NRF registration never needs an NRF address.
    if (nef_config_inst->register_nrf())
      nrf_addr =
          nf_to_addr(nef_config_inst->get_nf(oai::config::NRF_CONFIG_NAME));
  } catch (...) {
  }

  return oai::sba::nf_service::discover_nf(
      nrf_addr, "NEF", nf_type_to_str(nf_type), std::string{}, nf_endpoint);
}

//------------------------------------------------------------------------------
// nf_service policy hooks
//------------------------------------------------------------------------------
bool nef_client::nrf_registration_enabled() const {
  return nef_config_inst->register_nrf();
}

//------------------------------------------------------------------------------
bool nef_client::nrf_discovery_enabled() const {
  return nef_config_inst->register_nrf();
}

//------------------------------------------------------------------------------
// A statically pinned address wins over the NRF. Deployments that pin peer
// addresses, the h2c test harness among them, depend on that.
bool nef_client::resolve_endpoint_from_config(
    const std::string& target_nf_type, const std::string& service_name,
    std::string& endpoint) {
  (void) service_name;
  const std::string cfg_key = nf_type_to_config_name(target_nf_type);
  if (cfg_key.empty()) return false;

  try {
    auto nf_cfg = nef_config_inst->get_nf(cfg_key);
    if (nf_cfg) {
      endpoint = nf_cfg->get_url(nef_config_inst->enable_tls());
      Logger::nef_app().debug(
          "NF %s endpoint from config: %s", cfg_key.c_str(), endpoint.c_str());
      return true;
    }
  } catch (...) {
  }
  return false;
}

//------------------------------------------------------------------------------
// NEF picks the endpoint from the SearchResult itself instead of using the
// base's selection. The two differ in two ways:
//  - the http scheme is hardcoded;
//  - nfServices[0] is taken without checking that ipEndPoints is there, so a
//    missing one is a parse error rather than a fall-through to the instance
//    address.
//
// The base's selection is more forgiving, so using it would change which
// endpoint NEF talks to when a SearchResult is malformed. The result still goes
// into the base's cache, through discovery_cache_store().
bool nef_client::handle_discovery_response(
    const oai::nghttp2::response& search_result_resp,
    const std::string& target_nf_type, const std::string& service_name,
    std::string& endpoint) {
  try {
    nlohmann::json j = nlohmann::json::parse(search_result_resp.body);
    // SearchResult -> nfInstances[0] -> nfServices[0] -> ipEndPoints[0]
    auto& instances = j.at("nfInstances");
    if (instances.empty()) {
      Logger::nef_app().warn(
          "NF discovery: no instances found for %s", target_nf_type.c_str());
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
        auto& ep = inst["nfServices"][0].at("ipEndPoints")[0];
        endpoint = scheme + "://" + ep.value("ipv4Address", "") + ":" +
                   std::to_string(
                       ep.value("port", static_cast<int>(default_sbi_port())));
        found = true;
      } else if (
          inst.contains("ipv4Addresses") && !inst["ipv4Addresses"].empty()) {
        endpoint = "http://" + inst["ipv4Addresses"][0].get<std::string>() +
                   ":" + std::to_string(static_cast<int>(default_sbi_port()));
        found = true;
      }
      // TODO: for now, do not do NF selection, just take the first valid one
      if (found) break;
    }

    if (!found) {
      Logger::nef_app().warn(
          "NF discovery: no usable endpoint for %s", target_nf_type.c_str());
      return false;
    }

    Logger::nef_app().debug(
        "NF discovery: %s -> %s", target_nf_type.c_str(), endpoint.c_str());
    // Cache the result
    discovery_cache_store(target_nf_type, service_name, endpoint);
    return true;
  } catch (nlohmann::json::exception& e) {
    Logger::nef_app().warn("NF discovery parse error: %s", e.what());
    return false;
  }
}

//------------------------------------------------------------------------------
// Registration and discovery are retried and feed the SBI circuit breaker.
// De-registration (at shutdown) and the heartbeat are sent once, with no retry.
oai::nghttp2::response nef_client::send_with_policy(
    oai::sba::nrf_call_kind kind, const oai::common::sbi::method_e& method,
    const oai::nghttp2::request& req) {
  const bool is_registration = kind == oai::sba::nrf_call_kind::registration;
  if (!is_registration && kind != oai::sba::nrf_call_kind::discovery)
    return oai::sba::nf_service::send_with_policy(kind, method, req);

  oai::nghttp2::response resp{};
  auto sbi_sleep = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  sbi_call_with_retry(
      "NRF", /*is_post=*/is_registration,
      [&]() -> int {
        resp = http_client_inst->send_http_request(method, req);
        return static_cast<int>(resp.status_code);
      },
      sbi_circuit_breaker_registry::instance(), sbi_sleep, sbi_log);
  return resp;
}

//------------------------------------------------------------------------------
// Only the status code is checked. The base default also requires nfStatus in
// the body, but the NRF's answer to the registration PUT does not always
// include it.
bool nef_client::registration_succeeded(
    const oai::nghttp2::response& resp) const {
  return resp.status_code == http_status_code::OK ||
         resp.status_code == http_status_code::CREATED;
}

//------------------------------------------------------------------------------
// Only logs the outcome. nef_app owns both the 50 s heartbeat task and the
// re-registration that a heartbeat failure triggers, so neither of the base's
// timers is started.
void nef_client::on_registration_outcome(
    bool success, const oai::nghttp2::response& resp) {
  if (success) {
    Logger::nef_app().info(
        "NEF successfully registered to NRF (status %d)", resp.status_code);
    return;
  }
  Logger::nef_app().warn(
      "NEF NRF registration failed (status %d): %s", resp.status_code,
      resp.body.c_str());
}

//------------------------------------------------------------------------------
// Async NF discovery. Uses the same lookup order as discover_nf(), but never
// blocks. The callback is always called, with one of three outcomes:
//
//  - Static-config or discovery-cache hit: resolved with no network call at
//    all, and the callback runs synchronously. Its response is a synthetic 200
//    whose body is a minimal SearchResult carrying the resolved endpoint
//    ({"nfInstances":[{"nfServices":[{"ipEndPoints":[{ipv4Address,port}]}]}]}),
//    so the caller can parse it like a real NRF SearchResult.
//
//  - Cache miss with NRF discovery enabled: a single async GET goes to the
//    NRF, and its raw SearchResult response reaches the callback unchanged.
//
//  - Target not resolvable (NRF disabled and no static config): status_code 0
//    with an empty body.
//
// This reads the discovery cache but intentionally never writes to it.
void nef_client::discover_nf_async(
    nf_type_t nf_type, oai::nghttp2::response_cb cb) {
  auto deliver_endpoint = [&cb](const std::string& endpoint) {
    nlohmann::json ep;
    // The endpoint is "scheme://host:port". Split out host and port for the
    // synthetic ipEndPoints entry, so the caller parses it the same way as an
    // NRF answer.
    std::string host = endpoint;
    int port         = 8080;
    auto scheme_pos  = endpoint.find("://");
    std::string rest = (scheme_pos == std::string::npos) ?
                           endpoint :
                           endpoint.substr(scheme_pos + 3);
    auto colon_pos   = rest.rfind(':');
    if (colon_pos != std::string::npos) {
      host = rest.substr(0, colon_pos);
      try {
        port = std::stoi(rest.substr(colon_pos + 1));
      } catch (...) {
      }
    } else {
      host = rest;
    }
    nlohmann::json ip_ep    = {{"ipv4Address", host}, {"port", port}};
    nlohmann::json service  = {{"ipEndPoints", nlohmann::json::array({ip_ep})}};
    nlohmann::json instance = {
        {"nfServices", nlohmann::json::array({service})}};
    nlohmann::json search_res = {
        {"nfInstances", nlohmann::json::array({instance})}};
    oai::nghttp2::response synth{};
    synth.status_code = http_status_code::OK;
    synth.body        = search_res.dump();
    cb(std::move(synth));
  };

  // Local config first: a pinned endpoint wins over the NRF.
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
        deliver_endpoint(nf_cfg->get_url(nef_config_inst->enable_tls()));
        return;
      }
    } catch (...) {
    }
  }

  // No pinned endpoint, so fall back to NRF discovery if it is enabled.
  if (!nef_config_inst->register_nrf()) {
    Logger::nef_app().warn(
        "NRF discovery disabled and no static config for NF type %d",
        static_cast<int>(nf_type));
    oai::nghttp2::response err{};
    err.status_code = 0;
    cb(std::move(err));
    return;
  }

  std::string nf_type_str = nf_type_to_str(nf_type);

  // Then the discovery cache. It is the same cache the blocking discover_nf()
  // fills, since both use the base's hooks with an empty service name.
  {
    std::string cached_ep = {};
    if (discovery_cache_lookup(nf_type_str, std::string{}, cached_ep)) {
      deliver_endpoint(cached_ep);
      return;
    }
  }

  // Cache miss: ask the NRF for the NF type's endpoint with a single async
  // GET, and hand the raw SearchResult to the caller's callback. There is no
  // retry loop on the async path.
  std::string disc_uri = build_nrf_disc_uri() + "?" +
                         "target-nf-type=" + nf_type_str +
                         "&requester-nf-type=NEF";
  Logger::nef_app().debug("Async NF discovery URI (NRF): %s", disc_uri.c_str());

  oai::nghttp2::request req =
      http_client_inst->prepare_json_request(disc_uri, "");
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::GET, req, std::move(cb));
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
  std::string url = amf_url + nef_sbi_helper::AmfEvtsBase + "v1/subscriptions";

  // Inject NEF's own callback URL so the AMF knows where to send event
  // notifications. This subscription is created by the NEF on behalf of an AF;
  // the NF→AF mapping is updated once the subscription exists.
  // TS 29.518 names the change-notification field "subsChangeNotifyUri".
  nlohmann::json sub_body = subscription_data;
  // A fixed "/notify/amf" callback path is enough: the HTTP/2 server routes
  // inbound notifications by path segment, and the inbound correlation map is
  // keyed by the AMF subscription id, not by the URL.
  sub_body["eventNotifyUri"] = nef_config_inst->get_local()->get_url() +
                               nef_sbi_helper::NefNotifyBase + "v1/notify/amf";
  // TODO: sub_body["notifyCorrelationId"] = ;
  // TODO: verify whether we need to set subsChangeNotifyUri,
  // subsChangeNotifyCorrelationId (from AF)

  std::string body = sub_body.dump();

  oai::nghttp2::response amf_sub_resp{};
  auto sbi_sleep_amf = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log_amf = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  sbi_call_with_retry(
      "AMF", /*is_post=*/true,
      [&]() -> int {
        oai::nghttp2::request req =
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
  // A connection failure or a 503 suggests the cached endpoint is stale, so
  // drop it. The next discover_nf() then re-queries the NRF.
  if (amf_sub_resp.status_code == 0 ||
      amf_sub_resp.status_code == http_status_code::SERVICE_UNAVAILABLE) {
    discovery_cache_invalidate("AMF");
  }
  Logger::nef_app().warn(
      "AMF event subscription failed (status %d)", amf_sub_resp.status_code);
  return false;
}

//------------------------------------------------------------------------------
// Async variant of subscribe_amf_event_exposure. It sets eventNotifyUri in the
// body the same way the synchronous version does.
void nef_client::subscribe_amf_event_exposure_async(
    const nlohmann::json& subscription_data, oai::nghttp2::response_cb cb) {
  std::string amf_url = {};
  if (!discover_nf(nf_type_t::NF_TYPE_AMF, amf_url)) {
    Logger::nef_app().warn(
        "AMF not found — cannot subscribe to event exposure (async)");
    oai::nghttp2::response err{};
    err.status_code = 0;
    cb(std::move(err));
    return;
  }
  std::string url = amf_url + nef_sbi_helper::AmfEvtsBase + "v1/subscriptions";

  nlohmann::json sub_body    = subscription_data;
  sub_body["eventNotifyUri"] = nef_config_inst->get_local()->get_url() +
                               nef_sbi_helper::NefNotifyBase + "v1/notify/amf";
  std::string body = sub_body.dump();

  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, body);
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::POST, req, std::move(cb));
}

//------------------------------------------------------------------------------
bool nef_client::unsubscribe_amf_event_exposure(const std::string& amf_sub_id) {
  std::string amf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_AMF, amf_url)) return false;

  std::string url =
      amf_url + nef_sbi_helper::AmfEvtsBase + "v1/subscriptions/" + amf_sub_id;
  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, "");
  auto resp                 = http_client_inst->send_http_request(
      oai::common::sbi::method_e::DELETE, req);
  return (
      resp.status_code == http_status_code::NO_CONTENT ||
      resp.status_code == http_status_code::OK);
}

//------------------------------------------------------------------------------
// Async variant of unsubscribe_amf_event_exposure.
void nef_client::unsubscribe_amf_event_exposure_async(
    const std::string& amf_sub_id, oai::nghttp2::response_cb cb) {
  std::string amf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_AMF, amf_url)) {
    Logger::nef_app().warn(
        "AMF not found — cannot unsubscribe event exposure (async)");
    oai::nghttp2::response err{};
    err.status_code = 0;
    cb(std::move(err));
    return;
  }
  std::string url =
      amf_url + nef_sbi_helper::AmfEvtsBase + "v1/subscriptions/" + amf_sub_id;
  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, "");
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::DELETE, req, std::move(cb));
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

  // The caller (nef_app) builds the full NsmfEventExposure body, with its
  // eventSubs and target filters. Only two fields are added here, both defined
  // in TS 29.508: the NEF-chosen correlation id (notifId) and the
  // per-subscription inbound notification URI (notifUri).
  nlohmann::json sub_body = smf_body;
  sub_body["notifId"]     = notif_id;
  sub_body["notifUri"]    = notif_uri;

  std::string body = sub_body.dump();

  oai::nghttp2::response smf_sub_resp{};
  auto sbi_sleep_smf = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log_smf = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  sbi_call_with_retry(
      "SMF", /*is_post=*/true,
      [&]() -> int {
        oai::nghttp2::request req =
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
// Async variant of subscribe_smf_event_exposure. It sets notifId and notifUri
// in the body the same way the synchronous version does.
void nef_client::subscribe_smf_event_exposure_async(
    const nlohmann::json& smf_body, const std::string& notif_id,
    const std::string& notif_uri, oai::nghttp2::response_cb cb) {
  std::string smf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_SMF, smf_url)) {
    Logger::nef_app().warn(
        "SMF not found — cannot subscribe to event exposure (async)");
    oai::nghttp2::response err{};
    err.status_code = 0;
    cb(std::move(err));
    return;
  }
  std::string url =
      smf_url + nef_sbi_helper::SmfEventExposureBase + "v1/subscriptions";

  nlohmann::json sub_body = smf_body;
  sub_body["notifId"]     = notif_id;
  sub_body["notifUri"]    = notif_uri;
  std::string body        = sub_body.dump();

  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, body);
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::POST, req, std::move(cb));
}

//------------------------------------------------------------------------------
bool nef_client::unsubscribe_smf_event_exposure(const std::string& smf_sub_id) {
  std::string smf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_SMF, smf_url)) return false;

  std::string url = smf_url + nef_sbi_helper::SmfEventExposureBase +
                    "v1/subscriptions/" + smf_sub_id;
  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, "");
  auto resp                 = http_client_inst->send_http_request(
      oai::common::sbi::method_e::DELETE, req);
  return (
      resp.status_code == http_status_code::NO_CONTENT ||
      resp.status_code == http_status_code::OK);
}

//------------------------------------------------------------------------------
// Async variant of unsubscribe_smf_event_exposure.
void nef_client::unsubscribe_smf_event_exposure_async(
    const std::string& smf_sub_id, oai::nghttp2::response_cb cb) {
  std::string smf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_SMF, smf_url)) {
    Logger::nef_app().warn(
        "SMF not found — cannot unsubscribe event exposure (async)");
    oai::nghttp2::response err{};
    err.status_code = 0;
    cb(std::move(err));
    return;
  }
  std::string url = smf_url + nef_sbi_helper::SmfEventExposureBase +
                    "v1/subscriptions/" + smf_sub_id;
  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, "");
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::DELETE, req, std::move(cb));
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
  // Nsmf_EventExposure defines no PATCH, so the conformant update is a PUT,
  // which fully replaces the individual subscription resource. The caller
  // must already have put notifId/notifUri in smf_body.
  std::string url = smf_url + nef_sbi_helper::SmfEventExposureBase +
                    "v1/subscriptions/" + smf_sub_id;
  std::string body = smf_body.dump();

  oai::nghttp2::response smf_resp{};
  auto sbi_sleep_smf = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log_smf = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  sbi_call_with_retry(
      "SMF", /*is_post=*/false,
      [&]() -> int {
        oai::nghttp2::request req =
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

  oai::nghttp2::response pcf_auth_resp{};
  auto sbi_sleep_pcf = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log_pcf = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  sbi_call_with_retry(
      "PCF", /*is_post=*/true,
      [&]() -> int {
        oai::nghttp2::request req =
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
// Async variant of create_pcf_policy_auth. The appSessionId comes back either
// in the body or in the Location header; the caller reads it from whichever one
// has it.
void nef_client::create_pcf_policy_auth_async(
    const nlohmann::json& request_body, oai::nghttp2::response_cb cb) {
  std::string pcf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_PCF, pcf_url)) {
    Logger::nef_app().warn("PCF not found (async)");
    oai::nghttp2::response err{};
    err.status_code = 0;
    cb(std::move(err));
    return;
  }
  std::string url =
      pcf_url + nef_sbi_helper::PcfPolicyAuthBase + "v1/app-sessions";
  std::string body = request_body.dump();

  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, body);
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::POST, req, std::move(cb));
}

//------------------------------------------------------------------------------
// Discovery-free variant of create_pcf_policy_auth_async: the caller looks up
// the PCF endpoint on the dispatcher worker and passes it in. Because this does
// no discovery, it is safe to call from an oai-http-io continuation, where a
// blocking discovery would deadlock the io pool.
//
// The request itself is the same as the synchronous version's.
void nef_client::create_pcf_policy_auth_at_async(
    const std::string& pcf_endpoint, const nlohmann::json& request_body,
    oai::nghttp2::response_cb cb) {
  std::string url =
      pcf_endpoint + nef_sbi_helper::PcfPolicyAuthBase + "v1/app-sessions";
  std::string body = request_body.dump();

  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, body);
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::POST, req, std::move(cb));
}

//------------------------------------------------------------------------------
bool nef_client::update_pcf_policy_auth(
    const std::string& app_session_id, const nlohmann::json& request_body,
    uint32_t& http_code) {
  http_code = 0;
  std::string pcf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_PCF, pcf_url)) return false;

  std::string url = pcf_url + nef_sbi_helper::PcfPolicyAuthBase +
                    "v1/app-sessions/" + app_session_id;
  std::string body = request_body.dump();

  oai::nghttp2::response resp{};
  auto sbi_sleep_pcf = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log_pcf = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  sbi_call_with_retry(
      "PCF", /*is_post=*/false,
      [&]() -> int {
        oai::nghttp2::request req = http_client_inst->prepare_json_request(
            url, body, "application/merge-patch+json");
        resp = http_client_inst->send_http_request(
            oai::common::sbi::method_e::PATCH, req);
        return static_cast<int>(resp.status_code);
      },
      sbi_circuit_breaker_registry::instance(), sbi_sleep_pcf, sbi_log_pcf);
  http_code = resp.status_code;
  return (
      resp.status_code == http_status_code::OK ||
      resp.status_code == http_status_code::NO_CONTENT);
}

//------------------------------------------------------------------------------
// Async variant of update_pcf_policy_auth. Like the synchronous version, it
// sends the PATCH as application/merge-patch+json.
void nef_client::update_pcf_policy_auth_async(
    const std::string& app_session_id, const nlohmann::json& request_body,
    oai::nghttp2::response_cb cb) {
  std::string pcf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_PCF, pcf_url)) {
    oai::nghttp2::response err{};
    err.status_code = 0;
    cb(std::move(err));
    return;
  }
  std::string url = pcf_url + nef_sbi_helper::PcfPolicyAuthBase +
                    "v1/app-sessions/" + app_session_id;
  std::string body = request_body.dump();

  oai::nghttp2::request req = http_client_inst->prepare_json_request(
      url, body, "application/merge-patch+json");
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::PATCH, req, std::move(cb));
}

//------------------------------------------------------------------------------
bool nef_client::delete_pcf_policy_auth(
    const std::string& app_session_id, uint32_t& http_code) {
  http_code = 0;
  std::string pcf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_PCF, pcf_url)) return false;

  std::string url = pcf_url + nef_sbi_helper::PcfPolicyAuthBase +
                    "v1/app-sessions/" + app_session_id + "/delete";
  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, "{}");
  auto resp                 = http_client_inst->send_http_request(
      oai::common::sbi::method_e::POST, req);
  http_code = resp.status_code;
  return (
      resp.status_code == http_status_code::NO_CONTENT ||
      resp.status_code == http_status_code::OK);
}

//------------------------------------------------------------------------------
// Async variant of delete_pcf_policy_auth. Note the method: PCF defines this
// operation as a POST to .../delete with an empty JSON object as the body, not
// as an HTTP DELETE.
void nef_client::delete_pcf_policy_auth_async(
    const std::string& app_session_id, oai::nghttp2::response_cb cb) {
  std::string pcf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_PCF, pcf_url)) {
    Logger::nef_app().warn("PCF not found (async delete app-session)");
    oai::nghttp2::response err{};
    err.status_code = 0;
    cb(std::move(err));
    return;
  }
  std::string url = pcf_url + nef_sbi_helper::PcfPolicyAuthBase +
                    "v1/app-sessions/" + app_session_id + "/delete";
  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, "{}");
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::POST, req, std::move(cb));
}

//------------------------------------------------------------------------------
// Discovery-free variant of delete_pcf_policy_auth_async. The PCF endpoint
// comes from the caller, so this is safe to call from an oai-http-io
// continuation.
void nef_client::delete_pcf_policy_auth_at_async(
    const std::string& pcf_endpoint, const std::string& app_session_id,
    oai::nghttp2::response_cb cb) {
  std::string url = pcf_endpoint + nef_sbi_helper::PcfPolicyAuthBase +
                    "v1/app-sessions/" + app_session_id + "/delete";
  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, "{}");
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::POST, req, std::move(cb));
}

//------------------------------------------------------------------------------
bool nef_client::subscribe_pcf_events(
    const std::string& app_session_id, const nlohmann::json& ev_subsc_body,
    uint32_t& http_code) {
  http_code = 0;
  std::string pcf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_PCF, pcf_url)) return false;

  const std::string url = pcf_url + nef_sbi_helper::PcfPolicyAuthBase +
                          "v1/app-sessions/" + app_session_id +
                          "/events-subscription";
  const std::string body = ev_subsc_body.dump();

  oai::nghttp2::response resp{};
  auto sbi_sleep_pcf = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log_pcf = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  sbi_call_with_retry(
      "PCF", /*is_post=*/false,
      [&]() -> int {
        oai::nghttp2::request req =
            http_client_inst->prepare_json_request(url, body);
        resp = http_client_inst->send_http_request(
            oai::common::sbi::method_e::PUT, req);
        return static_cast<int>(resp.status_code);
      },
      sbi_circuit_breaker_registry::instance(), sbi_sleep_pcf, sbi_log_pcf);
  http_code = resp.status_code;
  return (
      resp.status_code == http_status_code::CREATED ||
      resp.status_code == http_status_code::OK ||
      resp.status_code == http_status_code::NO_CONTENT);
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
  oai::nghttp2::request req =
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
// Async variant of create_pcf_bdt_policy. Note that a 303 See Other means
// success here, and the caller's continuation treats it that way.
void nef_client::create_pcf_bdt_policy_async(
    const nlohmann::json& bdt_req, oai::nghttp2::response_cb cb) {
  std::string pcf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_PCF, pcf_url)) {
    Logger::nef_app().warn("PCF not found (async BDT create)");
    oai::nghttp2::response err{};
    err.status_code = 0;
    cb(std::move(err));
    return;
  }
  const std::string url =
      pcf_url + nef_sbi_helper::PcfBdtPolicyControlBase + "v1/bdtpolicies";
  oai::nghttp2::request req =
      http_client_inst->prepare_json_request(url, bdt_req.dump());
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::POST, req, std::move(cb));
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
  oai::nghttp2::request req =
      http_client_inst->prepare_json_request(url, bdt_patch.dump());
  auto resp = http_client_inst->send_http_request(
      oai::common::sbi::method_e::PATCH, req);
  http_code = resp.status_code;

  return (
      resp.status_code == http_status_code::OK ||
      resp.status_code == http_status_code::NO_CONTENT);
}

//------------------------------------------------------------------------------
// Async variant of update_pcf_bdt_policy.
void nef_client::update_pcf_bdt_policy_async(
    const std::string& bdt_policy_id, const nlohmann::json& bdt_patch,
    oai::nghttp2::response_cb cb) {
  std::string pcf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_PCF, pcf_url)) {
    Logger::nef_app().warn("PCF not found (async BDT update)");
    oai::nghttp2::response err{};
    err.status_code = 0;
    cb(std::move(err));
    return;
  }
  const std::string url = pcf_url + nef_sbi_helper::PcfBdtPolicyControlBase +
                          "v1/bdtpolicies/" + bdt_policy_id;
  oai::nghttp2::request req =
      http_client_inst->prepare_json_request(url, bdt_patch.dump());
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::PATCH, req, std::move(cb));
}

//------------------------------------------------------------------------------
bool nef_client::delete_pcf_bdt_policy(
    const std::string& bdt_policy_id, uint32_t& http_code) {
  http_code = 0;
  std::string pcf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_PCF, pcf_url)) return false;

  const std::string url = pcf_url + nef_sbi_helper::PcfBdtPolicyControlBase +
                          "v1/bdtpolicies/" + bdt_policy_id;
  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, "");
  auto resp                 = http_client_inst->send_http_request(
      oai::common::sbi::method_e::DELETE, req);
  http_code = resp.status_code;

  return (
      resp.status_code == http_status_code::NO_CONTENT ||
      resp.status_code == http_status_code::OK);
}

//------------------------------------------------------------------------------
// Async variant of delete_pcf_bdt_policy.
void nef_client::delete_pcf_bdt_policy_async(
    const std::string& bdt_policy_id, oai::nghttp2::response_cb cb) {
  std::string pcf_url;
  if (!discover_nf(nf_type_t::NF_TYPE_PCF, pcf_url)) {
    Logger::nef_app().warn("PCF not found (async BDT delete)");
    oai::nghttp2::response err{};
    err.status_code = 0;
    cb(std::move(err));
    return;
  }
  const std::string url = pcf_url + nef_sbi_helper::PcfBdtPolicyControlBase +
                          "v1/bdtpolicies/" + bdt_policy_id;
  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, "");
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::DELETE, req, std::move(cb));
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
        oai::nghttp2::request req =
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
// Async variant of udr_put_pfd_data. Uses the v1 PFD path.
void nef_client::udr_put_pfd_data_async(
    const std::string& app_id, const nlohmann::json& pfd_data,
    oai::nghttp2::response_cb cb) {
  std::string udr_url;
  if (!discover_nf(nf_type_t::NF_TYPE_UDR, udr_url)) {
    Logger::nef_app().warn("UDR not found (async)");
    oai::nghttp2::response err{};
    err.status_code = 0;
    cb(std::move(err));
    return;
  }
  // Nudr_DataRepository: PUT /nudr-dr/v1/application-data/pfds/{appId}
  std::string url = udr_url + nef_sbi_helper::UdrDataRepositoryBase +
                    "v1/application-data/pfds/" + app_id;
  std::string body = pfd_data.dump();

  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, body);
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::PUT, req, std::move(cb));
}

//------------------------------------------------------------------------------
// Discovery-free variant of udr_put_pfd_data_async: the UDR endpoint comes
// from the caller. This uses the v1 PFD path, matching udr_put_pfd_data; the
// v2 path is for GET only.
void nef_client::udr_put_pfd_data_at_async(
    const std::string& udr_endpoint, const std::string& app_id,
    const nlohmann::json& pfd_data, oai::nghttp2::response_cb cb) {
  std::string url = udr_endpoint + nef_sbi_helper::UdrDataRepositoryBase +
                    "v1/application-data/pfds/" + app_id;
  std::string body = pfd_data.dump();

  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, body);
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::PUT, req, std::move(cb));
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
        oai::nghttp2::request req =
            http_client_inst->prepare_json_request(url, "");
        auto resp = http_client_inst->send_http_request(
            oai::common::sbi::method_e::DELETE, req);
        return static_cast<int>(resp.status_code);
      },
      sbi_circuit_breaker_registry::instance(), sbi_sleep_udrd, sbi_log_udrd);
  return (status == http_status_code::NO_CONTENT);
}

//------------------------------------------------------------------------------
// Async variant of udr_delete_pfd_data. Uses the v1 PFD path.
void nef_client::udr_delete_pfd_data_async(
    const std::string& app_id, oai::nghttp2::response_cb cb) {
  std::string udr_url;
  if (!discover_nf(nf_type_t::NF_TYPE_UDR, udr_url)) {
    Logger::nef_app().warn("UDR not found (async PFD delete)");
    oai::nghttp2::response err{};
    err.status_code = 0;
    cb(std::move(err));
    return;
  }
  std::string url = udr_url + nef_sbi_helper::UdrDataRepositoryBase +
                    "v1/application-data/pfds/" + app_id;
  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, "");
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::DELETE, req, std::move(cb));
}

//------------------------------------------------------------------------------
// Discovery-free variant of udr_delete_pfd_data_async: the UDR endpoint comes
// from the caller. Uses the v1 PFD path.
void nef_client::udr_delete_pfd_data_at_async(
    const std::string& udr_endpoint, const std::string& app_id,
    oai::nghttp2::response_cb cb) {
  std::string url = udr_endpoint + nef_sbi_helper::UdrDataRepositoryBase +
                    "v1/application-data/pfds/" + app_id;
  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, "");
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::DELETE, req, std::move(cb));
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

  oai::nghttp2::response last_get_resp{};
  auto sbi_sleep_udrg = [](std::chrono::milliseconds d) {
    std::this_thread::sleep_for(d);
  };
  auto sbi_log_udrg = [](const std::string& m) {
    Logger::nef_app().warn("%s", m.c_str());
  };
  sbi_call_with_retry(
      "UDR", /*is_post=*/false,
      [&]() -> int {
        oai::nghttp2::request req =
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
// Async variant of udr_get_pfd_data. GET goes to the v2 PFD path; PUT and
// DELETE use v1.
void nef_client::udr_get_pfd_data_async(
    const std::string& app_id, oai::nghttp2::response_cb cb) {
  std::string udr_url;
  if (!discover_nf(nf_type_t::NF_TYPE_UDR, udr_url)) {
    Logger::nef_app().warn("UDR not found (async PFD get)");
    oai::nghttp2::response err{};
    err.status_code = 0;
    cb(std::move(err));
    return;
  }
  const std::string url = udr_url + nef_sbi_helper::UdrDataRepositoryBase +
                          "v2/application-data/pfds/" + app_id;
  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, "");
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::GET, req, std::move(cb));
}

//------------------------------------------------------------------------------
// Discovery-free variant of udr_get_pfd_data_async: the UDR endpoint comes
// from the caller. This uses the v2 PFD path, matching udr_get_pfd_data; the
// v1 path is for PUT and DELETE.
void nef_client::udr_get_pfd_data_at_async(
    const std::string& udr_endpoint, const std::string& app_id,
    oai::nghttp2::response_cb cb) {
  const std::string url = udr_endpoint + nef_sbi_helper::UdrDataRepositoryBase +
                          "v2/application-data/pfds/" + app_id;
  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, "");
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::GET, req, std::move(cb));
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
  oai::nghttp2::request req =
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
// Async variant of udr_put_influence_data. Uses the v2 influence-data path.
void nef_client::udr_put_influence_data_async(
    const std::string& ti_id, const nlohmann::json& data,
    oai::nghttp2::response_cb cb) {
  std::string udr_url;
  if (!discover_nf(nf_type_t::NF_TYPE_UDR, udr_url)) {
    Logger::nef_app().warn("UDR not found (async)");
    oai::nghttp2::response err{};
    err.status_code = 0;
    cb(std::move(err));
    return;
  }
  const std::string url = udr_url + nef_sbi_helper::UdrDataRepositoryBase +
                          "v2/application-data/influenceData/" + ti_id;

  oai::nghttp2::request req =
      http_client_inst->prepare_json_request(url, data.dump());
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::PUT, req, std::move(cb));
}

//------------------------------------------------------------------------------
// Discovery-free variant of udr_put_influence_data_async: the UDR endpoint
// comes from the caller. Uses the v2 influence-data path.
void nef_client::udr_put_influence_data_at_async(
    const std::string& udr_endpoint, const std::string& ti_id,
    const nlohmann::json& data, oai::nghttp2::response_cb cb) {
  const std::string url = udr_endpoint + nef_sbi_helper::UdrDataRepositoryBase +
                          "v2/application-data/influenceData/" + ti_id;

  oai::nghttp2::request req =
      http_client_inst->prepare_json_request(url, data.dump());
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::PUT, req, std::move(cb));
}

//------------------------------------------------------------------------------
bool nef_client::udr_delete_influence_data(
    const std::string& ti_id, uint32_t& http_code) {
  http_code = 0;
  std::string udr_url;
  if (!discover_nf(nf_type_t::NF_TYPE_UDR, udr_url)) return false;

  const std::string url = udr_url + nef_sbi_helper::UdrDataRepositoryBase +
                          "v2/application-data/influenceData/" + ti_id;
  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, "");
  auto resp                 = http_client_inst->send_http_request(
      oai::common::sbi::method_e::DELETE, req);
  http_code = resp.status_code;

  return (
      resp.status_code == http_status_code::NO_CONTENT ||
      resp.status_code == http_status_code::OK);
}

//------------------------------------------------------------------------------
// Async variant of udr_delete_influence_data. Uses the v2 influence-data path.
void nef_client::udr_delete_influence_data_async(
    const std::string& ti_id, oai::nghttp2::response_cb cb) {
  std::string udr_url;
  if (!discover_nf(nf_type_t::NF_TYPE_UDR, udr_url)) {
    Logger::nef_app().warn("UDR not found (async influence delete)");
    oai::nghttp2::response err{};
    err.status_code = 0;
    cb(std::move(err));
    return;
  }
  const std::string url = udr_url + nef_sbi_helper::UdrDataRepositoryBase +
                          "v2/application-data/influenceData/" + ti_id;
  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, "");
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::DELETE, req, std::move(cb));
}

//------------------------------------------------------------------------------
// Discovery-free variant of udr_delete_influence_data_async: the UDR endpoint
// comes from the caller. Uses the v2 influence-data path.
void nef_client::udr_delete_influence_data_at_async(
    const std::string& udr_endpoint, const std::string& ti_id,
    oai::nghttp2::response_cb cb) {
  const std::string url = udr_endpoint + nef_sbi_helper::UdrDataRepositoryBase +
                          "v2/application-data/influenceData/" + ti_id;
  oai::nghttp2::request req = http_client_inst->prepare_json_request(url, "");
  http_client_inst->send_http_request_async(
      oai::common::sbi::method_e::DELETE, req, std::move(cb));
}

// NEF's own notification callback URL for one NF subscription
//------------------------------------------------------------------------------
std::string nef_client::get_nef_notify_uri(const std::string& nf_sub_id) {
  // Build:  http://<nef_host>:<port>/nef-notify/v1/notify/<nf_sub_id>
  // get_url() returns "http://host:port", with no trailing slash.
  std::string nef_url = nef_config_inst->get_local()->get_url();
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
    oai::nghttp2::request req =
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
