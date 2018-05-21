// file      : bdep/project.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bdep/project.hxx>
#include <bdep/project-odb.hxx>

#include <libbpkg/manifest.hxx>

#include <bdep/database.hxx>
#include <bdep/diagnostics.hxx>

using namespace std;

namespace bdep
{
  configurations
  find_configurations (const project_options& po,
                       const dir_path& prj,
                       transaction& t,
                       bool fallback_default,
                       bool validate)
  {
    configurations r;

    // Weed out duplicates.
    //
    auto add = [&r] (shared_ptr<configuration> c)
    {
      if (find_if (r.begin (),
                   r.end (),
                   [&c] (const shared_ptr<configuration>& e)
                   {
                     return *c->id == *e->id;
                   }) == r.end ())
        r.push_back (move (c));
    };

    database& db (t.database ());
    using query = bdep::query<configuration>;

    // @<cfg-name>
    //
    if (po.config_name_specified ())
    {
      for (const string& n: po.config_name ())
      {
        if (auto c = db.query_one<configuration> (query::name == n))
          add (c);
        else
          fail << "no configuration name '" << n << "' in project " << prj;
      }
    }

    // --config <cfg-dir>
    //
    if (po.config_specified ())
    {
      for (dir_path d: po.config ())
      {
        d.complete ();
        d.normalize ();

        if (auto c = db.query_one<configuration> (query::path == d.string ()))
          add (c);
        else
          fail << "no configuration directory " << d << " in project " << prj;
      }
    }

    // --config-id <cfg-num>
    //
    if (po.config_id_specified ())
    {
      for (uint64_t id: po.config_id ())
      {
        if (auto c = db.find<configuration> (id))
          add (c);
        else
          fail << "no configuration id " << id << " in project " << prj;
      }
    }

    // --all
    //
    if (po.all ())
    {
      for (auto c: pointer_result (db.query<configuration> ()))
        add (c);

      if (r.empty ())
        fail << "no existing configurations";
    }

    // default
    //
    if (r.empty ())
    {
      if (fallback_default)
      {
        if (auto c = db.query_one<configuration> (query::default_))
          add (c);
        else
          fail << "no default configuration in project " << prj <<
            info << "use (@<cfg-name> | --config|-c <cfg-dir> | --all|-a) to "
               << "specify configuration explicitly";
      }
      else
        fail << "no configurations specified";
    }

    // Validate all the returned configuration directories are still there.
    //
    if (validate)
    {
      for (const shared_ptr<configuration>& c: r)
      {
        if (!exists (c->path))
          fail << "configuration directory " << c->path << " no longer exists";
      }
    }

    return r;
  }

  // Given a directory which can be a project root, a package root, or one of
  // their subdirectories, return the absolute project (first) and relative
  // package (second) directories. The package directory may be absent if the
  // given directory is not within a package root or empty if the project and
  // package roots are the same.
  //
  struct project_package
  {
    dir_path           project;
    optional<dir_path> package;
  };

  static project_package
  find_project_packages (const dir_path& start)
  {
    dir_path prj;
    optional<dir_path> pkg;

    dir_path d (start);
    d.complete ();
    d.normalize ();
    for (; !d.empty (); d = d.directory ())
    {
      // Ignore errors when checking for file existence since we may be
      // iterating over directories past any reasonable project boundaries.
      //
      bool p (exists (d / manifest_file, true));
      if (p)
      {
        if (pkg)
        {
          fail << "multiple package manifests between " << start
               << " and project root" <<
            info << "first  manifest is in " << *pkg <<
            info << "second manifest is in " << d;
        }

        pkg = d;

        // Fall through (can also be the project root).
      }

      // Check for the database file first since an (initialized) simple
      // project mosl likely won't have any *.manifest files.
      //
      if (exists (d / bdep_file, true)     ||
          exists (d / packages_file, true) ||
          //
          // We should only consider {repositories,configurations}.manifest if
          // we have either packages.manifest or manifest (i.e., this is a
          // valid project root).
          //
          (p && (exists (d / repositories_file, true) ||
                 exists (d / configurations_file, true))))
      {
        prj = move (d);
        break;
      }
    }

    if (prj.empty ())
    {
      if (!pkg)
        fail << start << " is no a (sub)directory of a package or project";

      // Project and package are the same.
      //
      prj = move (*pkg);
      pkg = dir_path ();
    }
    else if (pkg)
      pkg = pkg->leaf (prj);

    return project_package {move (prj), move (pkg)};
  }

  static package_locations
  load_package_locations (const dir_path& prj)
  {
    package_locations pls;

    // If exists, load packages.manifest from the project root. Otherwise,
    // this must be a simple, single-package project.
    //
    path f (prj / packages_file);

    if (exists (f))
    {
      using bpkg::package_manifest;
      using bpkg::dir_package_manifests;

      auto ms (parse_manifest<dir_package_manifests> (f, "packages"));

      // While an empty repository is legal, in our case it doesn't make much
      // sense and will just further complicate things.
      //
      if (ms.empty ())
        fail << "no packages listed in " << f;

      for (package_manifest& m: ms)
      {
        // Convert the package location from POSIX to the host form and make
        // sure the current directory is represented as an empty path.
        //
        assert (m.location);
        dir_path d (path_cast<dir_path> (move (*m.location)));
        d.normalize (false /* actualize */, true /* cur_empty */);

        pls.push_back (package_location {string (), move (d)});
      }
    }
    else
      pls.push_back (package_location {string (), dir_path ()});

    return pls;
  }

  static void
  load_package_names (const dir_path& prj, package_locations& pls)
  {
    // Load each package's manifest and obtain its name (name is normally the
    // first value so we could optimize this, if necessary).
    //
    for (package_location& pl: pls)
    {
      path f (prj / pl.path / manifest_file);
      auto m (parse_manifest<bpkg::package_manifest> (f, "package"));
      pl.name = move (m.name);
    }
  }

  package_locations
  load_packages (const dir_path& prj)
  {
    package_locations pls (load_package_locations (prj));
    load_package_names (prj, pls);
    return pls;
  }

  project_packages
  find_project_packages (const project_options& po,
                         bool ignore_packages,
                         bool load_packages)
  {
    project_packages r;

    if (po.directory_specified ())
    {
      for (const dir_path& d: po.directory ())
      {
        project_package p (find_project_packages (d));

        // We only work on one project at a time.
        //
        if (r.project.empty ())
        {
          r.project = move (p.project);
        }
        else if (r.project != p.project)
        {
          fail << "multiple project directories specified" <<
            info << r.project <<
            info << p.project;
        }

        if (!ignore_packages && p.package)
        {
          // Suppress duplicate packages.
          //
          if (find_if (r.packages.begin (),
                       r.packages.end (),
                       [&p] (const package_location& pl)
                       {
                         return *p.package == pl.path;
                       }) == r.packages.end ())
          {
            // Name is to be extracted later.
            //
            r.packages.push_back (package_location {"", move (*p.package)});
          }
        }
      }
    }
    else
    {
      project_package p (find_project_packages (path::current_directory ()));

      r.project = move (p.project);

      if (!ignore_packages && p.package)
      {
        // Name is to be extracted later.
        //
        r.packages.push_back (package_location {"", *p.package});
      }
    }

    if (!ignore_packages)
    {
      // Load the package locations and either verify that the discovered
      // packages are in it or, if nothing was discovered, use it as the
      // source for the package list.
      //
      package_locations pls (load_package_locations (r.project));

      if (!r.packages.empty ())
      {
        for (const package_location& x: r.packages)
        {
          if (find_if (pls.begin (),
                       pls.end (),
                       [&x] (const package_location& y)
                       {
                         return x.path == y.path;
                       }) == pls.end ())
          {
            fail << "package directory " << x.path << " is not listed in "
                 << r.project;
          }
        }
      }
      else if (load_packages)
      {
        // Names to be extracted later.
        //
        r.packages = move (pls);
      }

      if (!r.packages.empty ())
        load_package_names (r.project, r.packages);
    }

    return r;
  }

  void
  verify_project_packages (const project_packages& pp,
                           const configurations& cfgs)
  {
    for (const shared_ptr<configuration>& c: cfgs)
    {
      for (const package_location& p: pp.packages)
      {
        if (find_if (c->packages.begin (),
                     c->packages.end (),
                     [&p] (const package_state& s)
                     {
                       return p.name == s.name;
                     }) == c->packages.end ())
        {
          fail << "package " << p.name << " is not initialized "
               << "in configuration " << *c;
        }
      }
    }
  }
}
