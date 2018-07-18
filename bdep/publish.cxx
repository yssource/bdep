// file      : bdep/publish.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bdep/publish.hxx>

#include <cstdlib> // strtoul()

#include <libbutl/manifest-parser.mxx>
#include <libbutl/standard-version.mxx>

#include <bdep/git.hxx>
#include <bdep/project.hxx>
#include <bdep/project-email.hxx>
#include <bdep/database.hxx>
#include <bdep/diagnostics.hxx>

#include <bdep/sync.hxx>

using namespace std;
using namespace butl;

namespace bdep
{
  static inline url
  parse_url (const string& s, const char* what)
  {
    try
    {
      return url (s);
    }
    catch (const invalid_argument& e)
    {
      fail << "invalid " << what << " value '" << s << "': " << e << endf;
    }
  };

  // Get the project's control repository URL.
  //
  static url
  control_url (const dir_path& prj)
  {
    if (git (prj))
    {
      // First try remote.origin.build2ControlUrl which can be used to specify
      // a custom URL (e.g., if a correct one cannot be automatically derived
      // from remote.origin.url).
      //
      if (optional<string> l = git_line (prj,
                                         true /* ignore_error */,
                                         "config",
                                         "--get",
                                         "remote.origin.build2ControlUrl"))
      {
        return parse_url (*l, "remote.origin.build2ControlUrl");
      }

      // Otherwise, get remote.origin.url and try to derive an HTTPS URL from
      // it.
      //
      if (optional<string> l = git_line (prj,
                                         true /* ignore_error */,
                                         "config",
                                         "--get",
                                         "remote.origin.url"))
      {
        string& s (*l);

        // This one will be fuzzy and hairy. Here are some representative
        // examples of what we can encounter:
        //
        //            example.org:/path/to/repo.git
        //       user@example.org:/path/to/repo.git
        //       user@example.org:~user/path/to/repo.git
        // ssh://user@example.org/path/to/repo.git
        //
        // git://example.org/path/to/repo.git
        //
        // http://example.org/path/to/repo.git
        // https://example.org/path/to/repo.git
        //
        //        /path/to/repo.git
        // file:///path/to/repo.git
        //
        // Note that git seem to always make remote.origin.url absolute in
        // case of a local filesystem path.
        //
        // So the algorithm will be as follows:
        //
        // 1. If there is scheme, then parse as URL.
        //
        // 2. Otherwise, check if this is an absolute path.
        //
        // 3. Otherwise, assume SSH <host>:<path> thing.
        //
        url u;

        // Find the scheme.
        //
        // Note that in example.org:/path/... example.org is a valid scheme.
        // To distinguish this, we check if the scheme contains any dots (none
        // of the schemes recognized by git currently do and probably never
        // will).
        //
        size_t p (s.find (':'));
        if (p != string::npos                  && // Has ':'.
            url::traits::find (s, p) == 0      && // Scheme starts at 0.
            s.rfind ('.', p - 1) == string::npos) // No dots in scheme.
        {
          u = parse_url (s, "remote.origin.url");
        }
        else
        {
          // Absolute path or the SSH thing.
          //
          if (path::traits::absolute (s))
          {
            // This is what we want to end up with:
            //
            // file:///tmp
            // file:///c:/tmp
            //
            const char* h (s[0] == '/' ? "file://" : "file:///");
            u = parse_url (h + s, "remote.origin.url");
          }
          else if (p != string::npos)
          {
            // This can still include user (user@host) so let's add the
            // scheme, replace/erase ':', and parse it as a string
            // representation of a URL.
            //
            if (s[p + 1] == '/') // POSIX notation.
              s.erase (p, 1);
            else
              s[p] = '/';

            u = parse_url ("ssh://" + s, "remote.origin.url");
          }
          else
            fail << "invalid remote.origin.url value '" << s << "': not a URL";
        }

        if (u.scheme == "http" || u.scheme == "https")
          return u;

        // Derive an HTTPS URL from a remote URL (and hope for the best).
        //
        if (u.scheme != "file" && u.authority && u.path)
          return url ("https", u.authority->host, *u.path);

        fail << "unable to derive control repository URL from " << u <<
          info << "consider setting remote.origin.build2ControlUrl" <<
          info << "or use --control to specify explicitly";
      }

      fail << "unable to discover control repository URL: no git "
           << "remote.origin.url value";
    }

    fail << "unable to discover control repository URL" <<
      info << "use --control to specify explicitly" << endf;
  }

  static standard_version
  package_version (const common_options& o,
                   const dir_path& cfg,
                   const string& p)
  {
    // We could have used bpkg-pkg-status but then we would have to deal with
    // iterations. So we use the build system's info meta-operation directly.
    //
    string v;
    {
      process pr;
      bool io (false);
      try
      {
        fdpipe pipe (fdopen_pipe ()); // Text mode seems appropriate.

        // Note: the package directory inside the configuration is a bit of an
        // assumption.
        //
        pr = start_b (o,
                      pipe /* stdout */,
                      2    /* stderr */,
                      "info:", (dir_path (cfg) /= p).representation ());

        pipe.out.close ();
        ifdstream is (move (pipe.in), fdstream_mode::skip, ifdstream::badbit);

        for (string l; !eof (getline (is, l)); )
        {
          // Verify the name for good measure (comes before version).
          //
          if (l.compare (0, 9, "project: ") == 0)
          {
            if (l.compare (9, string::npos, p) != 0)
              fail << "name mismatch for package " << p;
          }
          else if (l.compare (0, 9, "version: ") == 0)
          {
            v = string (l, 9);
            break;
          }
        }

        is.close (); // Detect errors.
      }
      catch (const io_error&)
      {
        // Presumably the child process failed and issued diagnostics so let
        // finish_b() try to deal with that first.
        //
        io = true;
      }

      finish_b (o, pr, io);
    }

    try
    {
      return standard_version (v);
    }
    catch (const invalid_argument& e)
    {
      fail << "invalid package " << p << " version " << v << ": " << e << endf;
    }
  }

  // Submit package archive using the curl program and parse the response
  // manifest. On success, return the submission reference (first) and message
  // (second). Issue diagnostics and fail if anything goes wrong.
  //
  static pair<string, string>
  submit (const cmd_publish_options& o,
          const path& archive,
          const string& checksum,
          const string& section,
          const string& email,
          const optional<url>& ctrl)
  {
    using parser     = manifest_parser;
    using parsing    = manifest_parsing;
    using name_value = manifest_name_value;

    // The overall plan is to post the archive using the curl program, read
    // the HTTP response status and content type, read and parse the body
    // according to the content type, and obtain the result reference and
    // message in case of both the submission success and failure.
    //
    // The successful submission response (HTTP status code 200) is expected
    // to contain the submission result manifest (text/manifest content type).
    // The faulty response (HTTP status code other than 200) can either
    // contain the result manifest or a plain text error description
    // (text/plain content type) or some other content (for example
    // text/html). We will print the manifest message value, if available or
    // the first line of the plain text error description or, as a last
    // resort, construct the message from the HTTP status code and reason
    // phrase.
    //
    string message;
    optional<string> reference; // Must be present on the submission success.

    // None of the 3XX redirect code semantics assume automatic re-posting. We
    // will treat all such codes as failures, additionally printing the
    // location header value to advise the user to try the other URL for the
    // package submission.
    //
    // Note that repositories that move to a new URL may well be responding
    // with the moved permanently (301) code.
    //
    optional<url> location;

    // Note that it's a bad idea to issue the diagnostics while curl is
    // running, as it will be messed up with the progress output. Thus, we
    // throw the runtime_error exception on the HTTP response parsing error
    // (rather than use our fail stream) and issue the diagnostics after curl
    // finishes.
    //
    // Also note that we prefer the start/finish process facility for running
    // curl over using butl::curl because in this context it is restrictive
    // and inconvenient.
    //
    process pr;
    bool io (false);
    try
    {
      url u (o.repository ());
      u.query = "submit";

      // Map the verbosity level.
      //
      cstrings v;
      if (verb < 1)
      {
        v.push_back ("-s");
        v.push_back ("-S"); // But show errors.
      }
      else if (verb == 1)
        v.push_back ("--progress-bar");
      else if (verb > 3)
        v.push_back ("-v");

      // Start curl program.
      //
      fdpipe pipe (fdopen_pipe ()); // Text mode seems appropriate.

      // Note that we don't specify any default timeouts, assuming that bdep
      // is an interactive program and the user can always interrupt the
      // command (or pass the timeout with --curl-option).
      //
      pr = start (0          /* stdin  */,
                  pipe       /* stdout */,
                  2          /* stderr */,
                  o.curl (),
                  v,
                  "-A", (BDEP_USER_AGENT " curl"),

                  o.curl_option (),

                  // Include the response headers in the output so we can get
                  // the status code/reason, content type, and the redirect
                  // location.
                  //
                  "--include",

                  "--form",        "archive=@"  + archive.string (),
                  "--form-string", "sha256sum=" + checksum,
                  "--form-string", "section="   + section,
                  "--form-string", "email="     + email,

                  ctrl
                  ? strings ({"--form-string", "control=" + ctrl->string ()})
                  : strings (),

                  o.simulate_specified ()
                  ? strings ({"--form-string", "simulate=" + o.simulate ()})
                  : strings (),

                  u.string ());

      pipe.out.close ();

      // First we read the HTTP response status line and headers. At this
      // stage we will read until the empty line (containing just CRLF). Not
      // being able to reach such a line is an error, which is the reason for
      // the exception mask choice.
      //
      ifdstream is (
        move (pipe.in),
        fdstream_mode::skip,
        ifdstream::badbit | ifdstream::failbit | ifdstream::eofbit);

      // Parse and return the HTTP status code. Return 0 if the argument is
      // invalid.
      //
      auto status_code = [] (const string& s)
      {
        char* e (nullptr);
        unsigned long c (strtoul (s.c_str (), &e, 10)); // Can't throw.
        assert (e != nullptr);

        return *e == '\0' && c >= 100 && c < 600
               ? static_cast<uint16_t> (c)
               : 0;
      };

      // Read the CRLF-terminated line from the stream stripping the trailing
      // CRLF.
      //
      auto read_line = [&is] ()
      {
        string l;
        getline (is, l); // Strips the trailing LF (0xA).

        // Note that on POSIX CRLF is not automatically translated into LF,
        // so we need to strip CR (0xD) manually.
        //
        if (!l.empty () && l.back () == '\r')
          l.pop_back ();

        return l;
      };

      auto bad_response = [] (const string& d) {throw runtime_error (d);};

      // Read and parse the HTTP response status line, return the status code
      // and the reason phrase.
      //
      struct http_status
      {
        uint16_t code;
        string reason;
      };

      auto read_status = [&read_line, &status_code, &bad_response] ()
      {
        string l (read_line ());

        for (;;) // Breakout loop.
        {
          if (l.compare (0, 5, "HTTP/") != 0)
            break;

          size_t p (l.find (' ', 5));             // Finds the protocol end.
          if (p == string::npos)
            break;

          p = l.find_first_not_of (' ', p + 1);   // Finds the code start.
          if (p == string::npos)
            break;

          size_t e (l.find (' ', p + 1));         // Finds the code end.
          if (e == string::npos)
            break;

          uint16_t c (status_code (string (l, p, e - p)));
          if (c == 0)
            break;

          string r;
          p = l.find_first_not_of (' ', e + 1);   // Finds the reason start.
          if (p != string::npos)
          {
            e = l.find_last_not_of (' ');         // Finds the reason end.
            assert (e != string::npos && e >= p);

            r = string (l, p, e - p + 1);
          }

          return http_status {c, move (r)};
        }

        bad_response ("invalid HTTP response status line '" + l + "'");

        assert (false); // Can't be here.
        return http_status {};
      };

      // The curl output for a successfull submission looks like this:
      //
      // HTTP/1.1 100 Continue
      //
      // HTTP/1.1 200 OK
      // Content-Length: 83
      // Content-Type: text/manifest;charset=utf-8
      //
      // : 1
      // status: 200
      // message: submission queued
      // reference: 256910ca46d5
      //
      // curl normally sends the 'Expect: 100-continue' header for uploads,
      // so we need to handle the interim HTTP server response with the
      // continue (100) status code.
      //
      // Interestingly, Apache can respond with the continue (100) code and
      // with the not found (404) code afterwords. Can it be configured to
      // just respond with 404?
      //
      http_status rs (read_status ());

      if (rs.code == 100)
      {
        while (!read_line ().empty ()) ; // Skips the interim response.
        rs = read_status ();             // Reads the final status code.
      }

      // Read through the response headers until the empty line is encountered
      // and obtain the content type and/or the redirect location, if present.
      //
      optional<string> ctype;

      // Check if the line contains the specified header and return its value
      // if that's the case. Return nullopt otherwise.
      //
      // Note that we don't expect the header values that we are interested in
      // to span over multiple lines.
      //
      string l;
      auto header = [&l] (const char* name) -> optional<string>
      {
        size_t n (string::traits_type::length (name));
        if (!(casecmp (name, l, n) == 0 && l[n] == ':'))
          return nullopt;

        string r;
        size_t p (l.find_first_not_of (' ', n + 1)); // Finds value begin.
        if (p != string::npos)
        {
          size_t e (l.find_last_not_of (' '));       // Finds value end.
          assert (e != string::npos && e >= p);

          r = string (l, p, e - p + 1);
        }

        return optional<string> (move (r));
      };

      while (!(l = read_line ()).empty ())
      {
        if (optional<string> v = header ("Content-Type"))
          ctype = move (v);
        else if (optional<string> v = header ("Location"))
        {
          if ((rs.code >= 301 && rs.code <= 303) || rs.code == 307)
          try
          {
            location = url (*v);
            location->query = nullopt; // Can possibly contain '?submit'.
          }
          catch (const invalid_argument&)
          {
            // Let's just ignore invalid locations.
            //
          }
        }
      }

      assert (!eof (is)); // Would have already failed otherwise.

      // Now parse the response payload if the content type is specified and
      // is recognized (text/manifest or text/plain), skip it (with the
      // ifdstream's close() function) otherwise.
      //
      // Note that eof and getline() fail conditions are not errors anymore,
      // so we adjust the exception mask accordingly.
      //
      is.exceptions (ifdstream::badbit);

      bool manifest (false);

      if (ctype)
      {
        if (casecmp ("text/manifest", *ctype, 13) == 0)
        {
          parser p (is, "manifest");
          name_value nv (p.next ());

          if (nv.empty ())
            bad_response ("empty manifest");

          const string& n (nv.name);
          string& v (nv.value);

          // The format version pair is verified by the parser.
          //
          assert (n.empty () && v == "1");

          auto bad_value = [&p, &nv] (const string& d) {
            throw parsing (p.name (), nv.value_line, nv.value_column, d);};

          // Get and verify the HTTP status.
          //
          nv = p.next ();
          if (n != "status")
            bad_value ("no status specified");

          uint16_t c (status_code (v));
          if (c == 0)
            bad_value ("invalid HTTP status '" + v + "'");

          if (c != rs.code)
            bad_value ("status " + v + " doesn't match HTTP response "
                       "code " + to_string (rs.code));

          // Get the message.
          //
          nv = p.next ();
          if (n != "message" || v.empty ())
            bad_value ("no message specified");

          message = move (v);

          // Get the reference if the submission succeeded.
          //
          if (c == 200)
          {
            nv = p.next ();
            if (n != "reference" || v.empty ())
              bad_value ("no reference specified");

            reference = move (v);
          }

          // Skip the remaining name/value pairs.
          //
          for (nv = p.next (); !nv.empty (); nv = p.next ()) ;

          manifest = true;
        }
        else if (casecmp ("text/plain", *ctype, 10) == 0)
          getline (is, message); // Can result in the empty message.
      }

      is.close (); // Detect errors.

      // The meaningful result we expect is either manifest (status code is
      // not necessarily 200) or HTTP redirect (location is present). We
      // unable to interpret any other cases and so report them as a bad
      // response.
      //
      if (!manifest)
      {
        if (rs.code == 200)
          bad_response ("manifest expected");

        if (message.empty ())
        {
          message = "HTTP status code " + to_string (rs.code);

          if (!rs.reason.empty ())
            message += " (" + lcase (rs.reason) + ")";
        }

        if (!location)
          bad_response (message);
      }
    }
    catch (const io_error&)
    {
      // Presumably the child process failed and issued diagnostics so let
      // finish() try to deal with that first.
      //
      io = true;
    }
    // Handle all parsing errors, including the manifest_parsing exception that
    // inherits from the runtime_error exception.
    //
    // Note that the io_error class inherits from the runtime_error class, so
    // this catch-clause must go last.
    //
    catch (const runtime_error& e)
    {
      finish (o.curl (), pr); // Throws on process failure.

      // Finally we can safely issue the diagnostics (see above for details).
      //
      diag_record dr (fail);
      dr << e <<
        info << "consider reporting this to " << o.repository ()
         << " repository maintainers";

      if (reference)
        dr << info << "reference: " << *reference;
      else
        dr << info << "checksum: " << checksum;
    }

    finish (o.curl (), pr, io);

    assert (!message.empty ());

    // Print the submission failure reason and fail.
    //
    if (!reference)
    {
      diag_record dr (fail);
      dr << message;

      if (location)
        dr << info << "new repository location: " << *location;
    }

    return make_pair (move (*reference), message);
  }

  static int
  cmd_publish (const cmd_publish_options& o,
               const dir_path& prj,
               const dir_path& cfg,
               const cstrings& pkg_names)
  {
    const url& repo (o.repository ());

    optional<url> ctrl;
    if (o.control_specified ())
    {
      if (o.control () != "none")
        ctrl = parse_url (o.control (), "--control option");
    }
    else
      ctrl = control_url (prj);

    string email;
    if (o.email_specified ())
      email = o.email ();
    else if (optional<string> r = project_email (prj))
      email = move (*r);
    else
      fail << "unable to obtain publisher's email" <<
        info << "use --email to specify explicitly";

    // Collect package information (version, project, section).
    //
    // @@ It would have been nice to publish them in the dependency order.
    //    Perhaps we need something like bpkg-pkg-order (also would be needed
    //    in init --clone).
    //
    struct package
    {
      string           name;
      standard_version version;
      string           project;
      string           section; // alpha|beta|stable (or --section)

      path             archive;
      string           checksum;
    };
    vector<package> pkgs;

    for (string n: pkg_names)
    {
      standard_version v (package_version (o, cfg, n));

      // Should we allow publishing snapshots and, if so, to which section?
      // For example, is it correct to consider a "between betas" snapshot a
      // beta version?
      //
      if (v.snapshot ())
        fail << "package " << n << " version " << v << " is a snapshot";

      string p (prj.leaf ().string ()); // @@ TODO/TMP

      // Per semver we treat zero major versions as alpha.
      //
      string s (o.section_specified () ? o.section ()   :
                v.alpha () || v.major () == 0 ? "alpha" :
                v.beta ()                     ? "beta"  : "stable");

      pkgs.push_back (
        package {move (n), move (v), move (p), move (s), path (), string ()});
    }

    // Print the plan and ask for confirmation.
    //
    if (!o.yes ())
    {
      text << "publishing:" << '\n'
           << "  to:      " << repo << '\n'
           << "  as:      " << email
           << '\n';

      for (size_t i (0); i != pkgs.size (); ++i)
      {
        const package& p (pkgs[i]);

        diag_record dr (text);

        // If printing multiple packages, separate them with a blank line.
        //
        if (i != 0)
          dr << '\n';

        // While currently the control repository is the same for all
        // packages, this could change in the future (e.g., multi-project
        // publishing).
        //
        dr << "  package: " << p.name    << '\n'
           << "  version: " << p.version << '\n'
           << "  project: " << p.project << '\n'
           << "  section: " << p.section;

        if (ctrl)
          dr << '\n'
             << "  control: " << *ctrl;
      }

      if (!yn_prompt ("continue? [y/n]"))
        return 1;
    }

    // Prepare package archives and calculate their checksums. Also verify
    // each archive with bpkg-pkg-verify for good measure.
    //
    auto_rmdir dr_rm (tmp_dir ("publish"));
    const dir_path& dr (dr_rm.path);   // dist.root
    mk (dr);

    for (package& p: pkgs)
    {
      // Similar to extracting package version, we call the build system
      // directly to prepare the distribution. If/when we have bpkg-pkg-dist,
      // we may want to switch to that.
      //
      run_b (o,
             "dist:", (dir_path (cfg) /= p.name).representation (),
             "config.dist.root=" + dr.representation (),
             "config.dist.archives=tar.gz",
             "config.dist.checksums=sha256");

      // This is the canonical package archive name that we expect dist to
      // produce.
      //
      path a (dr / p.name + '-' + p.version.string () + ".tar.gz");
      path c (a + ".sha256");

      if (!exists (a))
        fail << "package distribution did not produce expected archive " << a;

      if (!exists (c))
        fail << "package distribution did not produce expected checksum " << c;

      // Verify that archive name/content all match.
      //
      run_bpkg (2 /* verbosity */, o, "pkg-verify", a);

      // Read the checksum.
      //
      try
      {
        ifdstream is (c);
        string l;
        getline (is, l);
        is.close ();

        p.checksum = string (l, 0, 64);
      }
      catch (const io_error& e)
      {
        fail << "unable to read " << c << ": " << e;
      }

      p.archive = move (a);
    }

    // Submit each package.
    //
    for (const package& p: pkgs)
    {
      // The path points into the temporary directory so let's omit the
      // directory part.
      //
      if (verb)
        text << "submitting " << p.archive.leaf ();

      pair<string, string> r (
        submit (o, p.archive, p.checksum, p.section, email, ctrl));

      if (verb)
        text << r.second << " (" << r.first << ")";

      //@@ TODO [phase 2]: add checksum file to build2-control branch, commit
      //   and push (this will need some more discussion).
      //
      //  - name (abbrev 12 char checksum) and subdir?
      //
      //  - make the checksum file a manifest with basic info (name, version)
      //
      //  - what if already exists (previous failed attempt)? Ignore?
      //
      //  - make a separate checkout (in tmp facility) reusing the external
      //    .git/ dir?
      //
      //  - should probably first fetch to avoid push conflicts. Or maybe
      //    fetch on push conflict (more efficient/robust)?
      //
    }

    return 0;
  }

  int
  cmd_publish (const cmd_publish_options& o, cli::scanner&)
  {
    tracer trace ("publish");

    // The same ignore/load story as in sync.
    //
    project_packages pp (
      find_project_packages (o,
                             false /* ignore_packages */,
                             false /* load_packages   */));

    const dir_path& prj (pp.project);
    database db (open (prj, trace));

    // We need a single configuration to prepare package distribution.
    //
    shared_ptr<configuration> cfg;
    {
      transaction t (db.begin ());
      configurations cfgs (find_configurations (o, prj, t));
      t.commit ();

      if (cfgs.size () > 1)
        fail << "multiple configurations specified for publish";

      shared_ptr<configuration>& c (cfgs[0]);

      // If specified, verify packages are present in the configuration.
      // Otherwise, make sure the configuration is not empty.
      //
      if (!pp.packages.empty ())
        verify_project_packages (pp, cfgs);
      else if (c->packages.empty ())
        fail << "no packages initialized in configuration " << *c;

      cfg = move (c);
    }

    // Pre-sync the configuration to avoid triggering the build system hook
    // (see sync for details).
    //
    cmd_sync (o, prj, cfg, strings () /* pkg_args */, true /* implicit */);

    // If no packages were explicitly specified, then we publish all that have
    // been initialized in the configuration.
    //
    cstrings pkgs;
    if (pp.packages.empty ())
    {
      for (const package_state& p: cfg->packages)
        pkgs.push_back (p.name.string ().c_str ());
    }
    else
    {
      for (const package_location& p: pp.packages)
        pkgs.push_back (p.name.string ().c_str ());
    }

    return cmd_publish (o, prj, cfg->path, pkgs);
  }
}
