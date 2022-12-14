#!/bin/bash
# -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
# vim:expandtab:shiftwidth=4:tabstop=4:

#
#  All rights reserved (c) 2014-2022 CEA/DAM.
#
#  This file is part of Phobos.
#
#  Phobos is free software: you can redistribute it and/or modify it under
#  the terms of the GNU Lesser General Public License as published by
#  the Free Software Foundation, either version 2.1 of the License, or
#  (at your option) any later version.
#
#  Phobos is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU Lesser General Public License for more details.
#
#  You should have received a copy of the GNU Lesser General Public License
#  along with Phobos. If not, see <http://www.gnu.org/licenses/>.
#

#
# Wrapper to skip SCSI tests when /dev/changer is not writable
#

test_bin_dir=$(dirname $(readlink -e $0))
test_bin="$test_bin_dir/test_scsi"
. $test_bin_dir/../../test_env.sh

if  [[ -w /dev/changer ]]; then
    echo "library is accessible"
    echo "changer:"
    ls -ld /dev/changer

    # make sure no process uses the drive
    /usr/share/ltfs/ltfs stop || true

    $LOG_COMPILER $test_bin
else
    echo "Cannot access library: test skipped"
    exit 77 # special value to mark test as 'skipped'
fi
