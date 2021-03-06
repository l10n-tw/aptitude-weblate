// cmdline_clean.cc
//
// Copyright (C) 2004, 2010 Daniel Burrows
// Copyright (C) 2015-2016 Manuel A. Fernandez Montecelo
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; see the file COPYING.  If not, write to
// the Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
// Boston, MA 02110-1301, USA.


// Local includes:
#include "cmdline_clean.h"

#include "cmdline_util.h"

#include "text_progress.h"
#include "terminal.h"

#include <aptitude.h>

#include <generic/apt/apt.h>
#include <generic/apt/config_signal.h>


// System includes:
#include <apt-pkg/acquire.h>
#include <apt-pkg/clean.h>
#include <apt-pkg/error.h>
#include <apt-pkg/fileutl.h>
#include <apt-pkg/strutl.h>

#include <iostream>

#include <memory>

#include <cstdio>
#include <cstdint>
#include <sys/stat.h>
#include <unistd.h>

using namespace std;

using aptitude::cmdline::create_terminal;
using aptitude::cmdline::make_text_progress;
using aptitude::cmdline::terminal_io;
using aptitude::cmdline::terminal_locale;

int cmdline_clean(int argc, char *argv[], bool simulate)
{
  const string archivedir = aptcfg->FindDir("Dir::Cache::archives");

  aptitude::cmdline::on_apt_errors_print_and_die();

  if(argc != 1)
    {
      fprintf(stderr, _("E: The clean command takes no arguments\n"));
      return -1;
    }  

  if(simulate)
    {
      printf(_("Del %s* %spartial/*\n"), archivedir.c_str(), archivedir.c_str());
      return 0;
    }

  // do clean
  bool result_ok = aptitude::apt::clean_cache_dir();

  // results
  if (result_ok)
    {
      return 0;
    }
  else
    {
      _error->DumpErrors();
      return -1;
    }
}

// Shamelessly stolen from apt-get:
class LogCleaner : public pkgArchiveCleaner
{
  bool simulate;

  uint64_t total_size;

protected:
#if APT_PKG_ABI >= 590
  virtual void Erase(int dirfd, const char *File,const std::string &Pkg,const std::string &Ver,const struct stat &St) override
#else
  virtual void Erase(const char *File,std::string Pkg,std::string Ver,struct stat &St)
#endif
  {
    printf(_("Del %s %s [%sB]\n"),
	   Pkg.c_str(),
	   Ver.c_str(),
	   SizeToStr(St.st_size).c_str());

    if (!simulate)
      {
#if APT_PKG_ABI >= 590
	if(unlinkat(dirfd, File, 0)==0)
#else
	if(unlink(File)==0)
#endif
	  total_size+=St.st_size;
      }
    else
      total_size+=St.st_size;
  }

public:
  LogCleaner(bool _simulate) : simulate(_simulate), total_size(0) { }

  uint64_t get_total_size() const { return total_size; }
};

int cmdline_autoclean(int argc, char *argv[], bool simulate)
{
  const string archivedir = aptcfg->FindDir("Dir::Cache::archives");
  const std::shared_ptr<terminal_io> term = create_terminal();

  aptitude::cmdline::on_apt_errors_print_and_die();

  if(argc != 1)
    {
      fprintf(stderr, _("E: The autoclean command takes no arguments\n"));
      return -1;
    }  

  // Lock the archive directory
  FileFd lock;
  if (!simulate &&
      _config->FindB("Debug::NoLocking",false) == false)
    {
      lock.Fd(GetLock(archivedir + "lock"));
      if (_error->PendingError())
        {
          _error->Error(_("Unable to lock the download directory"));

	  aptitude::cmdline::on_apt_errors_print_and_die();
        }
    }

  std::shared_ptr<OpProgress> progress = make_text_progress(false, term, term, term);

  bool operation_needs_lock = true;
  apt_init(progress.get(), false, operation_needs_lock, nullptr);

  aptitude::cmdline::on_apt_errors_print_and_die();

  LogCleaner cleaner(simulate);
  int rval=0;
  if(!(cleaner.Go(archivedir, *apt_cache_file) &&
       cleaner.Go(archivedir+"partial/", *apt_cache_file)) ||
     _error->PendingError())
    rval=-1;

  aptitude::cmdline::on_apt_errors_print_and_die();

  if(simulate)
    printf(_("Would free %sB of disk space\n"),
	   SizeToStr(cleaner.get_total_size()).c_str());
  else
    printf(_("Freed %sB of disk space\n"),
	   SizeToStr(cleaner.get_total_size()).c_str());

  return rval;
}

