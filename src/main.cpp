/*
 * This file is part of the Trojan Plus project.
 * Trojan is an unidentifiable mechanism that helps you bypass GFW.
 * Trojan Plus is derived from original trojan project and writing
 * for more experimental features.
 * Copyright (C) 2017-2020  The Trojan Authors.
 * Copyright (C) 2020 The Trojan Plus Group Authors.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <boost/asio/signal_set.hpp>
#include <boost/program_options.hpp>
#include <boost/version.hpp>
#include <cstdlib>
#include <iostream>
#include <openssl/opensslv.h>

#ifdef ENABLE_MYSQL
#include <mysql.h>
#endif // ENABLE_MYSQL

#include "core/service.h"
#include "core/version.h"

using namespace std;
using namespace boost::asio;
namespace po = boost::program_options;

#ifndef DEFAULT_CONFIG
#define DEFAULT_CONFIG "config.json"
#endif // DEFAULT_CONFIG

void signal_async_wait(signal_set& sig, Service& service, bool& restart) {
    sig.async_wait([&](const boost::system::error_code error, int signum) {
        if (error) {
            return;
        }
        _log_with_date_time("got signal: " + to_string(signum), Log::WARN);
        switch (signum) {
            case SIGINT:
            case SIGTERM:
                service.stop();
                break;
#ifndef _WIN32
            case SIGUSR2: // for Android Close
                service.stop();
                break;
            case SIGHUP:
                restart = true;
                service.stop();
                break;
            case SIGUSR1:
                service.reload_cert();
                signal_async_wait(sig, service, restart);
                break;
#endif // _WIN32
        }
    });
}

// global service to avoid calling Service::~Service for Android,
// to speed up Android VPN disconnection. io_context::~io_context might hang for 30 - 50 sec
// after disconnection, the whole process will be killed in Android
static shared_ptr<Service> g_service;

int main(int argc, const char* argv[]) {
    try {
        Log::log("Trojan Plus v" + Version::get_version() + " starts.", Log::FATAL);
        string config_file;
        string log_file;
        string keylog_file;
        bool test;
        po::options_description desc("options");
        desc.add_options()("config,c",
          po::value<string>(&config_file)->default_value(DEFAULT_CONFIG)->value_name("CONFIG"),
          "specify config file")("help,h", "print help message")("keylog,k",
          po::value<string>(&keylog_file)->value_name("KEYLOG"), "specify keylog file location (OpenSSL >= 1.1.1)")(
          "log,l", po::value<string>(&log_file)->value_name("LOG"), "specify log file location")(
          "test,t", po::bool_switch(&test), "test config file")("version,v", "print version and build info");
        po::positional_options_description pd;
        pd.add("config", 1);
        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv).options(desc).positional(pd).run(), vm);
        po::notify(vm);
        if (vm.count("help")) {
            Log::log(string("usage: ") + argv[0] + " [-htv] [-l LOG] [-k KEYLOG] [[-c] CONFIG]", Log::FATAL);
            cerr << desc;
            exit(EXIT_SUCCESS);
        }
        if (vm.count("version")) {
            Log::log(string("Boost ") + BOOST_LIB_VERSION + ", " + OpenSSL_version(OPENSSL_VERSION), Log::FATAL);
#ifdef ENABLE_MYSQL
            Log::log(string(" [Enabled] MySQL Support (") + mysql_get_client_info() + ')', Log::FATAL);
#else  // ENABLE_MYSQL
            Log::log("[Disabled] MySQL Support", Log::FATAL);
#endif // ENABLE_MYSQL
#ifdef TCP_FASTOPEN
            Log::log(" [Enabled] TCP_FASTOPEN Support", Log::FATAL);
#else  // TCP_FASTOPEN
            Log::log("[Disabled] TCP_FASTOPEN Support", Log::FATAL);
#endif // TCP_FASTOPEN
#ifdef TCP_FASTOPEN_CONNECT
            Log::log(" [Enabled] TCP_FASTOPEN_CONNECT Support", Log::FATAL);
#else  // TCP_FASTOPEN_CONNECT
            Log::log("[Disabled] TCP_FASTOPEN_CONNECT Support", Log::FATAL);
#endif // TCP_FASTOPEN_CONNECT
#if ENABLE_SSL_KEYLOG
            Log::log(" [Enabled] SSL KeyLog Support", Log::FATAL);
#else  // ENABLE_SSL_KEYLOG
            Log::log("[Disabled] SSL KeyLog Support", Log::FATAL);
#endif // ENABLE_SSL_KEYLOG
#ifdef ENABLE_NAT
            Log::log(" [Enabled] NAT Support", Log::FATAL);
#else  // ENABLE_NAT
            Log::log("[Disabled] NAT Support", Log::FATAL);
#endif // ENABLE_NAT
#ifdef ENABLE_TLS13_CIPHERSUITES
            Log::log(" [Enabled] TLS1.3 Ciphersuites Support", Log::FATAL);
#else  // ENABLE_TLS13_CIPHERSUITES
            Log::log("[Disabled] TLS1.3 Ciphersuites Support", Log::FATAL);
#endif // ENABLE_TLS13_CIPHERSUITES
#ifdef ENABLE_REUSE_PORT
            Log::log(" [Enabled] TCP Port Reuse Support", Log::FATAL);
#else  // ENABLE_REUSE_PORT
            Log::log("[Disabled] TCP Port Reuse Support", Log::FATAL);
#endif // ENABLE_REUSE_PORT
            Log::log("OpenSSL Information", Log::FATAL);
            if (OpenSSL_version_num() != OPENSSL_VERSION_NUMBER) {
                Log::log(string("\tCompile-time Version: ") + OPENSSL_VERSION_TEXT, Log::FATAL);
            }
            Log::log(string("\tBuild Flags: ") + OpenSSL_version(OPENSSL_CFLAGS), Log::FATAL);
            exit(EXIT_SUCCESS);
        }
        if (vm.count("log")) {
            Log::redirect(log_file);
        }
        if (vm.count("keylog")) {
            Log::redirect_keylog(keylog_file);
        }
        bool restart;
        Config config;
        do {
            restart = false;
            if (config.sip003()) {
                _log_with_date_time("SIP003 is loaded", Log::WARN);
            } else {
                config.load(config_file);
            }
            g_service = make_shared<Service>(config, test);

            if (test) {
                Log::log("The config file looks good.", Log::OFF);
                exit(EXIT_SUCCESS);
            }
            signal_set sig(g_service->get_io_context());
            sig.add(SIGINT);
            sig.add(SIGTERM);
#ifndef _WIN32
            sig.add(SIGHUP);
            sig.add(SIGUSR1);
            sig.add(SIGUSR2); // for Android Close
#endif                        // _WIN32
            signal_async_wait(sig, *g_service, restart);
            g_service->run();
            if (restart) {
                _log_with_date_time("trojan service restarting. . . ", Log::WARN);
            }
        } while (restart);
        _log_with_date_time("trojan service exit.", Log::WARN);
        Log::reset();

#ifndef __ANDROID__
        g_service.reset();
        exit(EXIT_SUCCESS);
#endif

    } catch (const exception& e) {
        _log_with_date_time(string("fatal: ") + e.what(), Log::FATAL);
        _log_with_date_time("exiting. . . ", Log::FATAL);

#ifndef __ANDROID__
        g_service.reset();
        exit(EXIT_FAILURE);
#endif
    }
}
