#!/bin/bash
. $(dirname $0)/common.inc

./ld64 -help | grep -Eq -- '-dependency_info <FILE>[[:space:]]+Write dependency information'
! ./ld64 -help | grep -Eq -- '-dependency_info <FILE>[[:space:]]+Ignored' || false
