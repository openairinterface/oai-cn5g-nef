/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_app_adapter.hpp"

#include <nlohmann/json.hpp>

#include <utility>

#include <memory>

#include "3gpp_29.500.h"
#include "Helpers.h"
#include "http2-server.h"  // full http2_deferred_response definition
#include "nef_app.hpp"

using oai::common::sbi::http_status_code;

namespace oai::nef::app {

//------------------------------------------------------------------------------
nef_app_adapter::nef_app_adapter(
    nef_app* app, bool async, std::size_t n_threads,
    std::size_t http_worker_count, std::size_t max_queue)
    : m_app(app),
      m_async(async),
      m_dispatcher(n_threads, http_worker_count, max_queue) {}

void nef_app_adapter::stop() {
  m_dispatcher.stop();
}

std::size_t nef_app_adapter::queue_depth() const {
  return m_dispatcher.queue_depth();
}

//------------------------------------------------------------------------------
// Out-of-line definition: sees the complete nef_app type (nef_app.hpp above).
template<typename Fn>
void nef_app_adapter::execute_with_token(const std::string& token, Fn&& fn) {
  m_app->set_request_bearer_token(token);
  std::forward<Fn>(fn)(*m_app);
  m_app->clear_request_bearer_token();
}

namespace {
// Shared helper: build the 422 ProblemDetails body produced when a nef_app
// handler throws ValidationException.
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
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, ti_id, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_ti_get(af_id, ti_id, t, s);
    });
  }
  execute_ti_get(af_id, ti_id, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_ti_list(
    const std::string& af_id, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch(
        [this, af_id, t = std::move(token), s = std::move(sink)]() mutable {
          execute_ti_list(af_id, t, s);
        });
  }
  execute_ti_list(af_id, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_ti_create(
    const std::string& af_id, const nlohmann::json& body, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, body, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_ti_create(af_id, body, t, s);
    });
  }
  execute_ti_create(af_id, body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_ti_create(
    const std::string& af_id, const nlohmann::json& body,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    std::string ti_id;
    try {
      app.handle_traffic_influence_create(
          af_id, body, ti_id, resp_body, http_code);
      sink(http_code, resp_body.dump());
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_unprocessable(e.what()).dump());
    }
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_ti_update(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& body, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, ti_id, body,
                                  t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_ti_update(af_id, ti_id, body, t, s);
    });
  }
  execute_ti_update(af_id, ti_id, body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_ti_update(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& body, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    try {
      app.handle_traffic_influence_update(
          af_id, ti_id, body, resp_body, http_code);
      sink(http_code, resp_body.dump());
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_unprocessable(e.what()).dump());
    }
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_ti_delete(
    const std::string& af_id, const std::string& ti_id, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, ti_id, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_ti_delete(af_id, ti_id, t, s);
    });
  }
  execute_ti_delete(af_id, ti_id, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_ti_delete(
    const std::string& af_id, const std::string& ti_id,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_traffic_influence_delete(af_id, ti_id, http_code);
    sink(http_code, "");
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_ti_patch(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& patch_body, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, ti_id, patch_body,
                                  t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_ti_patch(af_id, ti_id, patch_body, t, s);
    });
  }
  execute_ti_patch(af_id, ti_id, patch_body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_ti_patch(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& patch_body, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    try {
      app.handle_traffic_influence_patch(
          af_id, ti_id, patch_body, resp_body, http_code);
      sink(http_code, resp_body.dump());
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_unprocessable(e.what()).dump());
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Monitoring Event
// ─────────────────────────────────────────────────────────────────────────────
nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_monitoring_event_subscribe(
    const std::string& scs_as_id, const nlohmann::json& body, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, scs_as_id, body, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_monitoring_event_subscribe(scs_as_id, body, t, s);
    });
  }
  execute_monitoring_event_subscribe(scs_as_id, body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_monitoring_event_subscribe(
    const std::string& scs_as_id, const nlohmann::json& body,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    std::string sub_id;
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_monitoring_event_subscription_create(
        scs_as_id, body, sub_id, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_monitoring_event_unsubscribe(
    const std::string& scs_as_id, const std::string& sub_id, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, scs_as_id, sub_id, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_monitoring_event_unsubscribe(scs_as_id, sub_id, t, s);
    });
  }
  execute_monitoring_event_unsubscribe(scs_as_id, sub_id, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_monitoring_event_unsubscribe(
    const std::string& scs_as_id, const std::string& sub_id,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_monitoring_event_subscription_delete(
        scs_as_id, sub_id, http_code);
    sink(http_code, "");
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_monitoring_event_get(
    const std::string& scs_as_id, const std::string& sub_id, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, scs_as_id, sub_id, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_monitoring_event_get(scs_as_id, sub_id, t, s);
    });
  }
  execute_monitoring_event_get(scs_as_id, sub_id, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_monitoring_event_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& body, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, scs_as_id, sub_id, body,
                                  t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_monitoring_event_update(scs_as_id, sub_id, body, t, s);
    });
  }
  execute_monitoring_event_update(scs_as_id, sub_id, body, token, sink);
  return dispatch_status::ok;
}
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

// ─────────────────────────────────────────────────────────────────────────────
// QoS
// ─────────────────────────────────────────────────────────────────────────────
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_qos_create(
    const std::string& af_id, const nlohmann::json& body, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, body, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_qos_create(af_id, body, t, s);
    });
  }
  execute_qos_create(af_id, body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_qos_create(
    const std::string& af_id, const nlohmann::json& body,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    std::string sub_id;
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    try {
      app.handle_qos_subscription_create(
          af_id, body, sub_id, resp_body, http_code);
      sink(http_code, resp_body.dump());
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_unprocessable(e.what()).dump());
    }
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_qos_delete(
    const std::string& af_id, const std::string& sub_id, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, sub_id, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_qos_delete(af_id, sub_id, t, s);
    });
  }
  execute_qos_delete(af_id, sub_id, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_qos_delete(
    const std::string& af_id, const std::string& sub_id,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_qos_subscription_delete(af_id, sub_id, http_code);
    sink(http_code, "");
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_qos_get(
    const std::string& af_id, const std::string& sub_id, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, sub_id, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_qos_get(af_id, sub_id, t, s);
    });
  }
  execute_qos_get(af_id, sub_id, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_qos_update(
    const std::string& af_id, const std::string& sub_id,
    const nlohmann::json& body, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, sub_id, body,
                                  t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_qos_update(af_id, sub_id, body, t, s);
    });
  }
  execute_qos_update(af_id, sub_id, body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_qos_update(
    const std::string& af_id, const std::string& sub_id,
    const nlohmann::json& body, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    try {
      app.handle_qos_subscription_update(
          af_id, sub_id, body, resp_body, http_code);
      sink(http_code, resp_body.dump());
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_unprocessable(e.what()).dump());
    }
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_qos_patch(
    const std::string& af_id, const std::string& sub_id,
    const nlohmann::json& patch_body, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, sub_id, patch_body,
                                  t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_qos_patch(af_id, sub_id, patch_body, t, s);
    });
  }
  execute_qos_patch(af_id, sub_id, patch_body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_qos_patch(
    const std::string& af_id, const std::string& sub_id,
    const nlohmann::json& patch_body, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    try {
      app.handle_qos_subscription_patch(
          af_id, sub_id, patch_body, resp_body, http_code);
      sink(http_code, resp_body.dump());
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_unprocessable(e.what()).dump());
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// BDT
// ─────────────────────────────────────────────────────────────────────────────
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_bdt_create(
    const std::string& af_id, const nlohmann::json& body, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, body, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_bdt_create(af_id, body, t, s);
    });
  }
  execute_bdt_create(af_id, body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_bdt_create(
    const std::string& af_id, const nlohmann::json& body,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    std::string bdt_id;
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    try {
      app.handle_bdt_policy_create(af_id, body, bdt_id, resp_body, http_code);
      sink(http_code, resp_body.dump());
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_unprocessable(e.what()).dump());
    }
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_bdt_update(
    const std::string& af_id, const std::string& bdt_id,
    const nlohmann::json& body, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, bdt_id, body,
                                  t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_bdt_update(af_id, bdt_id, body, t, s);
    });
  }
  execute_bdt_update(af_id, bdt_id, body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_bdt_update(
    const std::string& af_id, const std::string& bdt_id,
    const nlohmann::json& body, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    try {
      app.handle_bdt_policy_update(af_id, bdt_id, body, resp_body, http_code);
      sink(http_code, resp_body.dump());
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_unprocessable(e.what()).dump());
    }
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_bdt_delete(
    const std::string& af_id, const std::string& bdt_id, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, bdt_id, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_bdt_delete(af_id, bdt_id, t, s);
    });
  }
  execute_bdt_delete(af_id, bdt_id, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_bdt_delete(
    const std::string& af_id, const std::string& bdt_id,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_bdt_policy_delete(af_id, bdt_id, http_code);
    sink(http_code, "");
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_bdt_get(
    const std::string& af_id, const std::string& bdt_id, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, bdt_id, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_bdt_get(af_id, bdt_id, t, s);
    });
  }
  execute_bdt_get(af_id, bdt_id, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_bdt_patch(
    const std::string& af_id, const std::string& bdt_id,
    const nlohmann::json& patch_body, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, bdt_id, patch_body,
                                  t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_bdt_patch(af_id, bdt_id, patch_body, t, s);
    });
  }
  execute_bdt_patch(af_id, bdt_id, patch_body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_bdt_patch(
    const std::string& af_id, const std::string& bdt_id,
    const nlohmann::json& patch_body, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    try {
      app.handle_bdt_policy_patch(
          af_id, bdt_id, patch_body, resp_body, http_code);
      sink(http_code, resp_body.dump());
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_unprocessable(e.what()).dump());
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Analytics
// ─────────────────────────────────────────────────────────────────────────────
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_analytics_create(
    const std::string& af_id, const nlohmann::json& body, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, body, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_analytics_create(af_id, body, t, s);
    });
  }
  execute_analytics_create(af_id, body, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_analytics_delete(
    const std::string& af_id, const std::string& sub_id, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, sub_id, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_analytics_delete(af_id, sub_id, t, s);
    });
  }
  execute_analytics_delete(af_id, sub_id, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_analytics_delete(
    const std::string& af_id, const std::string& sub_id,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_analytics_subscription_delete(af_id, sub_id, http_code);
    sink(http_code, "");
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_analytics_get(
    const std::string& af_id, const std::string& sub_id, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, sub_id, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_analytics_get(af_id, sub_id, t, s);
    });
  }
  execute_analytics_get(af_id, sub_id, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_analytics_update(
    const std::string& af_id, const std::string& sub_id,
    const nlohmann::json& body, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, sub_id, body,
                                  t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_analytics_update(af_id, sub_id, body, t, s);
    });
  }
  execute_analytics_update(af_id, sub_id, body, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_analytics_fetch(
    const std::string& af_id, const nlohmann::json& body, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, af_id, body, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_analytics_fetch(af_id, body, t, s);
    });
  }
  execute_analytics_fetch(af_id, body, token, sink);
  return dispatch_status::ok;
}
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

// ─────────────────────────────────────────────────────────────────────────────
// PFD (T8)
// ─────────────────────────────────────────────────────────────────────────────
nef_app_adapter::dispatch_status nef_app_adapter::dispatch_pfd_create(
    const std::string& app_id, const nlohmann::json& body, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, app_id, body, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_pfd_create(app_id, body, t, s);
    });
  }
  execute_pfd_create(app_id, body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_pfd_create(
    const std::string& app_id, const nlohmann::json& body,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_pfd_create(app_id, body, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_pfd_delete(
    const std::string& app_id, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch(
        [this, app_id, t = std::move(token), s = std::move(sink)]() mutable {
          execute_pfd_delete(app_id, t, s);
        });
  }
  execute_pfd_delete(app_id, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_pfd_delete(
    const std::string& app_id, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_pfd_delete(app_id, http_code);
    sink(http_code, "");
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_pfd_transaction_list(
    const std::string& scs_as_id, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch(
        [this, scs_as_id, t = std::move(token), s = std::move(sink)]() mutable {
          execute_pfd_transaction_list(scs_as_id, t, s);
        });
  }
  execute_pfd_transaction_list(scs_as_id, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_pfd_transaction_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const nlohmann::json& body, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, scs_as_id, trans_id, body,
                                  t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_pfd_transaction_put(scs_as_id, trans_id, body, t, s);
    });
  }
  execute_pfd_transaction_put(scs_as_id, trans_id, body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_pfd_transaction_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const nlohmann::json& body, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_pfd_transaction_put(
        scs_as_id, trans_id, body, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_pfd_transaction_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, scs_as_id, trans_id,
                                  t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_pfd_transaction_delete(scs_as_id, trans_id, t, s);
    });
  }
  execute_pfd_transaction_delete(scs_as_id, trans_id, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_pfd_transaction_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_pfd_transaction_delete(scs_as_id, trans_id, http_code);
    sink(http_code, "");
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_pfd_app_get(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, scs_as_id, trans_id, app_id,
                                  t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_pfd_app_get(scs_as_id, trans_id, app_id, t, s);
    });
  }
  execute_pfd_app_get(scs_as_id, trans_id, app_id, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_pfd_app_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const nlohmann::json& body, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, scs_as_id, trans_id, app_id, body,
                                  t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_pfd_app_put(scs_as_id, trans_id, app_id, body, t, s);
    });
  }
  execute_pfd_app_put(scs_as_id, trans_id, app_id, body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_pfd_app_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const nlohmann::json& body,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_pfd_app_put(
        scs_as_id, trans_id, app_id, body, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_pfd_app_patch(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const nlohmann::json& patch_body,
    std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, scs_as_id, trans_id, app_id, patch_body,
                                  t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_pfd_app_patch(scs_as_id, trans_id, app_id, patch_body, t, s);
    });
  }
  execute_pfd_app_patch(scs_as_id, trans_id, app_id, patch_body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_pfd_app_patch(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const nlohmann::json& patch_body,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_pfd_app_patch(
        scs_as_id, trans_id, app_id, patch_body, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_pfd_app_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, scs_as_id, trans_id, app_id,
                                  t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_pfd_app_delete(scs_as_id, trans_id, app_id, t, s);
    });
  }
  execute_pfd_app_delete(scs_as_id, trans_id, app_id, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_pfd_app_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_pfd_app_delete(scs_as_id, trans_id, app_id, http_code);
    sink(http_code, "");
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// Nnef_PFDmanagement
// ─────────────────────────────────────────────────────────────────────────────
nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_list_transactions(
    std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch(
        [this, t = std::move(token), s = std::move(sink)]() mutable {
          execute_nnef_pfd_list_transactions(t, s);
        });
  }
  execute_nnef_pfd_list_transactions(token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_nnef_pfd_list_transactions(
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_pfd_list_transactions(resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_put_transaction(
    const std::string& trans_id, const nlohmann::json& body, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, trans_id, body, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_nnef_pfd_put_transaction(trans_id, body, t, s);
    });
  }
  execute_nnef_pfd_put_transaction(trans_id, body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_nnef_pfd_put_transaction(
    const std::string& trans_id, const nlohmann::json& body,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_pfd_put_transaction(trans_id, body, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_get_transaction(
    const std::string& trans_id, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch(
        [this, trans_id, t = std::move(token), s = std::move(sink)]() mutable {
          execute_nnef_pfd_get_transaction(trans_id, t, s);
        });
  }
  execute_nnef_pfd_get_transaction(trans_id, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_delete_transaction(
    const std::string& trans_id, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch(
        [this, trans_id, t = std::move(token), s = std::move(sink)]() mutable {
          execute_nnef_pfd_delete_transaction(trans_id, t, s);
        });
  }
  execute_nnef_pfd_delete_transaction(trans_id, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_nnef_pfd_delete_transaction(
    const std::string& trans_id, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_pfd_delete_transaction(trans_id, http_code);
    sink(http_code, "");
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_nnef_pfd_get_app(
    const std::string& trans_id, const std::string& app_id, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, trans_id, app_id, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_nnef_pfd_get_app(trans_id, app_id, t, s);
    });
  }
  execute_nnef_pfd_get_app(trans_id, app_id, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_nnef_pfd_put_app(
    const std::string& trans_id, const std::string& app_id,
    const nlohmann::json& body, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, trans_id, app_id, body,
                                  t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_nnef_pfd_put_app(trans_id, app_id, body, t, s);
    });
  }
  execute_nnef_pfd_put_app(trans_id, app_id, body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_nnef_pfd_put_app(
    const std::string& trans_id, const std::string& app_id,
    const nlohmann::json& body, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_pfd_put_app(trans_id, app_id, body, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

nef_app_adapter::dispatch_status nef_app_adapter::dispatch_nnef_pfd_delete_app(
    const std::string& trans_id, const std::string& app_id, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, trans_id, app_id, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_nnef_pfd_delete_app(trans_id, app_id, t, s);
    });
  }
  execute_nnef_pfd_delete_app(trans_id, app_id, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_nnef_pfd_delete_app(
    const std::string& trans_id, const std::string& app_id,
    const std::string& token, const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_pfd_delete_app(trans_id, app_id, http_code);
    sink(http_code, "");
  });
}

nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_get_applications(
    const std::vector<std::string>& app_ids_filter, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, app_ids_filter, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_nnef_pfd_get_applications(app_ids_filter, t, s);
    });
  }
  execute_nnef_pfd_get_applications(app_ids_filter, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_partial_pull(
    const nlohmann::json& body, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch(
        [this, body, t = std::move(token), s = std::move(sink)]() mutable {
          execute_nnef_pfd_partial_pull(body, t, s);
        });
  }
  execute_nnef_pfd_partial_pull(body, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_nnef_pfd_partial_pull(
    const nlohmann::json& body, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    nlohmann::json resp_body;
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_pfd_partial_pull(body, resp_body, http_code);
    sink(http_code, resp_body.dump());
  });
}

nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_subscription_create(
    const nlohmann::json& body, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch(
        [this, body, t = std::move(token), s = std::move(sink)]() mutable {
          execute_nnef_pfd_subscription_create(body, t, s);
        });
  }
  execute_nnef_pfd_subscription_create(body, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_subscription_get(
    const std::string& sub_id, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch(
        [this, sub_id, t = std::move(token), s = std::move(sink)]() mutable {
          execute_nnef_pfd_subscription_get(sub_id, t, s);
        });
  }
  execute_nnef_pfd_subscription_get(sub_id, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_subscription_put(
    const std::string& sub_id, const nlohmann::json& body, std::string token,
    response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, sub_id, body, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_nnef_pfd_subscription_put(sub_id, body, t, s);
    });
  }
  execute_nnef_pfd_subscription_put(sub_id, body, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_pfd_subscription_delete(
    const std::string& sub_id, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch(
        [this, sub_id, t = std::move(token), s = std::move(sink)]() mutable {
          execute_nnef_pfd_subscription_delete(sub_id, t, s);
        });
  }
  execute_nnef_pfd_subscription_delete(sub_id, token, sink);
  return dispatch_status::ok;
}
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
  if (m_async) {
    return m_dispatcher.dispatch(
        [this, body, t = std::move(token), s = std::move(sink)]() mutable {
          execute_nnef_event_exposure_subscribe(body, t, s);
        });
  }
  execute_nnef_event_exposure_subscribe(body, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_event_exposure_unsubscribe(
    const std::string& subscription_id, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, subscription_id, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_nnef_event_exposure_unsubscribe(subscription_id, t, s);
    });
  }
  execute_nnef_event_exposure_unsubscribe(subscription_id, token, sink);
  return dispatch_status::ok;
}
void nef_app_adapter::execute_nnef_event_exposure_unsubscribe(
    const std::string& subscription_id, const std::string& token,
    const response_sink& sink) {
  execute_with_token(token, [&](nef_app& app) {
    int http_code = http_status_code::NO_RESPONSE;
    app.handle_nnef_event_exposure_unsubscribe(subscription_id, http_code);
    sink(http_code, "");
  });
}

nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_event_exposure_get(
    const std::string& subscription_id, std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, subscription_id, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_nnef_event_exposure_get(subscription_id, t, s);
    });
  }
  execute_nnef_event_exposure_get(subscription_id, token, sink);
  return dispatch_status::ok;
}
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

nef_app_adapter::dispatch_status
nef_app_adapter::dispatch_nnef_event_exposure_update(
    const std::string& subscription_id, const nlohmann::json& body,
    std::string token, response_sink sink) {
  if (m_async) {
    return m_dispatcher.dispatch([this, subscription_id, body,
                                  t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_nnef_event_exposure_update(subscription_id, body, t, s);
    });
  }
  execute_nnef_event_exposure_update(subscription_id, body, token, sink);
  return dispatch_status::ok;
}
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
  if (m_async) {
    return m_dispatcher.dispatch([this, nf_sub_id, body, t = std::move(token),
                                  s = std::move(sink)]() mutable {
      execute_nf_notification(nf_sub_id, body, t, s);
    });
  }
  execute_nf_notification(nf_sub_id, body, token, sink);
  return dispatch_status::ok;
}
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
// Each builds a response_sink that owns the moved-in http2_deferred_response
// (wrapped in a shared_ptr because response_sink is a std::function and the
// handle is move-only) and delivers (status, headers, body) through it. The
// existing execute_* handler runs on the dispatcher worker (async mode) or
// inline (sync mode). The HTTP worker that called dispatch_*_async() has
// already returned. Returns false if the dispatch was rejected; in that case
// the handle's destructor (running here, on the calling thread) posts a 500.
// ─────────────────────────────────────────────────────────────────────────────
namespace {
// Wrap a move-only deferred response in a copyable response_sink that delivers
// JSON-bodied responses (content-type: application/json).
response_sink make_deferred_json_sink(
    std::shared_ptr<http2_deferred_response> dr) {
  return [dr = std::move(dr)](int code, std::string body) mutable {
    dr->send(code, {{"content-type", "application/json"}}, std::move(body));
  };
}

// Send a 503 ProblemDetails through the deferred handle (used when the
// dispatcher rejected the task — queue_full/stopped). Without this the handle
// destructor would post a generic 500; an explicit 503 is more accurate for an
// overloaded server.
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

bool nef_app_adapter::dispatch_monitoring_event_subscribe_async(
    const std::string& scs_as_id, const nlohmann::json& body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  if (m_async) {
    const auto st =
        m_dispatcher.dispatch([this, scs_as_id, body, t = std::move(token),
                               s = std::move(sink)]() mutable {
          execute_monitoring_event_subscribe(scs_as_id, body, t, s);
        });
    if (st != dispatch_status::ok) {
      send_deferred_503(*dr);
      return false;
    }
    return true;
  }
  execute_monitoring_event_subscribe(scs_as_id, body, token, sink);
  return true;
}

bool nef_app_adapter::dispatch_qos_create_async(
    const std::string& af_id, const nlohmann::json& body, std::string token,
    const std::string& server_address, http2_deferred_response deferred) {
  auto dr = std::make_shared<http2_deferred_response>(std::move(deferred));
  // QoS create needs a Location header (and an absolute self URI) on 201.
  // Replicate the header logic from the server shim here so it runs wherever
  // the response is produced.
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
  if (m_async) {
    const auto st =
        m_dispatcher.dispatch([this, af_id, body, t = std::move(token),
                               s = std::move(sink)]() mutable {
          execute_qos_create(af_id, body, t, s);
        });
    if (st != dispatch_status::ok) {
      send_deferred_503(*dr);
      return false;
    }
    return true;
  }
  execute_qos_create(af_id, body, token, sink);
  return true;
}

bool nef_app_adapter::dispatch_ti_create_async(
    const std::string& af_id, const nlohmann::json& body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  if (m_async) {
    const auto st =
        m_dispatcher.dispatch([this, af_id, body, t = std::move(token),
                               s = std::move(sink)]() mutable {
          execute_ti_create(af_id, body, t, s);
        });
    if (st != dispatch_status::ok) {
      send_deferred_503(*dr);
      return false;
    }
    return true;
  }
  execute_ti_create(af_id, body, token, sink);
  return true;
}

bool nef_app_adapter::dispatch_ti_update_async(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  if (m_async) {
    const auto st =
        m_dispatcher.dispatch([this, af_id, ti_id, body, t = std::move(token),
                               s = std::move(sink)]() mutable {
          execute_ti_update(af_id, ti_id, body, t, s);
        });
    if (st != dispatch_status::ok) {
      send_deferred_503(*dr);
      return false;
    }
    return true;
  }
  execute_ti_update(af_id, ti_id, body, token, sink);
  return true;
}

bool nef_app_adapter::dispatch_ti_patch_async(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& patch_body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  if (m_async) {
    const auto st = m_dispatcher.dispatch([this, af_id, ti_id, patch_body,
                                           t = std::move(token),
                                           s = std::move(sink)]() mutable {
      execute_ti_patch(af_id, ti_id, patch_body, t, s);
    });
    if (st != dispatch_status::ok) {
      send_deferred_503(*dr);
      return false;
    }
    return true;
  }
  execute_ti_patch(af_id, ti_id, patch_body, token, sink);
  return true;
}

bool nef_app_adapter::dispatch_pfd_app_put_async(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const nlohmann::json& body, std::string token,
    http2_deferred_response deferred) {
  auto dr   = std::make_shared<http2_deferred_response>(std::move(deferred));
  auto sink = make_deferred_json_sink(dr);
  if (m_async) {
    const auto st = m_dispatcher.dispatch([this, scs_as_id, trans_id, app_id,
                                           body, t = std::move(token),
                                           s = std::move(sink)]() mutable {
      execute_pfd_app_put(scs_as_id, trans_id, app_id, body, t, s);
    });
    if (st != dispatch_status::ok) {
      send_deferred_503(*dr);
      return false;
    }
    return true;
  }
  execute_pfd_app_put(scs_as_id, trans_id, app_id, body, token, sink);
  return true;
}

}  // namespace oai::nef::app
