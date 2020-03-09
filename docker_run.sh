#!/bin/bash

PWD=$(pwd)
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

docker run -it -v $DIR/../:/simplx -u $(id -u):$(id -g) --rm volatilebitfield/cpp:$i bash -c ./p2p.bin 
