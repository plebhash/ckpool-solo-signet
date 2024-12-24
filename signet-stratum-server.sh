#!/bin/sh

# we assume build.sh has already been executed

STRATUM_SERVER_BIN=$PWD/stratum-server
STRATUM_SERVER_CONF=$STRATUM_SERVER_DIR/stratum-server.conf

./$STRATUM_SERVER_BIN -B -c $STRATUM_SERVER_CONF
