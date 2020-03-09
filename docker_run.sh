#!/bin/bash

PWD=$(pwd)
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

echo $PWD
echo $DIR

docker run -it -v $DIR/:/p2p -u $(id -u):$(id -g) debian bash -c ./p2p/p2p.bin 
