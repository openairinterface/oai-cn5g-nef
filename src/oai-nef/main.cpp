/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.
 */

#include <cerrno>
#include <signal.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <thread>

#include "conversions.hpp"
#include "http_client.hpp"
#include "logger.hpp"
#include "nef-http2-server.h"
#include "nef_app.hpp"
#include "task_manager.hpp"
#include "nef_config.hpp"
#include "options.hpp"
#include "pid_file.hpp"
#include "sbi_helper.hpp"

using namespace oai::nef::app;
using namespace oai::utils;
using namespace oai::config::nef;
using oai::nef::api::nef_http2_server;

std::shared_ptr<nef_app> nef_app_inst                               = nullptr;
std::unique_ptr<nef_config> nef_config_inst                         = nullptr;
std::shared_ptr<oai::sba::http_client> http_client_inst             = nullptr;
std::unique_ptr<nef_http2_server> nef_api_server_2                  = nullptr;
std::unique_ptr<oai::sba::task_manager> tm_inst                     = nullptr;
std::unique_ptr<oai::config::lttng_configuration> lttng_config_yaml = nullptr;

static int shutdown_efd_g = -1;

//------------------------------------------------------------------------------
static void my_shutdown_signal_handler(int /*s*/) {
  const uint64_t one = 1;
  // NOLINTNEXTLINE: write() in signal handler is async-signal-safe
  ::write(shutdown_efd_g, &one, sizeof(one));
}

//------------------------------------------------------------------------------
int main(int argc, char** argv) {
  srand(time(NULL));

  if (!Options::parse(argc, argv)) {
    std::cout << "Options::parse() failed" << std::endl;
    return 1;
  }

  const std::string conf_file_name =
      static_cast<std::string>(Options::getlibconfigConfig());

  std::cout << "Reading configuration file: " << conf_file_name << "\n";
  lttng_config_yaml =
      std::make_unique<oai::config::lttng_configuration>(conf_file_name);
  lttng_config_yaml->read_from_file();

#ifdef LOGGER_CAN_USE_LTTNG
  std::cout << "LTTNG Log Activation: " << lttng_config_yaml->is_lttng_active()
            << "\n";
#else
  std::cout << "LTTNG Tracing disabled at build-time!\n";
#endif

  Logger::set_lttng(static_cast<bool>(lttng_config_yaml->is_lttng_active()));
  Logger::init("nef", Options::getlogStdout(), Options::getlogRotFilelog());
  // The HTTP/2 server lives in common-src and logs through the registry;
  // point its diagnostics at the NEF_APP category.
  oai::sba::set_http2_server_logger(NEF_APP);
  Logger::nef_app().startup("Options parsed");

  // eventfd gives the handler an async-signal-safe way to wake main().
  shutdown_efd_g = eventfd(0, EFD_CLOEXEC);
  if (shutdown_efd_g < 0) {
    Logger::nef_sbi().error("eventfd creation failed: errno=%d", errno);
    return EXIT_FAILURE;
  }

  // Install signal handlers (async-signal-safe: only write to eventfd)
  struct sigaction sa {};
  sa.sa_handler = my_shutdown_signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);

  // Configuration: YAML, via YAML::LoadFile inside nef_config::init().
  nef_config_inst = std::make_unique<nef_config>(
      Options::getlibconfigConfig(), Options::getlogStdout(),
      Options::getlogRotFilelog());
  if (!nef_config_inst->init()) {
    nef_config_inst->display();
    Logger::system().error("Reading the configuration failed. Exiting");
    return 1;
  }
  nef_config_inst->display();

  // HTTP Client
  http_client_inst = oai::sba::http_client::create_instance(
      Logger::nef_sbi(), oai::common::sbi::kNfDefaultHttpRequestTimeout,
      nef_config_inst->local().get_sbi().get_if_name(), 2);

  // Event subsystem: shared by nef_app (signals) and task_manager (tick).
  std::shared_ptr<nef_event> ev = std::make_shared<nef_event>();

  // NEF application layer
  nef_app_inst = std::make_shared<nef_app>(Options::getlibconfigConfig(), ev);

  // Task Manager
  tm_inst = std::make_unique<oai::sba::task_manager>(ev);
  std::thread task_manager_thread(&oai::sba::task_manager::run, tm_inst.get());

  // PID file
  std::string pid_file_name =
      oai::utils::get_exe_absolute_path("/var/run", nef_config_inst->instance);
  if (!oai::utils::is_pid_file_lock_success(pid_file_name.c_str())) {
    Logger::nef_app().error("Lock PID file %s failed\n", pid_file_name.c_str());
    exit(-EDEADLK);
  }

  // HTTP/2 — nghttp2
  http2_server_config cfg;
  cfg.num_worker_threads   = std::max(1U, std::thread::hardware_concurrency());
  cfg.dispatcher_pool_size = nef_config_inst->nef()->get_dispatcher_pool_size();

  nef_api_server_2 = std::make_unique<nef_http2_server>(
      conv::toString(nef_config_inst->local().get_sbi().get_addr4()),
      nef_config_inst->local().get_sbi().get_port(), nef_app_inst, cfg);
  std::thread nef_http2_manager(
      &nef_http2_server::start, nef_api_server_2.get());

  FILE* fp             = NULL;
  std::string filename = fmt::format("/tmp/nef_{}.status", getpid());
  fp                   = fopen(filename.c_str(), "w+");
  fprintf(fp, "STARTED\n");
  fflush(fp);
  fclose(fp);

  Logger::nef_app().info("Initiation done!");

  // Park the main thread until a signal fires the eventfd.
  uint64_t val = 0;
  ::read(shutdown_efd_g, &val, sizeof(val));
  close(shutdown_efd_g);

  auto shutdown_start = std::chrono::system_clock::now();
  Logger::set_level(spdlog::level::debug);
  Logger::system().info(
      "Signal received \xe2\x80\x94 starting graceful shutdown");

  // Step 1: Enter drain mode so new requests get 503 immediately.
  if (nef_api_server_2) {
    nef_api_server_2->initiate_graceful_shutdown();
  }

  // Step 2: Deregister from NRF so load balancers route new traffic elsewhere.
  if (nef_app_inst) {
    nef_app_inst->deregister_from_nrf();
  }

  // Step 3: Brief drain window for in-flight requests and notifications.
  Logger::system().info("Graceful shutdown...");
  std::this_thread::sleep_for(std::chrono::seconds(2));

  Logger::system().debug("Freeing allocated memory...");

  if (nef_api_server_2) {
    nef_api_server_2->stop();
    nef_http2_manager.join();  // wait for event loop to fully exit
    nef_api_server_2.reset();
  }
  Logger::system().debug("NEF API Server memory done");

  if (tm_inst) {
    task_manager_thread.detach();
    tm_inst.reset();
  }
  Logger::system().debug("Stopped the NEF Task Manager.");

  if (nef_app_inst) {
    nef_app_inst.reset();
  }
  Logger::system().debug("NEF APP memory done");
  Logger::system().info("Freeing allocated memory done");

  auto elapsed = std::chrono::system_clock::now() - shutdown_start;
  auto ms_diff = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
  Logger::system().info(
      "Bye. Graceful shutdown completed in %ld ms",
      static_cast<long>(ms_diff.count()));

  return EXIT_SUCCESS;
}
