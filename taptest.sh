#!/bin/sh

set -x 
ip link set lower up
ip link set upper up
ip link set lower master newbr
bridge link set dev lower learning off
ip addr add 172.16.56.10/24 dev upper

