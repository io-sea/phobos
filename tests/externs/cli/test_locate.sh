#!/bin/bash

#
#  All rights reserved (c) 2014-2022 CEA/DAM.
#
#  This file is part of Phobos.
#
#  Phobos is free software: you can redistribute it and/or modify it under
#  the terms of the GNU Lesser General Public License as published by
#  the Free Software Foundation, either version 2.1 of the Licence, or
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
# Integration test for phobos_locate call
#

test_dir=$(dirname $(readlink -e $0))
medium_locker_bin="$test_dir/medium_locker"
. $test_dir/../../test_env.sh
. $test_dir/../../setup_db.sh
. $test_dir/../../test_launch_daemon.sh
. $test_dir/../../tape_drive.sh
. test_locate_common.sh

set -xe

function dir_setup
{
    export dirs="
        $(mktemp -d /tmp/test.pho.XXXX)
        $(mktemp -d /tmp/test.pho.XXXX)
    "
    $phobos dir add $dirs
    $phobos dir format --fs posix --unlock $dirs
}

function setup
{
    setup_tables
    invoke_daemon
    dir_setup
}

function tape_setup
{
    local N_TAPES=2
    local N_DRIVES=8
    local LTO5_TAGS=$TAGS,lto5
    local LTO6_TAGS=$TAGS,lto6

    # get LTO5 tapes
    local lto5_tapes="$(get_tapes L5 $N_TAPES)"
    $phobos tape add --tags $LTO5_TAGS --type lto5 "$lto5_tapes"

    # get LTO6 tapes
    local lto6_tapes="$(get_tapes L6 $N_TAPES)"
    $phobos tape add --tags $LTO6_TAGS --type lto6 "$lto6_tapes"

    # set tapes
    export tapes="$lto5_tapes,$lto6_tapes"

    # unlock all tapes
    for t in $tapes; do
        $phobos tape unlock $t
    done

    # get drives
    local drives=$(get_drives $N_DRIVES)
    $phobos drive add $drives

    # show a drive info
    local dr1=$(echo $drives | awk '{print $1}')
    # check drive status
    $phobos drive list --output adm_status $dr1 --format=csv |
        grep "^locked" || error "Drive should be added with locked state"

    # unlock all drives
    for d in $($phobos drive list); do
        $phobos drive unlock $d
    done

    # format lto5 tapes
    $phobos tape format $lto5_tapes --unlock
    # format lto6 tapes
    $phobos tape format $lto6_tapes --unlock
}

function cleanup
{
    waive_daemon
    drop_tables
    drain_all_drives
    rm -rf $dirs
    rm -rf /tmp/out*
}

function test_locate_compatibility
{
    local N_TAPES=2
    local N_DRIVES=2
    local LTO5_TAGS="lto5"
    local LTO6_TAGS="lto6"
    local self_hostname="$(uname -n)"
    local other_hostname="blob"
    local family="tape"

    # get LTO5 tapes
    local lto5_tapes="$(get_tapes L5 $N_TAPES)"
    $phobos tape add --tags $LTO5_TAGS --type lto5 "$lto5_tapes"

    # get LTO6 tapes
    local lto6_tapes="$(get_tapes L6 $N_TAPES)"
    $phobos tape add --tags $LTO6_TAGS --type lto6 "$lto6_tapes"

    # set tapes
    local tapes="$lto5_tapes,$lto6_tapes"

    # unlock all tapes
    for t in $tapes; do
        $phobos tape unlock $t
    done

    # get drives
    local lto6drives=$(get_lto_drives 6 $N_DRIVES)
    IFS=' ' read -r -a lto6drives <<< "$lto6drives"
    local self_lto6drive="${lto6drives[0]}"
    local other_lto6drive="${lto6drives[1]}"

    local lto5drives=$(get_lto_drives 5 $N_DRIVES)
    IFS=' ' read -r -a lto5drives <<< "$lto5drives"
    local self_lto5drive="${lto5drives[0]}"
    local other_lto5drive="${lto5drives[1]}"

    # first add to the current host one LT05 and one LT06 drive
    $phobos drive add --unlock $other_lto6drive
    $phobos drive add --unlock $other_lto5drive

    # format lto5 tapes
    $phobos tape format $lto5_tapes --unlock
    # format lto6 tapes
    $phobos tape format $lto6_tapes --unlock

    # add the second LT05 and LT06 drives to the current host
    $phobos drive add --unlock $self_lto5drive $self_lto6drive

    # put an object on the two LT06 tapes
    local oid="oid_get_locate"
    $phobos put --lyt-params repl_count=2 --tags $LTO6_TAGS /etc/hosts $oid ||
        error "Error while putting $oid"

    waive_daemon

    # unload the tapes so that the daemon doesn't take any lock when waking up
    drain_all_drives

    # change the host of one lto5 and one lto6 drive to give them to
    # $other_hostname
    $PSQL << EOF
UPDATE device SET host = '$other_hostname' WHERE path = '$other_lto5drive';
UPDATE device SET host = '$other_hostname' WHERE path = '$other_lto6drive';
EOF

    invoke_daemon

    # At this point:
    #   - the current host has one LT05 and one LTO6 drive
    #   - the other host has the other LT05 and LT06 drives

    local locate_hostname=$($valg_phobos locate $oid)
    if [ "$locate_hostname" != "$self_hostname" ]; then
        error "locate on $oid returned $locate_hostname instead of " \
              "$self_hostname when every medium is unlocked and " \
              "$self_hostname has compatible devices"
    fi

    # The previous locate added locks to the media with the hostname
    # $self_hostname, so --focus-host $other_hostname should be ignored here
    locate_hostname=$($valg_phobos locate --focus-host $other_hostname $oid)
    if [ "$locate_hostname" != "$self_hostname" ]; then
        error "locate on $oid returned $locate_hostname instead of " \
              "$self_hostname even though it has a lock on a replica "
    fi

    local tapes_to_unlock=$($phobos tape list -o name,lock_hostname |
                            grep $self_hostname | cut -d '|' -f2 | xargs)
    IFS=' ' read -r -a tapes_to_unlock <<< "$tapes_to_unlock"
    $phobos lock clean --force -f tape -t media -i ${tapes_to_unlock[@]}

    locate_hostname=$($valg_phobos locate --focus-host $other_hostname $oid)
    if [ "$locate_hostname" != "$other_hostname" ]; then
        error "locate on $oid returned $locate_hostname instead of " \
              "$other_hostname when every medium and drive is unlocked and " \
              "--focus-host is set to $other_hostname"
    fi

    # "lock clean" cannot be used here because the locks are attributed to
    # another hostname
    # Lock all the drives $self_hostname has access to
    $PSQL << EOF
DELETE FROM lock;
EOF
    $phobos drive lock $self_lto5drive $self_lto6drive

    locate_hostname=$($valg_phobos locate --focus-host $other_hostname $oid)
    if [ "$locate_hostname" != "$other_hostname" ]; then
        error "locate on $oid returned $locate_hostname instead of " \
              "$other_hostname when $self_hostname has no compatible " \
              "unlocked device"
    fi

    $PSQL << EOF
DELETE FROM lock;
EOF

    $valg_phobos locate --focus-host $self_hostname $oid &&
        error "locate on $oid did not fail even though $self_hostname " \
              "has no compatible device and $other_hostname has no lock " \
              "in the DB" ||
        true

    # lock all the drives $other_hostname has, unlock the drives $self_hostname
    # has, and remove all concurrency locks
    # "drive lock" cannot be used because the drive belongs to another host
    $PSQL << EOF
UPDATE device SET adm_status = 'locked' WHERE host = '$other_hostname';
DELETE FROM lock;
EOF

    $phobos drive unlock $self_lto5drive $self_lto6drive

    locate_hostname=$($valg_phobos locate --focus-host $self_hostname $oid)
    if [ "$locate_hostname" != "$self_hostname" ]; then
        error "locate on $oid returned $locate_hostname instead of " \
              "$self_hostname when focus-host is set to $self_hostname and" \
              "$other_hostname has no usable device"
    fi

    locate_hostname=$($valg_phobos locate --focus-host $other_hostname $oid)
    if [ "$locate_hostname" != "$self_hostname" ]; then
        error "locate on $oid returned $locate_hostname instead of " \
              "$self_hostname even though $other_hostname has no usable " \
              "device and $self_hostname has a lock on a replica"
    fi
}

trap cleanup EXIT
setup

# test locate on disk
export PHOBOS_STORE_default_family="dir"
test_medium_locate dir
test_locate_cli dir
test_get_locate_cli dir

if [[ -w /dev/changer ]]; then
    cleanup
    echo "Tape test mode"
    setup_tables
    invoke_daemon
    export PHOBOS_STORE_default_family="tape"
    tape_setup
    test_medium_locate tape
    test_locate_cli tape
    test_get_locate_cli tape

    cleanup
    setup_tables
    invoke_daemon
    test_locate_compatibility
fi
