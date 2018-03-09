// file      : bdep/utility.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BDEP_UTILITY_HXX
#define BDEP_UTILITY_HXX

#include <memory>    // make_shared()
#include <string>    // to_string()
#include <utility>   // move(), forward(), declval(), make_pair()
#include <cassert>   // assert()
#include <iterator>  // make_move_iterator()
#include <algorithm> // find(), find_if()

#include <libbutl/ft/lang.hxx>

#include <libbutl/utility.mxx>    // casecmp(), reverse_iterate(), etc
#include <libbutl/filesystem.mxx>

#include <bdep/types.hxx>
#include <bdep/version.hxx>

namespace bdep
{
  using std::move;
  using std::forward;
  using std::declval;

  using std::make_pair;
  using std::make_shared;
  using std::make_move_iterator;
  using std::to_string;

  using std::find;
  using std::find_if;

  // <libbutl/utility.mxx>
  //
  using butl::casecmp;
  using butl::reverse_iterate;

  using butl::exception_guard;
  using butl::make_exception_guard;

  // <libbutl/filesystem.mxx>
  //
  using butl::auto_rmfile;
  using butl::auto_rmdir;

  // Empty string and path.
  //
  extern const string   empty_string;
  extern const path     empty_path;
  extern const dir_path empty_dir_path;

  // Widely-used paths.
  //
  extern const dir_path bdep_dir;  // .bdep/
  extern const path     bdep_file; // .bdep/bdep.sqlite3

  extern const path manifest_file;       // manifest
  extern const path packages_file;       // packages.manifest
  extern const path repositories_file;   // repositories.manifest
  extern const path configurations_file; // configurations.manifest

  // Directory extracted from argv[0] (i.e., this process' recall directory)
  // or empty if there is none. Can be used as a search fallback.
  //
  extern dir_path exec_dir;

  // Filesystem.
  //
  bool
  exists (const path&, bool ignore_error = false);

  bool
  exists (const dir_path&, bool ignore_error = false);

  bool
  empty (const dir_path&);

  void
  mk (const dir_path&);

  void
  mk_p (const dir_path&);

  void
  rm (const path&, uint16_t verbosity = 3);

  // Run the bpkg process.
  //
  class common_options;

  const char*
  name_bpkg (const common_options&);

  template <typename O, typename E, typename... A>
  process
  start_bpkg (const common_options&, O&& out, E&& err, A&&... args);

  template <typename... A>
  process_exit
  run_bpkg (const common_options&, A&&... args);

  // Manifest parsing and serialization.
  //
  // For parsing, if path is '-', then read from stdin.
  //
  template <typename T>
  T
  parse_manifest (const path&,
                  const char* what,
                  bool ignore_unknown = false);

  template <typename T>
  T
  parse_manifest (istream&,
                  const string& name,
                  const char* what,
                  bool ignore_unknown = false);

  template <typename T>
  void
  serialize_manifest (const T&, const path&, const char* what);

  template <typename T>
  void
  serialize_manifest (const T&,
                      ostream&,
                      const string& name,
                      const char* what);
}

#include <bdep/utility.txx>

#endif // BDEP_UTILITY_HXX
