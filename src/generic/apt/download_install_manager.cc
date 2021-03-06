// download_install_manager.cc
//
//   Copyright (C) 2005-2011 Daniel Burrows
//   Copyright (C) 2015-2017 Manuel A. Fernandez Montecelo
//
//   This program is free software; you can redistribute it and/or
//   modify it under the terms of the GNU General Public License as
//   published by the Free Software Foundation; either version 2 of
//   the License, or (at your option) any later version.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//   General Public License for more details.
//
//   You should have received a copy of the GNU General Public License
//   along with this program; see the file COPYING.  If not, write to
//   the Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
//   Boston, MA 02110-1301, USA.

#include "download_install_manager.h"

#include "config_signal.h"
#include "download_signal_log.h"
#include "log.h"

#include <aptitude.h>

#include <generic/apt/apt.h>

#include <apt-pkg/acquire-item.h>
#include <apt-pkg/dpkgpm.h>
#include <apt-pkg/error.h>
#include <apt-pkg/install-progress.h>
#include <apt-pkg/sourcelist.h>
#include <apt-pkg/pkgsystem.h>

#include <sigc++/bind.h>

#include <pthread.h>
#include <signal.h>

using namespace std;


download_install_manager::download_install_manager(bool _download_only,
						   const run_dpkg_in_terminal_func &_run_dpkg_in_terminal)
  : log(NULL), download_only(_download_only), pm(_system->CreatePM(*apt_cache_file)),
    run_dpkg_in_terminal(_run_dpkg_in_terminal)
{
}

download_install_manager::~download_install_manager()
{
  delete pm;
}

bool download_install_manager::prepare(OpProgress &progress,
				       pkgAcquireStatus &acqlog,
				       download_signal_log *signallog)
{
  log = signallog;

  if(apt_cache_file == NULL)
    {
      _error->Error(_("The package cache is not available; unable to download and install packages."));
      return false;
    }

  if(!(*apt_cache_file)->save_selection_list(&progress))
    return false;

  // Abort here so we don't spew random messages below.
  if(_error->PendingError())
    return false;

  fetcher = new pkgAcquire;
  fetcher->SetLog(&acqlog);
  // even if we would like to not have to get locks and have to call
  // fetcher->Run() if no (remote) fetch is needed (see #766122), it is needed
  // anyway to work with local repositories (see #816537)
  if (fetcher->GetLock(aptcfg->FindDir("Dir::Cache::archives")) == false)
    {
      delete fetcher;
      fetcher = NULL;
      return false;
    }

  if(!src_list.ReadMainList())
    {
      _error->Error(_("Couldn't read source list"));

      delete fetcher;
      fetcher = NULL;
      return false;
    }

  if(!pm->GetArchives(fetcher, &src_list, apt_package_records) ||
     _error->PendingError())
    {
      _error->Error(_("Internal error: couldn't generate list of packages to download"));
      // has to be an Error (not e.g. Notice), otherwise "DumpError" (which
      // later phases rely on) doesn't include the following message
      _error->Error(_("Perhaps the package lists are out of date, please try 'aptitude update' (or equivalent); otherwise some packages or versions are not available from the current repository sources"));

      delete fetcher;
      fetcher = NULL;
      return false;
    }

  return true;
}

download_manager::result download_install_manager::finish_pre_dpkg(pkgAcquire::RunResult res)
{
  if(res != pkgAcquire::Continue)
    return failure;

  bool failed=false;
  for(pkgAcquire::ItemIterator i = fetcher->ItemsBegin();
      i != fetcher->ItemsEnd(); ++i)
    {
      if((*i)->Status == pkgAcquire::Item::StatDone &&
	 (*i)->Complete)
	continue;

      if((*i)->Status == pkgAcquire::Item::StatIdle)
	continue;

      failed=true;
      _error->Error(_("Failed to fetch %s: %s"), (*i)->DescURI().c_str(), (*i)->ErrorText.c_str());
      break;
    }

  if(download_only)
    {
      // TODO: Handle files on other CDROMs (StatIdle?).
      if(failed)
	{
	  _error->Error(_("Some files failed to download"));
	  return failure;
	}
      else
	{
	  return success;
	}
    }

  const char* fix_missing_cstr = "APT::Get::Fix-Missing";
  bool fix_missing = aptcfg->FindB(fix_missing_cstr, false);
  if (failed)
    {
      if (fix_missing)
	{
	  if (!pm->FixMissing())
	    {
	      _error->Error(_("Unable to correct for unavailable packages"));
	      return failure;
	    }
	}
      else
	{
	  _error->Error(_("Unable to fetch some packages; try '-o %s=true' to continue with missing packages"), fix_missing_cstr);
	  return failure;
	}
    }

  log_changes();

  // Note that someone could grab the lock before dpkg takes it;
  // without a more complicated synchronization protocol (and I don't
  // control the code at dpkg's end), them's the breaks.
  apt_cache_file->ReleaseLock();

  result rval = success;

  const pkgPackageManager::OrderResult pre_fork_result =
    pm->DoInstallPreFork();

  if(pre_fork_result == pkgPackageManager::Failed)
    rval = failure;

  return rval;
}

pkgPackageManager::OrderResult download_install_manager::run_dpkg(int status_fd)
{
  sigset_t allsignals;
  sigset_t oldsignals;
  sigfillset(&allsignals);

  pthread_sigmask(SIG_UNBLOCK, &allsignals, &oldsignals);
  std::unique_ptr<APT::Progress::PackageManager> progress;
  if (status_fd > 0)
    progress = std::make_unique<APT::Progress::PackageManagerProgressFd>(status_fd);
  else
    progress = std::unique_ptr<APT::Progress::PackageManager> { APT::Progress::PackageManagerProgressFactory() };
  pkgPackageManager::OrderResult pmres = pm->DoInstallPostFork(progress.get());

  switch(pmres)
    {
    case pkgPackageManager::Failed:
      _error->DumpErrors();
      //cout << _("Failed to perform requested operation on package.  Trying to recover:") << endl;
      if(system("DPKG_NO_TSTP=1 dpkg --configure -a") != 0) { /* ignore */ }
      break;
    case pkgPackageManager::Completed:
      break;

    case pkgPackageManager::Incomplete:
      break;
    }

  pthread_sigmask(SIG_SETMASK, &oldsignals, NULL);

  return pmres;
}

void download_install_manager::finish_post_dpkg(pkgPackageManager::OrderResult dpkg_result,
						OpProgress *progress,
						const sigc::slot1<void, result> &k)
{
  result rval = success;

  switch(dpkg_result)
    {
    case pkgPackageManager::Failed:
      rval = failure;
      break;
    case pkgPackageManager::Completed:
      break;

    case pkgPackageManager::Incomplete:
      rval = do_again;
      break;
    }

  fetcher->Shutdown();

  // Get the archives again.  This was necessary for multi-CD
  // installs, according to my comments in an old commit log in the
  // Subversion repository.
  if(!pm->GetArchives(fetcher, &src_list, apt_package_records))
    rval = failure;
  else if(!apt_cache_file->GainLock())
    // This really shouldn't happen.
    {
      _error->Error(_("Could not regain the system lock!  (Perhaps another apt or dpkg is running?)"));
      rval = failure;
    }

  if(rval != do_again)
    {
      apt_close_cache();

      if(log != NULL)
	log->Complete();

      // after the fix for '"reinstall" planned action is now preserved (Closes:
      // #255587, #785641)', we need to clear the state of the package,
      // otherwise it remains forever wanting to be reinstalled
      bool reset_reinstall = (dpkg_result == pkgPackageManager::Completed) ? true : false;

      // We absolutely need to do this here.  Yes, it slows things
      // down, but without this we get stuff like #429388 due to
      // inconsistencies between aptitude's state file and the real
      // world.
      //
      // This implicitly updates the package state file on disk.
      if(!download_only)
	{
	  bool operation_needs_lock = true;
	  apt_load_cache(progress, true, operation_needs_lock, nullptr, reset_reinstall);
	}

      if(aptcfg->FindB(PACKAGE "::Forget-New-On-Install", false)
	 && !download_only)
	{
	  if(apt_cache_file != NULL)
	    {
	      (*apt_cache_file)->forget_new(NULL);
	      (*apt_cache_file)->save_selection_list(progress);
	      post_forget_new_hook();
	    }
	}
    }

    if (rval == success && !download_only)
      {
	if (aptcfg->FindB(PACKAGE "::Clean-After-Install", false))
	  {
	    pre_clean_after_install_hook();

	    bool result_ok = aptitude::apt::clean_cache_dir();

	    if (!result_ok)
	      {
		rval = failure;
	      }

	    post_clean_after_install_hook();
	  }
      }

  k(rval);
}

void download_install_manager::finish(pkgAcquire::RunResult result,
				      OpProgress *progress,
				      const sigc::slot1<void, download_manager::result> &k)
{
  const download_manager::result pre_res = finish_pre_dpkg(result);

  if(pre_res == success && !download_only)
    {
      run_dpkg_in_terminal(sigc::mem_fun(*this, &download_install_manager::run_dpkg),
			   sigc::bind(sigc::mem_fun(*this, &download_install_manager::finish_post_dpkg),
				      progress,
				      k));
      return;
    }
  else
    {
      pkgPackageManager::OrderResult res;

      switch(pre_res)
	{
	case success:
	  res = pkgPackageManager::Completed;
	  break;
	case do_again:
	  res = pkgPackageManager::Incomplete;
	  break;
	case failure:
	default:
	  res = pkgPackageManager::Failed;
	  break;
	}

      finish_post_dpkg(res,
		       progress,
		       k);
      return;
    }
}
