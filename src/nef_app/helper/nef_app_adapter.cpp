/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_app_adapter.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <string>
#include <utility>

#include <memory>

#include "3gpp_29.500.h"
#include "Helpers.h"
#include "http2_server.h"
#include "nef_app.hpp"

using oai::common::sbi::http_status_code;

namespace oai::nef::app {

//------------------------------------------------------------------------------
nef_app_adapter::nef_app_adapter(
    const std::shared_ptr<nef_app>& app, std::size_t n_threads,
    std::size_t http_worker_count, std::size_t max_queue)
    : m_app(app), m_dispatcher(n_threads, http_worker_count, max_queue) {}

//------------------------------------------------------------------------------
void nef_app_adapter::stop() {
  m_dispatcher.stop();
}

//------------------------------------------------------------------------------
std::size_t nef_app_adapter::queue_depth() const {
  return m_dispatcher.queue_depth();
}

//------------------------------------------------------------------------------
// Defined out of line so the body sees the complete nef_app type, which
// nef_app.hpp above supplies. The bearer_token_scope below is the RAII guard
// that clears the token on every exit path, exceptions included.
template<typename Fn>
void nef_app_adapter::execute_with_token(const std::string& token, Fn&& fn) {
  class bearer_token_scope {
   public:
    bearer_token_scope(nef_app& app, const std::string& token) : m_app(app) {
      m_app.set_request_bearer_token(token);
    }
    ~bearer_token_scope() { m_app.clear_request_bearer_token(); }

    bearer_token_scope(const bearer_token_scope&) = delete;
    bearer_token_scope& operator=(const bearer_token_scope&) = delete;

   private:
    nef_app& m_app;
  };

  bearer_token_scope token_scope(*m_app, token);
  std::forward<Fn>(fn)(*m_app);
}

//------------------------------------------------------------------------------
namespace {
// The 422 ProblemDetails body sent when a nef_app handler throws
// ValidationException.
nlohmann::json make_unprocessable(const std::string& detail) {
  nlohmann::json pd;
  pd["type"]   = "about:blank";
  pd["title"]  = "Unprocessable Entity";
  pd["status"] = http_status_code::UNPROCESSABLE_ENTITY;
  pd["detail"] = detail;
  return pd;
}
}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Traffic Influence
// ─────────────────────────────────────────────────────────────────────────────
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_ti_get(
    const std::string& af_id, const std::string& ti_id, std::string token,
    response_sink sink) {
  return m_dispatcher.dispatch(
      [this, af_id, ti_id, t = std::move(token),
       s = std::move(sink)]() mutable { execute_ti_get(af_id, ti_id, t, s); });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_ti_get(
    const std::string& af_id, const std::string& ti_id,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_traffic_influence_get(af_id, ti_id, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_ti_list(
    const std::string& af_id, std::string token, response_sink sink) {
  return m_dispatcher.dispatch(
      [this, af_id, t = std::move(token), s = std::move(sink)]() mutable {
        execute_ti_list(af_id, t, s);
      });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_ti_list(
    const std::string& af_id, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_traffic_influence_list(af_id, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_monitoring_event_get(
    const std::string& scs_as_id, const std::string& sub_id, std::string token,
    response_sink sink) {
  return m_dispatcher.dispatch([this, scs_as_id, sub_id, t = std::move(token),
                                s = std::move(sink)]() mutable {
    execute_monitoring_event_get(scs_as_id, sub_id, t, s);
  });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_monitoring_event_get(
    const std::string& scs_as_id, const std::string& sub_id,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_monitoring_event_subscription_get(
        scs_as_id, sub_id, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_monitoring_event_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& body, std::string token, response_sink sink) {
  return m_dispatcher.dispatch([this, scs_as_id, sub_id, body,
                                t = std::move(token),
                                s = std::move(sink)]() mutable {
    execute_monitoring_event_update(scs_as_id, sub_id, body, t, s);
  });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_monitoring_event_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& body, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_monitoring_event_subscription_update(
        scs_as_id, sub_id, body, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_qos_get(
    const std::string& af_id, const std::string& sub_id, std::string token,
    response_sink sink) {
  return m_dispatcher.dispatch([this, af_id, sub_id, t = std::move(token),
                                s = std::move(sink)]() mutable {
    execute_qos_get(af_id, sub_id, t, s);
  });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_qos_get(
    const std::string& af_id, const std::string& sub_id,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    if (sub_id.empty()) {
      app.handle_qos_subscription_list(af_id, resp_body, http_code);
    } else {
      app.handle_qos_subscription_get(af_id, sub_id, resp_body, http_code);
    }
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_bdt_get(
    const std::string& af_id, const std::string& bdt_id, std::string token,
    response_sink sink) {
  return m_dispatcher.dispatch([this, af_id, bdt_id, t = std::move(token),
                                s = std::move(sink)]() mutable {
    execute_bdt_get(af_id, bdt_id, t, s);
  });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_bdt_get(
    const std::string& af_id, const std::string& bdt_id,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    if (bdt_id.empty()) {
      app.handle_bdt_policy_list(af_id, resp_body, http_code);
    } else {
      app.handle_bdt_policy_get(af_id, bdt_id, resp_body, http_code);
    }
    sink(http_code, resp_body.dump());
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Analytics
// ─────────────────────────────────────────────────────────────────────────────
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_analytics_create(
    const std::string& af_id, const nlohmann::json& body, std::string token,
    response_sink sink) {
  return m_dispatcher.dispatch(
      [this, af_id, body, t = std::move(token), s = std::move(sink)]() mutable {
        execute_analytics_create(af_id, body, t, s);
      });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_analytics_create(
    const std::string& af_id, const nlohmann::json& body,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    std::string sub_id;
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_analytics_subscription_create(
        af_id, body, sub_id, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_analytics_delete(
    const std::string& af_id, const std::string& sub_id, std::string token,
    response_sink sink) {
  return m_dispatcher.dispatch([this, af_id, sub_id, t = std::move(token),
                                s = std::move(sink)]() mutable {
    execute_analytics_delete(af_id, sub_id, t, s);
  });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_analytics_delete(
    const std::string& af_id, const std::string& sub_id,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_analytics_subscription_delete(af_id, sub_id, http_code);
    sink(http_code, "");
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_analytics_get(
    const std::string& af_id, const std::string& sub_id, std::string token,
    response_sink sink) {
  return m_dispatcher.dispatch([this, af_id, sub_id, t = std::move(token),
                                s = std::move(sink)]() mutable {
    execute_analytics_get(af_id, sub_id, t, s);
  });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_analytics_get(
    const std::string& af_id, const std::string& sub_id,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    if (sub_id.empty()) {
      app.handle_analytics_subscription_list(af_id, resp_body, http_code);
    } else {
      app.handle_analytics_subscription_get(
          af_id, sub_id, resp_body, http_code);
    }
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_analytics_update(
    const std::string& af_id, const std::string& sub_id,
    const nlohmann::json& body, std::string token, response_sink sink) {
  return m_dispatcher.dispatch([this, af_id, sub_id, body, t = std::move(token),
                                s = std::move(sink)]() mutable {
    execute_analytics_update(af_id, sub_id, body, t, s);
  });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_analytics_update(
    const std::string& af_id, const std::string& sub_id,
    const nlohmann::json& body, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_analytics_subscription_update(
        af_id, sub_id, body, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_analytics_fetch(
    const std::string& af_id, const nlohmann::json& body, std::string token,
    response_sink sink) {
  return m_dispatcher.dispatch(
      [this, af_id, body, t = std::move(token), s = std::move(sink)]() mutable {
        execute_analytics_fetch(af_id, body, t, s);
      });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_analytics_fetch(
    const std::string& af_id, const nlohmann::json& body,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_analytics_fetch(af_id, body, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_pfd_transaction_list(
    const std::string& scs_as_id, std::string token, response_sink sink) {
  return m_dispatcher.dispatch(
      [this, scs_as_id, t = std::move(token), s = std::move(sink)]() mutable {
        execute_pfd_transaction_list(scs_as_id, t, s);
      });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_pfd_transaction_list(
    const std::string& scs_as_id, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_pfd_transaction_list(scs_as_id, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_pfd_app_get(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, std::string token, response_sink sink) {
  return m_dispatcher.dispatch([this, scs_as_id, trans_id, app_id,
                                t = std::move(token),
                                s = std::move(sink)]() mutable {
    execute_pfd_app_get(scs_as_id, trans_id, app_id, t, s);
  });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_pfd_app_get(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_pfd_app_get(scs_as_id, trans_id, app_id, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Nnef_PFDmanagement
// ─────────────────────────────────────────────────────────────────────────────
nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_list_transactions(
    std::string token, response_sink sink) {
  return m_dispatcher.dispatch(
      [this, t = std::move(token), s = std::move(sink)]() mutable {
        execute_nnef_pfd_list_transactions(t, s);
      });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_nnef_pfd_list_transactions(
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_pfd_list_transactions(resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_get_transaction(
    const std::string& trans_id, std::string token, response_sink sink) {
  return m_dispatcher.dispatch(
      [this, trans_id, t = std::move(token), s = std::move(sink)]() mutable {
        execute_nnef_pfd_get_transaction(trans_id, t, s);
      });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_nnef_pfd_get_transaction(
    const std::string& trans_id, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_pfd_get_transaction(trans_id, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_nnef_pfd_get_app(
    const std::string& trans_id, const std::string& app_id, std::string token,
    response_sink sink) {
  return m_dispatcher.dispatch([this, trans_id, app_id, t = std::move(token),
                                s = std::move(sink)]() mutable {
    execute_nnef_pfd_get_app(trans_id, app_id, t, s);
  });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_nnef_pfd_get_app(
    const std::string& trans_id, const std::string& app_id,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_pfd_get_app(trans_id, app_id, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_get_applications(
    const std::vector<std::string>& app_ids_filter, std::string token,
    response_sink sink) {
  return m_dispatcher.dispatch([this, app_ids_filter, t = std::move(token),
                                s = std::move(sink)]() mutable {
    execute_nnef_pfd_get_applications(app_ids_filter, t, s);
  });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_nnef_pfd_get_applications(
    const std::vector<std::string>& app_ids_filter, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_pfd_get_applications(app_ids_filter, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_subscription_create(
    const nlohmann::json& body, std::string token, response_sink sink) {
  return m_dispatcher.dispatch(
      [this, body, t = std::move(token), s = std::move(sink)]() mutable {
        execute_nnef_pfd_subscription_create(body, t, s);
      });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_nnef_pfd_subscription_create(
    const nlohmann::json& body, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    std::string sub_id;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_pfd_subscription_create(body, sub_id, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_subscription_get(
    const std::string& sub_id, std::string token, response_sink sink) {
  return m_dispatcher.dispatch(
      [this, sub_id, t = std::move(token), s = std::move(sink)]() mutable {
        execute_nnef_pfd_subscription_get(sub_id, t, s);
      });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_nnef_pfd_subscription_get(
    const std::string& sub_id, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_pfd_subscription_get(sub_id, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_subscription_put(
    const std::string& sub_id, const nlohmann::json& body, std::string token,
    response_sink sink) {
  return m_dispatcher.dispatch([this, sub_id, body, t = std::move(token),
                                s = std::move(sink)]() mutable {
    execute_nnef_pfd_subscription_put(sub_id, body, t, s);
  });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_nnef_pfd_subscription_put(
    const std::string& sub_id, const nlohmann::json& body,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_pfd_subscription_put(sub_id, body, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_subscription_delete(
    const std::string& sub_id, std::string token, response_sink sink) {
  return m_dispatcher.dispatch(
      [this, sub_id, t = std::move(token), s = std::move(sink)]() mutable {
        execute_nnef_pfd_subscription_delete(sub_id, t, s);
      });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_nnef_pfd_subscription_delete(
    const std::string& sub_id, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_pfd_subscription_delete(sub_id, http_code);
    sink(http_code, "");
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Nnef_EventExposure
// ─────────────────────────────────────────────────────────────────────────────
nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_event_exposure_subscribe(
    const nlohmann::json& body, std::string token, response_sink sink) {
  return m_dispatcher.dispatch(
      [this, body, t = std::move(token), s = std::move(sink)]() mutable {
        execute_nnef_event_exposure_subscribe(body, t, s);
      });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_nnef_event_exposure_subscribe(
    const nlohmann::json& body, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    try {
      app.handle_nnef_event_exposure_subscribe(body, resp_body, http_code);
      sink(http_code, resp_body.dump());
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_unprocessable(e.what()).dump());
    }
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_event_exposure_unsubscribe(
    const std::string& subscription_id, std::string token, response_sink sink) {
  return m_dispatcher.dispatch([this, subscription_id, t = std::move(token),
                                s = std::move(sink)]() mutable {
    execute_nnef_event_exposure_unsubscribe(subscription_id, t, s);
  });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_nnef_event_exposure_unsubscribe(
    const std::string& subscription_id, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_event_exposure_unsubscribe(subscription_id, http_code);
    sink(http_code, "");
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_event_exposure_get(
    const std::string& subscription_id, std::string token, response_sink sink) {
  return m_dispatcher.dispatch([this, subscription_id, t = std::move(token),
                                s = std::move(sink)]() mutable {
    execute_nnef_event_exposure_get(subscription_id, t, s);
  });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_nnef_event_exposure_get(
    const std::string& subscription_id, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_event_exposure_get(subscription_id, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

//------------------------------------------------------------------------------
nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_event_exposure_update(
    const std::string& subscription_id, const nlohmann::json& body,
    std::string token, response_sink sink) {
  return m_dispatcher.dispatch([this, subscription_id, body,
                                t = std::move(token),
                                s = std::move(sink)]() mutable {
    execute_nnef_event_exposure_update(subscription_id, body, t, s);
  });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_nnef_event_exposure_update(
    const std::string& subscription_id, const nlohmann::json& body,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    try {
      app.handle_nnef_event_exposure_update(
          subscription_id, body, resp_body, http_code);
      sink(http_code, resp_body.dump());
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_unprocessable(e.what()).dump());
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Inbound NF notification (bool return → 204/404)
// ─────────────────────────────────────────────────────────────────────────────
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_nf_notification(
    const std::string& nf_sub_id, const nlohmann::json& body, std::string token,
    response_sink sink) {
  return m_dispatcher.dispatch([this, nf_sub_id, body, t = std::move(token),
                                s = std::move(sink)]() mutable {
    execute_nf_notification(nf_sub_id, body, t, s);
  });
}

//------------------------------------------------------------------------------
void nef_app_adapter::execute_nf_notification(
    const std::string& nf_sub_id, const nlohmann::json& body,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    const bool found = app.handle_nf_notification(nf_sub_id, body);
    sink(
        found ? http_status_code::NO_CONTENT : http_status_code::NOT_FOUND, "");
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Async dispatch variants
//
// Each builds a response_sink around the deferred response handle and hands
// it to a nef_app entry method, which then runs on a dispatcher worker. By
// the time the answer arrives, the HTTP worker that called
// dispatch_*_async() has long since returned.
//
// response_sink is a std::function and so must be copyable, while the
// deferred handle is move-only — hence the shared_ptr wrapper.
//
// A rejected dispatch returns false, after sending an explicit 503 through
// the handle on the calling thread. Without that the handle's destructor
// would post its generic fallback 500 instead.
// ─────────────────────────────────────────────────────────────────────────────
namespace {
//------------------------------------------------------------------------------
// Wrap a move-only deferred response in a copyable response_sink that
// delivers JSON-bodied responses.
//
// Success (2xx/3xx) bodies are application/json. Error (>=400) bodies are
// ProblemDetails, so they carry application/problem+json per RFC 7807 /
// 3GPP TS 29.122 §5.2.4.
response_sink make_deferred_json_sink(
    std::shared_ptr<http2_deferred_response> dr) {
  return [dr = std::move(dr)](int code, std::string body) mutable {
    const char* content_type =
        (code >= 400) ? "application/problem+json" : "application/json";
    dr->send(code, {{"content-type", content_type}}, std::move(body));
  };
}

//------------------------------------------------------------------------------
// Same, for a response with an empty body and no content-type header. Used by
// the DELETE-style handlers, whose 204 carries no body.
//
// The `body` the continuation supplies is discarded on purpose: a
// continuation written against the generic sink contract still ends up
// producing a bare status here.
response_sink make_deferred_empty_sink(
    std::shared_ptr<http2_deferred_response> dr) {
  return [dr = std::move(dr)](int code, std::string /*body*/) mutable {
    dr->send(code, {}, "");
  };
}

//------------------------------------------------------------------------------
// As make_deferred_empty_sink, but carrying a fixed header map — no
// content-type unless the caller put one in `headers`.
//
// Mirrors dispatch_and_wait_empty's header-carrying form. Used by BDT delete,
// to keep the x-deprecated legacy-path header on the 204.
response_sink make_deferred_empty_sink_h(
    std::shared_ptr<http2_deferred_response> dr,
    std::map<std::string, std::string> headers) {
  return [dr = std::move(dr), headers = std::move(headers)](
             int code, std::string /*body*/) mutable {
    dr->send(code, headers, "");
  };
}

//------------------------------------------------------------------------------
// Same, for handlers whose response headers — INCLUDING the per-branch
// content-type — depend on the (status, body) produced.
//
// header_fn receives the status code and a MUTABLE parsed JSON body, and
// returns the COMPLETE header map. Mutable, because header_fn may rewrite
// fields, such as a relative `self` into an absolute URI; the body is
// re-serialized after header_fn runs. header_fn OWNS all headers — this
// wrapper hard-codes none, not even the content-type, so header_fn is also
// where application/json vs application/problem+json is chosen per branch.
//
// Used for the BDT create Location header. It is the generic form of the
// hand-rolled QoS-create header sink in dispatch_qos_create_async.
template<typename HeaderFn>
response_sink make_deferred_header_sink(
    std::shared_ptr<http2_deferred_response> dr, HeaderFn header_fn) {
  return [dr = std::move(dr), header_fn = std::move(header_fn)](
             int code, std::string body) mutable {
    nlohmann::json resp_body;
    if (!body.empty()) {
      try {
        resp_body = nlohmann::json::parse(body);
      } catch (...) {
        resp_body = nlohmann::json::object();
      }
    }
    std::map<std::string, std::string> headers = header_fn(code, resp_body);
    dr->send(code, headers, resp_body.dump());
  };
}

//------------------------------------------------------------------------------
// Send a 503 ProblemDetails through the deferred handle. Used when the
// dispatcher rejected the task (queue_full/stopped).
//
// Without this the handle destructor would post a generic 500, and 503 is the
// accurate answer for an overloaded server.
void send_deferred_503(http2_deferred_response& dr) {
  nlohmann::json pd;
  pd["type"]   = "about:blank";
  pd["title"]  = "Service Unavailable";
  pd["status"] = http_status_code::SERVICE_UNAVAILABLE;
  pd["detail"] = "Server is overloaded, please retry later";
  dr.send(
      http_status_code::SERVICE_UNAVAILABLE,
      {{"content-type", "application/problem+json"}}, pd.dump());
}
}  // namespace

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_monitoring_event_subscribe_async(
    const std::string& scs_as_id, const nlohmann::json& body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  const auto st =
      m_dispatcher.dispatch([this, scs_as_id, body, t = std::move(token),
                             s = std::move(sink)]() mutable {
        // Fire and return; the continuation completes the deferred.
        m_app->monitoring_event_subscribe(scs_as_id, body, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_qos_create_async(
    const std::string& af_id, const nlohmann::json& body, std::string token,
    const std::string& server_address, http2_deferred_response deferred) {
  auto dr = std::make_shared<http2_deferred_response>(std::move(deferred));
  // QoS create needs a Location header, and an absolute self URI, on 201.
  // The header logic is repeated from the server shim so that it runs
  // wherever the response is produced.
  response_sink sink = [dr, server_address](
                           int code, std::string body) mutable {
    nlohmann::json resp_body;
    if (!body.empty()) {
      try {
        resp_body = nlohmann::json::parse(body);
      } catch (...) {
        resp_body = nlohmann::json::object();
      }
    }
    std::map<std::string, std::string> h;
    h["content-type"] = "application/problem+json";
    if (code == http_status_code::CREATED && resp_body.contains("self") &&
        resp_body["self"].is_string()) {
      const std::string loc =
          server_address + resp_body["self"].get<std::string>();
      resp_body["self"] = loc;
      h["location"]     = loc;
      h["content-type"] = "application/json";
    }
    dr->send(code, h, resp_body.dump());
  };
  const auto st = m_dispatcher.dispatch(
      [this, af_id, body, t = std::move(token), s = std::move(sink)]() mutable {
        m_app->qos_create(af_id, body, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_ti_create_async(
    const std::string& af_id, const nlohmann::json& body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  const auto st = m_dispatcher.dispatch(
      [this, af_id, body, t = std::move(token), s = std::move(sink)]() mutable {
        m_app->ti_create(af_id, body, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_ti_update_async(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  const auto st =
      m_dispatcher.dispatch([this, af_id, ti_id, body, t = std::move(token),
                             s = std::move(sink)]() mutable {
        m_app->ti_update(af_id, ti_id, body, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_ti_patch_async(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& patch_body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  const auto st = m_dispatcher.dispatch([this, af_id, ti_id, patch_body,
                                         t = std::move(token),
                                         s = std::move(sink)]() mutable {
    m_app->ti_patch(af_id, ti_id, patch_body, t, std::move(s));
  });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_pfd_app_put_async(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const nlohmann::json& body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  const auto st = m_dispatcher.dispatch([this, scs_as_id, trans_id, app_id,
                                         body, t = std::move(token),
                                         s = std::move(sink)]() mutable {
    m_app->pfd_app_put(scs_as_id, trans_id, app_id, body, t, std::move(s));
  });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_monitoring_event_unsubscribe_async(
    const std::string& scs_as_id, const std::string& sub_id, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_empty_sink(dr);
  const auto st =
      m_dispatcher.dispatch([this, scs_as_id, sub_id, t = std::move(token),
                             s = std::move(sink)]() mutable {
        m_app->monitoring_event_unsubscribe(scs_as_id, sub_id, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_qos_update_async(
    const std::string& af_id, const std::string& sub_id,
    const nlohmann::json& body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  const auto st =
      m_dispatcher.dispatch([this, af_id, sub_id, body, t = std::move(token),
                             s = std::move(sink)]() mutable {
        m_app->qos_update(af_id, sub_id, body, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_qos_patch_async(
    const std::string& af_id, const std::string& sub_id,
    const nlohmann::json& patch_body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  const auto st = m_dispatcher.dispatch([this, af_id, sub_id, patch_body,
                                         t = std::move(token),
                                         s = std::move(sink)]() mutable {
    m_app->qos_patch(af_id, sub_id, patch_body, t, std::move(s));
  });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_qos_delete_async(
    const std::string& af_id, const std::string& sub_id, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_empty_sink(dr);
  const auto st =
      m_dispatcher.dispatch([this, af_id, sub_id, t = std::move(token),
                             s = std::move(sink)]() mutable {
        m_app->qos_delete(af_id, sub_id, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_bdt_create_async(
    const std::string& af_id, const nlohmann::json& body, std::string token,
    bool deprecated, http2_deferred_response deferred) {
  auto dr = std::make_shared<http2_deferred_response>(std::move(deferred));
  // Always application/json, plus the x-deprecated marker on legacy paths —
  // matching handle_bdt_create's header sink.
  auto sink = make_deferred_header_sink(
      dr, [deprecated](int /*code*/, nlohmann::json& /*resp_body*/) {
        std::map<std::string, std::string> h;
        h["content-type"] = "application/json";
        if (deprecated) h["x-deprecated"] = "true";
        return h;
      });
  const auto st = m_dispatcher.dispatch(
      [this, af_id, body, t = std::move(token), s = std::move(sink)]() mutable {
        m_app->bdt_create(af_id, body, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_bdt_update_async(
    const std::string& af_id, const std::string& bdt_id,
    const nlohmann::json& body, std::string token, bool deprecated,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_header_sink(
      dr, [deprecated](int /*code*/, nlohmann::json& /*resp_body*/) {
        std::map<std::string, std::string> h;
        h["content-type"] = "application/json";
        if (deprecated) h["x-deprecated"] = "true";
        return h;
      });
  const auto st =
      m_dispatcher.dispatch([this, af_id, bdt_id, body, t = std::move(token),
                             s = std::move(sink)]() mutable {
        m_app->bdt_update(af_id, bdt_id, body, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_bdt_patch_async(
    const std::string& af_id, const std::string& bdt_id,
    const nlohmann::json& patch_body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  const auto st = m_dispatcher.dispatch([this, af_id, bdt_id, patch_body,
                                         t = std::move(token),
                                         s = std::move(sink)]() mutable {
    m_app->bdt_patch(af_id, bdt_id, patch_body, t, std::move(s));
  });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_bdt_delete_async(
    const std::string& af_id, const std::string& bdt_id, std::string token,
    bool deprecated, http2_deferred_response deferred) {
  auto dr = std::make_shared<http2_deferred_response>(std::move(deferred));
  std::map<std::string, std::string> headers;
  if (deprecated) headers["x-deprecated"] = "true";
  auto sink = make_deferred_empty_sink_h(dr, std::move(headers));
  const auto st =
      m_dispatcher.dispatch([this, af_id, bdt_id, t = std::move(token),
                             s = std::move(sink)]() mutable {
        m_app->bdt_delete(af_id, bdt_id, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_pfd_app_patch_async(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const nlohmann::json& patch_body,
    std::string token, http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  const auto st = m_dispatcher.dispatch([this, scs_as_id, trans_id, app_id,
                                         patch_body, t = std::move(token),
                                         s = std::move(sink)]() mutable {
    m_app->pfd_app_patch(
        scs_as_id, trans_id, app_id, patch_body, t, std::move(s));
  });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_pfd_app_delete_async(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_empty_sink(dr);
  const auto st = m_dispatcher.dispatch([this, scs_as_id, trans_id, app_id,
                                         t = std::move(token),
                                         s = std::move(sink)]() mutable {
    m_app->pfd_app_delete(scs_as_id, trans_id, app_id, t, std::move(s));
  });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

// Nnef-PFD
//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_nnef_pfd_put_app_async(
    const std::string& trans_id, const std::string& app_id,
    const nlohmann::json& body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  const auto st =
      m_dispatcher.dispatch([this, trans_id, app_id, body, t = std::move(token),
                             s = std::move(sink)]() mutable {
        m_app->nnef_pfd_put_app(trans_id, app_id, body, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_nnef_pfd_delete_app_async(
    const std::string& trans_id, const std::string& app_id, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_empty_sink(dr);
  const auto st =
      m_dispatcher.dispatch([this, trans_id, app_id, t = std::move(token),
                             s = std::move(sink)]() mutable {
        m_app->nnef_pfd_delete_app(trans_id, app_id, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_pfd_transaction_put_async(
    const std::string& scs_as_id, const std::string& trans_id,
    const nlohmann::json& body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  const auto st = m_dispatcher.dispatch([this, scs_as_id, trans_id, body,
                                         t = std::move(token),
                                         s = std::move(sink)]() mutable {
    m_app->pfd_transaction_put(scs_as_id, trans_id, body, t, std::move(s));
  });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_pfd_transaction_delete_async(
    const std::string& scs_as_id, const std::string& trans_id,
    std::string token, http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_empty_sink(dr);
  const auto st =
      m_dispatcher.dispatch([this, scs_as_id, trans_id, t = std::move(token),
                             s = std::move(sink)]() mutable {
        m_app->pfd_transaction_delete(scs_as_id, trans_id, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_ti_delete_async(
    const std::string& af_id, const std::string& ti_id, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_empty_sink(dr);
  const auto st =
      m_dispatcher.dispatch([this, af_id, ti_id, t = std::move(token),
                             s = std::move(sink)]() mutable {
        m_app->ti_delete(af_id, ti_id, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_nnef_pfd_put_transaction_async(
    const std::string& trans_id, const nlohmann::json& body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  const auto st =
      m_dispatcher.dispatch([this, trans_id, body, t = std::move(token),
                             s = std::move(sink)]() mutable {
        m_app->nnef_pfd_put_transaction(trans_id, body, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_nnef_pfd_delete_transaction_async(
    const std::string& trans_id, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_empty_sink(dr);
  const auto st = m_dispatcher.dispatch(
      [this, trans_id, t = std::move(token), s = std::move(sink)]() mutable {
        m_app->nnef_pfd_delete_transaction(trans_id, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app_adapter::dispatch_nnef_pfd_partial_pull_async(
    const nlohmann::json& body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  const auto st = m_dispatcher.dispatch(
      [this, body, t = std::move(token), s = std::move(sink)]() mutable {
        m_app->nnef_pfd_partial_pull(body, t, std::move(s));
      });
  if (st != dispatch_status::ok) {
    send_deferred_503(*dr);
    return false;
  }
  return true;
}

}  // namespace oai::nef::app
