#!/bin/sh

echo script args are: "$@"

docker run -it -v $(pwd)/:/p2p -u $(id -u):$(id -g) debian bash -c ./p2p/p2p.bin "$@"
