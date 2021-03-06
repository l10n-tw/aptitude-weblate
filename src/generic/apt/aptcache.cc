// aptcache.cc
//
//  Copyright 1999-2009, 2011 Daniel Burrows
//  Copyright 2015-2017 Manuel A. Fernandez Montecelo
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; see the file COPYING.  If not, write to
//  the Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
//  Boston, MA 02110-1301, USA.

#include "aptcache.h"

#include <aptitude.h>
#include <loggers.h>

#include "apt.h"
#include "aptitude_resolver_universe.h"
#include "aptitudepolicy.h"
#include "config_signal.h"
#include "dpkg_selections.h"
#include <generic/apt/matching/match.h>
#include <generic/apt/matching/parse.h>
#include <generic/apt/matching/pattern.h>
#include <generic/problemresolver/solution.h>
#include <generic/util/undo.h>

#include <apt-pkg/error.h>
#include <apt-pkg/sourcelist.h>
#include <apt-pkg/pkgcachegen.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/tagfile.h>
#include <apt-pkg/fileutl.h>
#include <apt-pkg/algorithms.h>
#include <apt-pkg/pkgsystem.h>
#include <apt-pkg/policy.h>
#include <apt-pkg/progress.h>
#include <apt-pkg/version.h>

#include <vector>

#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>

#include <cwidget/generic/util/eassert.h>
#include <cwidget/generic/util/ssprintf.h>

#include <sigc++/adaptors/bind.h>
#include <sigc++/functors/mem_fun.h>

namespace cw = cwidget;

using namespace std;
using aptitude::Loggers;

class aptitudeDepCache::apt_undoer:public undoable
// Allows an action performed on the package cache to be undone.  My first
// thought was to just snapshot the package cache before the action and then
// copy over the state, but this works better (IMO :) ) -- the other method
// had the drawback of not telling what was actually happening, and potentially
// leaving bits of the cache in weird states.
//
// Of course, there's always the danger that this won't properly restore the
// cache state, so I'll have to revert to the original method..
{
  PkgIterator pkg;
  int prev_mode;  // One of Delete,Keep,Install
  int prev_iflags, prev_flags;
  changed_reason prev_removereason;
  pkgCache::State::PkgSelectedState prev_selection_state;

  string prev_forbidver;

  aptitudeDepCache *owner;
public:
  apt_undoer(PkgIterator _pkg, int _prev_mode, int _prev_flags, int _prev_iflags,
	     changed_reason _prev_removereason,
	     pkgCache::State::PkgSelectedState _prev_selection_state,
	     string _prev_forbidver,
	     aptitudeDepCache *_owner)
    :pkg(_pkg), prev_mode(_prev_mode), prev_iflags(_prev_iflags), prev_flags(_prev_flags),
     prev_removereason(_prev_removereason),
     prev_selection_state(_prev_selection_state),
     prev_forbidver(_prev_forbidver),
     owner(_owner)
  {
  }

  void undo()
  {
    aptitudeDepCache::action_group group(*owner);

    owner->pre_package_state_changed();

    if(prev_iflags&ReInstall)
      owner->internal_mark_install(pkg, false, true);
    else
      {
	switch (prev_mode)
	  {
	  case ModeDelete:
	    owner->internal_mark_delete(pkg, prev_iflags & Purge, prev_removereason == unused);
	    break;
	  case ModeKeep:
	    owner->internal_mark_keep(pkg, prev_iflags & AutoKept, prev_selection_state == pkgCache::State::Hold);
	    break;
	  case ModeInstall:
	    owner->internal_mark_install(pkg, false, false);
	    break;
	  }
      }

    // make sure that everything is really set.
    owner->MarkAuto(pkg, (prev_flags & Flag::Auto));
    owner->get_ext_state(pkg).remove_reason=prev_removereason;
    owner->get_ext_state(pkg).forbidver=prev_forbidver;
  }
};

extern sigc::signal0<void> cache_reloaded;

class aptitudeDepCache::forget_undoer:public undoable
// Undoes a "forget_new" command
{
  vector<pkgCache::PkgIterator> packages;

  aptitudeDepCache *owner;
public:
  forget_undoer(aptitudeDepCache *_owner):owner(_owner) {}

  void add_item(pkgCache::PkgIterator item)
  {
    packages.push_back(item);
  }

  bool empty()
  {
    return packages.empty();
  }

  void undo()
  {
    for(vector<pkgCache::PkgIterator>::iterator i=packages.begin(); i!=packages.end(); i++)
      owner->set_new_flag(*i, true);

    // Hack to make all the trees rebuild themselves.
    cache_reloaded();
  }
};

class aptitudeDepCache::candver_undoer:public undoable
// Undoes a "set candidate version" command.
{
  pkgCache::VerIterator oldver;

  aptitudeDepCache *owner;
public:
  candver_undoer(pkgCache::VerIterator _oldver,
		 aptitudeDepCache *_owner)
    :oldver(_oldver), owner(_owner)
  {
  }

  void undo()
  {
    owner->pre_package_state_changed();
    owner->set_candidate_version(oldver, NULL);
    owner->package_state_changed();
  }
};

aptitudeDepCache::action_group::action_group(aptitudeDepCache &cache,
					     undo_group *group)
  : parent_group(new pkgDepCache::ActionGroup(cache)),
    cache(cache), group(group)
{
  cache.begin_action_group();
}

aptitudeDepCache::action_group::~action_group()
{
  // Force the parent to mark-and-sweep first.
  delete parent_group;

  cache.end_action_group(group);
}

aptitudeDepCache::aptitudeDepCache(pkgCache *Cache, Policy *Plcy)
  :pkgDepCache(Cache, Plcy), dirty(false), read_only(true),
   package_states(NULL), lock(-1), group_level(0),
   new_package_count(0), records(NULL)
{
  // When the "install recommended packages" flag changes, collect garbage.
#if 0
  aptcfg->connect("APT::Install-Recommends",
		  sigc::bind(sigc::mem_fun(*this,
					   &pkgDepCache::MarkAndSweep),
			     (undo_group *) NULL));

  aptcfg->connect(PACKAGE "::Keep-Recommends",
		  sigc::bind(sigc::mem_fun(*this,
					   &pkgDepCache::MarkAndSweep),
			     (undo_group *) NULL));

  // sim.
  aptcfg->connect(PACKAGE "::Keep-Suggests",
		  sigc::bind(sigc::mem_fun(*this,
					   &pkgDepCache::MarkAndSweep),
			     (undo_group *) NULL));
#endif
}

bool aptitudeDepCache::Init(OpProgress *Prog, bool WithLock, bool do_initselections, const char *status_fname, bool reset_reinstall)
{
  return build_selection_list(Prog, WithLock, do_initselections, status_fname, reset_reinstall);
}

aptitudeDepCache::~aptitudeDepCache()
{
  delete records;
  delete[] package_states;

  if(lock!=-1)
    close(lock);
}

void aptitudeDepCache::set_read_only(bool new_read_only)
{
  read_only = new_read_only;
}

bool aptitudeDepCache::build_selection_list(OpProgress* Prog,
					    bool WithLock,
					    bool do_initselections,
					    const char* status_fname,
					    bool reset_reinstall)
{
  action_group group(*this);

  bool initial_open=false;
  // This will be set to true if the state file does not exist.

  if(!pkgDepCache::Init(Prog))
    return false;

  records = new pkgRecords(*this);

  // This is necessary so that the Garbage flags are initialized.
  // Some of the Mark* methods perturb the Auto flag in largely
  // uncontrollable ways, and one defense against this is to get
  // Garbage set up (they behave more the way they ought to then).
  MarkAndSweep();

  delete package_states;
  package_states=new aptitude_state[Head().PackageCount];
  user_tags.clear();
  for(unsigned int i=0; i<Head().PackageCount; i++)
    {
      package_states[i].new_package=true;
      package_states[i].reinstall=false;
      package_states[i].user_tags.clear();
      package_states[i].remove_reason=manual;
      package_states[i].original_selection_state = pkgCache::State::Unknown;
      package_states[i].selection_state = pkgCache::State::Unknown;
      package_states[i].previously_auto_package = false;
    }

  if(WithLock && lock==-1)
    {
      lock = GetLock(aptcfg->Find("Aptitude::LockFile", LOCKFILE));

      if(_error->PendingError())
	{
	  if(lock!=-1)
	    close(lock);
	  lock=-1;
	  read_only = true;
	  return false;
	}
    }
  // Errrrr, I think we need to do this first in case the stuff below
  // manages to trigger a mark operation.
  duplicate_cache(&backup_state);

  // Should this not go under Dir:: ?  I'm not sure..
  string statedir = aptcfg->FindDir("Dir::Aptitude::state", STATEDIR);
  string statefilepath = (status_fname) ? status_fname : (statedir + "/" + "pkgstates");

  FileFd state_file;

  // Read in the states that we saved
  state_file.Open(statefilepath, FileFd::ReadOnly);

  // Have to make the file NOT read-only to set up the initial state.
  read_only = false;

  if(!state_file.IsOpen())
    {
      _error->Discard();
      if(errno!=ENOENT)
	_error->Warning(_("Can't open Aptitude extended state file"));
      else
	{
	  initial_open=true;
	  // Mark the cache as dirty so that we'll create the
	  // pkgstates file later.  We need to do this even if the
	  // user doesn't change anything because otherwise we won't
	  // know which packages are new until we save the cache (#429732).
	  dirty = true;
	}
    }
  else
    {
      // last percent shown in progress -- do not update on every cycle
      int last_pct_shown = 0;
      int amt = 0;
      int file_size = state_file.Size();
      if (Prog)
	{
	  Prog->OverallProgress(0, file_size, 1, _("Reading extended state information"));
	}

      pkgTagFile tagfile(&state_file);
      pkgTagSection section;

      bool do_dselect=aptcfg->FindB(PACKAGE "::Track-Dselect-State", true);
      while(tagfile.Step(section))
	{
	  std::string package_name(section.FindS("Package"));
          std::string arch(section.FindS("Architecture"));
	  PkgIterator pkg;
          // TODO: Wheezy+n can assume that all sections will have the
          // Architecture tag (probably ;-).
          if(arch.empty())
            pkg=FindPkg(package_name);
          else
            pkg=FindPkg(package_name, arch);
	  if(!pkg.end() && !pkg.VersionList().end())
	    // Silently ignore unknown packages and packages with no actual
	    // version.
	    {
	      unsigned long tmp=0;
	      string candver;

	      aptitude_state &pkg_state=get_ext_state(pkg);

	      section.FindFlag("Unseen", tmp, 1);

	      pkg_state.new_package=(tmp==1);

	      tmp=0;
	      section.FindFlag("Upgrade", tmp, 1);
	      pkg_state.upgrade=(tmp==1);

	      tmp=0;
	      section.FindFlag("Reinstall", tmp, 1);
	      if (tmp == 1)
		{
		  if (!pkg.CurrentVer().end() && is_version_available(pkg, pkg.CurrentVer().VerStr()))
		    {
		      pkg_state.reinstall = true;
		    }
		  else
		    {
		      // warning, not an error, it's not that severe and also
		      // otherwise the algorithm to read from saved state stops
		      _error->Warning(_("Package %s had been marked to reinstall, but the file for the current installed version %s is not available"),
				      pkg.FullName(true).c_str(), (pkg.CurVersion() ? pkg.CurVersion() : "<none>"));
		    }
		}

	      // if the last installation was successful reset_reinstall==true,
	      // to unmark .reinstall property of the package, otherwise there
	      // is no way to control that this is not repeated forever.
	      //
	      // doing the more expensive test "(reset_reinstall &&
	      // pkg_state.reinstall)" rather than blindly marking as false,
	      // because of "dirty" (so we only mark as dirty and pending to
	      // save when we really changed the state.
	      if (reset_reinstall && pkg_state.reinstall)
		{
		  pkg_state.reinstall = false;
		  dirty = true;
		}

	      unsigned long auto_new_install = 0;
	      section.FindFlag("Auto-New-Install", auto_new_install, 1);
	      if(auto_new_install)
		pkg_state.previously_auto_package = true;

	      // The install reason is much more important to preserve
	      // from previous versions, so support the outdated name
	      // for it.
	      changed_reason install_reason=(changed_reason)
		section.FindI("Install-Reason",
			      section.FindI("Last-Change", manual));

	      if(install_reason != manual)
		pkg_state.previously_auto_package = true;

	      pkg_state.remove_reason=(changed_reason)
		section.FindI("Remove-Reason", manual);

	      // marked as auto-installed from apt? -- bug #841347
	      //
	      // using PkgState[pkg->ID] because passing "pkg" needs
	      // apt_cache_file initialised
	      if (is_auto_installed(PkgState[pkg->ID]))
		pkg_state.previously_auto_package = true;

	      candver=section.FindS("Version");

	      pkg_state.selection_state=(pkgCache::State::PkgSelectedState) section.FindI("State", pkgCache::State::Unknown);
	      pkg_state.original_selection_state = static_cast<pkgCache::State::PkgSelectedState>(pkg->SelectedState);
	      pkgCache::State::PkgSelectedState last_dselect_state
		= (pkgCache::State::PkgSelectedState)
		    section.FindI("Dselect-State", pkg->SelectedState);
	      pkg_state.candver=candver;
	      pkg_state.forbidver=section.FindS("ForbidVer");

	      {
		const char *start, *end;
		if (section.Find("User-Tags", start, end))
		  {
		    bool parse_ok = user_tags.parse(pkg_state.user_tags, start, end,
						    package_name);
		    if (!parse_ok)
		      {
			_error->Error(_("Cannot parse user-tags for package: %s: '%s'"), pkg.FullName(true).c_str(), string(start, end).c_str());
			// do not return now, loading would hang
		      }
		  }
	      }

	      if(do_dselect && pkg->SelectedState != last_dselect_state)
		{
		  MarkFromDselect(pkg);
		  // dirty should be set to "true" so that we update
		  // the on-disk dselect state ASAP, even if no
		  // package states change as a result.
		  dirty=true;

		  // We need to update the package state from the
		  // dselect state regardless of whether we're doing
		  // initselections.  This is so that, e.g., if the
		  // user installed a package outside aptitude (so the
		  // dselect state says to install it), our internal
		  // state isn't left at "remove".  But if we aren't
		  // supposed to set up stored installs/removals, we
		  // should cancel this at the apt-get level (so the
		  // package doesn't get changed if dselect said to
		  // install it, but this isn't stored in our database
		  // for future runs).
		  //
		  // In the past, we skipped doing MarkFromDselect in
		  // this case.  BAD.
		  if(!do_initselections)
		    MarkKeep(pkg, false);
		}

	      // if the package is already in the version that we wanted to
	      // target, but pkgstates still lists as "upgrade" with a candidate
	      // version (downgrades always do, upgrades not always), mark as
	      // dirty to update pkgstates, and reset "pkg_state".  otherwise:
	      //
	      // - if pkgstates is not written at most at the end of the current
	      // session, the packages downgraded do not show up as "upgradable"
	      // until there is an update forced for other reasons (see #787658
	      // and #714429), even in subsequent invokations of aptitude
	      //
	      // - without resetting "pkg_state" it's marked for "upgrade" (or
	      // "downgrade") again down in this function, and while for
	      // "upgrades" it is not a problem, with "downgrades" it will not
	      // show as upgradable in the current interactive session.
	      //
	      //
	      // also unmark as upgrade as soon as already upgraded, even if no
	      // candidate version required in pkgstates -- see #721426
	      std::string installed_ver = pkg.CurVersion() ? pkg.CurVersion() : "";
	      if (pkg_state.upgrade)
		{
		  bool version_as_in_pkgstates = (!pkg_state.candver.empty() && (installed_ver == pkg_state.candver));
		  bool version_as_candidate = (!GetCandidateVersion(pkg).end() && !pkg.CurrentVer().end() && (GetCandidateVersion(pkg) == pkg.CurrentVer()));
		  if (version_as_in_pkgstates || (pkg_state.candver.empty() && version_as_candidate))
		    {
		      pkg_state.upgrade = false;
		      pkg_state.candver = "";
		      dirty = true;
		    }
		}
	    }

	  if (Prog)
	    {
	      // update progress, but not every time -- very expensive
	      amt += section.size();
	      int pct = (file_size > 0) ? (100*amt) / file_size : 0;
	      if ((pct % 10 == 1) && last_pct_shown != pct)
		{
		  last_pct_shown = pct;
		  Prog->OverallProgress(amt, file_size, 1, _("Reading extended state information"));
		}
	    }
	}

      // if pkgTagFile.Step() throws errors, file likely corrupt -- see #405506
      if (_error->PendingError())
	{
	  _error->Error(_("Problem parsing '%s', is it corrupt or malformed? You can try to recover from '%s.old'."),
			statefilepath.c_str(), statefilepath.c_str());
	  return false;
	}

      if (Prog)
	{
	  Prog->OverallProgress(file_size, file_size, 1, _("Reading extended state information"));
	  Prog->Done();
	}
    }

  int progress_num = 0;
  int progress_total = Head().PackageCount;
  // only update if we're going to increase 10% or so, minimum 1 (to avoid
  // divide by zero)
  int update_progress_10pct = std::max(progress_total / 10, 1);
  if (Prog)
    {
      Prog->OverallProgress(0, progress_total, 1, _("Initializing package states"));
    }

  new_package_count=0;

  pre_package_state_changed();

  // Act on them
  for(pkgCache::PkgIterator i=PkgBegin(); !i.end(); i++)
    {
      StateCache &state=(*this)[i];
      aptitude_state &estate=get_ext_state(i);

      if(initial_open) // Don't make everything "new".
	estate.new_package=false;
      else if(!i.VersionList().end() && estate.new_package)
	++new_package_count;

      switch(estate.selection_state)
	{
	case pkgCache::State::Unknown:
	  if(i.CurrentVer().end())
	    estate.selection_state=pkgCache::State::DeInstall;
	  else
	    estate.selection_state=pkgCache::State::Install;
	  break;
	case pkgCache::State::Install:
	  if(!do_initselections)
	    break;

	  // FIXME: should I check this for "unknown" packages as well?
	  // Does that even make sense??
	  if(!estate.candver.empty())
	    {
	      for(pkgCache::VerIterator ver=i.VersionList(); !ver.end(); ++ver)
		if(ver.VerStr()==estate.candver &&
		   (ver.Downloadable() ||
		    (ver == ver.ParentPkg().CurrentVer() &&
		     ver.ParentPkg()->CurrentState != pkgCache::State::ConfigFiles)))
		  SetCandidateVersion(ver);

	      MarkInstall(i, false);
	    }
	  else
	    {
	      if(i.CurrentVer().end())
		MarkInstall(i, false);
	      else
		{
		  SetReInstall(i, estate.reinstall);

		  if(estate.upgrade && state.Upgradable())
		    MarkInstall(i, false);
		}
	    }
	  break;
	case pkgCache::State::Hold:
	  if(!do_initselections)
	    break;

	  MarkKeep(i, false);
	  break;
	case pkgCache::State::DeInstall:
	  if(!do_initselections)
	    break;

	  if(!i.CurrentVer().end())
	    MarkDelete(i, false);
	  break;
	case pkgCache::State::Purge:
	  if(!do_initselections)
	    break;

	  if(!i.CurrentVer().end())
	    MarkDelete(i, true);
	  break;
	}

      if(estate.previously_auto_package)
	{
	  MarkAuto(i, true);
	  dirty = true;
	}

      if (Prog)
	{
	  // don't update on every cycle
	  if ((++progress_num % update_progress_10pct) == 1)
	    {
	      Prog->OverallProgress(progress_num, progress_total, 1, _("Initializing package states"));
	    }
	}
    }

  if (Prog)
    Prog->OverallProgress(progress_total, progress_total, 1, _("Initializing package states"));

  duplicate_cache(&backup_state);

  if(aptcfg->FindB(PACKAGE "::Auto-Upgrade", false) && do_initselections)
    mark_all_upgradable(aptcfg->FindB(PACKAGE "::Auto-Install", true),
			true, NULL);

  if (Prog)
    Prog->Done();

  read_only = (lock == -1);

  if (_error->PendingError())
    return false;
  else
    return true;
}

void aptitudeDepCache::mark_all_upgradable(bool with_autoinst,
					   bool ignore_removed,
					   undo_group *undo)
{
  if(read_only && !read_only_permission())
    {
      if(group_level == 0)
	read_only_fail();
      return;
    }

  pre_package_state_changed();

  action_group group(*this, undo);

  for(int iter=0; iter==0 || (iter==1 && with_autoinst); ++iter)
    {
      // Do this twice, only turning auto-install on the second time.
      // A reason for this is the following scenario:
      //
      // Packages A and B are installed at 1.0.  Package C is not installed.
      // Version 2.0 of each package is available.
      //
      // Version 2.0 of A depends on "C (= 2.0) | B (= 2.0)".
      //
      // Upgrading A if B is not upgraded will cause this dependency to
      // break.  Auto-install will then cheerfully fulfill it by installing
      // C.
      //
      // A real-life example of this is xemacs21, xemacs21-mule, and
      // xemacs21-nomule; aptitude would keep trying to install the mule
      // version on upgrades.
      bool do_autoinstall=(iter==1);

      std::set<pkgCache::PkgIterator> to_upgrade;
      get_upgradable(ignore_removed, to_upgrade);
      for(std::set<pkgCache::PkgIterator>::const_iterator it =
	    to_upgrade.begin(); it != to_upgrade.end(); ++it)
	{
	  pre_package_state_changed();
	  dirty = true;

	  internal_mark_install(*it, do_autoinstall, false);
	}
    }
}

void aptitudeDepCache::get_upgradable(bool ignore_removed,
				      std::set<pkgCache::PkgIterator> &upgradable)
{
  logging::LoggerPtr logger(Loggers::getAptitudeAptCache());

  LOG_TRACE(logger, "Fetching the list of upgradable packages.");

  for(pkgCache::PkgIterator p = PkgBegin(); !p.end(); ++p)
    {
      aptitude_state &estate = get_ext_state(p);

      if(p.CurrentVer().end())
	{
	  LOG_TRACE(logger, p.FullName(false) << " is not upgradable: it is not currently installed.");
	  continue;
	}

      bool do_upgrade = false;

      if(!ignore_removed)
	{
	  // allow downgrades when pinned high -- see #344700, #348679
	  do_upgrade = !is_held(p) && !GetCandidateVersion(p).end() && (GetCandidateVersion(p) != p.CurrentVer());

	  if(do_upgrade)
	    LOG_DEBUG(logger, p.FullName(false) << " is upgradable.");
	  else
	    LOG_TRACE(logger, p.FullName(false) << " is not upgradable: no newer version is available, or it is held back.");
	}
      else
	{
	  switch(estate.selection_state)
	    {
	      // This case shouldn't really happen:
	    case pkgCache::State::Unknown:
	      estate.selection_state = pkgCache::State::Install;
	      LOG_WARN(logger, p.FullName(false) << " has not been seen before, but it should have been initialized on startup.");

	      // Fall through
	    case pkgCache::State::Install:
	      // allow downgrades when pinned high -- see #344700, #348679
	      if (!is_held(p) && !GetCandidateVersion(p).end() && (GetCandidateVersion(p) != p.CurrentVer()))
		{
		  do_upgrade = true;
		  LOG_TRACE(logger, p.FullName(false) << " is upgradable.");
		}
	      else
		LOG_TRACE(logger, p.FullName(false) << " is not upgradable: no newer version is available, or it is held back.");
	      break;
	    default:
	      LOG_TRACE(logger, p.FullName(false) << " is not upgradable: its state is " << estate.selection_state << " instead of " << pkgCache::State::Install << ".");
	      break;
	    }
	}

      if(do_upgrade)
	upgradable.insert(p);
    }
}

// If fd is -1, just write unconditionally to the given fd.
//
//  FIXME: clean up the logic by having an internal "write to this fd"
// routine and an exported "ok, set up for the write and then clean up"
// routine.
bool aptitudeDepCache::save_selection_list(OpProgress* Prog,
					   const char* status_fname)
{
  // Refuse to write to disk if nothing changed and we aren't writing
  // to an unusual file
  if(!dirty && !status_fname)
    return true;

  if(lock==-1 && !status_fname)
    return true;

  // Don't write the global apt state file if we're not writing our
  // own global state.  TODO: this means that su-to-root will lose
  // automatic states! (but we couldn't do anything about it anyway
  // because we're not root and so can't write the global state file)
  // Ow, that sucks.  The solution will be to write the apt states to
  // a separate file.
  if(status_fname == NULL)
    writeStateFile(Prog, false);

  // helper class to save selection state of packages to dpkg database
  aptitude::apt::dpkg::DpkgSelections dpkg_selections;

  string statefile=_config->FindDir("Dir::Aptitude::state", STATEDIR)+"pkgstates";

  FileFd newstate;

  if(!status_fname)
    newstate.Open(statefile+".new", FileFd::WriteEmpty, 0644);
  else
    newstate.Open(status_fname, FileFd::WriteEmpty, 0644);

  // The user might have a restrictive umask -- make sure we get a
  // mode 644 file.
  fchmod(newstate.Fd(), 0644);

  if(!newstate.IsOpen())
    {
      _error->Error(_("Cannot open Aptitude state file"));
      if (Prog)
	Prog->Done();
      return false;
    }
  else
    {
      int progress_num = 0;
      int progress_total = Head().PackageCount;
      // only update if we're going to increase 10% or so, minimum 1 (to avoid
      // divide by zero)
      int update_progress_10pct = std::max(progress_total / 10, 1);
      if (Prog)
	{
	  Prog->OverallProgress(0, progress_total, 1, _("Writing extended state information"));
	}

      // save some allocations in the loop and other optimisations -- see #312920
      std::stringstream newstate_tmpbuffer;
      string line;
      string forbidstr;
      string upgradestr;
      string reinstall_str;
      string autostr;
      string tailstr;
      string user_tags;
      string select_arch;

      for(PkgIterator i=PkgBegin(); !i.end(); i++)
	if (i.VersionList().end())
	  {
	    ++progress_num;
	  }
	else
	  {
	    StateCache &state=(*this)[i];
	    aptitude_state &estate=get_ext_state(i);

	    forbidstr = (!estate.forbidver.empty()) ? ("ForbidVer: " + estate.forbidver + "\n") : "";

	    upgradestr = ((!i.CurrentVer().end()) && state.Install()) ? "Upgrade: yes\n" : "";

	    reinstall_str = (estate.reinstall) ? ("Reinstall: yes\n") : "";

	    // packages that are auto and not yet installed are marked in this
	    // way in aptitude's DB, to set the flag accordingly when installing
	    // for the first time.  apt does not save auto state on uninstalled
	    // packages.
	    //
	    // it comes originally from fix to #435079; and then to fix problem
	    // when install-auto scheduled from previous sessions (but were not
	    // installed at that point) were losing the auto flag (#563877).
	    //
	    // the flag is removed if the package is already installed, to fix
	    // having this flag and preventing code handling auto in different
	    // parts from working normally (#816497)
	    bool is_or_was_auto = ((state.Flags & Flag::Auto) != 0) || estate.previously_auto_package;
	    bool auto_new_install = (i.CurrentVer().end() &&
				     state.Install() &&
				     is_or_was_auto);
	    autostr = (auto_new_install) ? "Auto-New-Install: yes\n" : "";


	    if(state.Install() &&
	       !estate.candver.empty() &&
	       (GetCandidateVersion(i).end() ||
		GetCandidateVersion(i).VerStr() != estate.candver))
	      tailstr = "Version: " + estate.candver + "\n";
	    else
	      tailstr = "";

	    // Build the list of usertags for this package.
	    if (!estate.user_tags.empty())
	      {
		user_tags = "User-Tags:";

		// get sorted usertags so we get predictable outputs
		std::vector<std::string> sorted_pkg_user_tags = get_user_tags(i);

		// append user tags to the field
		for (const auto& tag : sorted_pkg_user_tags)
		  {
		    user_tags.push_back(' ');
		    user_tags += tag;
		  }

		user_tags.push_back('\n');
	      }
	    else
	      user_tags = "";

	    // write section for this package
	    using cw::util::ssprintf;
	    line = ssprintf("Package: %s\nArchitecture: %s\nUnseen: %s\nState: %i\nDselect-State: %i\nRemove-Reason: %i\n%s%s%s%s%s%s\n",
				      i.Name(),
                                      i.Arch(),
				      estate.new_package?"yes":"no",
				      estate.selection_state,
				      i->SelectedState,
				      estate.remove_reason,
				      upgradestr.c_str(),
				      reinstall_str.c_str(),
				      autostr.c_str(),
				      forbidstr.c_str(),
				      user_tags.c_str(),
				      tailstr.c_str());
	    newstate_tmpbuffer << line;

	    // dpkg-dselect state
	    if (estate.original_selection_state != estate.selection_state
		&& ! (estate.original_selection_state == pkgCache::State::Unknown && estate.selection_state == pkgCache::State::DeInstall) )
	      {
		// for internal reasons, apt's PkgIterator is always arch:any,
		// not arch:all.  VerIterator can be arch:all, though.
		select_arch = i.Arch();

		// all install, downgrade, upgrade, etc -- try to use candidate version
		if (state.Install() && !GetCandidateVersion(i).end())
		  {
		    select_arch = GetCandidateVersion(i).Arch();
		  }
		// all delete, purge, hold, keep etc -- try to use current version
		else if (! i.CurrentVer().end())
		  {
		    select_arch = i.CurrentVer().Arch();
		  }

		// add to selections to save
		dpkg_selections.add(i.Name(), select_arch, estate.selection_state);

		// new state is also the original now
		estate.original_selection_state = estate.selection_state;
	      }

	    if (Prog)
	      {
		// don't update on every cycle
		if ((++progress_num % update_progress_10pct) == 1)
		  {
		    Prog->OverallProgress(progress_num, progress_total, 1, _("Writing extended state information"));
		  }
	      }
	  }

      if (Prog)
	Prog->OverallProgress(progress_total, progress_total, 1, _("Writing extended state information"));

      if (newstate.Failed() ||
	  !newstate.Write(newstate_tmpbuffer.str().c_str(), newstate_tmpbuffer.str().size()))
	{
	  _error->Error(_("Couldn't write state file"));
	  newstate.Close();

	  if (!status_fname)
	    unlink((statefile+".new").c_str());

	  if (Prog)
	    Prog->Done();
	  return false;
	}
      newstate.Close();

      // FIXME!  This potentially breaks badly on NFS.. (?) -- actually, it
      //       wouldn't be harmful; you'd just get gratuitous errors..
      if(!status_fname)
	{
	  string oldstr(statefile + ".old"), newstr(statefile + ".new");

	  if(unlink(oldstr.c_str()) != 0 && errno != ENOENT)
	    {
	      _error->Errno("save_selection_list", _("failed to remove %s"), oldstr.c_str());
	      if (Prog)
		Prog->Done();
	      return false;
	    }

	  if(link(statefile.c_str(), oldstr.c_str()) != 0 && errno != ENOENT)
	    {
	      _error->Errno("save_selection_list", _("failed to rename %s to %s"),
			    statefile.c_str(), (statefile + ".old").c_str());
	      if (Prog)
		Prog->Done();
	      return false;
	    }

	  if(rename(newstr.c_str(), statefile.c_str()) != 0)
	    {
	      _error->Errno("save_selection_list", _("couldn't replace %s with %s"), statefile.c_str(), newstr.c_str());
	      if (Prog)
		Prog->Done();
	      return false;
	    }
	}

      // save selection state to dpkg database
      //
      // \todo Need to release lock before the operation, otherwise internal
      // calls to dpkg fail with:
      //
      //   'error: dpkg status database is locked by another process'
      //
      // Review if dpkg provides a better way to do this, currently (Sep 2015)
      // it does not.
      apt_cache_file->ReleaseLock();
      bool dpkg_selections_saved = dpkg_selections.save_selections();
      if (! apt_cache_file->GainLock())
	_error->Error(_("Could not regain the system lock!  (Perhaps another apt or dpkg is running?)"));
      if (! dpkg_selections_saved)
	{
	  _error->Error(_("failed to save selections to dpkg database"));
	  if (Prog)
	    Prog->Done();
	  return false;
	}
    }

  if (Prog)
    Prog->Done();

  return true;
}

void aptitudeDepCache::set_new_flag(const pkgCache::PkgIterator& pkg,
				    bool is_new)
{
  if (read_only && !read_only_permission())
    {
      if (group_level == 0)
	read_only_fail();
      return;
    }

  aptitude_state& estate = get_ext_state(pkg);

  if (estate.new_package && !is_new)
    {
      --new_package_count;
      estate.new_package = is_new;
    }
  else if (!estate.new_package && is_new)
    {
      ++new_package_count;
      estate.new_package = is_new;
    }
}

void aptitudeDepCache::forget_new(undoable **undoer)
{
  std::vector<pkgCache::PkgIterator> pkg_its;
  for (pkgCache::PkgIterator it = PkgBegin(); !it.end(); ++it)
    {
      pkg_its.push_back(it);
    }

  forget_new(undoer, pkg_its);
}

void aptitudeDepCache::forget_new(undoable **undoer, const std::vector<pkgCache::PkgIterator>& pkg_its)
{
  if (read_only && !read_only_permission())
    {
      if (group_level == 0)
	read_only_fail();
      return;
    }

  forget_undoer* undo = undoer ? new forget_undoer(this) : NULL;

  for (pkgCache::PkgIterator i : pkg_its)
    {
      if (package_states[i->ID].new_package)
	{
	  if (package_states[i->ID].new_package && (new_package_count > 0))
	    --new_package_count;

	  dirty = true;
	  package_states[i->ID].new_package = false;
	  if (undo)
	    undo->add_item(i);
	}
    }

  if (undoer && undo && !undo->empty())
    *undoer = undo;
  else
    delete undo;

  duplicate_cache(&backup_state);

  // Umm, is this a hack? dunno.
  cache_reloaded();
}

undoable *aptitudeDepCache::state_restorer(PkgIterator pkg, StateCache &state, aptitude_state &ext_state)
{
  return new apt_undoer(pkg, state.Mode, state.Flags, state.iFlags,
			ext_state.remove_reason,
			ext_state.selection_state,
			ext_state.forbidver, this);
}

void aptitudeDepCache::cleanup_after_change(undo_group *undo,
					    std::set<pkgCache::PkgIterator> *changed_packages,
					    bool alter_stickies)
  // Finds any packages whose states have changed and: (a) updates the
  // selected_state if it's not already updated; (b) adds an item to the
  // undo group.
{
  // We get here with NULL backup_state in certain very early failures
  // (e.g., when someone else is holding a lock).  In this case we
  // don't know what the previous state was, so we can't possibly
  // build a collection of undoers to return to it or find out which
  // packages changed relative to it.
  if(backup_state.PkgState == NULL ||
     backup_state.DepState == NULL ||
     backup_state.AptitudeState == NULL)
    return;

  for(pkgCache::PkgIterator pkg=PkgBegin(); !pkg.end(); pkg++)
    {
      // Set to true if we should signal that this package's visible
      // state changed.
      bool visibly_changed = false;

      if(PkgState[pkg->ID].Mode!=backup_state.PkgState[pkg->ID].Mode ||
	 (PkgState[pkg->ID].Flags & pkgCache::Flag::Auto) != (backup_state.PkgState[pkg->ID].Flags & pkgCache::Flag::Auto) ||
	 package_states[pkg->ID].selection_state!=backup_state.AptitudeState[pkg->ID].selection_state ||
	 package_states[pkg->ID].reinstall!=backup_state.AptitudeState[pkg->ID].reinstall ||
	 package_states[pkg->ID].remove_reason!=backup_state.AptitudeState[pkg->ID].remove_reason ||
	 package_states[pkg->ID].forbidver!=backup_state.AptitudeState[pkg->ID].forbidver)
	{
	  // Technically this could be invoked only when we're really
	  // about to change a package's state, but placing it here
	  // should avoid signalling changes unnecessarily while still
	  // signalling a change whenever something changed.
	  //
	  // You could argue that this invocation of
	  // pre_package_state_changed makes all other unnecessary,
	  // but I think they should be left in for safety's sake.
	  // Forgetting to call pre_package_state_changed can lead to
	  // hard to track down bugs like #432411.
	  pre_package_state_changed();

	  if(alter_stickies &&
	     PkgState[pkg->ID].Mode!=backup_state.PkgState[pkg->ID].Mode &&
	     package_states[pkg->ID].selection_state==backup_state.AptitudeState[pkg->ID].selection_state)
	    // Catch packages which switched without altering their Aptitude
	    // selection mode
	    {
	      switch(PkgState[pkg->ID].Mode)
		{
		case ModeDelete:
		  if(package_states[pkg->ID].selection_state!=pkgCache::State::DeInstall)
		    {
		      if(!pkg.CurrentVer().end())
			package_states[pkg->ID].remove_reason=libapt;

		      package_states[pkg->ID].selection_state=pkgCache::State::DeInstall;
		    }
		  break;
		case ModeKeep:
		  if(!pkg.CurrentVer().end())
		    package_states[pkg->ID].selection_state=pkgCache::State::Install;
		  else if(pkg->CurrentState==pkgCache::State::NotInstalled)
		    package_states[pkg->ID].selection_state=pkgCache::State::Purge;
		  else
		    package_states[pkg->ID].selection_state=pkgCache::State::DeInstall;
		  break;
		case ModeInstall:
		  if(package_states[pkg->ID].selection_state!=pkgCache::State::Install)
		    package_states[pkg->ID].selection_state=pkgCache::State::Install;
		  break;
		}
	    }

	  visibly_changed = true;

	  if(undo)
	    undo->add_item(state_restorer(pkg,
					  backup_state.PkgState[pkg->ID],
					  backup_state.AptitudeState[pkg->ID]));
	}
      // Detect things like broken-ness and other changes that
      // shouldn't trigger undo but might trigger updating the
      // package's display.
      else if(PkgState[pkg->ID].Flags != backup_state.PkgState[pkg->ID].Flags ||
	      PkgState[pkg->ID].DepState != backup_state.PkgState[pkg->ID].DepState ||
	      PkgState[pkg->ID].CandidateVer != backup_state.PkgState[pkg->ID].CandidateVer ||
	      PkgState[pkg->ID].Marked != backup_state.PkgState[pkg->ID].Marked ||
	      PkgState[pkg->ID].Garbage != backup_state.PkgState[pkg->ID].Garbage ||
	      package_states[pkg->ID].user_tags != backup_state.AptitudeState[pkg->ID].user_tags ||
	      package_states[pkg->ID].new_package != backup_state.AptitudeState[pkg->ID].new_package)
	{
	  visibly_changed = true;
	}

      if(visibly_changed && changed_packages != NULL)
	{
	  changed_packages->insert(pkg);
	}
    }
}

void aptitudeDepCache::mark_install(const PkgIterator &Pkg,
				    bool AutoInst,
				    bool ReInstall,
				    undo_group *undo)
{
  if(read_only && !read_only_permission())
    {
      if(group_level == 0)
	read_only_fail();
      return;
    }

  action_group group(*this, undo);

  pre_package_state_changed();

  internal_mark_install(Pkg, AutoInst, ReInstall);
}

void aptitudeDepCache::internal_mark_install(const PkgIterator &Pkg,
					     bool AutoInst,
					     bool ReInstall)
{
  dirty=true;

  bool set_to_manual =
    ((Pkg.CurrentVer().end()  && !(*this)[Pkg].Install()) ||
     (!Pkg.CurrentVer().end() && (*this)[Pkg].Delete() && get_ext_state(Pkg).remove_reason == unused));

  // MarkInstall and friends like to modify the auto flag, so save it
  // here and restore it afterwards:
  bool previously_auto = ((*this)[Pkg].Flags & Flag::Auto) != 0;

  bool final_auto = previously_auto;
  if (set_to_manual)
    final_auto = false;

  if(!ReInstall)
    {
      pkgDepCache::MarkInstall(Pkg, AutoInst);
    }
  else
    pkgDepCache::MarkKeep(Pkg, AutoInst);

  pkgDepCache::SetReInstall(Pkg, ReInstall);

  MarkAuto(Pkg, final_auto);

  get_ext_state(Pkg).selection_state=pkgCache::State::Install;
  get_ext_state(Pkg).reinstall=ReInstall;
  get_ext_state(Pkg).forbidver="";
  get_ext_state(Pkg).previously_auto_package = final_auto;
}

void aptitudeDepCache::mark_delete(const PkgIterator &Pkg,
				   bool Purge,
				   bool unused_delete,
				   undo_group *undo)
{
  // refuse to remove itself -- see #319782, #568548
  //
  // but allow purge if already removed (#822331)
  if (!Pkg.end() && is_installed(Pkg) && string("aptitude") == Pkg.Name())
    {
      _error->Error(_("Cannot remove aptitude within aptitude"));
      return;
    }

  if(read_only && !read_only_permission())
    {
      if(group_level == 0)
	read_only_fail();
      return;
    }

  action_group group(*this, undo);

  pre_package_state_changed();

  internal_mark_delete(Pkg, Purge, unused_delete);
}

void aptitudeDepCache::internal_mark_delete(const PkgIterator &Pkg,
					    bool Purge,
					    bool unused_delete)
{
  std::vector<unsigned int> unused_already_visited;
  internal_mark_delete(Pkg, Purge, unused_delete, unused_already_visited);
}

void aptitudeDepCache::internal_mark_delete(const PkgIterator &Pkg,
					    bool Purge,
					    bool unused_delete,
					    std::vector<unsigned int>& unused_already_visited)
{
  // refuse to remove itself -- see #319782, #568548
  //
  // but allow purge if already removed (#822331)
  if (!Pkg.end() && is_installed(Pkg) && string("aptitude") == Pkg.Name())
    {
      _error->Error(_("Cannot remove aptitude within aptitude"));
      return;
    }

  // honour ::Purge-Unused in the main entry point for removing packages, it
  // should catch cases of automatically installed and unused packages not
  // purged (#724034 and others)
  bool purge_unused = aptcfg->FindB(PACKAGE "::Purge-Unused", false);
  if (unused_delete && purge_unused)
    {
      Purge = true;
    }

  dirty=true;

  bool previously_to_delete=(*this)[Pkg].Delete();

  pkgDepCache::MarkDelete(Pkg, Purge);
  pkgDepCache::SetReInstall(Pkg, false);

  get_ext_state(Pkg).selection_state=(Purge?pkgCache::State::Purge:pkgCache::State::DeInstall);
  get_ext_state(Pkg).reinstall=false;

  if(!previously_to_delete)
    {
      if(unused_delete)
	get_ext_state(Pkg).remove_reason=unused;
      else
	get_ext_state(Pkg).remove_reason=manual;
    }

  // after marking a package for delete, check all the dependencies to see if
  // they can be pro-actively marked for removal (because no other package
  // depends on them).
  //
  // apt sometimes marks them as "Garbage", and so sweep() routines catch them
  // later and they get removed within the same aptitude run, but sometimes it
  // is not so, and they are only marked for removal in future sessions (see
  // #637257, #478116 -- among many others).  so here is an attempt to directly
  // mark for delete depends which seem safe to remove, because they are
  // automatically installed and don't have other remaining rdeps.

  if (!aptcfg->FindB(PACKAGE "::Delete-Unused", true))
    return;

  if (Pkg.CurrentVer().end())
    return;

  // to avoid endless recursion/crashes, check if this package has already been
  // visited for this purpose (the container has to be empty at the start of
  // each run)
  auto it = std::find(unused_already_visited.begin(), unused_already_visited.end(), Pkg->ID);
  if (it == std::end(unused_already_visited))
    {
      // not previously visited, register
      unused_already_visited.push_back(Pkg->ID);
    }
  else
    {
      // already visited
      return;
    }

  // from now and for the remaining of this function, these are "unused
  // deletes", so set variable accordingly
  unused_delete = true;

  // not to purge unused lightly, can cause data loss -- see comments in #661188
  Purge = aptcfg->FindB(PACKAGE "::Purge-Unused", false);

  bool keep_recommends_installed =
    aptcfg->FindB("APT::Install-Recommends", true)
    || aptcfg->FindB("APT::AutoRemove::RecommendsImportant", true)
    || aptcfg->FindB(PACKAGE "::Keep-Recommends", false);
  bool keep_suggests_installed   =
    aptcfg->FindB("APT::AutoRemove::SuggestsImportant", true)
    || aptcfg->FindB(PACKAGE "::Keep-Suggests", false);

  for (pkgCache::DepIterator dep = Pkg.CurrentVer().DependsList(); !dep.end(); ++dep)
    {
      // if not a valid target, not consider
      auto dep_pkg = dep.TargetPkg();
      if (dep_pkg.end())
	{
	  continue;
	}

      // consider only these type of dependencies
      if (! ((dep->Type == pkgCache::Dep::Depends) ||
	     (dep->Type == pkgCache::Dep::PreDepends) ||
	     (dep->Type == pkgCache::Dep::Recommends && keep_recommends_installed) ||
	     (dep->Type == pkgCache::Dep::Suggests   && keep_suggests_installed)))
	{
	  continue;
	}

      // special case for virtual packages
      //
      // if the considered dependency is a virtual package, look if both the
      // virtual and provider packages can be removed (if they do not have other
      // rdeps installed, etc)
      if (is_virtual(dep_pkg))
	{
	  // consider all provided virtual packages
	  for (pkgCache::PrvIterator dep_prv = dep_pkg.ProvidesList(); !dep_prv.end(); ++dep_prv)
	    {
	      // virtual package itself
	      if (! can_remove_autoinstalled(dep_pkg, (*this), keep_recommends_installed, keep_suggests_installed)) {
		continue;
	      }

	      // real package
	      if (! is_auto_installed((*this)[dep_prv.OwnerPkg()]) ||
		  ! can_remove_autoinstalled(dep_prv.OwnerPkg(), (*this), keep_recommends_installed, keep_suggests_installed)) {
		continue;
	      }

	      // if we reach here, can delete the real package providing the
	      // dependency
	      internal_mark_delete(dep_prv.OwnerPkg(), Purge, unused_delete, unused_already_visited);
	    }

	  // it was a virtual package -- so stop processing the considered
	  // dependency
	  continue;
	}

      // if not virtual and no current version installed, no need to consider
      auto dep_ver = dep_pkg.CurrentVer();
      if (dep_ver.end())
	{
	  continue;
	}

      // consider only automatically installed dependencies, or dependencies
      // which are not required on their own right
      pkgDepCache::StateCache& state = (*this)[dep_pkg];
      bool is_auto = is_auto_installed(state);
      bool is_not_required = ! ((state.Flags & pkgCache::Flag::Essential) ||
				(state.Flags & pkgCache::Flag::Important) ||
				(dep_ver->Priority == pkgCache::State::Important) ||
				(dep_ver->Priority == pkgCache::State::Required));

      if (is_installed(dep_pkg) && is_auto && is_not_required)
	{
	  if (! can_remove_autoinstalled(dep_pkg, (*this), keep_recommends_installed, keep_suggests_installed)) {
	    continue;
	  }

	  internal_mark_delete(dep_pkg, Purge, unused_delete, unused_already_visited);
	}
    }
}

void aptitudeDepCache::mark_keep(const PkgIterator &Pkg, bool Automatic, bool SetHold, undo_group *undo)
{
  if(read_only && !read_only_permission())
    {
      if(group_level == 0)
	read_only_fail();
      return;
    }

  action_group group(*this, undo);

  pre_package_state_changed();

  internal_mark_keep(Pkg, Automatic, SetHold);
}

void aptitudeDepCache::internal_mark_keep(const PkgIterator &Pkg, bool Automatic, bool SetHold)
{
  dirty=true;


  // If the package is currently installed and is being garbage
  // collected, switch it to manual mode.  We check
  // pkg.CurrentVer().end() to avoid fiddling with packages that are
  // in the ConfigFiles state and are being purged.
  bool was_garbage_removed =
    (*this)[Pkg].Delete() &&
    !Pkg.CurrentVer().end() &&
    get_ext_state(Pkg).remove_reason == unused;

  if(was_garbage_removed)
    MarkAuto(Pkg, false);

  set_candidate_version(GetCandidateVersion(Pkg), NULL);

  pkgDepCache::MarkKeep(Pkg, false, !Automatic);
  pkgDepCache::SetReInstall(Pkg, false);
  get_ext_state(Pkg).reinstall=false;
  get_ext_state(Pkg).forbidver="";

  // explicitly mark auto-installed, sometimes apt does not apply it properly in
  // some cases -- see #508428
  pkgDepCache::MarkAuto(Pkg, Automatic);

  if(Pkg.CurrentVer().end())
    {
      if((*this)[Pkg].iFlags&Purge)
	get_ext_state(Pkg).selection_state=pkgCache::State::Purge;
      else
	get_ext_state(Pkg).selection_state=pkgCache::State::DeInstall;
    }
  else if(SetHold)
    get_ext_state(Pkg).selection_state=pkgCache::State::Hold;
  else
    get_ext_state(Pkg).selection_state=pkgCache::State::Install;
}

void aptitudeDepCache::set_candidate_version(const VerIterator &ver,
					     undo_group *undo)
{
  if(read_only && !read_only_permission())
    {
      if(group_level == 0)
	read_only_fail();
      return;
    }

  dirty=true;

  if(!ver.end() &&
     (ver.Downloadable() ||
      (ver == ver.ParentPkg().CurrentVer() &&
       ver.ParentPkg()->CurrentState != pkgCache::State::ConfigFiles)))
    {
      pre_package_state_changed();


      // Make the package manually installed if it was being
      // garbage-collected.
      bool set_to_manual =
	(ver.ParentPkg().CurrentVer().end() && !(*this)[ver.ParentPkg()].Install()) ||
	(!ver.ParentPkg().CurrentVer().end() &&
	 (*this)[ver.ParentPkg()].Delete() &&
	 get_ext_state(ver.ParentPkg()).remove_reason == unused);

      if(set_to_manual)
	MarkAuto(ver.ParentPkg(), false);

      VerIterator prev=(*this)[(ver.ParentPkg())].CandidateVerIter(GetCache());

      aptitude_state &estate = get_ext_state(ver.ParentPkg());

      if(ver!=GetCandidateVersion(ver.ParentPkg()))
	estate.candver=ver.VerStr();
      else
	estate.candver="";

      estate.selection_state = pkgCache::State::Install;

      SetCandidateVersion(ver);

      if(group_level == 0)
	{
	  if(undo)
	    undo->add_item(new candver_undoer(prev, this));

	  MarkAndSweep();

	  //if(BrokenCount()>0)
	  //create_resolver();
	  //
	  // EW - rely on the fact that mark_and_sweep implicitly calls
	  // begin/end_action_group(), which in turn does just this.

	  package_state_changed();
	}
    }
}

void aptitudeDepCache::forbid_upgrade(const PkgIterator &pkg,
				      string verstr, undo_group *undo)
{
  if(read_only && !read_only_permission())
    {
      if(group_level == 0)
	read_only_fail();
      return;
    }

  aptitude_state &estate=get_ext_state(pkg);

  if(verstr!=estate.forbidver)
    {
      action_group group(*this, undo);

      pre_package_state_changed();

      pkgCache::VerIterator candver=(*this)[pkg].CandidateVerIter(*this);

      dirty=true;

      estate.forbidver=verstr;
      if(!candver.end() && candver.VerStr()==verstr && (*this)[pkg].Install())
	MarkKeep(pkg, false);
    }
}

void aptitudeDepCache::mark_single_install(const PkgIterator &Pkg, undo_group *undo)
{
  if(read_only && !read_only_permission())
    {
      if(group_level == 0)
	read_only_fail();
      return;
    }

  action_group group(*this, undo);

  pre_package_state_changed();
  dirty=true;

  for(PkgIterator i=PkgBegin(); !i.end(); i++)
    pkgDepCache::MarkKeep(i, true);


  bool set_to_manual =
    ((Pkg.CurrentVer().end()  && !(*this)[Pkg].Install()) ||
     (!Pkg.CurrentVer().end() && (*this)[Pkg].Delete() && get_ext_state(Pkg).remove_reason == unused));

  if(set_to_manual)
    MarkAuto(Pkg, false);

  internal_mark_install(Pkg, true, false);
}

void aptitudeDepCache::mark_auto_installed(const PkgIterator &Pkg,
					   bool set_auto,
					   undo_group *undo)
{
  if(read_only && !read_only_permission())
    {
      if(group_level == 0)
	read_only_fail();
      return;
    }

  // it is faster to check first
  bool current_value = is_auto_installed((*this)[Pkg]);
  if (set_auto != current_value)
    {
      action_group group(*this, undo);

      pre_package_state_changed();
      dirty=true;

      MarkAuto(Pkg, set_auto);
    }
}

// Undoers for the tag manipulators below.
namespace
{
  class attach_user_tag_undoer : public undoable
  {
    aptitudeDepCache *parent;
    pkgCache::PkgIterator pkg;
    std::string tag;

  public:
    attach_user_tag_undoer(aptitudeDepCache *_parent,
		      const pkgCache::PkgIterator &_pkg,
		      const std::string &_tag)
      : parent(_parent), pkg(_pkg), tag(_tag)
    {
    }

    void undo()
    {
      parent->detach_user_tag(pkg, tag, NULL);
    }
  };

  class detach_user_tag_undoer : public undoable
  {
    aptitudeDepCache *parent;
    pkgCache::PkgIterator pkg;
    std::string tag;

  public:
    detach_user_tag_undoer(aptitudeDepCache *_parent,
			   const pkgCache::PkgIterator &_pkg,
			   const std::string &_tag)
      : parent(_parent), pkg(_pkg), tag(_tag)
    {
    }

    void undo()
    {
      parent->attach_user_tag(pkg, tag, NULL);
    }
  };
}

bool aptitudeDepCache::attach_user_tag(const PkgIterator &pkg,
				       const std::string &tag,
				       undo_group *undo)
{
  if(read_only && !read_only_permission())
    {
      if(group_level == 0)
	read_only_fail();
      return false;
    }

  // check for valid tag
  if ( ! user_tags.check_valid(tag) )
    {
      return false;
    }

  // find the tag in cache or add it
  user_tag_reference ref;
  user_tags.add(tag, ref);

  // try to add to package
  aptitude_state& estate = get_ext_state(pkg);
  auto found = estate.user_tags.find(user_tag{ref});
  if (found != estate.user_tags.end())
    {
      // tag already present, return "OK"
      _error->Notice(_("User-tag '%s' already present for %s"), tag.c_str(), pkg.FullName(true).c_str());
      return true;
    }
  else
    {
      auto insert_result = estate.user_tags.insert(user_tag{ref});
      if (insert_result.second)
	{
	  dirty = true;
	  if (undo != NULL)
	    undo->add_item(new attach_user_tag_undoer(this, pkg, tag));

	  return true;
	}
      else
	{
	  _error->Error(_("Could not add user-tag '%s' to package %s"), tag.c_str(), pkg.FullName(true).c_str());
	  return false;
	}
    }
}

bool aptitudeDepCache::detach_user_tag(const PkgIterator &pkg,
				       const std::string &tag,
				       undo_group *undo)
{
  if(read_only && !read_only_permission())
    {
      if(group_level == 0)
	read_only_fail();
      return false;
    }

  // check for valid tag
  if ( ! user_tags.check_valid(tag) )
    {
      return false;
    }

  user_tag_reference tag_ref = user_tags.get_ref(tag);
  if (tag_ref < 0)
    {
      _error->Error(_("Could not find valid user-tag '%s'"), tag.c_str());
      return false;
    }
  else
    {
      std::set<user_tag>::size_type num_erased = get_ext_state(pkg).user_tags.erase(user_tag{tag_ref});
      if (num_erased > 0)
	{
	  dirty = true;
	  if(undo != NULL)
	    undo->add_item(new detach_user_tag_undoer(this, pkg, tag));

	  return true;
	}
      else
	{
	  _error->Error(_("Could not remove user-tag '%s' from package %s"), tag.c_str(), pkg.FullName(true).c_str());
	  return false;
	}
    }
}

std::vector<std::string> aptitudeDepCache::get_user_tags(const PkgIterator& pkg)
{
  // sanity check
  if (pkg.end())
    return {};

  auto estate = get_ext_state(pkg);

  std::vector<std::string> all_tags;
  all_tags.reserve(estate.user_tags.size());

  for (const auto& it : estate.user_tags)
    {
      all_tags.push_back(user_tags.deref_user_tag(it));
    }

  std::sort(all_tags.begin(), all_tags.end());

  return all_tags;
}

bool aptitudeDepCache::all_upgrade(bool with_autoinst, undo_group *undo)
{
  if(read_only && !read_only_permission())
    {
      if(group_level == 0)
	read_only_fail();
      return false;
    }

  action_group group(*this, undo);

  pre_package_state_changed();

  pkgProblemResolver fixer(this);

  if(BrokenCount()!=0)
    return false;

  for(pkgCache::PkgIterator pkg=PkgBegin(); !pkg.end(); ++pkg)
    {
      if((*this)[pkg].Install())
	fixer.Protect(pkg);

      if(!is_held(pkg) &&
	 !pkg.CurrentVer().end() && !(*this)[pkg].Install())
	MarkInstall(pkg, with_autoinst);
    }

  bool rval=fixer.ResolveByKeep();

  return rval;
}

bool aptitudeDepCache::try_fix_broken(pkgProblemResolver &fixer, undo_group *undo)
{
  if(read_only && !read_only_permission())
    {
      if(group_level == 0)
	read_only_fail();
      return false;
    }

  action_group group(*this, undo);

  pre_package_state_changed();
  dirty=true;
  bool founderr=false;
  if(!fixer.Resolve(true))
    founderr=true;

  if(founderr)
    _error->Error(_("Unable to correct dependencies, some packages cannot be installed"));

  return !founderr;
}

bool aptitudeDepCache::try_fix_broken(undo_group *undo)
{
  if(read_only && !read_only_permission())
    {
      if(group_level == 0)
	read_only_fail();
      return false;
    }

  pkgProblemResolver fixer(this);
  pre_package_state_changed();
  for(pkgCache::PkgIterator i=PkgBegin(); !i.end(); i++)
    {
      fixer.Clear(i);
      if(!i.CurrentVer().end() &&
	 get_ext_state(i).selection_state==pkgCache::pkgCache::State::Hold)
	fixer.Protect(i);
      else
	{
	  pkgDepCache::StateCache &state=(*this)[i];
	  if(state.InstBroken() || state.NowBroken())
            internal_mark_install(i, true, false);
	  else if(state.Delete())
	    fixer.Remove(i);
	}
    }

  return try_fix_broken(fixer, undo);
}

/** Update the given package's aptitude state based on its state
 *  according to dpkg/dselect.
 *
 *  \param Pkg the package to modify.
 */
void aptitudeDepCache::MarkFromDselect(const PkgIterator &Pkg)
{
  if(read_only && !read_only_permission())
    {
      if(group_level == 0)
	read_only_fail();
      return;
    }

  aptitude_state &state=get_ext_state(Pkg);

  state.original_selection_state = static_cast<pkgCache::State::PkgSelectedState>(Pkg->SelectedState);

  if(Pkg->SelectedState!=state.selection_state)
    {
      switch(Pkg->SelectedState)
	{
	case pkgCache::State::Purge:
	  if( (!Pkg.CurrentVer().end()) || !((*this)[Pkg].iFlags&Purge) )
	    mark_delete(Pkg, true, false, NULL);
	  else
	    mark_keep(Pkg, false, false, NULL);
	  break;
	case pkgCache::State::Unknown:
	  // previously ignored (no action, just a break), it was causing
	  // problems not respecting actions taken with other tools (see #328616
	  // and duplicates).  so instead, if it's unknown to dpkg, mark it for
	  // removal -- moved next to DeInstall and fall through.
	  //
	  // ('purge' is more dangerous, one could loose configuration files if
	  // accidentally something goes wrong)
	  //
	  //break;
	case pkgCache::State::DeInstall:
	  if(!Pkg.CurrentVer().end())
	    mark_delete(Pkg, false, false, NULL);
	  else
	    mark_keep(Pkg, false, false, NULL);
	  break;
	case pkgCache::State::Hold:
	  if(!Pkg.CurrentVer().end())
	    mark_keep(Pkg, false, true, NULL);
	  break;
	case pkgCache::State::Install:
	  if(Pkg.CurrentVer().end())
	    mark_install(Pkg, false, false, NULL);
	  else
	    mark_keep(Pkg, false, false, NULL);
	  break;
	}
    }
}

void aptitudeDepCache::duplicate_cache(apt_state_snapshot *target)
  // Remember: the tables in the target have to be correctly sized!
{
  if(!target->PkgState)
    target->PkgState=new StateCache[Head().PackageCount];
  if(!target->DepState)
    target->DepState=new unsigned char[Head().DependsCount];
  if(!target->AptitudeState)
    target->AptitudeState=new aptitude_state[Head().PackageCount];

  memcpy(target->PkgState, PkgState, sizeof(StateCache)*Head().PackageCount);
  memcpy(target->DepState, DepState, sizeof(char)*Head().DependsCount);
  // memcpy doesn't work here because the aptitude_state structure
  // contains a std::string.  (would it be worthwhile/possible to
  // change things so that it doesn't?)
  for(unsigned int i=0; i<Head().PackageCount; ++i)
    target->AptitudeState[i]=package_states[i];

  target->iUsrSize=iUsrSize;
  target->iDownloadSize=iDownloadSize;
  target->iInstCount=iInstCount;
  target->iDelCount=iDelCount;
  target->iKeepCount=iKeepCount;
  target->iBrokenCount=iBrokenCount;
  target->iBadCount=iBadCount;
}

// Helpers for aptitudeDepCache::sweep().
namespace
{
  // Remove reverse dependencies of the given version from the set of
  // reinstated packages.  All the packages in the set are assumed to
  // be installed at their current version when checking dependencies.
  void remove_reverse_current_versions(std::set<pkgCache::PkgIterator> &reinstated,
				       pkgCache::VerIterator bad_ver)
  {
    logging::LoggerPtr logger(Loggers::getAptitudeAptCache());
    LOG_TRACE(logger, "Removing reverse dependencies of "
	      << bad_ver.ParentPkg().FullName(false) << " "
	      << bad_ver.VerStr() << " from the reinstate set.");
    // Follow direct revdeps.
    for(pkgCache::DepIterator dep = bad_ver.ParentPkg().RevDependsList();
	!dep.end(); ++dep)
      {
	// Skip self-deps.
	if(dep.ParentPkg() == bad_ver.ParentPkg())
	  continue;

	// Skip packages that aren't in the reinstate set.
	if(reinstated.find(dep.ParentPkg()) == reinstated.end())
	  continue;

	if((dep->Type == pkgCache::Dep::Depends ||
	    dep->Type == pkgCache::Dep::PreDepends) &&
	   dep.ParentVer() == dep.ParentPkg().CurrentVer() &&
	   _system->VS->CheckDep(bad_ver.VerStr(),
				 dep->CompareOp,
				 dep.TargetVer()))
	  {
	    LOG_DEBUG(logger,
		      "Not reinstating " << dep.ParentPkg().FullName(false)
		      << " due to its dependency on "
		      << bad_ver.ParentPkg().FullName(false)
		      << " " << bad_ver.VerStr());
	    reinstated.erase(dep.ParentPkg());
	    remove_reverse_current_versions(reinstated, dep.ParentVer());
	  }
      }

    // Follow indirect revdeps.
    for(pkgCache::PrvIterator prv = bad_ver.ProvidesList();
	!prv.end(); ++prv)
      for(pkgCache::DepIterator dep = prv.ParentPkg().RevDependsList();
	  !dep.end(); ++dep)
	{
	  // Skip self-deps.
	  if(dep.ParentPkg() == bad_ver.ParentPkg())
	    continue;

	  // Skip packages that aren't in the reinstate set.
	  if(reinstated.find(dep.ParentPkg()) == reinstated.end())
	    continue;

	  if((dep->Type == pkgCache::Dep::Depends ||
	      dep->Type == pkgCache::Dep::PreDepends) &&
	     dep.ParentVer() == dep.ParentPkg().CurrentVer() &&
	     _system->VS->CheckDep(prv.ProvideVersion(),
				   dep->CompareOp,
				   dep.TargetVer()))
	    {
	      LOG_DEBUG(logger,
			"Not reinstating " << dep.ParentPkg().FullName(false)
			<< " due to its dependency on "
			<< bad_ver.ParentPkg().FullName(false)
			<< " " << bad_ver.VerStr()
			<< " via the virtual package "
			<< prv.ParentPkg().Name());
	      reinstated.erase(dep.ParentPkg());
	      remove_reverse_current_versions(reinstated, dep.ParentVer());
	    }
	}
  }

  // Given a package that we know is not orphaned, add all the
  // transitive dependencies of its current version that are in
  // "reinstated" to the not-orphaned set.  (the condition on
  // reinstated is so we don't pick the wrong branch of an OR)
  void trace_not_orphaned(const pkgCache::PkgIterator &notOrphan,
			  const std::set<pkgCache::PkgIterator> &reinstated,
			  pkgDepCache &cache,
			  std::set<pkgCache::PkgIterator> &not_orphaned)
  {
    logging::LoggerPtr logger(Loggers::getAptitudeAptCache());

    if(not_orphaned.find(notOrphan) != not_orphaned.end())
      {
	LOG_TRACE(logger, "Ignoring " << notOrphan.FullName(false)
		  << ": it was already visited.");
	return;
      }

    // Sanity-check.
    if(notOrphan->CurrentState == pkgCache::State::NotInstalled ||
       notOrphan->CurrentState == pkgCache::State::ConfigFiles ||
       notOrphan.CurrentVer().end())
      {
	LOG_WARN(logger, "Sanity-check failed: assuming the package "
		 << notOrphan.FullName(false)
		 << " is orphaned, since it is not currently installed.");
	return;
      }

    if(reinstated.find(notOrphan) == reinstated.end())
      {
	LOG_DEBUG(logger, "Treating the package "
		  << notOrphan.FullName(false)
		  << " as an orphan, since it is not in the reinstatement set.");
	return;
      }

    LOG_DEBUG(logger, "The package " << notOrphan.FullName(false)
	      << " is not an orphan.");

    not_orphaned.insert(notOrphan);
    for(pkgCache::DepIterator dep = notOrphan.CurrentVer().DependsList();
	!dep.end(); ++dep)
      {
	if(!cache.IsImportantDep(dep))
	  continue;

	// If the target is installed, check if it matches the dep and
	// is reinstated.
	pkgCache::PkgIterator targetPkg(dep.TargetPkg());
	if(!(targetPkg->CurrentState == pkgCache::State::NotInstalled ||
	     targetPkg->CurrentState == pkgCache::State::ConfigFiles))
	  {
	    if(_system->VS->CheckDep(targetPkg.CurrentVer().VerStr(),
				     dep->CompareOp,
				     dep.TargetVer()))
	      trace_not_orphaned(targetPkg,
				 reinstated,
				 cache,
				 not_orphaned);
	  }

	for(pkgCache::PrvIterator prv = targetPkg.ProvidesList();
	    !prv.end(); ++prv)
	  {
	    if(_system->VS->CheckDep(prv.ProvideVersion(),
				     dep->CompareOp,
				     dep.TargetVer()))
	      trace_not_orphaned(prv.OwnerPkg(),
				 reinstated,
				 cache,
				 not_orphaned);
	  }
      }
  }

  // If the given package is a direct
  // dependency/pre-dependency/recommendation of a manually installed
  // package, find the set of its transitive dependencies that's
  // closed under reinstatement.  NB: we can assume here that the
  // reinstated set is consistent (all dependencies will be met)
  // because any strong dependencies on stuff that was thrown out
  // would also have been thrown out.
  void find_not_orphaned(const pkgCache::PkgIterator &maybeOrphan,
			 const std::set<pkgCache::PkgIterator> &reinstated,
			 pkgDepCache &cache,
			 std::set<pkgCache::PkgIterator> &not_orphaned)
  {
    logging::LoggerPtr logger(Loggers::getAptitudeAptCache());

    // Sanity-check.
    if(maybeOrphan->CurrentState == pkgCache::State::NotInstalled ||
       maybeOrphan->CurrentState == pkgCache::State::ConfigFiles ||
       maybeOrphan.CurrentVer().end())
      {
	LOG_WARN(logger, "Sanity-check failed: assuming the package "
		 << maybeOrphan.FullName(false)
		 << " is orphaned, since it is not currently installed.");
	return;
      }

    pkgCache::VerIterator maybeOrphanCurrentVer(maybeOrphan.CurrentVer());
    for(pkgCache::DepIterator dep = maybeOrphan.RevDependsList();
	!dep.end(); ++dep)
      {
	if ((dep.ParentPkg() != maybeOrphan) &&
	    (dep->Type == pkgCache::Dep::Depends || dep->Type == pkgCache::Dep::PreDepends))
	  {
	    // only consider installed or to be installed packages
	    const pkgDepCache::StateCache& state = cache[dep.ParentPkg()];
	    if ( ! (state.Install() || (is_installed(dep.ParentPkg()) && ! state.Delete())))
	      {
		continue;
	      }

	    if(_system->VS->CheckDep(maybeOrphanCurrentVer.VerStr(),
				     dep->CompareOp,
				     dep.TargetVer()))
	      {
		trace_not_orphaned(maybeOrphan, reinstated, cache, not_orphaned);
	      }
	  }
      }


    for(pkgCache::PrvIterator prv = maybeOrphanCurrentVer.ProvidesList();
	!prv.end(); ++prv)
      for(pkgCache::DepIterator dep = prv.ParentPkg().RevDependsList();
	  !dep.end(); ++dep)
	{
	  if ((dep.ParentPkg() != maybeOrphan) &&
	      (dep->Type == pkgCache::Dep::Depends || dep->Type == pkgCache::Dep::PreDepends))
	    {
	      // only consider installed or to be installed packages
	      const pkgDepCache::StateCache& state = cache[dep.ParentPkg()];
	      if ( ! (state.Install() || (is_installed(dep.ParentPkg()) && ! state.Delete())))
		{
		  continue;
		}

	      if(_system->VS->CheckDep(prv.ProvideVersion(),
				       dep->CompareOp,
				       dep.TargetVer()))
		{
		  trace_not_orphaned(maybeOrphan, reinstated, cache, not_orphaned);
		}
	    }
	}
  }
}

void aptitudeDepCache::sweep()
{
  if(!aptcfg->FindB(PACKAGE "::Delete-Unused", true))
    return;

  logging::LoggerPtr logger(Loggers::getAptitudeAptCache());
  // "reinstated" holds packages that should be reinstated, and
  // "reinstated_bad" holds packages that *shouldn't* be reinstated
  // because they conflict with an installed package.
  //
  // We have to be careful because reinstating packages could lead to
  // situations where a valid resolver solution led to broken
  // dependencies.  For instance, the package that's being brought
  // back onto the system might conflict with another package that's
  // being installed by this solution!
  //
  // See Debian bugs #522881 and #524667.
  std::set<pkgCache::PkgIterator> reinstated, reinstated_bad;

  // Suppress intermediate removals.
  //
  // \todo this may cause problems if we do undo tracking via ActionGroups.
  pkgDepCache::ActionGroup group(*this);

  bool purge_unused = aptcfg->FindB(PACKAGE "::Purge-Unused", false);

  for(pkgCache::PkgIterator pkg = PkgBegin(); !pkg.end(); ++pkg)
    {
      if(PkgState[pkg->ID].Garbage)
	{
	  if(pkg.CurrentVer() != 0 && pkg->CurrentState != pkgCache::State::ConfigFiles)
	    {
	      // NB: apt sets the Garbage flag on packages that are
	      // being deleted and are automatic (this is due to some
	      // internal details of the mark&sweep algorithm and its
	      // aptitude heritage).  To compensate for this, we
	      // *only* set the unused-delete flag if the package was
	      // not previously being deleted.
	      if(!PkgState[pkg->ID].Delete())
		{
		  LOG_DEBUG(logger, "aptitudeDepCache::sweep(): Removing " << pkg.FullName(false) << ": it is unused.");

		  pre_package_state_changed();
		  MarkDelete(pkg, purge_unused);
		  package_states[pkg->ID].selection_state =
		    (purge_unused ? pkgCache::State::Purge : pkgCache::State::DeInstall);
		  package_states[pkg->ID].remove_reason = unused;
		}
	    }
	  else
	    {
	      if(pkg.CurrentVer().end())
		{
		  if((*this)[pkg].iFlags & Purge)
		    package_states[pkg->ID].selection_state = pkgCache::State::Purge;
		  else
		    package_states[pkg->ID].selection_state = pkgCache::State::DeInstall;
		}
	      else
		package_states[pkg->ID].selection_state = pkgCache::State::Install;
	      pre_package_state_changed();

	      if(!PkgState[pkg->ID].Keep())
		LOG_DEBUG(logger, "aptitudeDepCache::sweep(): Cancelling the installation of " << pkg.FullName(false) << ": it is unused.");

	      MarkKeep(pkg, false, false);
	    }
	}
      else if(PkgState[pkg->ID].Delete() && package_states[pkg->ID].remove_reason == unused)
	{
	  pkgCache::DepIterator conflict = is_conflicted(pkg.CurrentVer(), *this);
	  if(!conflict.end())
	    {
	      LOG_DEBUG(logger, "aptitudeDepCache::sweep(): not scheduling "
			<< pkg.FullName(false) << " for reinstatement due to the conflict between "
			<< conflict.ParentPkg().FullName(false)
			<< " and " << conflict.TargetPkg().FullName(false));

	      reinstated_bad.insert(pkg);
	    }
	  else
	    {
	      LOG_DEBUG(logger, "aptitudeDepCache::sweep(): provisionally scheduling "
			<< pkg.FullName(false) << " for reinstatement.");

	      reinstated.insert(pkg);
	    }
	}
    }

  // Remove packages that transitively depend on a package in
  // reinstated_bad from reinstated.
  for(std::set<pkgCache::PkgIterator>::const_iterator it =
	reinstated_bad.begin(); it != reinstated_bad.end(); ++it)
    remove_reverse_current_versions(reinstated, it->CurrentVer());

  // Figure out which reinstated packages aren't orphaned.
  std::set<pkgCache::PkgIterator> not_orphaned;
  for(std::set<pkgCache::PkgIterator>::const_iterator it =
	reinstated.begin(); it != reinstated.end(); ++it)
    find_not_orphaned(*it,
		      reinstated,
		      *this,
		      not_orphaned);

  // The ones that survived should be reinstated:
  for(std::set<pkgCache::PkgIterator>::const_iterator it =
	not_orphaned.begin(); it != not_orphaned.end(); ++it)
    {
      pkgCache::PkgIterator pkg(*it);
      LOG_INFO(logger, "aptitudeDepCache::sweep(): reinstating "
	       << pkg.FullName(false));
      MarkKeep(pkg, false, false);
    }
}

void aptitudeDepCache::begin_action_group()
{
  group_level++;
}

void aptitudeDepCache::end_action_group(undo_group *undo)
{
  std::set<pkgCache::PkgIterator> changed_packages;

  eassert(group_level>0);

  if(group_level==1)
    {
      if(read_only && !read_only_permission())
	{
	  if(group_level == 0)
	    read_only_fail();

	  group_level--;
	  return;
	}

      sweep();

      cleanup_after_change(undo, &changed_packages);

      duplicate_cache(&backup_state);

      package_state_changed();
      package_states_changed(&changed_packages);
    }

  group_level--;
}

const aptitudeDepCache::apt_state_snapshot *aptitudeDepCache::snapshot_apt_state()
{
  apt_state_snapshot *rval=new apt_state_snapshot;
  duplicate_cache(rval);

  return rval;
}

void aptitudeDepCache::restore_apt_state(const apt_state_snapshot *snapshot)
{
  if(read_only && !read_only_permission())
    {
      if(group_level == 0)
	read_only_fail();
      return;
    }

  memcpy(PkgState, snapshot->PkgState, sizeof(StateCache)*Head().PackageCount);
  memcpy(DepState, snapshot->DepState, sizeof(char)*Head().DependsCount);
  // memcpy doesn't work here because the aptitude_state structure
  // contains a std::string.  (would it be worthwhile/possible to
  // change things so that it doesn't?)
  for(unsigned int i=0; i<Head().PackageCount; ++i)
    package_states[i]=snapshot->AptitudeState[i];

  iUsrSize=snapshot->iUsrSize;
  iDownloadSize=snapshot->iDownloadSize;
  iInstCount=snapshot->iInstCount;
  iDelCount=snapshot->iDelCount;
  iKeepCount=snapshot->iKeepCount;
  iBrokenCount=snapshot->iBrokenCount;
  iBadCount=snapshot->iBadCount;
}

void aptitudeDepCache::apply_solution(const generic_solution<aptitude_universe> &realSol,
				      undo_group *undo)
{
  logging::LoggerPtr logger(Loggers::getAptitudeAptCache());

  // Make a local copy so we don't crash when applying the solution:
  // applying the solution might trigger a callback that causes
  // something else to throw away its reference to the solution =>
  // BOOM.
  const generic_solution<aptitude_universe> sol(realSol);

  LOG_DEBUG(logger, "Applying solution: " << sol);

  if(read_only && !read_only_permission())
    {
      if(group_level == 0)
	read_only_fail();
      LOG_DEBUG(logger, "Not applying solution: the cache is read-only.");
      return;
    }

  action_group group(*this, undo);

  pre_package_state_changed();

  // Build a list of all the resolver versions that are to be
  // installed: versions selected in the solution as well as the
  // versions that were initially installed.  The boolean values that
  // tag along indicate whether each version was automatically
  // installed (true if it was, false if it wasn't).
  std::vector<std::pair<aptitude_resolver_version, bool> > versions;

  LOG_TRACE(logger, "Collecting initial versions from the solution:");

  std::set<aptitude_resolver_version> initial_versions;
  sol.get_initial_state().get_initial_versions(initial_versions);
  for(std::set<aptitude_resolver_version>::const_iterator it =
	initial_versions.begin(); it != initial_versions.end(); ++it)
    {
      aptitude_resolver_version ver(*it);

      LOG_TRACE(logger, "Adding initial version: " << ver);
      versions.push_back(std::make_pair(*it, false));
    }

  for(generic_choice_set<aptitude_universe>::const_iterator
	i = sol.get_choices().begin();
      i != sol.get_choices().end(); ++i)
    {
      if(i->get_type() == generic_choice<aptitude_universe>::install_version)
	{
	  aptitude_resolver_version ver(i->get_ver());
	  LOG_TRACE(logger, "Adding version chosen by the resolver: " << ver);
	  // auto_installed true by default because "install_version" is very
	  // broad, but might mean to "keep the same version"
	  constexpr bool auto_installed = true;
	  versions.push_back(std::make_pair(ver, auto_installed));
	}
      else
	LOG_TRACE(logger, "Skipping " << *i << ": it is not a version install.");
    }

  for(std::vector<std::pair<aptitude_resolver_version, bool> >::const_iterator it =
	versions.begin(); it != versions.end(); ++it)
    {
      const bool is_auto = it->second;

      LOG_TRACE(logger, "Selecting " << it->first << " "
		<< (is_auto ? "automatically" : "manually"));

      pkgCache::PkgIterator pkg = it->first.get_pkg();
      pkgCache::VerIterator curver=pkg.CurrentVer();
      pkgCache::VerIterator instver = (*apt_cache_file)[pkg].InstVerIter(*apt_cache_file);
      pkgCache::VerIterator actionver = it->first.get_ver();

      // Check what type of action it is.
      if(actionver.end())
	{
	  LOG_TRACE(logger, "Removing " << pkg.FullName(false));

	  // removal.
	  internal_mark_delete(pkg, false, false);
	  if(is_auto && !curver.end())
	    get_ext_state(pkg).remove_reason = from_resolver;
	}
      else if(actionver == curver)
	{
	  LOG_TRACE(logger, "Keeping " << pkg.FullName(false)
		    << " at its current version ("
		    << curver.VerStr() << ")");

	  // auto_installed true because "install_version" is very broad, but
	  // might mean to "keep the same version"
	  bool was_auto = is_auto_installed(pkg);

	  internal_mark_keep(pkg, was_auto, false);
	}
      else
	// install a particular version that's not the current one.
	{
	  LOG_TRACE(logger, "Installing " << pkg.FullName(false) << " " << actionver.VerStr());

	  set_candidate_version(actionver, NULL);
	  internal_mark_install(pkg, false, false);
	  // Mark the package as automatic iff it isn't currently
	  // going to be installed.  Thus packages that are currently
	  // manually installed don't get marked as auto, packages
	  // that are going to be manually installed don't get marked
	  // as auto, but packages that are being removed *do* get
	  // marked as auto.
	  if(is_auto && instver.end())
	    MarkAuto(pkg, true);
	}
    }
}

aptitudeCacheFile::aptitudeCacheFile()
  :Map(NULL), Cache(NULL), DCache(NULL), have_system_lock(false), Policy(NULL)
{
}

aptitudeCacheFile::~aptitudeCacheFile()
{
  delete Cache;
  delete Map;
  ReleaseLock();

  delete DCache;
  delete Policy;
}

bool aptitudeCacheFile::Open(OpProgress* Progress,
			     bool do_initselections,
			     bool WithLock,
			     const char* status_fname,
			     bool reset_reinstall)
{
  if(WithLock)
    {
      if(!_system->Lock())
	return false;

      have_system_lock=true;
    }

  if(_error->PendingError())
    return false;

  pkgSourceList List;
  if(!List.ReadMainList())
    return _error->Error(_("The list of sources could not be read."));

  // Read the caches:
  bool Res = pkgCacheGenerator::MakeStatusCache(List, Progress, &Map, !WithLock);
  if (Progress)
    Progress->Done();

  if(!Res)
    return _error->Error(_("The package lists or status file could not be parsed or opened."));

  if(!_error->empty())
    _error->Warning(_("You may want to update the package lists to correct these missing files"));

  Cache=new pkgCache(Map);
  if(_error->PendingError())
    return false;

  Policy=new aptitudePolicy(Cache);
  if(_error->PendingError())
    return false;
  if(ReadPinFile(*Policy) == false || ReadPinDir(*Policy) == false)
    return false;

  {
    DCache=new aptitudeDepCache(Cache, Policy);
    if (_error->PendingError())
      {
	_error->Error(_("Could not create dependency cache"));
	return false;
      }

    bool init_result = DCache->Init(Progress, WithLock, do_initselections, status_fname, reset_reinstall);
    if (Progress)
      Progress->Done();

    if (!init_result || _error->PendingError())
      {
	_error->Error(_("Could not initialize dependency cache"));
	return false;
      }
  }

  return true;
}

void aptitudeCacheFile::ReleaseLock()
{
  if(have_system_lock)
    {
      _system->UnLock();
      have_system_lock=false;
    }
}

bool aptitudeCacheFile::GainLock()
{
  if(have_system_lock)
    return true;

  if(!_system->Lock())
    return false;

  have_system_lock=true;
  return true;
}

bool aptitudeDepCache::is_held(const PkgIterator &pkg)
{
  aptitude_state state=get_ext_state(pkg);

  pkgCache::VerIterator candver=(*this)[pkg].CandidateVerIter(*this);

  return !pkg.CurrentVer().end() &&
    (state.selection_state == pkgCache::State::Hold ||
     (!candver.end() && candver.VerStr() == state.forbidver));
}

bool aptitudeDepCache::MarkFollowsRecommends()
{
  return pkgDepCache::MarkFollowsRecommends() ||
    aptcfg->FindB("APT::Install-Recommends", true) ||
    aptcfg->FindB(PACKAGE "::Keep-Recommends", false);
}

bool aptitudeDepCache::MarkFollowsSuggests()
{
  return pkgDepCache::MarkFollowsSuggests() ||
    aptcfg->FindB(PACKAGE "::Keep-Suggests", false) ||
    aptcfg->FindB(PACKAGE "::Suggests-Important", false);
}

class AptitudeInRootSetFunc : public pkgDepCache::InRootSetFunc
{
  /** \brief A pointer to the cache in which we're matching. */
  aptitudeDepCache &cache;

  /** A pattern if one could be created; otherwise NULL. */
  cwidget::util::ref_ptr<aptitude::matching::pattern> p;

  /** \brief The search cache to use in applying this function. */
  cwidget::util::ref_ptr<aptitude::matching::search_cache> search_info;

  /** \b true if the package was successfully constructed. */
  bool constructedSuccessfully;

  /** The secondary test to apply.  Only set if creating the match
   *  succeeds.
   */
  pkgDepCache::InRootSetFunc *chain;
public:
  AptitudeInRootSetFunc(pkgDepCache::InRootSetFunc *_chain,
			aptitudeDepCache &_cache)
    : cache(_cache), p(NULL),
      search_info(aptitude::matching::search_cache::create()),
      constructedSuccessfully(false), chain(NULL)
  {
    std::string matchterm = aptcfg->Find(PACKAGE "::Keep-Unused-Pattern", "~nlinux-image-.*");
    if(matchterm.empty()) // Bug-compatibility with old versions.
      matchterm = aptcfg->Find(PACKAGE "::Delete-Unused-Pattern");

    if(matchterm.empty())
      constructedSuccessfully = true;
    else
      {
	p = aptitude::matching::parse(matchterm);
	if(p.valid())
	  constructedSuccessfully = true;
      }

    if(constructedSuccessfully)
      chain = _chain;
  }

  bool wasConstructedSuccessfully() const
  {
    return constructedSuccessfully;
  }

  bool InRootSet(const pkgCache::PkgIterator &pkg)
  {
    pkgRecords &records(cache.get_records());
    if(p.valid() && aptitude::matching::get_match(p, pkg, search_info, cache, records).valid())
      return true;
    else
      return chain != NULL && chain->InRootSet(pkg);
  }

  ~AptitudeInRootSetFunc()
  {
    delete chain;
  }
};

pkgDepCache::InRootSetFunc *aptitudeDepCache::GetRootSetFunc()
{
  InRootSetFunc *superFunc = pkgDepCache::GetRootSetFunc();

  AptitudeInRootSetFunc *f = new AptitudeInRootSetFunc(superFunc, *this);

  if(f->wasConstructedSuccessfully())
    return f;
  else
    {
      delete f;
      return superFunc;
    }
}

bool aptitudeDepCache::IsInstallOk(const pkgCache::PkgIterator &pkg,
				   bool AutoInst,
				   unsigned long Depth,
				   bool FromUser)
{
  if(Depth == 0)
    // This is a straight-up MarkInstall() call, not a dependency
    // resolution; allow it.
    return true;

  pkgCache::VerIterator candver((*this)[pkg].CandidateVerIter(*this));
  const aptitude_state &estate = get_ext_state(pkg);

  if(candver.end())
    {
      LOG_WARN(Loggers::getAptitudeAptCache(), "The package " << pkg.FullName(false) << " has no candidate version, unsure whether it should be installed.");
      return true;
    }

  if(estate.selection_state == pkgCache::State::Hold &&
     candver != pkg.CurrentVer())
    {
      LOG_INFO(Loggers::getAptitudeAptCache(), "Refusing to install version "
	       << candver.VerStr() << " of the held package "
	       << pkg.FullName(false));
      return false;
    }

  if(estate.forbidver == candver.VerStr())
    {
      LOG_INFO(Loggers::getAptitudeAptCache(),
	       "Refusing to install the forbidden version "
	       << candver.VerStr() << " of the package " << pkg.FullName(false));
      return false;
    }

  return true;
}

bool aptitudeDepCache::IsDeleteOk(const pkgCache::PkgIterator &pkg,
				  bool Purge,
				  unsigned long Depth,
				  bool FromUser)
{
  if(Depth == 0)
    // This is a straight-up MarkDelete() call, not a dependency
    // resolution; allow it.
    return true;

  if(!aptcfg->FindB(PACKAGE "::Auto-Install-Remove-Ok", false))
    return false;
  else
    {
      const aptitude_state &estate = get_ext_state(pkg);

      if(estate.selection_state == pkgCache::State::Hold)
	{
	  LOG_INFO(Loggers::getAptitudeAptCache(),
		   "Refusing to remove the held package "
		   << pkg.FullName(false));
	  return false;
	}

      return true;
    }
}
