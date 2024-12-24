<h1 align="center">
signet-stratum-server
</h1>

This is a fork of [`ckpool-solo`](https://bitbucket.org/ckolivas/ckpool-solo/) by the great Dr. CK.

It is modified to be able to mine Bitcoin `signet` blocks for the following purposes:
- educational workshops
- development setups

The name `ckpool-solo` was deliberately swapped for `stratum-server` to avoid confusing beginners (since we're not doing pooled mining).

# assumptions

- `bitcoind`:
  - connected to a private `signet` that does not require signatures (`signetchallenge=51`).
  - publishing blocks via ZMQ (`zmqpubhashblock=tcp:28332`)
- The `build.sh` script assumes an Ubuntu environment.

# build

Install the pre-requisistes for building:

```
$ sudo apt-get install build-essential yasm libzmq3-dev
```

Call the build script:

```
$ ./build.sh 
```

# config

Edit `stratum-server.conf`. The most important fields are:
- `btcd.auth`: RPC username on `bitcoind`
- `btcd.pass`: RPC password on `bitcoind`
- `btcaddress`: bitcoin address for coinbase payout
- `btcsig`: string for coinbase tag
- `mindiff`: min diff based on expected hashrate
- `startdiff`: start diff based on expected hashrate

# launch

```
$ signet-stratum-server.sh
```

# cpu mine

Install https://github.com/pooler/cpuminer

```shell
$ ./minerd
```
