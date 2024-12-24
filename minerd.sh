#!/bin/sh

UPSTREAM_HOST=localhost
UPSTREAM_PORT=3333

USERNAME="tb1qqh5v9y0cj3272g398sza9prq6jyuwxzw7gygz4"
PASSWORD="x"

N_CORES=2

minerd -a sha256d -o stratum+tcp://$UPSTREAM_HOST:$UPSTREAM_PORT -q -D -P -t $N_CORES -u $USERNAME -p $PASSWORD
