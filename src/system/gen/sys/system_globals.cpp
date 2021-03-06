/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
// @generated by HipHop Compiler

#include <runtime/base/hphp_system.h>
#include <system/gen/sys/literal_strings_remap.h>
#include <system/gen/sys/scalar_arrays_remap.h>
#include <sys/system_globals.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

SystemGlobals::SystemGlobals() {
  memset(&stgv_bool, 0, sizeof(stgv_bool));
  memset(&stgv_int, 0, sizeof(stgv_int));
  memset(&stgv_int64, 0, sizeof(stgv_int64));
  memset(&stgv_double, 0, sizeof(stgv_double));
  memset(&stgv_RedeclaredCallInfoConstPtr, 0, sizeof(stgv_RedeclaredCallInfoConstPtr));
  if (hhvm) {
    // HHVM globals initialization
    hg_global_storage = NEW(HphpArray)(0, true);
    hg_global_storage.set("GLOBALS", hg_global_storage);
    // XXX As a hack, we strongly bind hphpc's superglobals to the matching
    // keys in our globals array. While this will work for most PHP
    // programs, this is not strictly correct.
    hg_global_storage.set("argc", ref(gvm_argc));
    hg_global_storage.set("argv", ref(gvm_argv));
    hg_global_storage.set("_SERVER", ref(gvm__SERVER));
    hg_global_storage.set("_GET", ref(gvm__GET));
    hg_global_storage.set("_POST", ref(gvm__POST));
    hg_global_storage.set("_COOKIE", ref(gvm__COOKIE));
    hg_global_storage.set("_FILES", ref(gvm__FILES));
    hg_global_storage.set("_ENV", ref(gvm__ENV));
    hg_global_storage.set("_REQUEST", ref(gvm__REQUEST));
    hg_global_storage.set("_SESSION", ref(gvm__SESSION));
    hg_global_storage.set("HTTP_RAW_POST_DATA",
                          ref(gvm_HTTP_RAW_POST_DATA));
    hg_global_storage.set("http_response_header",
                          ref(gvm_http_response_header));
  }

  // Redeclared Classes
}

void SystemGlobals::initialize() {
  Globals::initialize();
  Globals *globals = get_globals();
  pm_php$globals$symbols_php(false, globals, globals);
}

///////////////////////////////////////////////////////////////////////////////
}
