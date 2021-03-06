#!/bin/bash
#
# Test output AH
#  - 2 loop interfaces
#  - 10 packets
#  - Specify API mode on command line
odp_ipsec_api -i loop1,loop2 \
-r 192.168.222.2/32:loop2:08.00.27.F5.8B.DB \
-p 192.168.111.0/24:192.168.222.0/24:out:ah \
-a 192.168.111.2:192.168.222.2:md5:200:a731649644c5dee92cbd9c2e7e188ee6 \
-t 192.168.111.2:192.168.222.2:10.0.111.2:10.0.222.2 \
-s 192.168.111.2:192.168.222.2:loop1:loop2:10:100 \
-c 2 "$@"
