// cmdline_changelog.h                        -*-c++-*-
//
// Copyright (C) 2004, 2010 Daniel Burrows
// Copyright (C) 2014-2018 Manuel A. Fernandez Montecelo
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

#ifndef CMDLINE_CHANGELOG_H
#define CMDLINE_CHANGELOG_H

#include <memory>
#include <string>
#include <vector>

/** \file cmdline_changelog.h
 */

namespace aptitude
{
  namespace cmdline
  {
    class terminal_metrics;
  }
}

/** \brief Display the changelog of each of the given package specifiers.
 *
 *  The specifiers are literal package names, with optional version/archive
 *  descriptors.  DumpErrors() is called after each changelog is displayed.
 */
void do_cmdline_changelog(const std::vector<std::string> &packages,
                          const std::shared_ptr<aptitude::cmdline::terminal_metrics> &term_metrics);

int cmdline_changelog(int argc, char *argv[]);

#endif // CMDLINE_CHANGELOG_H
